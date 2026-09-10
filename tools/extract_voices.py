#!/usr/bin/env python3
"""Extract the factory Voice and Drum Voice lists into data/voices.json.

Authority: reference/motifrackxs_datalist.pdf
  Voice List       pages 2-10   (Normal Voices: PRE1-8, USR1-3, GM)
  Drum Voice List  pages 11-12  (Drum Voices: Preset, GM)

Both are two-column layouts; each half-page column holds one voice per line, so
the columns are cropped apart before parsing.

Program numbers are emitted 0-based, as MIDI Program Change carries them.
"""
import json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "motifrackxs_datalist.pdf"

BANK_RE = re.compile(r"^(PRE\d|USR\d|GM|Preset|User)\s*\(MSB=(\d+),\s*LSB=(\d+)\)")
# greedy name leaves exactly the trailing 4 category codes + polyphony
VOICE_RE = re.compile(
    r"^\s*(\d{1,3})\s+([A-H]\d{2})\s+(.+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\d+)\s*$")
DRUM_RE = re.compile(r"^\s*(\d{1,3})\s+(\S.*?)\s*$")


def column_lines(page, left):
    x, w = (0, 300) if left else (300, 295)
    return subprocess.run(
        ["pdftotext", "-layout", "-f", str(page), "-l", str(page),
         "-x", str(x), "-y", "0", "-W", str(w), "-H", "842", str(PDF), "-"],
        capture_output=True, text=True, check=True).stdout.splitlines()


def effective_banks(lines):
    """Bank header -> row mapping for one page column.

    A bank header only takes effect if voice rows actually follow it in the same
    column. The USR1-3 headers are declarations with no listing under them
    ("Any of the Preset Voices is pre-stored to each voice number of the User
    Banks"), and they sit at the foot of a column whose facing column continues
    the previous bank -- so honouring them blindly re-labels PRE8's second half.
    """
    hdrs = [(i, BANK_RE.match(l.strip())) for i, l in enumerate(lines)]
    hdrs = [(i, m) for i, m in hdrs if m]
    live = set()
    for k, (i, m) in enumerate(hdrs):
        end = hdrs[k + 1][0] if k + 1 < len(hdrs) else len(lines)
        if any(VOICE_RE.match(l) for l in lines[i + 1:end]):
            live.add(i)
    return live


def parse_normal(pages):
    out, bank, declared = [], None, {}
    for page in pages:
        for left in (True, False):
            lines = column_lines(page, left)
            live = effective_banks(lines)
            for i, line in enumerate(lines):
                m = BANK_RE.match(line.strip())
                if m:
                    b = {"name": m.group(1), "msb": int(m.group(2)), "lsb": int(m.group(3))}
                    declared[b["name"]] = b
                    if i in live:
                        bank = b
                    continue
                if not bank:
                    continue
                v = VOICE_RE.match(line)
                if not v:
                    continue
                num = int(v.group(1))
                if not 1 <= num <= 128:
                    continue
                out.append({
                    "bank": bank["name"], "msb": bank["msb"], "lsb": bank["lsb"],
                    "program": num - 1,
                    "slot": v.group(2),
                    "name": v.group(3).strip(),
                    "category_main": v.group(4), "category_sub": v.group(5),
                    "category2_main": v.group(6), "category2_sub": v.group(7),
                    "polyphony": int(v.group(8)),
                    "type": "normal",
                })
    return out, declared


def parse_drum(pages):
    out, bank, in_list = [], None, False
    for page in pages:
        for left in (True, False):
            for line in column_lines(page, left):
                s = line.strip()
                m = BANK_RE.match(s)
                if m:
                    bank = {"name": m.group(1), "msb": int(m.group(2)), "lsb": int(m.group(3))}
                    in_list = True
                    continue
                if s.startswith("Drum Kit Assign") or "Waveform List" in s:
                    in_list = False
                if not (bank and in_list):
                    continue
                if s.startswith("Pre No") or s.startswith("Usr No") or not s:
                    continue
                d = DRUM_RE.match(line)
                if not d:
                    continue
                num, name = int(d.group(1)), d.group(2).strip()
                if not 1 <= num <= 128 or len(name) < 3 or name[0].isdigit():
                    continue
                out.append({
                    "bank": bank["name"], "msb": bank["msb"], "lsb": bank["lsb"],
                    "program": num - 1, "slot": None, "name": name,
                    "category_main": "Drum", "category_sub": None,
                    "category2_main": None, "category2_sub": None,
                    "polyphony": None, "type": "drum",
                })
    return out


def main():
    normal, declared = parse_normal(range(2, 11))
    drum = parse_drum(range(11, 13))
    voices = normal + drum

    doc = {
        "device": "Yamaha MOTIF-RACK XS",
        "source": {"document": "MOTIF-RACK XS Data List",
                   "file": "reference/motifrackxs_datalist.pdf",
                   "sections": "Voice List pp.2-10, Drum Voice List pp.11-12"},
        "program_numbers": "0-based, as sent by MIDI Program Change",
        "count": len(voices),
        "banks": sorted(declared.values(), key=lambda b: (b["msb"], b["lsb"])),
        "user_banks_note": ("USR1-3 (MSB=63, LSB=8-10) are user-writable and ship "
                            "pre-loaded with preset voices; the Data List publishes "
                            "no listing for them, so their contents must be read "
                            "from the device."),
        "voices": voices,
    }
    (ROOT / "data").mkdir(exist_ok=True)
    (ROOT / "data" / "voices.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False))

    from collections import Counter
    per = Counter((v["bank"], v["msb"], v["lsb"]) for v in voices)
    print(f"wrote data/voices.json  ({len(voices)} voices)")
    for k in sorted(per, key=lambda k: (k[1], k[2])):
        print(f"  {k[0]:<7} MSB={k[1]:<4} LSB={k[2]:<3} {per[k]:>4} voices")


if __name__ == "__main__":
    main()
