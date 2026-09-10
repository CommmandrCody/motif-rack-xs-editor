#!/usr/bin/env python3
"""Extract the MIDI PARAMETER CHANGE TABLEs from the MOTIF-RACK XS Data List.

Authority: reference/motifrackxs_datalist.pdf, pages 64-76 ("MIDI Data Table").

Approach
--------
The pages are two-column A4 and each table header wraps onto 2-3 lines, so
character-offset slicing of `pdftotext -layout` drifts badly. This reads true
word coordinates from `pdftotext -bbox-layout` instead.

Column boundaries: the header labels are *centred* over their columns while the
cell content is left-aligned, so a label's own x is not a usable edge (measured
offsets run from -4pt to +13pt). Instead the real boundary is taken to be a
vertical whitespace corridor running the height of the table body, and the
header labels are used only to decide which corridor separates which pair of
columns. The header vocabulary is identical across all ten tables:

    Address | Size | Data Range (HEX) | Parameter Name | Description | Notes

Addresses: Yamaha prints only the bytes that change from one row to the next,
always a *suffix* of (High, Mid, Low) -- e.g. "40 00 00" then "01", "02". So
the printed tokens fill the low end and the unstated high bytes are carried
forward, including across page and column breaks within one table.
"""
import json, re, subprocess, sys
from collections import defaultdict, Counter
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
PDF = ROOT / "reference" / "motifrackxs_datalist.pdf"
PAGES = list(range(64, 77))
PAGE_SPLIT = 298.0        # two-column page boundary, in points
MIN_GAP = 3.5             # narrowest whitespace corridor counted as a separator
COLUMN_OFFSET = 266.5     # fixed page grid: right column x == left column x + this
RUNNING_HEAD = ("MIDI Data Table", "MOTIF-RACK XS Data List")
NS = "{http://www.w3.org/1999/xhtml}"

ADDR_TOK = re.compile(r"^(?:[0-9A-F]{2}|nn|pp|ee|mm|kk|[0-9A-F]n)$")
# Range cells are lexically distinctive: uppercase hex, en-dashes and commas.
# Parameter names never look like this, which lets the Address/Size/Range/Name
# boundaries be recovered by token shape rather than by fragile x-positions.
RANGE_TOK = re.compile(r"^(?:[0-9A-F]{2,4},?|[-–,]|\*\d?)$")
HDR_LABELS = ("Address", "Size", "Data", "Range", "Parameter", "Name",
              "Description", "Notes", "(HEX)")
LABEL_GROUPS = [
    ("address",     ("Address",)),
    ("size",        ("Size",)),
    ("range",       ("Data", "Range", "(HEX)")),
    ("name",        ("Parameter", "Name")),
    ("description", ("Description",)),
    ("notes",       ("Notes",)),
]


def words_for_page(page):
    xml = subprocess.run(
        ["pdftotext", "-bbox-layout", "-f", str(page), "-l", str(page), str(PDF), "-"],
        capture_output=True, text=True, check=True).stdout
    out = []
    for w in ET.fromstring(xml).iter(f"{NS}word"):
        t = (w.text or "").strip()
        if t:
            out.append({"x": float(w.get("xMin")), "x2": float(w.get("xMax")),
                        "y": float(w.get("yMin")), "y2": float(w.get("yMax")), "t": t})
    return out


def group_lines(words, tol=3.0):
    ws = sorted(words, key=lambda w: ((w["y"] + w["y2"]) / 2, w["x"]))
    lines, cur, base = [], [], None
    for w in ws:
        mid = (w["y"] + w["y2"]) / 2
        if base is None or abs(mid - base) <= tol:
            cur.append(w)
            if base is None:
                base = mid
        else:
            lines.append(sorted(cur, key=lambda z: z["x"]))
            cur, base = [w], mid
    if cur:
        lines.append(sorted(cur, key=lambda z: z["x"]))
    return lines


def is_header_line(line):
    t = [w["t"] for w in line]
    return "Address" in t and "Size" in t


def label_centres(hdr_words):
    pos = defaultdict(list)
    for w in hdr_words:
        pos[w["t"]].append(w)
    out = []
    for key, labels in LABEL_GROUPS:
        ws = [w for lab in labels for w in pos.get(lab, [])]
        if ws:
            out.append((key, (min(w["x"] for w in ws) + max(w["x2"] for w in ws)) / 2))
    return out


def corridors(words, min_gap):
    if not words:
        return []
    lo = min(w["x"] for w in words) - 1
    hi = max(w["x2"] for w in words) + 1
    n = int(hi - lo) + 2
    occ = bytearray(n)
    for w in words:
        a = max(0, int(w["x"] - lo)); b = min(n - 1, int(w["x2"] - lo) + 1)
        for i in range(a, b + 1):
            occ[i] = 1
    gaps, start = [], None
    for i in range(n):
        if not occ[i] and start is None:
            start = i
        elif occ[i] and start is not None:
            if i - start >= min_gap:
                gaps.append((lo + start, lo + i))
            start = None
    return gaps


def build_starts(hdr_words, body_lines):
    centres = label_centres(hdr_words)
    if len(centres) < 3:
        return None
    gaps = corridors([w for ln in body_lines for w in ln], MIN_GAP)
    starts = [(centres[0][0], -1e9)]
    for i in range(len(centres) - 1):
        c0, c1 = centres[i][1], centres[i + 1][1]
        between = [g for g in gaps if c0 < (g[0] + g[1]) / 2 < c1]
        if between:
            # widest corridor wins; ties break leftwards. A genuine column
            # separator carries more padding than an incidental word gap.
            g = max(between, key=lambda g: (g[1] - g[0], -g[0]))
            edge = (g[0] + g[1]) / 2
        else:
            edge = (c0 + c1) / 2
        starts.append((centres[i + 1][0], edge))
    return starts


def cell_of(word, starts):
    key = starts[0][0]
    for name, sx in starts:
        if word["x"] >= sx:
            key = name
        else:
            break
    return key


def split_cells(line, starts):
    cells = defaultdict(list)
    for w in line:
        cells[cell_of(w, starts)].append(w)
    return cells


def parse():
    rows = []
    cur_title, pending = None, None
    carry = [None, None, None]          # last seen High/Mid/Low, per table
    side_starts = {}                    # column geometry per page side, per table

    for page in PAGES:
        words = words_for_page(page)
        for side in (0, 1):
            sel = [w for w in words if (w["x"] < PAGE_SPLIT) == (side == 0)]
            lines = group_lines(sel)
            lines = [ln for ln in lines
                     if " ".join(w["t"] for w in ln).strip() not in RUNNING_HEAD]

            # A table may continue into the next column with no repeated header.
            has_header = any(is_header_line(ln) for ln in lines)
            has_title = any("MIDI PARAMETER CHANGE TABLE" in " ".join(w["t"] for w in ln)
                            for ln in lines)
            if lines and not has_header and not has_title and cur_title:
                st = side_starts.get(side)
                if st is None and side_starts.get(1 - side):
                    d = COLUMN_OFFSET if side == 1 else -COLUMN_OFFSET
                    st = [(k, v + d if v > -1e8 else v)
                          for k, v in side_starts[1 - side]]
                if st:
                    body = [ln for ln in lines
                            if not re.fullmatch(r"\d{1,3}", " ".join(w["t"] for w in ln).strip())]
                    new, carry = emit(body, st, cur_title, page, carry)
                    rows.extend(new)
                continue

            i = 0
            while i < len(lines):
                txt = " ".join(w["t"] for w in lines[i])
                if "MIDI PARAMETER CHANGE TABLE" in txt:
                    tail = txt.split("MIDI PARAMETER CHANGE TABLE", 1)[1].strip()
                    if not tail and i + 1 < len(lines):
                        tail = " ".join(w["t"] for w in lines[i + 1])
                    pending = tail.strip().strip("()")
                    i += 1
                    continue
                if is_header_line(lines[i]):
                    hdr = list(lines[i])
                    j = i + 1
                    while j < len(lines) and lines[j] and \
                            all(w["t"] in HDR_LABELS for w in lines[j]):
                        hdr += lines[j]; j += 1
                    if pending:
                        cur_title, pending = pending, None
                        carry = [None, None, None]   # new table: reset carry
                        side_starts.clear()
                    body = []
                    while j < len(lines):
                        t = " ".join(w["t"] for w in lines[j])
                        if "MIDI PARAMETER CHANGE TABLE" in t or is_header_line(lines[j]):
                            break
                        body.append(lines[j]); j += 1
                    anchor = [ln for ln in body if ln and ADDR_TOK.match(ln[0]["t"])]
                    starts = build_starts(hdr, anchor or body)
                    if starts:
                        side_starts[side] = starts
                        new, carry = emit(body, starts, cur_title, page, carry)
                        rows.extend(new)
                    i = j
                    continue
                i += 1
    return rows


def split_row(line, starts):
    """Address/Size/Range/Name by token shape; Description/Notes by x-position.

    The left-hand columns are packed tightly enough that a whitespace corridor
    sometimes falls inside a cell (e.g. the range "00, 20 - 7E" straddles the
    Size/Range boundary), so they are parsed lexically. Only the free-text
    columns, which have no distinguishing token shape, are split by geometry.
    """
    bound = dict(starts)
    size_x = bound.get("size", 1e9)
    desc_x = bound.get("description", 1e9)
    note_x = bound.get("notes", 1e9)

    left = [w for w in line if w["x"] < desc_x]
    desc = " ".join(w["t"] for w in line if desc_x <= w["x"] < note_x).strip()
    note = " ".join(w["t"] for w in line if w["x"] >= note_x).strip()

    i, addr = 0, []
    while i < len(left) and len(addr) < 3 and \
            ADDR_TOK.match(left[i]["t"]) and left[i]["x"] < size_x:
        addr.append(left[i]["t"]); i += 1
    size = None
    if i < len(left) and re.fullmatch(r"\d{1,3}", left[i]["t"]):
        size = left[i]["t"]; i += 1
    rng = []
    while i < len(left) and RANGE_TOK.match(left[i]["t"]):
        rng.append(left[i]["t"]); i += 1
    name = " ".join(w["t"] for w in left[i:]).strip()
    return addr, size, " ".join(rng).strip(), name, desc, note


def emit(body, starts, title, page, carry):
    out = []
    for ln in body:
        atoks, size_v, rng, name, desc, note = split_row(ln, starts)
        get = {"range": rng, "name": name, "description": desc, "notes": note}.get

        if atoks and size_v:
            # printed tokens are a suffix of (High, Mid, Low)
            addr = list(carry)
            addr[3 - len(atoks):] = atoks
            if any(v is None for v in addr):
                continue
            carry = list(addr)
            out.append({
                "scope": title or "UNKNOWN",
                "address": addr,
                "size": int(size_v),
                "range": rng,
                "range_extra": [],
                "name": name,
                "description": desc,
                "notes": note,
                "page": page,
            })
        elif out:
            r = out[-1]
            if get("range"):
                r["range_extra"].append(get("range"))
            for k in ("name", "description", "notes"):
                v = get(k)
                if v:
                    r[k] = (r[k] + " " + v).strip()
    return out, carry


if __name__ == "__main__":
    rows = parse()
    print(f"parsed {len(rows)} parameter rows", file=sys.stderr)
    for k, v in Counter(r["scope"] for r in rows).most_common():
        print(f"  {v:5d}  {k}", file=sys.stderr)
    json.dump(rows, sys.stdout, indent=1, ensure_ascii=False)
