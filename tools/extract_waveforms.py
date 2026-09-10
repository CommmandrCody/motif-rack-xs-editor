#!/usr/bin/env python3
"""Extract the Waveform List into data/waveforms.json.

Authority: reference/motifrackxs_datalist.pdf, "Waveform List" (pp. 29-33).

Layout: six repeated groups of
    No | Category | Name
across the page, so a single text line holds up to six entries. The fields
inside an entry are separated by the same kind of whitespace run that separates
one entry from the next, and a long name can leave only a single space before
the following entry ("P-Bass Rndwound Med+ 467  St  Pizzicato1 St"), so no
whitespace rule can split the line reliably.

The six groups sit at a measured 85.05pt pitch (the "No" headers are at
44.7, 129.8, 214.8, 299.8, 384.9, 469.9), so each group is cropped separately
and only the first entry on each cropped line is taken -- anything bleeding in
from the next group is ignored.

Validation: the element Wave Number parameter documents its range as 1 - 2670,
so a correct extraction yields exactly 2670 contiguous waveforms.
"""
import json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "motifrackxs_datalist.pdf"
PAGES = range(29, 34)

GROUP_X0 = 42.0     # just left of the first "No" column
GROUP_PITCH = 85.05
GROUPS = 6
CROP_W = 130        # generous: long names must not be clipped

# number, short category token, then the name up to the next number-like token.
# Not anchored to the line start: an over-long name in the previous group spills
# into this crop, so the line can begin mid-word ("d+ 467  St  Pizzicato1 St").
ENTRY = re.compile(
    r"(\d{1,4})\s+([A-Za-z][A-Za-z.]{0,3})\s+(\S.*?)"
    r"(?=\s\s+\d{1,4}\s|\s*$)")
MAX_SPILL = 16          # how far into the crop this group's number may start


def group_lines(page, group):
    x = int(GROUP_X0 + GROUP_PITCH * group)   # pdftotext wants integers
    return subprocess.run(
        ["pdftotext", "-layout", "-f", str(page), "-l", str(page),
         "-x", str(x), "-y", "0", "-W", str(CROP_W), "-H", "842", str(PDF), "-"],
        capture_output=True, text=True, check=True).stdout.splitlines()


def parse():
    found = {}
    for page in PAGES:
        for group in range(GROUPS):
            for line in group_lines(page, group):
                if "Category" in line or "Waveform" in line:
                    continue
                m = ENTRY.search(line.rstrip())     # first entry only
                if not m or m.start(1) > MAX_SPILL:
                    continue
                num = int(m.group(1))
                name = m.group(3).strip()
                if not (1 <= num <= 4000) or not name:
                    continue
                found.setdefault(num, {"number": num,
                                       "category": m.group(2),
                                       "name": name})
    return [found[k] for k in sorted(found)]


def main():
    waves = parse()
    nums = [w["number"] for w in waves]
    gaps = [n for n in range(1, max(nums) + 1) if n not in set(nums)] if nums else []
    doc = {
        "device": "Yamaha MOTIF-RACK XS",
        "source": {"document": "MOTIF-RACK XS Data List",
                   "file": "reference/motifrackxs_datalist.pdf",
                   "section": "Waveform List, pages 29-33"},
        "count": len(waves),
        "range": [min(nums), max(nums)] if nums else None,
        "waveforms": waves,
    }
    (ROOT / "data").mkdir(exist_ok=True)
    (ROOT / "data" / "waveforms.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False))
    print(f"wrote data/waveforms.json  ({len(waves)} waveforms, "
          f"{min(nums)}-{max(nums)}, {len(gaps)} gaps)")
    if gaps:
        print(f"  missing: {gaps[:20]}{' ...' if len(gaps) > 20 else ''}", file=sys.stderr)
    from collections import Counter
    for k, v in Counter(w["category"] for w in waves).most_common(8):
        print(f"    {k:<5} {v}")


if __name__ == "__main__":
    main()
