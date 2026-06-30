#!/usr/bin/env python3
"""
generate_dict_xml.py
=====================================================================
Generate OSIS-style XML dictionary files for the ESP32 Bible firmware's
**Dictionary** mode (SD card folder /dictionary/).

ESP32 counterpart of fz_dictionary/sd_prep/prepare_dict.py (dict.cc) and
prepare_wiktionary.py (Kaikki). Instead of letter-bucket .txt files it
emits ONE <direction>.xml + <direction>.toc per dictionary — the same
on-SD contract the firmware uses for the Bible.

Hierarchy mapping (Bible engine  ->  Dictionary)
  translation file  =  dictionary direction / language  (<dir>.xml)
  section           =  1  ("<dir> Dictionary")
  book              =  first-letter bucket  (A..Z, 0-9, Other  -> 28 books)
  chapter           =  page of N entries    (--page, default 100)
  verse             =  one entry            text = "word - definition"

Two input formats (subcommands):

  dict.cc tab export (bilingual):
    python generate_dict_xml.py dictcc EN-DE.txt --name DE-EN
    python generate_dict_xml.py dictcc EN-DE.txt --bidirectional \
           --name DE-EN --reverse-name EN-DE

  Kaikki.org Wiktionary JSONL (monolingual):
    python generate_dict_xml.py wiktionary kaikki-German.jsonl --lang de --name DE
    python generate_dict_xml.py wiktionary kaikki-English.jsonl --lang en --name EN

Output (default ./dict_out/):
    <dir>.xml   <verse osisID="LETTER.page.idx">word - definition</verse>
    <dir>.toc   S|<label>  then  B|<code>|<display>|<chapters>|0|<offset>

Copy the .xml + .toc files into  /dictionary/  on the SD card.

Notes
-----
* Streams the (very large) input once, writing per-letter temp files, then
  sorts + emits each bucket — memory stays bounded to one bucket at a time.
* Text is sanitised to what the firmware can render: ASCII + German umlauts
  (ä ö ü Ä Ö Ü ß). Other accented Latin letters are transliterated to their
  base letter; remaining non-renderable characters are dropped. The firmware
  compresses the umlauts to its private byte codes on load.
"""

import argparse
import html
import json
import os
import re
import shutil
import sys
import unicodedata

# Firmware limits (keep in sync with BibleInterface.h)
VERSE_MAX   = 500          # BIBLE_VERSE_BUF (512) headroom
DISPLAY_MAX = 47
DEF_OUT     = "dict_out"
DEF_PAGE    = 100

# Letter buckets:  a..z, then digits -> "NUM", then everything else -> "SYM"
LETTERS = list("abcdefghijklmnopqrstuvwxyz")
BUCKET_ORDER = LETTERS + ["NUM", "SYM"]
BUCKET_CODE = {**{c: c.upper() for c in LETTERS}, "NUM": "NUM", "SYM": "SYM"}
BUCKET_DISP = {**{c: c.upper() for c in LETTERS}, "NUM": "0-9", "SYM": "Other"}

# Map accented Latin letters to a base letter so they bucket and render sensibly.
ACCENT_BASE = {
    'á':'a','à':'a','â':'a','ã':'a','å':'a','ā':'a',
    'ç':'c','ć':'c',
    'é':'e','è':'e','ê':'e','ë':'e','ē':'e',
    'í':'i','ì':'i','î':'i','ï':'i','ī':'i',
    'ñ':'n',
    'ó':'o','ò':'o','ô':'o','õ':'o','ō':'o','ø':'o',
    'ú':'u','ù':'u','û':'u','ū':'u',
    'ý':'y','ÿ':'y',
}
# German umlauts the firmware CAN render — keep as-is.
KEEP_UMLAUT = set("äöüÄÖÜß")

LANG_NAME = {"de": "German", "en": "English"}
POS_LABELS = {
    "de": {"noun":"Subst.","verb":"Verb","adj":"Adj.","adv":"Adv.","prep":"Präp.",
           "conj":"Konj.","pron":"Pron.","article":"Art.","num":"Num.","intj":"Interj.",
           "particle":"Partikel","prefix":"Präfix","suffix":"Suffix","name":"Eigenname",
           "phrase":"Phrase","proverb":"Sprichw.","abbrev":"Abk."},
    "en": {"noun":"n.","verb":"v.","adj":"adj.","adv":"adv.","prep":"prep.","conj":"conj.",
           "pron":"pron.","article":"art.","num":"num.","intj":"interj.","name":"proper n.",
           "phrase":"phrase","proverb":"prov.","abbrev":"abbrev."},
}
GENDER_TAGS = {"masculine":"mask.","feminine":"fem.","neuter":"neutr."}


# ── Text handling ────────────────────────────────────────────────────────────
def sanitize(s: str) -> str:
    """Reduce text to characters the firmware can render."""
    s = html.unescape(s)
    out = []
    for ch in s:
        if ch in KEEP_UMLAUT:
            out.append(ch)
        elif ord(ch) < 128:
            out.append(ch)
        elif ch in ACCENT_BASE:
            out.append(ACCENT_BASE[ch])
        else:
            # Try NFKD decomposition (strip combining marks); else drop.
            dec = unicodedata.normalize("NFKD", ch)
            base = "".join(c for c in dec if not unicodedata.combining(c) and ord(c) < 128)
            out.append(base)        # may be "" -> dropped
    s = "".join(out)
    s = re.sub(r'[ \t]+', ' ', s).strip()
    return s


def bucket_for(word: str) -> str:
    if not word:
        return "SYM"
    ch = word[0].lower()
    if 'a' <= ch <= 'z':
        return ch
    if ch in ('ä',):       return 'a'
    if ch in ('ö',):       return 'o'
    if ch in ('ü',):       return 'u'
    if ch == 'ß':          return 's'
    if ch in ACCENT_BASE:  return ACCENT_BASE[ch]
    if '0' <= ch <= '9':   return "NUM"
    return "SYM"


def xml_escape(s: str) -> str:
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def clip(s: str, n: int) -> str:
    return s if len(s) <= n else s[:n]


# ── Input parsers: yield (word, definition) ──────────────────────────────────
def parse_dictcc(path, reverse=False):
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            src = parts[0].strip()
            dst = parts[1].strip()
            notes = " ".join(p.strip() for p in parts[2:] if p.strip())
            if reverse:
                src, dst = dst, src
            word = sanitize(src)
            definition = sanitize(dst)
            if not word or not definition:
                continue
            if notes:
                definition = f"{definition} [{sanitize(notes)}]"
            yield word, definition


def parse_wiktionary(path, lang, max_senses=5):
    lang_name = LANG_NAME[lang]
    pos_labels = POS_LABELS[lang]
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("lang") != lang_name:
                continue
            word = (obj.get("word") or "").strip()
            senses = obj.get("senses") or []
            if not word or not senses:
                continue
            pos = (obj.get("pos") or "").strip()
            label = pos_labels.get(pos, pos)

            gender = ""
            for sense in senses:
                for tag in sense.get("tags", []):
                    if tag in GENDER_TAGS:
                        gender = GENDER_TAGS[tag]; break
                if gender:
                    break

            texts = []
            for sense in senses:
                glosses = sense.get("glosses") or []
                if not glosses:
                    continue
                g = re.sub(r'^\([^)]{1,40}\)\s*', '', glosses[-1]).strip()
                if not g or (g.startswith("(") and g.endswith(")")):
                    continue
                texts.append(g)
                if len(texts) >= max_senses:
                    break
            if not texts:
                continue

            header = f"[{label}]" + (f" ({gender})" if gender else "")
            if len(texts) == 1:
                definition = f"{header} {texts[0]}"
            else:
                definition = header + " " + "  ".join(f"{i+1}. {t}" for i, t in enumerate(texts))

            word = sanitize(word)
            definition = sanitize(definition)
            if word and definition:
                yield word, definition


# ── Bucketed XML writer ──────────────────────────────────────────────────────
def build_direction(entries, stem, label, out_dir, page, disp_name=None):
    """
    entries: iterable of (word, definition).
    Streams into per-bucket temp files, then sorts each bucket and emits
    <stem>.xml + <stem>.toc.  Returns total entry count.
    disp_name: name shown in the firmware menus (defaults to label minus the
    trailing " Dictionary").
    """
    if disp_name is None:
        disp_name = label[:-len(" Dictionary")] if label.endswith(" Dictionary") else label
    tmp_dir = os.path.join(out_dir, "." + stem + ".tmp")
    os.makedirs(tmp_dir, exist_ok=True)

    handles = {}
    counts = {b: 0 for b in BUCKET_ORDER}

    def th(b):
        if b not in handles:
            handles[b] = open(os.path.join(tmp_dir, b + ".tsv"),
                              "w", encoding="utf-8", newline="\n")
        return handles[b]

    total = 0
    for word, definition in entries:
        b = bucket_for(word)
        # store word and definition tab-separated; strip stray tabs/newlines
        word = word.replace("\t", " ")
        definition = definition.replace("\t", " ").replace("\n", " ")
        th(b).write(f"{word}\t{definition}\n")
        counts[b] += 1
        total += 1
        if total % 100_000 == 0:
            print(".", end="", flush=True)
    for h in handles.values():
        h.close()
    print()

    xml_path = os.path.join(out_dir, stem + ".xml")
    book_entries = []   # (code, display, chapters, offset)
    pgx_lines  = []     # "code|page|firstword|lastword" — page index for the firmware

    def pgw(x):         # page-label headword: drop dict.cc markup, no delimiters, short
        x = x.replace("|", " ").replace("\t", " ").strip()
        x = re.split(r"\s*[\{\[\<]", x, 1)[0].strip()   # cut gender/category/notes
        return x[:18]

    with open(xml_path, "wb") as xf:
        def w(s):
            xf.write(s.encode("utf-8"))
        w('<?xml version="1.0" encoding="UTF-8"?>\n<osis>\n')

        for b in BUCKET_ORDER:
            tsv = os.path.join(tmp_dir, b + ".tsv")
            if not os.path.isfile(tsv) or counts[b] == 0:
                continue
            rows = []
            with open(tsv, encoding="utf-8") as tf:
                for ln in tf:
                    ln = ln.rstrip("\n")
                    if "\t" in ln:
                        wd, df = ln.split("\t", 1)
                        rows.append((wd, df))
            rows.sort(key=lambda r: (r[0].lower(), r[0]))

            chapters = (len(rows) + page - 1) // page
            code = BUCKET_CODE[b]
            offset = xf.tell()       # first verse of this letter-book
            book_entries.append((code, BUCKET_DISP[b], chapters, offset))

            pg_first = ""
            for i, (wd, df) in enumerate(rows):
                chap = i // page + 1
                vno = i % page + 1
                if vno == 1:
                    pg_first = wd
                text = clip(f"{wd} - {df}", VERSE_MAX)
                w(f'<verse osisID="{code}.{chap}.{vno}">{xml_escape(text)}</verse>\n')
                # End of page (or last entry) → record the page's first/last word.
                if vno == page or i == len(rows) - 1:
                    pgx_lines.append(f"{code}|{chap}|{pgw(pg_first)}|{pgw(wd)}")

        w('</osis>\n')

    toc_path = os.path.join(out_dir, stem + ".toc")
    toc_disp = disp_name.replace("|", " ").replace("\t", " ").strip()
    with open(toc_path, "w", encoding="utf-8", newline="\n") as tf:
        # T| display name first so the firmware shows the proper dictionary name
        # in its menus; older firmware ignores the line.
        tf.write(f"T|{clip(toc_disp, DISPLAY_MAX)}\n")
        tf.write(f"S|{clip(label, DISPLAY_MAX)}\n")
        for code, disp, chapters, offset in book_entries:
            tf.write(f"B|{code}|{disp}|{chapters}|0|{offset}\n")

    # Page index: lets the firmware list pages as "firstword - lastword".
    pgx_path = os.path.join(out_dir, stem + ".pgx")
    with open(pgx_path, "w", encoding="utf-8", newline="\n") as pf:
        for line in pgx_lines:
            pf.write(line + "\n")

    shutil.rmtree(tmp_dir, ignore_errors=True)
    print(f"  {stem:<12} {total:>9,} entries  {len(book_entries)} letter-books")
    return total


def safe_stem(name: str) -> str:
    s = re.sub(r'[^A-Za-z0-9\-]+', '-', name).strip('-')
    return (s[:32] or "dict").lower()


# ── Commands ─────────────────────────────────────────────────────────────────
def cmd_dictcc(args):
    if not os.path.isfile(args.input):
        sys.exit(f"ERROR: file not found: {args.input}")
    os.makedirs(args.out, exist_ok=True)

    fwd_name = args.name or "dictcc"
    fwd_stem = safe_stem(fwd_name)
    print(f"dict.cc -> {fwd_stem}.xml (forward)")
    build_direction(parse_dictcc(args.input, reverse=False),
                    fwd_stem, fwd_name + " Dictionary", args.out, args.page)

    if args.bidirectional:
        rev_name = args.reverse_name or _flip(fwd_name)
        rev_stem = safe_stem(rev_name)
        print(f"dict.cc -> {rev_stem}.xml (reverse)")
        build_direction(parse_dictcc(args.input, reverse=True),
                        rev_stem, rev_name + " Dictionary", args.out, args.page)

    _finish(args.out)


def cmd_wiktionary(args):
    if not os.path.isfile(args.input):
        sys.exit(f"ERROR: file not found: {args.input}")
    os.makedirs(args.out, exist_ok=True)

    name = args.name or args.lang.upper()
    stem = safe_stem(name)
    print(f"Wiktionary ({LANG_NAME[args.lang]}) -> {stem}.xml")
    build_direction(parse_wiktionary(args.input, args.lang, args.max_senses),
                    stem, name + " Dictionary", args.out, args.page)
    _finish(args.out)


def _flip(name):
    for sep in ('-', '_'):
        parts = name.split(sep)
        if len(parts) == 2:
            return sep.join(reversed(parts))
    return name + "-REV"


def _finish(out_dir):
    print(f"\nDone.  Output: ./{out_dir}/")
    print(f"Copy the .xml + .toc + .pgx files into  /dictionary/  on the SD card.")


def build_parser():
    p = argparse.ArgumentParser(description="Build ESP32 Dictionary XML for SD /dictionary/")
    sub = p.add_subparsers(dest="command", required=True)

    dc = sub.add_parser("dictcc", help="Convert a dict.cc tab-separated export.")
    dc.add_argument("input", help="dict.cc .txt export")
    dc.add_argument("--name", "-n", default=None, help="forward direction name, e.g. DE-EN")
    dc.add_argument("--bidirectional", "-b", action="store_true",
                    help="also build the reversed direction")
    dc.add_argument("--reverse-name", "-r", default=None, help="reverse direction name")
    dc.add_argument("--out", "-o", default=DEF_OUT, help="output directory")
    dc.add_argument("--page", type=int, default=DEF_PAGE, help="entries per chapter")
    dc.set_defaults(func=cmd_dictcc)

    wk = sub.add_parser("wiktionary", help="Convert a Kaikki.org Wiktionary JSONL dump.")
    wk.add_argument("input", help="Kaikki JSONL file")
    wk.add_argument("--lang", "-l", required=True, choices=list(LANG_NAME))
    wk.add_argument("--name", "-n", default=None, help="output name (default: DE/EN)")
    wk.add_argument("--max-senses", type=int, default=5, help="max senses per entry")
    wk.add_argument("--out", "-o", default=DEF_OUT, help="output directory")
    wk.add_argument("--page", type=int, default=DEF_PAGE, help="entries per chapter")
    wk.set_defaults(func=cmd_wiktionary)
    return p


def main():
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
