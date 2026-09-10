#!/usr/bin/env python3
"""Extract the MOTIF-RACK XS Arpeggio Type List into data/arpeggios.json.

Authority: reference/motifrackxs_arpeggio_type_list.pdf (48 pages).
This is a *separate* download from the Data List, whose table of contents does
not mention it.

Layout notes
------------
* Two-column A4; each half-page column carries one arpeggio per line, so the
  columns are cropped apart before parsing.
* Ten columns:
      Main Category | Sub Category | ARP No. | ARP Name | Time Signature |
      Length | Original Tempo | Accent | Random SFX | Voice Type
* The Accent and Random SFX flags are marked with a capital "O"; which of the
  two a given "O" belongs to is decided by its character offset relative to the
  header labels, since both columns are otherwise blank.
* "Voice Type" uses a ditto mark (":") meaning "same as the row above"; those
  are forward-filled. Values in quotation marks name an actual MOTIF-RACK XS
  voice rather than a general voice type.
"""
import json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "motifrackxs_arpeggio_type_list.pdf"
PAGES = range(2, 49)

ROW = re.compile(r"^\s*(\S+)\s+(\S+)\s+(\d{1,4})\s+(.+?)\s+(\d+/\d+)\s+(\d+)\s+(\d+)\s*(.*)$")


def column_text(page, left):
    x, w = (0, 298) if left else (298, 297)
    return subprocess.run(
        ["pdftotext", "-layout", "-f", str(page), "-l", str(page),
         "-x", str(x), "-y", "0", "-W", str(w), "-H", "842", str(PDF), "-"],
        capture_output=True, text=True, check=True).stdout.splitlines()


# A flag is a bare capital "O" standing alone in its column -- never a letter
# inside a word, or a voice type such as "Rock Organ" would read as flagged.
LONE_O = re.compile(r"(?<![^\s])O(?![^\s])")
# Header label offsets drift by a few characters from page to page (Accent at
# 71-76, SFX at 80-85) but the two flag columns are far enough apart that any
# threshold in 76..82 separates them on every page; 79 is the fallback for the
# three columns that carry no header.
DEFAULT_FLAG_SPLIT = 79.0


def flag_split(lines):
    """Character offset separating the Accent column from the Random SFX column."""
    acc = sfx = None
    for l in lines[:8]:
        if acc is None and "Accent" in l:
            acc = l.index("Accent") + len("Accent") / 2
        m = re.search(r"\bSFX\b", l)
        if sfx is None and m:
            sfx = m.start() + 1.5
    if acc is None or sfx is None:
        return None
    return (acc + sfx) / 2


def parse():
    rows = []
    last_voice_type = None
    for page in PAGES:
        for left in (True, False):
            lines = column_text(page, left)
            # resolved per column: offsets shift page to page, so never inherit
            split = flag_split(lines)
            if split is None:
                split = DEFAULT_FLAG_SPLIT
            for line in lines:
                m = ROW.match(line.rstrip("\n"))
                if not m:
                    continue
                main, sub, num, name, sig, length, tempo, tail = m.groups()
                if not (1 <= int(num) <= 9999):
                    continue

                accent = random_sfx = False
                for mo in LONE_O.finditer(line):
                    if mo.start() < m.start(8):
                        continue               # not in the flag region
                    if mo.start() < split:
                        accent = True
                    else:
                        random_sfx = True

                vt = LONE_O.sub("", tail).strip()
                if vt in (":", ""):
                    vt = last_voice_type
                else:
                    last_voice_type = vt

                rows.append({
                    "number": int(num),
                    "name": name.strip(),
                    "main_category": main,
                    "sub_category": sub,
                    "time_signature": sig,
                    "length": int(length),
                    "original_tempo": int(tempo),
                    "accent": accent,
                    "random_sfx": random_sfx,
                    "voice_type": vt,
                })
    return rows


def main():
    rows = parse()
    rows.sort(key=lambda r: r["number"])
    seen, uniq = set(), []
    for r in rows:
        if r["number"] in seen:
            continue
        seen.add(r["number"]); uniq.append(r)

    nums = [r["number"] for r in uniq]
    gaps = [n for n in range(1, max(nums) + 1) if n not in seen] if nums else []

    doc = {
        "device": "Yamaha MOTIF-RACK XS",
        "source": {"document": "MOTIF-RACK XS Arpeggio Type List",
                   "file": "reference/motifrackxs_arpeggio_type_list.pdf",
                   "note": "Separate online document; not listed in the Data List contents."},
        "count": len(uniq),
        "range": [min(nums), max(nums)] if nums else None,
        "voice_type_note": ("Values in quotation marks name a specific MOTIF-RACK XS "
                            "voice; others are general voice types. For the Cntr "
                            "category the field carries the programmed Control Change "
                            "number or Pitch Bend range instead."),
        "arpeggios": uniq,
    }
    (ROOT / "data").mkdir(exist_ok=True)
    (ROOT / "data" / "arpeggios.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False))

    from collections import Counter
    print(f"wrote data/arpeggios.json  ({len(uniq)} arpeggios, "
          f"numbers {min(nums)}-{max(nums)}, {len(gaps)} gaps)")
    if gaps:
        print(f"  missing numbers (first 20): {gaps[:20]}")
    print(f"  accent flagged     : {sum(1 for r in uniq if r['accent'])}")
    print(f"  random SFX flagged : {sum(1 for r in uniq if r['random_sfx'])}")
    print("  main categories    :")
    for k, v in Counter(r["main_category"] for r in uniq).most_common():
        print(f"     {k:<8} {v}")


if __name__ == "__main__":
    main()
