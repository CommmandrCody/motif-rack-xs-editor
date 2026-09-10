#!/usr/bin/env python3
"""Normalise the raw parameter-table extraction into data/parameters.json.

Input : tools/extract_parameters.py output (raw table rows)
Output: a declarative parameter model keyed by scope, with parsed value ranges
        and the multi-byte encoding made explicit.

Yamaha packs values wider than 7 bits across several SysEx data bytes, in two
different ways, and the Data List signals which by how it names the rows:
  * "<name> MSB" / "<name> LSB"  -> 7 bits per byte  (encoding "msb_lsb")
  * a "bit 3-0 -> bit 15-12" note -> 4 bits per byte (encoding "nibbles")
Single-byte parameters are "direct".
"""
import json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCOPE_MAP = {
    "SYSTEM": "system",
    "Mode Change": "mode_change",
    "Part Set Control": "part_set_control",
    "Bulk Control": "bulk_control",
    "NORMAL VOICE COMMON": "normal_voice_common",
    "NORMAL VOICE ELEMENT": "normal_voice_element",
    "DRUM VOICE COMMON": "drum_voice_common",
    "DRUM VOICE KEY": "drum_voice_key",
    "MULTI COMMON for Pattern, Song": "multi_common",
    "MULTI PART": "multi_part",
}
HEX = re.compile(r"^[0-9A-F]+$")


def slug(scope, name):
    s = re.sub(r"[^A-Za-z0-9]+", "_", f"{scope}_{name}").strip("_").lower()
    return re.sub(r"_+", "_", s)


def clean_name(raw):
    n = re.split(r"\s+TOTAL SIZE", raw)[0].strip()
    # "Foo MSB Foo LSB" (and 4-byte nibble runs) collapse to the base name
    m = re.match(r"^(.*?)\s+MSB(?:\s+.*?\s+LSB)?$", n)
    if m and m.group(1):
        return m.group(1).strip(), True
    return n, False


DEC_RANGE = re.compile(r"(-?\d+)\s*-\s*(?:0\s*-\s*)?\+?(-?\d+)")


def parse_multibyte_range(range_text, description):
    """Raw value range for a parameter wider than one byte.

    The Data List's range column describes *each data byte* for multi-byte
    parameters ("00 - 7F" twice), so reading it as the value range gives
    nonsense like 0..1. The real range is in the description, in decimal
    ("0 - 255", "5 - 300", "off, 0001 - 6633").

    Returns (min, max) or (None, None) when it cannot be read confidently --
    callers must not range-check against a guess.
    """
    d = (description or "").replace("\u2013", "-").replace("\u2014", "-")
    # a decimal range with no fractional part is a raw value range
    for m in DEC_RANGE.finditer(d):
        lo_s, hi_s = m.group(1), m.group(2)
        start, end = m.span()
        if "." in d[max(0, start - 2):min(len(d), end + 3)]:
            continue          # "-102.4 - +102.3" is a display range, not raw
        lo, hi = int(lo_s), int(hi_s)
        if hi <= lo:
            continue
        if d.lstrip().lower().startswith("off"):
            lo = 0            # "off, 0001 - 6633" -> 0 means off
        return lo, hi
    return None, None


def parse_range(rng, size):
    """Return (min, max, literal_values) from a Data List range cell."""
    r = rng.replace("–", "-").replace("—", "-").strip().rstrip(",")
    if not r or r == "-":
        return None, None, []
    literals, lo, hi = [], None, None
    for part in [p.strip() for p in r.split(",") if p.strip()]:
        m = re.match(r"^([0-9A-F]+)\s*-\s*([0-9A-F]+)$", part)
        if m:
            a, b = int(m.group(1), 16), int(m.group(2), 16)
            lo = a if lo is None else min(lo, a)
            hi = b if hi is None else max(hi, b)
        elif HEX.match(part):
            literals.append(int(part, 16))
    if lo is None and literals:
        lo, hi = min(literals), max(literals)
    return lo, hi, sorted(set(literals))


def main():
    raw = json.loads(subprocess.run(
        [sys.executable, str(ROOT / "tools" / "extract_parameters.py")],
        capture_output=True, text=True, check=True).stdout)

    out, seen = [], set()
    for r in raw:
        scope = SCOPE_MAP.get(r["scope"], r["scope"])
        name, is_msb_lsb = clean_name(r["name"])
        if not name:
            name = "reserved"
        lo, hi, lits = parse_range(r["range"], r["size"])
        if r["size"] > 1:
            # the range column describes one byte; the real range is in prose
            lo, hi = parse_multibyte_range(r["range"], r["description"])
            lits = []

        if r["size"] == 1:
            enc = "direct"
        elif is_msb_lsb or any("MSB" in x for x in [r["name"]]):
            enc = "msb_lsb"
        elif "bit 3-0" in r["description"] or "bit 3-0" in r["notes"]:
            enc = "nibbles"
        else:
            enc = "msb_lsb"

        pid = slug(scope, name)
        n, base = 2, pid
        while pid in seen:
            pid = f"{base}_{n}"; n += 1
        seen.add(pid)

        out.append({
            "source": "datalist",
            "id": pid,
            "scope": scope,
            "name": name,
            "address": {"high": r["address"][0], "mid": r["address"][1], "low": r["address"][2]},
            "size": r["size"],
            "encoding": enc,
            "min": lo, "max": hi,
            "literals": lits,
            "range_text": r["range"],
            "description": r["description"],
            "notes": r["notes"],
            "reserved": name == "reserved",
            "source_page": r["page"],
        })

    # Merge in parameters established by hardware experiment where Yamaha
    # publishes no table. Kept in a separate file so regenerating from the PDF
    # never silently drops them, and so their provenance stays visible.
    extra_path = ROOT / "data" / "parameters_discovered.json"
    extra = []
    if extra_path.exists():
        extra = json.loads(extra_path.read_text())["parameters"]
        known = {p["id"] for p in out}
        for e in extra:
            if e["id"] in known:
                # A silent skip here would quietly drop a hardware finding.
                print(f"  warning: discovered parameter {e['id']!r} collides "
                      f"with a documented one; not merged", file=sys.stderr)
                continue
            e.setdefault("source", "hardware")
            e.setdefault("source_page", None)
            out.append(e)

    doc = {
        "device": "Yamaha MOTIF-RACK XS",
        "model_id": ["7F", "03"],
        "source": {
            "document": "MOTIF-RACK XS Data List",
            "file": "reference/motifrackxs_datalist.pdf",
            "section": "MIDI Data Table, pages 64-76",
        },
        "address_note": ("Address bytes are High/Mid/Low. Lower-case letters are "
                         "variable: ee=element/key index, pp=part index, nn=program "
                         "number, and 3n/5n/6n embed a table index in the low nibble."),
        "count": len(out),
        "parameters": out,
    }
    (ROOT / "data").mkdir(exist_ok=True)
    (ROOT / "data" / "parameters.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False))
    print(f"wrote data/parameters.json  ({len(out)} parameters, "
          f"{sum(1 for p in out if not p['reserved'])} non-reserved, "
          f"{sum(1 for p in out if p.get('source') == 'hardware')} hardware-discovered)")
    from collections import Counter
    for k, v in Counter(p["scope"] for p in out).most_common():
        print(f"  {v:5d}  {k}")


if __name__ == "__main__":
    main()
