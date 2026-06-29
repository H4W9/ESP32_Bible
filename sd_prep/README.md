# SD Card Data Prep — Songs & Dictionary

These two scripts build the **OSIS-style XML files** the ESP32 firmware reads in its
**Songs** and **Dictionary** modes. (Bible mode reads raw OSIS XML directly, no script.)

Each script produces, per "translation":

- `<name>.xml` — verses: `<verse osisID="Code.Chapter.Verse">text</verse>`
- `<name>.toc` — a small structure index the firmware loads: sections, books, chapter counts,
  and the **byte offset** of each book so chapter loads are instant (no on-device scan).
- `<name>.pgx` — **Dictionary only**: a page index (`firstword - lastword` per 100-word page)
  so a letter opens as a readable page list instead of a number grid.

You copy the resulting files onto the SD card (Dictionary needs `.xml` + `.toc` + `.pgx`;
Songs needs `.xml` + `.toc`):

| Mode       | SD folder      | Source script             |
|------------|----------------|---------------------------|
| Bible      | `/bible/`      | (raw OSIS XML, no script) |
| Songs      | `/songs/`      | `generate_songs_xml.py`   |
| Dictionary | `/dictionary/` | `generate_dict_xml.py`    |

> **Text the firmware can render:** ASCII, German umlauts (ä ö ü Ä Ö Ü ß — compressed to private
> byte codes on load), and common typographic marks (curly quotes, en/em dashes, ellipsis — the
> firmware maps these to their ASCII equivalents `' " - ...` at draw time). Accented Latin is
> transliterated to a base letter; anything else is dropped. Songs are also written in
> **alphabetical order** (categories, then song titles).

---

## Requirements

- **Python 3.7+** (verified with 3.14). Check: `python --version`
- `generate_songs_xml.py` needs **openpyxl**: `python -m pip install openpyxl`
- `generate_dict_xml.py` has **no dependencies** (standard library only).

---

## Quick start (Windows `cmd`)

All commands below are **single-line** — paste them straight into `cmd`. They assume your
source files sit in this `sd_prep` folder (Excel workbooks, the dict.cc `.txt`, the Kaikki
`.jsonl`). Start by opening a prompt here:

```cmd
cd /d "C:\Users\Wade\OneDrive\Documents\GitHub\H4W9\ESP32_Bible\sd_prep"
python -m pip install openpyxl
```

```cmd
:: 1) Songs  (reads SngData/SngBooks/SngCategory.xlsx in this folder)  -> songs\
python generate_songs_xml.py --out songs
```

```cmd
:: 2) Dictionary from dict.cc export, both directions  -> dictionary\
python generate_dict_xml.py dictcc "cngfdsffcc-206176115145-799o68.txt" --name DE-EN --bidirectional --reverse-name EN-DE --out dictionary
```

```cmd
:: 3) Dictionary from Wiktionary (Kaikki) dumps  -> dictionary\   (big/slow: ~1 GB DE, ~3 GB EN)
python generate_dict_xml.py wiktionary "kaikki.org-dictionary-German.jsonl"  --lang de --name DE --out dictionary
python generate_dict_xml.py wiktionary "kaikki.org-dictionary-English.jsonl" --lang en --name EN --out dictionary
```

```cmd
:: 4) Copy to the SD card (replace E: with your card's drive letter)
xcopy /Y "songs\*" "E:\songs\"
xcopy /Y "dictionary\*"  "E:\dictionary\"
```

> If a dict.cc export is still a `.zip`, extract it first (Windows has tar built in):
> ```cmd
> tar -xf "your-dictcc-export.zip" -C "."
> ```

---

## 1. Songs — `generate_songs_xml.py`

Reads the three Hutterian songbook Excel files and writes one XML per songbook.

**Mapping:** SongBook → file · Category → section · Song → book (title) · 1 chapter · Stanza → verse.

### Excel inputs
- `SngBooks.xlsx` — `Id, Title, …, Active`
- `SngCategory.xlsx` — `ID, BookId, Category, …`
- `SngData.xlsx` — `Id, BookId, Category, Title1, Song, …`

(Originals also live in `..\..\fz_songs\sd_directory_builder\`.)

### Examples (cmd)
```cmd
:: Defaults: looks for SngData.xlsx / SngBooks.xlsx / SngCategory.xlsx in the current folder
python generate_songs_xml.py

:: Custom output directory
python generate_songs_xml.py --out songs_out

:: Point at the originals in the fz_songs repo folder
python generate_songs_xml.py --data "..\..\fz_songs\sd_directory_builder\SngData.xlsx" --books "..\..\fz_songs\sd_directory_builder\SngBooks.xlsx" --cats "..\..\fz_songs\sd_directory_builder\SngCategory.xlsx" --out songs_out

:: Also bundle the output into songs_out_sd.zip
python generate_songs_xml.py --out songs_out --zip
```

### Options
| Option    | Default            | Meaning                     |
|-----------|--------------------|-----------------------------|
| `--data`  | `SngData.xlsx`     | songs workbook              |
| `--books` | `SngBooks.xlsx`    | songbooks workbook          |
| `--cats`  | `SngCategory.xlsx` | categories workbook         |
| `--out`   | `songs_out`        | output directory            |
| `--zip`   | off                | also write `<out>_sd.zip`   |

### Result (example run)
```
songs_out\
  lutherisches_gesangbuch.xml / .toc     (730 songs, 64 categories)
  kleines_gesangbuch_-_ed_14.xml / .toc  (202 songs, 28 categories)
  andere_lieder.xml / .toc               (193 songs, 17 categories)
  vaeter_gesangbuch.xml / .toc           (350 songs,  6 categories)
```
Copy every `.xml` + `.toc` into `/songs/`. Each file is a selectable songbook; categories are
the sections; songs are listed by title; stanzas keep their line breaks.

---

## 2. Dictionary — `generate_dict_xml.py`

Two input formats, each its own subcommand. One letter-bucket = one "book" (A–Z, `0-9`,
`Other`); entries are paged into chapters (default 100/chapter); each entry is one verse shown
as `word - definition`.

### a) dict.cc tab export (bilingual)
A `*.txt` export from dict.cc: `source <TAB> translation <TAB> wordtype <TAB> category`.

```cmd
:: Single direction (source word -> translation)
python generate_dict_xml.py dictcc "EN-DE.txt" --name DE-EN --out dict_out

:: Both directions from one file (forward + columns-swapped reverse)
python generate_dict_xml.py dictcc "EN-DE.txt" --name DE-EN --bidirectional --reverse-name EN-DE --out dict_out
```

| Option            | Default     | Meaning                                       |
|-------------------|-------------|-----------------------------------------------|
| `--name` / `-n`   | `dictcc`    | output name (→ `de-en.xml`, shown as "DE-EN") |
| `--bidirectional` / `-b` | off  | also build the reversed direction             |
| `--reverse-name` / `-r`  | auto-flip | name for the reversed file               |
| `--out` / `-o`    | `dict_out`  | output directory                              |
| `--page`          | `100`       | entries per chapter (keep ≤ 200)              |

### b) Kaikki.org Wiktionary JSONL (monolingual)
A full language dump from <https://kaikki.org/dictionary/> (`word -> definition senses`).

```cmd
python generate_dict_xml.py wiktionary "kaikki.org-dictionary-German.jsonl"  --lang de --name DE --out dict_out
python generate_dict_xml.py wiktionary "kaikki.org-dictionary-English.jsonl" --lang en --name EN --out dict_out

:: Smaller output: fewer senses, only main word classes
python generate_dict_xml.py wiktionary "kaikki.org-dictionary-German.jsonl" --lang de --name DE --max-senses 2 --out dict_out
```

| Option          | Default      | Meaning                          |
|-----------------|--------------|----------------------------------|
| `--lang` / `-l` | *(required)* | `de` or `en` (sets POS labels)   |
| `--name` / `-n` | `DE`/`EN`    | output name                      |
| `--max-senses`  | `5`          | senses kept per entry            |
| `--out` / `-o`  | `dict_out`   | output directory                 |
| `--page`        | `100`        | entries per chapter (keep ≤ 200) |

> The English JSONL is ~3 GB and the German ~1 GB. The script streams them in one pass
> (temporary per-letter files, then sorts each bucket), so memory stays low, but a full run
> still takes a few minutes.

### Result (example dict.cc run)
```
dict_out\
  de-en.xml / .toc / .pgx   (1,311,733 entries, 28 letter-books)
  en-de.xml / .toc / .pgx   (1,311,733 entries, reverse)
```
Copy every `.xml` + `.toc` + `.pgx` into `/dictionary/`. Each file is a selectable dictionary.
Pick a letter to get a **page list** ("firstword - lastword", 100 words/page), or just use
**Search** (the fast way — it scans only the matching letter bucket). If `.pgx` is missing the
pages still work, just labelled "Page N".

---

## Verifying output (optional)

Both scripts print a summary. To confirm a file is well-formed XML:

```cmd
python -c "import xml.etree.ElementTree as ET; ET.parse('songs_out/andere_lieder.xml'); print('XML OK')"
```

Malformed XML in one file makes only that translation fail to open; the others still work.

---

## Boot splash — `make_splash.py`

Optional. Converts an image into `splash.raw`, a raw RGB565 blob sized to your screen,
shown for 2.5 s on boot (and again whenever you tap the main-menu header). The image is
cropped to the screen's portrait aspect and centered on the chip.

```cmd
pip install pillow
:: Pancake (320x480):
python make_splash.py esp32c5.png --board pancake
:: V8 / V6.1 (240x320):
python make_splash.py esp32c5.png --board v8
:: nudge the crop onto the chip if needed (fractions of the image):
python make_splash.py esp32c5.png --board pancake --cx 0.62 --cy 0.48
```

Copy the result to the **SD card root** as `\splash.raw`. If it's absent, boot just skips the
splash. (If colors look swapped, tell me — it's a one-line byte-order flip in the firmware.)

## Final SD card layout

```
SD root\
  splash.raw                                   (optional, from make_splash.py)
  bible\        asv.xml, web.xml, …            (Bible mode)
  songs\        *.xml + *.toc                  (from generate_songs_xml.py)
  dictionary\   *.xml + *.toc + *.pgx          (from generate_dict_xml.py)
```

Each mode keeps its own `bookmarks.txt` and `srch_hist.txt` inside its own folder, and its own
settings in NVS — the three modes don't interfere with each other.
