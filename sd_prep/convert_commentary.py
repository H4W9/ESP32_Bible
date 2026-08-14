#!/usr/bin/env python3
"""
convert_commentary.py — MySword/e-Sword .cmti commentary → firmware .cmt

The ESP32_Bible firmware has no SQLite; it streams flat files from the SD card.
The "free modules" .cmti files are actually MySword SQLite databases:

    Details(Title, Abbreviation, Information, Version)
    BookCommentary(Book, Comments)                       -- book-scope notes
    ChapterCommentary(Book, Chapter, Comments)           -- chapter-scope notes
    VerseCommentary(Book, ChapterBegin, VerseBegin,
                    ChapterEnd, VerseEnd, Comments)       -- verse-scope notes

`Book` is the MySword canon number (1=Genesis .. 66=Revelation, no Apocrypha).
`Comments` is light HTML (<p> <b> <i> <ref>..</ref>, &-entities).

This tool flattens each commentary into ONE self-contained, seek-friendly binary
`<name>.cmt` that the firmware can random-access by (book, chapter, verse) at any
of the three scopes, without loading the whole file. Text is pre-converted to the
firmware's private-byte encoding (same scheme as the Bible/Songs data) so no
decoding is needed at read time.

    .cmt layout (all little-endian) -------------------------------------------
      magic      "BCMT"                              4
      version    u16 = 1                             2
      flags      u16 = 0                             2
      title_off  u32   abbr_off  u32                 (absolute file offsets)
      title_len  u16   abbr_len  u16
      n_book     u32   book_idx_off  u32
      n_chap     u32   chap_idx_off  u32
      n_verse    u32   verse_idx_off u32
      text_off   u32   reserved u32
    header = 56 bytes

      book  index entry (10B):  book u16, textoff u32, textlen u32
      chap  index entry (12B):  book u16, chap u16, textoff u32, textlen u32
      verse index entry (20B):  book u16, chB u16, vB u16, chE u16, vE u16,
                                pad u16, textoff u32, textlen u32
    Index tables are sorted so the firmware can binary-search them. textoff is
    an ABSOLUTE file offset to the entry's text; textlen is its byte length.
    Book maps to the firmware's BOOKS[] index (Apocrypha gap accounted for).

Usage:
    python convert_commentary.py [--in DIR] [--out DIR] [--only name1,name2]
Defaults: --in  "<repo>/ESP32_Sword/sd_data/sword/free modules"
          --out "commentary_out"   (copy its .cmt files to SD:/esp32_library/commentary)
"""

import argparse
import glob
import html as htmllib
import os
import re
import sqlite3
import struct
import sys

MAGIC = b"BCMT"
VERSION = 1

# ── MySword canon number (1..66) → firmware BOOKS[] index ──────────────────────
# Firmware BOOKS[] inserts a 10-book Apocrypha block between Malachi (idx 38) and
# Matthew (idx 49), so:  Gen..Mal (1..39) → idx-1 ;  Matt..Rev (40..66) → idx+9.
def mysword_to_fw(book):
    if 1 <= book <= 39:
        return book - 1
    if 40 <= book <= 66:
        return book + 9
    return None            # out-of-canon (some modules carry apocrypha) → skip

# ── HTML/entity → firmware private-byte text ──────────────────────────────────
# Matches utf8Encode() in the firmware:
#   0x80=Ä 0x81=ä 0x82=Ö 0x83=ö 0x84=Ü 0x85=ü 0x86=ß
#   0x87=„ 0x88=" 0x89=‚ 0x8A=' 0x8B=' 0x8C=– 0x8D=— 0x8E=…
UNI_TO_PRIV = {
    "Ä": 0x80, "ä": 0x81, "Ö": 0x82, "ö": 0x83,
    "Ü": 0x84, "ü": 0x85, "ß": 0x86,
    "„": 0x87, "“": 0x88, "‚": 0x89, "‘": 0x8A,
    "’": 0x8B, "–": 0x8C, "—": 0x8D, "…": 0x8E,
}
# Common punctuation the fonts don't carry → nearest ASCII.
UNI_TO_ASCII = {
    "”": '"',    # ” right double quote → "
    " ": " ",    # nbsp
    "′": "'", "″": '"',
    "‐": "-", "‑": "-", "‒": "-", "−": "-",
    "·": "*", "•": "*", "●": "*",
    "ﬁ": "fi", "ﬂ": "fl",
    "½": "1/2", "¼": "1/4", "¾": "3/4",
}

TAG_RE = re.compile(r"<[^>]+>")
WS_RE = re.compile(r"[ \t\r\f\v]+")
NL_RE = re.compile(r"\n{3,}")


def html_to_text(s):
    """Convert one Comments blob (HTML-ish) to plain text with \\n paragraph breaks."""
    if not s:
        return ""
    # Block-level structure → newlines BEFORE tags are stripped.
    s = re.sub(r"(?i)</p\s*>", "\n\n", s)
    s = re.sub(r"(?i)<p\b[^>]*>", "", s)
    s = re.sub(r"(?i)<br\s*/?>", "\n", s)
    s = re.sub(r"(?i)</?(div|li|tr)\b[^>]*>", "\n", s)
    # <ref>..</ref> / <a>..</a> keep their inner text; drop every other tag.
    s = TAG_RE.sub("", s)
    # Entities (&quot; &amp; &#8212; …).
    s = htmllib.unescape(s)
    # Normalise whitespace: collapse spaces, cap blank runs at one blank line.
    s = s.replace("\t", " ")
    s = WS_RE.sub(" ", s)
    s = re.sub(r" *\n *", "\n", s)
    s = NL_RE.sub("\n\n", s)
    return s.strip()


def encode_text(s):
    """Plain text → firmware private-byte bytes (0x0A kept as paragraph break)."""
    out = bytearray()
    for ch in s:
        if ch == "\n":
            out.append(0x0A)
            continue
        o = ord(ch)
        if 0x20 <= o < 0x7F:
            out.append(o)
        elif ch in UNI_TO_PRIV:
            out.append(UNI_TO_PRIV[ch])
        elif ch in UNI_TO_ASCII:
            out.extend(UNI_TO_ASCII[ch].encode("ascii"))
        elif o == 0x09:
            out.append(0x20)
        # else: unrepresentable glyph → drop (keeps the stream ASCII-safe)
    return bytes(out)


class TextPool:
    """Deduplicating text blob: identical Comments (common) are stored once."""
    def __init__(self):
        self.buf = bytearray()
        self.map = {}          # bytes → (rel_off, length)

    def add(self, data):
        if not data:
            return (0, 0)
        hit = self.map.get(data)
        if hit:
            return hit
        rel = len(self.buf)
        self.buf.extend(data)
        ent = (rel, len(data))
        self.map[data] = ent
        return ent


def convert_one(src, dst):
    con = sqlite3.connect(src)
    con.text_factory = lambda b: b.decode("utf-8", "replace")
    cur = con.cursor()

    title, abbr = os.path.splitext(os.path.basename(src))[0], ""
    try:
        row = cur.execute(
            "SELECT Title, Abbreviation FROM Details LIMIT 1").fetchone()
        if row:
            title = (row[0] or title).strip()
            abbr = (row[1] or "").strip()
    except sqlite3.OperationalError:
        pass

    pool = TextPool()
    book_idx, chap_idx, verse_idx = [], [], []
    skipped = 0

    def tables():
        names = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        return names

    have = tables()

    if "BookCommentary" in have:
        for bk, com in cur.execute(
                "SELECT Book, Comments FROM BookCommentary"):
            fw = mysword_to_fw(bk)
            if fw is None:
                skipped += 1
                continue
            rel, ln = pool.add(encode_text(html_to_text(com)))
            if ln:
                book_idx.append((fw, rel, ln))

    if "ChapterCommentary" in have:
        for bk, ch, com in cur.execute(
                "SELECT Book, Chapter, Comments FROM ChapterCommentary"):
            fw = mysword_to_fw(bk)
            if fw is None:
                skipped += 1
                continue
            rel, ln = pool.add(encode_text(html_to_text(com)))
            if ln:
                chap_idx.append((fw, ch, rel, ln))

    if "VerseCommentary" in have:
        for bk, chB, vB, chE, vE, com in cur.execute(
                "SELECT Book, ChapterBegin, VerseBegin, ChapterEnd, VerseEnd, "
                "Comments FROM VerseCommentary"):
            fw = mysword_to_fw(bk)
            if fw is None:
                skipped += 1
                continue
            rel, ln = pool.add(encode_text(html_to_text(com)))
            if ln:
                verse_idx.append((fw, chB or 0, vB or 0, chE or (chB or 0),
                                  vE or (vB or 0), rel, ln))
    con.close()

    # Sort for firmware binary search.
    book_idx.sort(key=lambda e: e[0])
    chap_idx.sort(key=lambda e: (e[0], e[1]))
    verse_idx.sort(key=lambda e: (e[0], e[1], e[2]))

    title_b = encode_text(title)[:255]
    abbr_b = encode_text(abbr)[:63]

    HEADER = 56
    book_sz, chap_sz, verse_sz = 10, 12, 20
    title_off = HEADER
    abbr_off = title_off + len(title_b)
    book_idx_off = abbr_off + len(abbr_b)
    chap_idx_off = book_idx_off + len(book_idx) * book_sz
    verse_idx_off = chap_idx_off + len(chap_idx) * chap_sz
    text_off = verse_idx_off + len(verse_idx) * verse_sz

    header = bytearray(HEADER)
    struct.pack_into("<4sHH", header, 0, MAGIC, VERSION, 0)
    struct.pack_into("<IHIH", header, 8, title_off, len(title_b),
                     abbr_off, len(abbr_b))
    struct.pack_into("<II", header, 20, len(book_idx), book_idx_off)
    struct.pack_into("<II", header, 28, len(chap_idx), chap_idx_off)
    struct.pack_into("<II", header, 36, len(verse_idx), verse_idx_off)
    struct.pack_into("<II", header, 44, text_off, 0)

    with open(dst, "wb") as f:
        f.write(header)
        f.write(title_b)
        f.write(abbr_b)
        for bk, rel, ln in book_idx:
            f.write(struct.pack("<HII", bk, text_off + rel, ln))
        for bk, ch, rel, ln in chap_idx:
            f.write(struct.pack("<HHII", bk, ch, text_off + rel, ln))
        for bk, chB, vB, chE, vE, rel, ln in verse_idx:
            f.write(struct.pack("<HHHHHHII", bk, chB, vB, chE, vE, 0,
                                text_off + rel, ln))
        f.write(pool.buf)

    return {
        "title": title, "abbr": abbr,
        "book": len(book_idx), "chap": len(chap_idx), "verse": len(verse_idx),
        "text_bytes": len(pool.buf), "skipped": skipped,
        "size": os.path.getsize(dst),
    }


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    default_in = os.path.join(
        repo, "ESP32_Sword", "sd_data", "sword", "free modules")
    ap = argparse.ArgumentParser(description="Convert .cmti commentaries to .cmt")
    ap.add_argument("--in", dest="indir", default=default_in)
    ap.add_argument("--out", dest="outdir", default="commentary_out")
    ap.add_argument("--only", default="",
                    help="comma-separated basenames (no .cmti) to convert")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8")     # Windows console is cp1252
    except Exception:
        pass

    os.makedirs(args.outdir, exist_ok=True)
    files = sorted(glob.glob(os.path.join(args.indir, "*.cmti")))
    only = {s.strip() for s in args.only.split(",") if s.strip()}
    if only:
        files = [f for f in files
                 if os.path.splitext(os.path.basename(f))[0] in only]
    if not files:
        print("No .cmti files matched.", file=sys.stderr)
        return 1

    print(f"Converting {len(files)} commentary file(s) -> {args.outdir}\n")
    for src in files:
        name = os.path.splitext(os.path.basename(src))[0]
        dst = os.path.join(args.outdir, name + ".cmt")
        try:
            s = convert_one(src, dst)
        except Exception as e:                      # noqa: BLE001
            print(f"  {name:14} FAILED: {e}", file=sys.stderr)
            continue
        print(f"  {name:14} {s['title'][:34]:34}  "
              f"bk={s['book']:>3} ch={s['chap']:>4} vs={s['verse']:>6}  "
              f"{s['size']/1e6:6.2f} MB"
              + (f"  (skipped {s['skipped']} apoc.)" if s['skipped'] else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
