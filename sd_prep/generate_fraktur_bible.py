#!/usr/bin/env python3
"""
generate_fraktur_bible.py
=====================================================================
Convert a **German** OSIS Bible XML into a Fraktur (blackletter) edition
for the ESP32 Bible firmware's Fraktur reading font — the same on-glyph
encoding the Songs mode already uses (see generate_songs_xml.py).

It is GERMAN-ONLY on purpose: Fraktur long-s / round-s orthography is a
property of German spelling, so the script refuses any input it can't
confirm is German (OSIS xml:lang / <language>, else a text heuristic).

WHAT IT DOES
------------
German Fraktur needs the right glyph in the right place:
  * long-s  ſ   — the DEFAULT s inside a syllable            -> plain 's'
  * round-s (Schluss-s) — end of a word / word-part          -> '#'
  * ch ligature                                              -> '¡'
  * ck ligature                                              -> '¿'
  * tz ligature                                              -> '|'
  * ß kept as-is;  sch = long-s + ch = "s¡";  st/sp keep long-s
These are exactly the markers the Fraktur VLW font renders (built by
make_vlw.py --extended from tfrakreg.TTF), so the output drops straight
onto the SD card next to the songbooks.

The hard part is *where* the round-s goes (it depends on morphology, e.g.
"Häus-chen" -> "Häu#¡en" but "Häuser" -> "Häuser"). So, as requested, the
already-correct **Fraktur songbooks are used as a reference dictionary**:

  1. whole-word lookup   — the word as spelled in the songbooks wins.
  2. compound split      — decompose into dictionary words (+ Fugen-s)
                           so e.g. Gottes+dienst reuses both parts.
  3. rule fallback       — default long-s, round-s word-finally / before a
                           small set of suffixes; mechanical ch/ck/tz.

USAGE
-----
  pip install openpyxl          # only if you use --dict-xlsx
  python generate_fraktur_bible.py luther1912.xml
  python generate_fraktur_bible.py bible/*.xml --out bible_fraktur_out
  python generate_fraktur_bible.py bible/luth1912ap.xml --dict ./songs_fraktur/lutherisches_gesangbuch_fraktur.xml --out bible_fraktur_out
  python generate_fraktur_bible.py kjv.xml      # -> refused (not German)

  --dict DIR/GLOB   where to read the Fraktur songbooks from for the
                    reference dictionary (default: ./songs_fraktur and
                    ./songs/*_fraktur.xml). Point at the .xml the songs
                    generator produced.
  --dict-xlsx FILE  additionally seed the dictionary from a
                    SngData_Fraktur.xlsx (DFX columns).
  --no-compound     disable the compound-splitting pass.
  --list-unknown F  write the words that fell through to the rule fallback
                    to F (handy for spot-checking coverage).

Output: <out>/<stem>_fraktur.xml (only verse/text nodes are rewritten; all
tags, osisIDs and attributes are left byte-for-byte intact so the firmware's
streaming parser still matches them).

NOTE — firmware support: the reader currently forces the normal font for
Bible mode. To actually *display* this file in Fraktur, the firmware must
honour the "_fraktur" translation stem in Bible mode too (today the Fraktur
path is gated to MODE_SONGS). The data this script emits is otherwise ready.
"""

import argparse
import glob
import os
import re
import sys
import unicodedata
from collections import Counter, defaultdict

# ── Firmware-renderable charset (mirrors generate_songs_xml.py) ───────────────
KEEP_UMLAUT = set("äöüÄÖÜß")
ACCENT_BASE = {
    'á':'a','à':'a','â':'a','ã':'a','å':'a','ā':'a',
    'ç':'c','ć':'c','é':'e','è':'e','ê':'e','ë':'e','ē':'e',
    'í':'i','ì':'i','î':'i','ï':'i','ī':'i','ñ':'n',
    'ó':'o','ò':'o','ô':'o','õ':'o','ō':'o','ø':'o',
    'ú':'u','ù':'u','û':'u','ū':'u','ý':'y','ÿ':'y',
}
# Typographic marks the Fraktur font can draw (private codes at run time). NOTE the
# en-dash U+2013 and right-double-quote U+201D are deliberately absent: the DFX font
# maps them to the round-s and tz glyphs, so real ones must be remapped first.
KEEP_TYPO = set("‘’‚‛“„—…•´")
# Applied before the keep-filter so real punctuation doesn't collide with the
# Fraktur ligature glyphs (– → round-s, ” → tz in the blackletter font).
PRE_REMAP = {
    "–": "—", "−": "—", "‐": "-", "‑": "-", "­": "",   # dashes / soft hyphen
    "”": "“", "‟": "“",                                      # closing dq → real quote glyph
    "«": "„", "»": "“", "‹": "‚", "›": "‘",                  # guillemets → German quotes
    " ": " ", " ": " ", " ": " ", " ": " ", " ": " ",
    "​": "",
}

# ── Fraktur markers ──────────────────────────────────────────────────────────
ROUND_S = '#'                       # Schluss-s (round s)
LIGATURES = (('ck', '¿'), ('tz', '|'), ('ch', '¡'))   # applied in this order

GERMAN_LETTERS = "A-Za-zÄÖÜäöüß"
WORD_RE      = re.compile(f"[{GERMAN_LETTERS}]+")
FRAK_WORD_RE = re.compile(f"[{GERMAN_LETTERS}#¡¿|–”]+")   # a word incl. Fraktur markers
TAG_RE       = re.compile(r"(<[^>]*>)", re.S)
ENT_RE       = re.compile(r"(&#?[0-9A-Za-z]+;)")

# Suffixes before which a stem-final s is round (rule fallback only). Deliberately
# excludes -chen / -lein: they're indistinguishable by rule from an "sch"+en word
# (Häus-chen vs frisch-en), so those diminutives are left to the dictionary instead.
ROUND_BEFORE_SUFFIX = ("bar", "los", "sam", "haft", "tum", "heit", "keit")
SUFFIX_SET = frozenset(ROUND_BEFORE_SUFFIX)

# Distinctly-German function words for the language heuristic.
GERMAN_FUNC = {
    "und","der","die","das","den","dem","des","ein","eine","einen","einem","eines",
    "ich","du","er","sie","es","wir","ihr","nicht","ist","sind","war","sein","seine",
    "hat","haben","wird","werden","auf","aus","bei","mit","nach","von","vor","zu",
    "zum","zur","im","in","an","auch","aber","denn","dass","daß","wenn","wie","was",
    "wer","so","da","noch","nur","sich","um","über","gott","herr","dir","mir","mein",
}


# ── Text sanitising (identical spirit to the songs generator) ────────────────
def sanitize_text(s: str) -> str:
    """Reduce text to firmware-renderable characters (ASCII + umlauts + kept marks),
    after remapping punctuation that would collide with Fraktur ligature glyphs."""
    out = []
    for ch in s:
        ch = PRE_REMAP.get(ch, ch)
        if not ch:
            continue
        if ch in KEEP_UMLAUT or ch in KEEP_TYPO:
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


# ── Reference dictionary from the Fraktur songbooks ──────────────────────────
def plain_key(tok: str) -> str:
    """Fraktur word -> plain lowercase German key (markers undone)."""
    s = (tok.replace('¡', 'ch').replace('¿', 'ck')
            .replace('|', 'tz').replace('”', 'tz')
            .replace('#', 's').replace('–', 's'))
    return s.lower()


def norm_value(tok: str) -> str:
    """Fraktur word -> canonical lowercase Fraktur form. Round-s / tz markers are
    unified, and ch/ck/tz are forced to their ligature so a source that spelled a
    word out (e.g. 'frischen') merges with its ligatured form ('fris¡en')."""
    s = tok.replace('–', ROUND_S).replace('”', '|').lower()
    for plain, lig in LIGATURES:
        s = s.replace(plain, lig)
    return s


def add_words(text: str, counts: dict) -> None:
    for tok in FRAK_WORD_RE.findall(text):
        key = plain_key(tok)
        if key:
            counts[key][norm_value(tok)] += 1


def build_dictionary(dict_args, xlsx_path):
    """Collect {plain_lower_word: fraktur_form} from the Fraktur songbooks."""
    counts = defaultdict(Counter)

    # Resolve --dict into a list of Fraktur .xml files.
    files = []
    if dict_args:
        for a in dict_args:
            if os.path.isdir(a):
                files += glob.glob(os.path.join(a, "*_fraktur.xml")) or \
                         glob.glob(os.path.join(a, "*.xml"))
            else:
                files += glob.glob(a)
    else:
        for d in ("songs_fraktur", "songs"):
            files += glob.glob(os.path.join(d, "*_fraktur.xml"))
    files = sorted(set(files))

    for fp in files:
        try:
            raw = open(fp, encoding="utf-8").read()
        except OSError as e:
            print(f"  ! could not read {fp}: {e}", file=sys.stderr)
            continue
        add_words(re.sub(r"<[^>]+>", " ", raw), counts)

    if xlsx_path:
        _seed_from_xlsx(xlsx_path, counts)

    # Majority Fraktur form per word.
    d = {k: c.most_common(1)[0][0] for k, c in counts.items()}
    print(f"Reference dictionary: {len(d):,} words "
          f"from {len(files)} Fraktur file(s)"
          + (" + xlsx" if xlsx_path else ""))
    if not d:
        print("  ! WARNING: no Fraktur reference words found — falling back to "
              "rules only. Point --dict at the songs_fraktur/*.xml output.",
              file=sys.stderr)
    return d


def _seed_from_xlsx(path, counts):
    try:
        import openpyxl
    except ImportError:
        sys.exit("ERROR: --dict-xlsx needs openpyxl  (pip install openpyxl)")
    ws = openpyxl.load_workbook(path, read_only=True).active
    hdr = None
    ti = si = None
    for row in ws.iter_rows(values_only=True):
        if hdr is None:
            hdr = list(row)
            ti = hdr.index("TFrakRegDFXTitle") if "TFrakRegDFXTitle" in hdr else None
            si = hdr.index("TFrakRegDFX") if "TFrakRegDFX" in hdr else None
            if si is None:
                sys.exit(f"ERROR: column 'TFrakRegDFX' not found in {path}")
            continue
        for idx in (ti, si):
            if idx is not None and idx < len(row) and row[idx]:
                add_words(str(row[idx]), counts)


# ── Rule-based fallback (for words the dictionary doesn't cover) ──────────────
def _is_round_s(w: str, i: int) -> bool:
    """Round-s for lowercase w[i]=='s'? Default is long-s; round only at clear ends."""
    n = len(w)
    if i == n - 1:                       # word-final s
        return True
    nxt = w[i + 1]
    if nxt == 's':                       # first s of 'ss' -> long (ſſ)
        return False
    if w[i - 1] == 's' if i > 0 else False:
        return i == n - 1                # second s of 'ss': round only if final
    # stem-final s directly before a known suffix -> round (e.g. le#bar, wei#heit)
    for suf in ROUND_BEFORE_SUFFIX:
        if nxt == suf[0] and w.endswith(suf) and (n - len(suf)) == i + 1:
            return True
    return False


def rule_word(w: str) -> str:
    """Lowercase German word -> Fraktur (long/round s, then ch/ck/tz ligatures)."""
    res = []
    for i, c in enumerate(w):
        res.append(ROUND_S if (c == 's' and _is_round_s(w, i)) else c)
    out = "".join(res)
    for plain, lig in LIGATURES:         # markers don't contain s, so order is safe
        out = out.replace(plain, lig)
    return out


# ── Compound splitting against the dictionary ────────────────────────────────
def compound_word(w: str, d: dict, depth: int = 0):
    """Decompose w into dictionary words (optionally joined by a Fugen-s). Returns the
    concatenated Fraktur form, or None if it can't be split cleanly."""
    if depth > 5:
        return None
    if w in d:
        return d[w]
    n = len(w)
    for cut in range(n - 3, 2, -1):      # longest prefix first, both parts >= 3
        pre = w[:cut]
        if pre not in d:
            continue
        rest = w[cut:]
        if rest in SUFFIX_SET:           # stem + derivational suffix: let the rule
            continue                     # place the round-s (le#bar, not les+bar)
        sub = compound_word(rest, d, depth + 1)
        if sub is not None:
            return d[pre] + sub
        if rest[0] == 's' and len(rest) > 3 and rest[1:] not in SUFFIX_SET:
            sub = compound_word(rest[1:], d, depth + 1)   # Fugen-s -> round on prefix
            if sub is not None:
                return d[pre] + ROUND_S + sub
    return None


# ── Word conversion (dictionary -> compound -> rule), case-preserving ─────────
class Stats:
    def __init__(self):
        self.total = self.dict = self.compound = self.rule = self.plain = 0
        self.unknown = Counter()


def frakturize_lower(w: str, d: dict, use_compound: bool, st: Stats) -> str:
    hit = d.get(w)
    if hit is not None:
        st.dict += 1
        return hit
    if use_compound:
        c = compound_word(w, d)
        if c is not None:
            st.compound += 1
            return c
    st.rule += 1
    if 's' in w or any(p in w for p, _ in LIGATURES):
        st.unknown[w] += 1
    return rule_word(w)


def convert_word(word: str, d: dict, use_compound: bool, st: Stats) -> str:
    st.total += 1
    lower = word.islower()
    title = (not lower) and word[:1].isupper() and word[1:].islower()
    if not (lower or title):
        st.plain += 1                    # ALL-CAPS / mixed: Fraktur caps have no s-form
        return word
    fk = frakturize_lower(word.lower(), d, use_compound, st)
    if title:
        fk = fk[:1].upper() + fk[1:]     # capital S/ligature has no long/round variant
    return fk


# ── XML rewriting (verse text only; tags/attrs/entities untouched) ───────────
def transform_text(s: str, d: dict, use_compound: bool, st: Stats) -> str:
    s = sanitize_text(s)
    out, i = [], 0
    for m in WORD_RE.finditer(s):
        out.append(s[i:m.start()])
        out.append(convert_word(m.group(), d, use_compound, st))
        i = m.end()
    out.append(s[i:])
    return "".join(out)


def transform_xml(raw: str, d: dict, use_compound: bool, st: Stats) -> str:
    parts = TAG_RE.split(raw)            # [text, <tag>, text, <tag>, ...]
    for idx in range(0, len(parts), 2):  # even indices are character data
        seg = parts[idx]
        if not seg:
            continue
        pieces = ENT_RE.split(seg)       # protect &amp; &#8217; etc.
        for j in range(0, len(pieces), 2):
            if pieces[j]:
                pieces[j] = transform_text(pieces[j], d, use_compound, st)
        parts[idx] = "".join(pieces)
    return "".join(parts)


# ── German-only guard ────────────────────────────────────────────────────────
_LANG_ATTR = re.compile(r'xml:lang\s*=\s*"([^"]+)"')
_LANG_TAG  = re.compile(r'<language[^>]*>\s*([^<\s]+)', re.I)


def is_german(raw: str):
    """Return (ok, reason). Prefer explicit OSIS language; else a text heuristic."""
    codes = _LANG_ATTR.findall(raw) + _LANG_TAG.findall(raw)
    for c in codes:
        cl = c.strip().lower()
        if cl.startswith(("de", "ger", "deu")):
            return True, f'xml:lang="{c}"'
        if cl and not cl.startswith("x-"):
            return False, f'declared language "{c}" is not German'

    text = re.sub(r"<[^>]+>", " ", raw)
    words = [w.lower() for w in WORD_RE.findall(text)]
    sample = words[:4000]
    if len(sample) < 40:
        return False, "too little text to confirm German"
    func = sum(1 for w in sample if w in GERMAN_FUNC) / len(sample)
    umlaut = any(ch in KEEP_UMLAUT for ch in text[:20000])
    if func >= 0.05 or (func >= 0.03 and umlaut):
        return True, f"heuristic (function-word share {func:.0%})"
    return False, f"heuristic says not German (function-word share {func:.0%})"


# ── Driver ───────────────────────────────────────────────────────────────────
def process_file(path, d, out_dir, use_compound, unknown_sink):
    raw = open(path, encoding="utf-8").read()

    ok, why = is_german(raw)
    if not ok:
        print(f"SKIP  {os.path.basename(path)} — not a German translation ({why})")
        return None

    st = Stats()
    result = transform_xml(raw, d, use_compound, st)

    stem = os.path.splitext(os.path.basename(path))[0]
    if not stem.endswith("_fraktur"):
        stem += "_fraktur"
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, stem + ".xml")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(result)

    covered = st.dict + st.compound
    conv = st.total - st.plain
    pct = (100.0 * covered / conv) if conv else 0.0
    print(f"OK    {os.path.basename(path)} -> {os.path.basename(out_path)}  "
          f"({why})")
    print(f"        words {st.total:,}  dict {st.dict:,}  compound {st.compound:,}  "
          f"rule {st.rule:,}  ({pct:.1f}% from songbooks)")
    if unknown_sink is not None:
        for w, n in st.unknown.most_common():
            unknown_sink[w] += n
    return out_path


def main():
    ap = argparse.ArgumentParser(
        description="Fraktur-ise a German OSIS Bible using the Fraktur songbooks "
                    "as a spelling reference (German input only).")
    ap.add_argument("inputs", nargs="+", help="German OSIS Bible .xml file(s)")
    ap.add_argument("--dict", nargs="*", default=None,
                    help="Fraktur songbook .xml dir(s)/glob(s) for the reference "
                         "dictionary (default: ./songs_fraktur and ./songs/*_fraktur.xml)")
    ap.add_argument("--dict-xlsx", default=None,
                    help="also seed the dictionary from a SngData_Fraktur.xlsx")
    ap.add_argument("--out", default="bible_fraktur_out", help="output directory")
    ap.add_argument("--no-compound", action="store_true",
                    help="disable dictionary-based compound splitting")
    ap.add_argument("--list-unknown", default=None,
                    help="write rule-fallback words (not covered by the songbooks) here")
    args = ap.parse_args()

    # Expand input globs / directories.
    inputs = []
    for a in args.inputs:
        if os.path.isdir(a):
            inputs += sorted(glob.glob(os.path.join(a, "*.xml")))
        else:
            inputs += sorted(glob.glob(a)) or [a]
    inputs = [p for p in inputs if not os.path.basename(p).endswith("_fraktur.xml")]
    if not inputs:
        sys.exit("ERROR: no input .xml files found")

    d = build_dictionary(args.dict, args.dict_xlsx)

    unknown_sink = Counter() if args.list_unknown else None
    written = 0
    for path in inputs:
        if not os.path.isfile(path):
            print(f"SKIP  {path} — not found", file=sys.stderr)
            continue
        if process_file(path, d, args.out, not args.no_compound, unknown_sink):
            written += 1

    if args.list_unknown and unknown_sink is not None:
        with open(args.list_unknown, "w", encoding="utf-8", newline="\n") as f:
            for w, n in unknown_sink.most_common():
                f.write(f"{n}\t{w}\n")
        print(f"\nRule-fallback words: {len(unknown_sink):,} unique "
              f"-> {args.list_unknown}")

    print(f"\nDone. {written} Fraktur Bible file(s) in ./{args.out}/")
    print("Copy the *_fraktur.xml into /bible/ on the SD card.")
    print("NOTE: the firmware must honour the '_fraktur' stem in Bible mode to "
          "render it in blackletter (today that path is gated to Songs).")


if __name__ == "__main__":
    main()
