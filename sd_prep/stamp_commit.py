#!/usr/bin/env python3
"""
stamp_commit.py
=====================================================================
Arduino IDE can't inject the git commit into the build, so this helper
rewrites the  #define BIBLE_FW_COMMIT "..."  line in bible_firmware/configs.h
with the current short commit SHA. Run it before cutting a release build:

    python sd_prep/stamp_commit.py

It only touches that one line; the version/name/author stay as you set them.

CI can pass the SHA explicitly so it doesn't depend on git in the runner:
    python sd_prep/stamp_commit.py 1a2b3c4
Otherwise (and locally) it reads `git rev-parse --short HEAD`.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIGS = os.path.normpath(os.path.join(HERE, "..", "bible_firmware", "configs.h"))


def main():
    # 1) explicit arg, 2) GIT_SHA env, 3) git. Trimmed to a 7-char short SHA.
    sha = (sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GIT_SHA", "")).strip()
    if not sha:
        try:
            sha = subprocess.check_output(
                ["git", "rev-parse", "--short", "HEAD"], cwd=HERE
            ).decode().strip()
        except Exception as e:
            sys.exit(f"ERROR: could not read git commit: {e}")
    sha = sha[:7]

    if not sha:
        sys.exit("ERROR: empty git commit.")

    with open(CONFIGS, "r", encoding="utf-8") as f:
        text = f.read()

    new_text, n = re.subn(
        r'(#define\s+BIBLE_FW_COMMIT\s+")[0-9a-fA-F]+(")',
        lambda m: m.group(1) + sha + m.group(2),
        text,
    )
    if n == 0:
        sys.exit("ERROR: no BIBLE_FW_COMMIT define found in configs.h")

    if new_text == text:
        print(f"BIBLE_FW_COMMIT already up to date ({sha}).")
        return

    with open(CONFIGS, "w", encoding="utf-8") as f:
        f.write(new_text)
    print(f"Stamped BIBLE_FW_COMMIT = {sha} into {CONFIGS}")


if __name__ == "__main__":
    main()
