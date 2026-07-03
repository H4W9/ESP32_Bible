#!/usr/bin/env python3
"""
generate_songs_xml.py
=====================================================================
Generate OSIS-style XML song files for the ESP32 Bible firmware's
**Songs** mode (SD card folder /songs/).

This is the ESP32 counterpart of fz_songs/sd_directory_builder/
generate_songs_sd.py (which builds the Flipper folder tree). Instead
of a folder-per-song tree, this emits ONE XML file per songbook plus a
small .toc index — the same on-SD contract the firmware uses for the
Bible (see BibleInterface::cacheChapter / loadToc).

Hierarchy mapping (Bible engine  ->  Songs)
  translation file  =  SongBook        (one <book>.xml per songbook)
  section           =  Category
  book              =  Song            (display = song title)
  chapter           =  1               (each song is a single chapter)
  verse             =  Stanza

Reads three Excel files (same as the Flipper builder):
  SngBooks.xlsx     - Id, Title, ..., Active
  SngCategory.xlsx  - ID, BookId, Category, ...
  SngData.xlsx      - Id, BookId, Category, Title1, Song, ...

Output (default ./songs_out/):
  <songbook>.xml    OSIS verses:  <verse osisID="Code.1.Stanza">text</verse>
  <songbook>.toc    structure index read by the firmware:
      S|<category display name>
      B|<code>|<song title>|<chapterCount=1>|<sectionIndex>|<byteOffset>

Copy the *contents* of ./songs_out/ into  /esp32_library/songs/  on the SD card.

Run:
  pip install openpyxl
  python generate_songs_xml.py
  # custom paths:
  python generate_songs_xml.py --data SngData.xlsx --books SngBooks.xlsx \
                               --cats SngCategory.xlsx --out songs_out
"""

import argparse
import os
import re
import sys
import unicodedata
import zipfile

# Characters the firmware CAN render: ASCII + these German umlauts/ß (compressed to
# private byte codes on load). Everything else must be transliterated or dropped —
# otherwise stray UTF-8 continuation bytes render as random "Ä"-like glyphs.
KEEP_UMLAUT = set("äöüÄÖÜß")
ACCENT_BASE = {
    'á':'a','à':'a','â':'a','ã':'a','å':'a','ā':'a',
    'ç':'c','ć':'c','é':'e','è':'e','ê':'e','ë':'e','ē':'e',
    'í':'i','ì':'i','î':'i','ï':'i','ī':'i','ñ':'n',
    'ó':'o','ò':'o','ô':'o','õ':'o','ō':'o','ø':'o',
    'ú':'u','ù':'u','û':'u','ū':'u','ý':'y','ÿ':'y',
}
# Typographic marks kept as-is in the XML. The firmware renders the German quotes
# („ " ‚ ' '), en/em dashes and ellipsis as real glyphs (private codes 0x87-0x8E);
# the few without a font glyph (− • ´) still fall back to ASCII at draw time.
KEEP_TYPO = set("‘’‚‛“”„‟–—−‐‑…•´")
# Whitespace variants normalised to a plain space (or dropped).
SPACE_MAP = {" ": " ", " ": " ", " ": " ", "​": ""}


def sanitize_text(s: str) -> str:
    """Reduce text to firmware-renderable characters (ASCII + German umlauts)."""
    out = []
    for ch in s:
        if ch in KEEP_UMLAUT:
            out.append(ch)
        elif ch in SPACE_MAP:
            out.append(SPACE_MAP[ch])
        elif ch in KEEP_TYPO:
            out.append(ch)
        elif ord(ch) < 128:
            out.append(ch)
        elif ch in ACCENT_BASE:
            out.append(ACCENT_BASE[ch])
        else:
            dec = unicodedata.normalize("NFKD", ch)
            out.append("".join(c for c in dec
                               if not unicodedata.combining(c) and ord(c) < 128))
    return "".join(out)

try:
    import openpyxl
except ImportError:
    sys.exit("ERROR: openpyxl is required.  Install with:  pip install openpyxl")

# ── Defaults ────────────────────────────────────────────────────────────────
DEF_DATA  = "SngData.xlsx"
DEF_BOOKS = "SngBooks.xlsx"
DEF_CATS  = "SngCategory.xlsx"
DEF_OUT   = "songs_out"

SKIP_BOOK_IDS = {0}          # BookId 0 is the "Major Categories" placeholder

# Firmware buffer limits (keep in sync with BibleInterface.h)
DISPLAY_MAX = 47             # ContentBook.display capacity - 1
SECNAME_MAX = 47             # section name capacity - 1
VERSE_MAX   = 500            # BIBLE_VERSE_BUF (512) headroom


# ── Helpers (ported from generate_songs_sd.py) ───────────────────────────────
def safe_stem(name: str) -> str:
    """FAT32-safe lowercase ASCII file stem for a songbook .xml/.toc."""
    s = str(name).strip()
    s = s.translate(str.maketrans({
        'ä': 'ae', 'ö': 'oe', 'ü': 'ue',
        'Ä': 'Ae', 'Ö': 'Oe', 'Ü': 'Ue', 'ß': 'ss',
    }))
    s = re.sub(r'[\s/\\:*?"<>|&\'+]+', '_', s)
    s = re.sub(r'[^\x20-\x7E]', '', s)
    s = re.sub(r'[^\w\-]', '', s)
    s = re.sub(r'_+', '_', s).strip('_')
    return (s[:32] or "book").lower()


def normalize_title(title: str) -> str:
    """Collapse a trailing run of spaces/asterisks to a single ' *'."""
    cleaned = re.sub(r'[\s*]+$', '', title)
    if title.rstrip() != cleaned:
        return cleaned.rstrip() + ' *'
    return title.strip()


def split_stanzas(song_text: str) -> list:
    """Split a song into stanzas on blank lines."""
    text = song_text.replace('\r\n', '\n').replace('\r', '\n')
    return [s.strip() for s in re.split(r'\n{2,}', text) if s.strip()]


def strip_leading_number(stanza: str) -> str:
    """
    Remove a leading stanza number like '1. ' / '12.' from the FIRST line.
    The firmware draws its own verse number, so a source '1.' would double up.
    Refrains / unnumbered stanzas are left untouched.
    """
    return re.sub(r'^\s*\d{1,3}\.\s*', '', stanza, count=1)


def xml_escape(s: str) -> str:
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def clip(s: str, n: int) -> str:
    return s if len(s) <= n else s[:n]


def toc_safe(s: str) -> str:
    """Strip characters that would break a pipe-delimited single-line .toc field."""
    return s.replace("|", "/").replace("\r", " ").replace("\n", " ").strip()


# ── Excel loading ────────────────────────────────────────────────────────────
def load_books(path: str) -> dict:
    """{book_id: title} for active books (skips placeholders / Active==False)."""
    ws = openpyxl.load_workbook(path).active
    books, first = {}, True
    for row in ws.iter_rows(values_only=True):
        if first:
            first = False
            continue
        if not row or row[0] is None:
            continue
        bid = int(row[0])
        title = str(row[1]).strip() if len(row) > 1 and row[1] else f"Book_{bid}"
        active = bool(row[6]) if len(row) > 6 and row[6] is not None else True
        if bid in SKIP_BOOK_IDS or not active:
            continue
        books[bid] = title
    return books


def load_categories(path: str, books: dict) -> dict:
    """{(book_id, cat_id): category_name} for categories of active books."""
    ws = openpyxl.load_workbook(path).active
    cats, first = {}, True
    for row in ws.iter_rows(values_only=True):
        if first:
            first = False
            continue
        if not row or row[0] is None:
            continue
        cid = int(row[0])
        bid = int(row[1]) if len(row) > 1 and row[1] is not None else None
        name = str(row[2]).strip() if len(row) > 2 and row[2] else f"Cat_{cid}"
        if bid not in books:
            continue
        cats[(bid, cid)] = name
    return cats


def load_songs(path: str):
    """Yield (book_id, cat_id, title, [stanzas]) for every song with text."""
    ws = openpyxl.load_workbook(path, read_only=True).active
    first = True
    for row in ws.iter_rows(values_only=True):
        if first:
            first = False
            continue
        if not row or row[0] is None:
            continue
        bid   = int(row[1]) if len(row) > 1 and row[1] is not None else None
        cid   = int(row[2]) if len(row) > 2 and row[2] is not None else None
        title = str(row[3]).strip() if len(row) > 3 and row[3] else ""
        song  = str(row[4]).strip() if len(row) > 4 and row[4] else ""
        if bid is None or cid is None or not song:
            continue
        stanzas = split_stanzas(song)
        if not stanzas:
            continue
        yield bid, cid, title, stanzas


# ── Main generation ──────────────────────────────────────────────────────────
def generate(data_path, books_path, cats_path, out_dir, make_zip,
             fraktur=False, suffix=""):
    for p in (data_path, books_path, cats_path):
        if not os.path.isfile(p):
            sys.exit(f"ERROR: file not found: {p}")

    print(f"Loading {books_path} ...")
    books = load_books(books_path)
    print(f"  {len(books)} active songbooks")

    print(f"Loading {cats_path} ...")
    cats = load_categories(cats_path, books)
    print(f"  {len(cats)} categories")

    print(f"Loading {data_path} ...")
    # Group songs:  songs_by_book[bid] = list of (cat_id, title, stanzas)
    songs_by_book = {}
    total_songs = 0
    for bid, cid, title, stanzas in load_songs(data_path):
        if (bid, cid) not in cats:
            continue
        songs_by_book.setdefault(bid, []).append((cid, title, stanzas))
        total_songs += 1
    print(f"  {total_songs} songs across {len(songs_by_book)} songbooks")

    os.makedirs(out_dir, exist_ok=True)
    written_files = []
    grand_verses = 0

    out_suffix = suffix if suffix is not None else ("_fraktur" if fraktur else "")
    for bid, songlist in songs_by_book.items():
        book_title = books[bid]
        stem = safe_stem(book_title) + out_suffix

        # Categories sorted alphabetically by display name; songs sorted
        # alphabetically by title within each category. Books stay grouped by
        # section and contiguous (engine requirement) because we emit per category.
        cat_ids = sorted(set(cid for cid, _, _ in songlist),
                         key=lambda c: sanitize_text(str(cats[(bid, c)])).strip().lower())

        xml_path = os.path.join(out_dir, stem + ".xml")
        toc_lines = []          # built in parallel, written after we know offsets
        book_entries = []       # (code, display, sec_idx, byte_offset)

        with open(xml_path, "wb") as xf:
            def w(s):
                xf.write(s.encode("utf-8"))

            w('<?xml version="1.0" encoding="UTF-8"?>\n<osis>\n')

            song_no = 0
            for sec_idx, cid in enumerate(cat_ids):
                cat_name = toc_safe(clip(sanitize_text(cats[(bid, cid)]), SECNAME_MAX))
                toc_lines.append(f"S|{cat_name}")

                songs_in_cat = [(title, stanzas) for c2, title, stanzas in songlist
                                if c2 == cid]
                songs_in_cat.sort(key=lambda t: sanitize_text(
                    normalize_title(t[0]) if t[0] else "").strip().lower())

                for title, stanzas in songs_in_cat:
                    song_no += 1
                    code = f"S{song_no}"
                    disp = normalize_title(title) if title else f"Song {song_no}"
                    disp = toc_safe(clip(sanitize_text(disp), DISPLAY_MAX))

                    offset = xf.tell()      # byte offset of this song's first verse
                    book_entries.append((code, disp, sec_idx, offset))

                    for si, stanza in enumerate(stanzas, start=1):
                        text = strip_leading_number(stanza)
                        text = clip(sanitize_text(text), VERSE_MAX)
                        w(f'<verse osisID="{code}.1.{si}">{xml_escape(text)}</verse>\n')
                        grand_verses += 1

            w('</osis>\n')

        # Write the .toc:  T| display name first, then all S| lines, then all B|
        # lines (with offsets). The firmware reads T| to show the proper songbook
        # name (with umlauts) in its menus; older firmware just ignores the line.
        toc_path = os.path.join(out_dir, stem + ".toc")
        disp_name = toc_safe(clip(sanitize_text(book_title), DISPLAY_MAX))
        with open(toc_path, "w", encoding="utf-8", newline="\n") as tf:
            tf.write(f"T|{disp_name}\n")
            if fraktur:
                tf.write("F|fraktur\n")   # firmware reads song text with the Fraktur font
            for line in toc_lines:
                tf.write(line + "\n")
            for code, disp, sec_idx, offset in book_entries:
                tf.write(f"B|{code}|{disp}|1|{sec_idx}|{offset}\n")

        written_files += [xml_path, toc_path]
        print(f"  {stem:<24} {len(book_entries):>4} songs  "
              f"{len(cat_ids):>3} categories")

    if make_zip:
        zip_path = out_dir.rstrip("/\\") + "_sd.zip"
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for fp in written_files:
                zf.write(fp, os.path.basename(fp))
        print(f"\nZIP archive: {zip_path}")

    print(f"\nDone.  {grand_verses:,} stanzas in {len(songs_by_book)} songbooks.")
    print(f"Output : ./{out_dir}/")
    print(f"Copy the .xml + .toc files into  /esp32_library/songs/  on the SD card.")


def main():
    ap = argparse.ArgumentParser(description="Build ESP32 Songs XML for SD /songs/")
    ap.add_argument("--data",  default=DEF_DATA,  help="SngData.xlsx")
    ap.add_argument("--books", default=DEF_BOOKS, help="SngBooks.xlsx")
    ap.add_argument("--cats",  default=DEF_CATS,  help="SngCategory.xlsx")
    ap.add_argument("--out",   default=DEF_OUT,   help="output directory")
    ap.add_argument("--zip",   action="store_true", help="also build <out>_sd.zip")
    ap.add_argument("--fraktur", action="store_true",
                    help="mark these songbooks to be read with the Fraktur font "
                         "(writes 'F|fraktur' into each .toc); also suffixes output "
                         "filenames with '_fraktur' unless --suffix overrides it")
    ap.add_argument("--suffix", default=None,
                    help="append this to each output filename stem (default: "
                         "'_fraktur' when --fraktur, else none)")
    args = ap.parse_args()
    generate(args.data, args.books, args.cats, args.out, args.zip,
             fraktur=args.fraktur, suffix=args.suffix)


if __name__ == "__main__":
    main()
