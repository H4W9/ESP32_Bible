#!/usr/bin/env python3
"""
make_splash.py
=====================================================================
Convert an image into the ESP32 firmware's boot-splash file: a raw
RGB565 (little-endian) blob sized to the display's native portrait
resolution. Copy the output to the SD card root as  /splash.raw  — the
firmware shows it for 2.5 s on boot (and again when you tap the main-menu
header). If /splash.raw is absent, boot just skips the splash.

The image is cropped to the screen's portrait aspect ratio, centered on a
point you choose (default: the right-of-centre area, where the ESP32-C5
chip sits in the promo photo), then scaled to the screen size.

Requires Pillow:   pip install pillow

Usage:
  python make_splash.py esp32c5.png --board pancake
  python make_splash.py esp32c5.png --board v8
  # nudge the crop centre (fractions of width/height) onto the chip:
  python make_splash.py esp32c5.png --board pancake --cx 0.62 --cy 0.48
  # or set an explicit size:
  python make_splash.py esp32c5.png --w 320 --h 480

Boards:  pancake = 320x480 (ST7796)   v8 / v6 = 240x320 (ILI9341)
"""

import argparse
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("ERROR: Pillow is required.  Install with:  pip install pillow")

BOARD_SIZE = {"pancake": (320, 480), "v8": (240, 320), "v6": (240, 320)}


def crop_to_aspect(img, target_w, target_h, cx, cy):
    """Crop img to target aspect ratio, centred on (cx,cy) fractions."""
    iw, ih = img.size
    target_ar = target_w / target_h
    # Largest rectangle of target aspect that fits inside the image.
    if iw / ih > target_ar:
        ch = ih
        cw = int(round(ch * target_ar))
    else:
        cw = iw
        ch = int(round(cw / target_ar))
    # Centre on the requested point, clamped to the image bounds.
    ccx = int(round(cx * iw))
    ccy = int(round(cy * ih))
    left = max(0, min(iw - cw, ccx - cw // 2))
    top  = max(0, min(ih - ch, ccy - ch // 2))
    return img.crop((left, top, left + cw, top + ch))


def fit_to_canvas(img, target_w, target_h, pad):
    """Scale the whole image to fit inside target (no cropping), pad the rest."""
    iw, ih = img.size
    scale = min(target_w / iw, target_h / ih)
    nw, nh = max(1, int(round(iw * scale))), max(1, int(round(ih * scale)))
    scaled = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGB", (target_w, target_h), pad)
    canvas.paste(scaled, ((target_w - nw) // 2, (target_h - nh) // 2))
    return canvas


def fill_height(img, target_w, target_h, cx):
    """Scale so the image height == screen height (no vertical black), then
    center-crop the horizontal overflow at fraction cx. No top/bottom bars."""
    iw, ih = img.size
    scale = target_h / ih
    nw, nh = max(1, int(round(iw * scale))), target_h
    scaled = img.resize((nw, nh), Image.LANCZOS)
    if nw <= target_w:
        # Image is narrower than the screen — pad sides (rare for a wide source).
        canvas = Image.new("RGB", (target_w, target_h), (0, 0, 0))
        canvas.paste(scaled, ((target_w - nw) // 2, 0))
        return canvas
    left = int(round(cx * nw)) - target_w // 2
    left = max(0, min(nw - target_w, left))
    return scaled.crop((left, 0, left + target_w, target_h))


def parse_rgb(s):
    parts = [int(p) for p in s.split(",")]
    if len(parts) != 3 or any(p < 0 or p > 255 for p in parts):
        raise argparse.ArgumentTypeError("--pad must be R,G,B with each 0..255")
    return tuple(parts)


def main():
    ap = argparse.ArgumentParser(description="Make /splash.raw for the ESP32 firmware.")
    ap.add_argument("input", help="source image (png/jpg/…)")
    ap.add_argument("--board", choices=list(BOARD_SIZE), help="target board size")
    ap.add_argument("--w", type=int, help="explicit width (overrides --board)")
    ap.add_argument("--h", type=int, help="explicit height (overrides --board)")
    ap.add_argument("--cx", type=float, default=0.5, help="crop centre X (0..1)")
    ap.add_argument("--cy", type=float, default=0.5, help="crop centre Y (0..1)")
    ap.add_argument("--out", default="splash.raw", help="output file (default splash.raw)")
    # Colour-correction toggles (if the splash looks wrong on your panel):
    ap.add_argument("--bgr",    action="store_true", help="swap red/blue (BGR panel)")
    ap.add_argument("--invert", action="store_true", help="invert colours (photo negative)")
    ap.add_argument("--swap",   action="store_true", help="big-endian byte order (default is little-endian)")
    ap.add_argument("--landscape", action="store_true",
                    help="size for a landscape orientation (swaps W/H)")
    ap.add_argument("--fit", action="store_true",
                    help="fit the whole image (no crop), pad the edges — avoids zoom")
    ap.add_argument("--fill-v", dest="fill_v", action="store_true",
                    help="fill the screen vertically (no black), center-crop the sides")
    ap.add_argument("--pad", type=parse_rgb, default=(0, 0, 0),
                    help="pad colour for --fit as R,G,B (default 0,0,0 black)")
    args = ap.parse_args()

    if args.w and args.h:
        W, H = args.w, args.h
    elif args.board:
        W, H = BOARD_SIZE[args.board]
    else:
        sys.exit("Specify --board (pancake/v8/v6) or both --w and --h.")
    if args.landscape:
        W, H = H, W   # the firmware shows the splash at the active rotation's size

    img = Image.open(args.input).convert("RGB")
    if args.fit:
        img = fit_to_canvas(img, W, H, args.pad)
    elif args.fill_v:
        img = fill_height(img, W, H, args.cx)
    else:
        img = crop_to_aspect(img, W, H, args.cx, args.cy)
        img = img.resize((W, H), Image.LANCZOS)

    # The firmware reads the file with tft.setSwapBytes(true), which sends each
    # 16-bit value MSB-first (same path as fillRect). For that to be correct, the
    # uint16 the ESP reads must equal the RGB565 value, i.e. the file must be
    # LITTLE-endian on the little-endian C5.  So little-endian is the default;
    # --swap falls back to big-endian only if a panel/lib variant needs it.
    fmt = ">H" if args.swap else "<H"
    with open(args.out, "wb") as f:
        # 8-byte header: "SPL1" + width + height (LE) so the firmware knows the
        # stride and can reject a file made for a different orientation/board.
        f.write(b"SPL1" + struct.pack("<HH", W, H))
        for y in range(H):
            for x in range(W):
                r, g, b = img.getpixel((x, y))
                if args.bgr:
                    r, b = b, r
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                if args.invert:
                    v = (~v) & 0xFFFF
                f.write(struct.pack(fmt, v))

    print(f"Wrote {args.out}  ({W}x{H}, {W*H*2:,} bytes)")
    print(f"Copy it to the SD card root as  /splash.raw")
    print(f"Tip: adjust --cx/--cy to re-centre the crop on the chip.")
    print(f"If colours look wrong, re-run adding --bgr (red/blue), --invert, or --swap.")


if __name__ == "__main__":
    main()
