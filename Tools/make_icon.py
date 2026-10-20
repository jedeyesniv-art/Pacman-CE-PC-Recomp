"""Draw the exe icon (an original neon-maze design, no game artwork) and write rexglue/pacman.ico.

Usage: python Tools/make_icon.py [output.ico]   (needs Pillow; run again after changing the design)
Each size is drawn separately: 256 and 64 carry the title, 48 and below drop it and use heavier lines so
they stay readable on the taskbar.
"""

import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SUPERSAMPLE = 4
FONT = "C:/Windows/Fonts/ariblk.ttf"  # Arial Black
SIZES = [(256, True), (64, True), (48, False), (32, False), (16, False)]  # (pixels, with title)


def render(size, with_text):
    """Draws the icon at `size` pixels. Coordinates are on a 256-unit grid."""
    full = size * SUPERSAMPLE
    k = size / 256 * SUPERSAMPLE

    def layer():
        return Image.new("RGBA", (full, full), (0, 0, 0, 0))

    def glow(img, radius, strength):
        blurred = img.filter(ImageFilter.GaussianBlur(radius * k))
        base = layer()
        for _ in range(strength):
            base = Image.alpha_composite(base, blurred)
        return Image.alpha_composite(base, img)

    def box(b):
        return [v * k for v in b]

    # Background: dark navy gradient in a rounded square.
    bg = layer()
    draw = ImageDraw.Draw(bg)
    for y in range(full):
        t = y / full
        draw.line([(0, y), (full, y)], fill=(int(8 + 10 * t), int(10 + 8 * t), int(32 + 26 * t), 255))
    mask = Image.new("L", (full, full), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, full - 1, full - 1], radius=48 * k, fill=255)

    # Neon maze: outer wall with an opening on the right, inner block with an opening on the left.
    maze = layer()
    draw = ImageDraw.Draw(maze)
    blue = (60, 140, 255, 255)
    line_width = max(1, int((7 if size > 32 else 10) * k))
    draw.rounded_rectangle(box((28, 28, 228, 228)), radius=34 * k, outline=blue, width=line_width)
    if with_text:
        inner, gap = (84, 84, 172, 140), (76, 100, 96, 124)  # shorter block, room for the title
    else:
        inner, gap = (84, 84, 172, 172), (76, 112, 96, 144)
    draw.rounded_rectangle(box(inner), radius=16 * k, outline=blue, width=line_width)
    draw.rectangle(box((210, 104, 240, 152)), fill=(0, 0, 0, 0))
    draw.rectangle(box(gap), fill=(0, 0, 0, 0))
    maze = glow(maze, 6, 3)

    # Pellet trail along the corridor.
    pellets = layer()
    draw = ImageDraw.Draw(pellets)
    r = 6 if size > 32 else 9
    for x, y in [(56, 128), (56, 92), (56, 56), (92, 56), (128, 56), (164, 56), (200, 56), (200, 92)]:
        draw.ellipse(box((x - r, y - r, x + r, y + r)), fill=(255, 214, 120, 255))
    pellets = glow(pellets, 7, 2)

    img = Image.alpha_composite(Image.alpha_composite(bg, maze), pellets)

    if with_text:
        text = layer()
        draw = ImageDraw.Draw(text)
        font = ImageFont.truetype(FONT, int(21 * k))
        label = "PAC-MAN CE"
        bb = draw.textbbox((0, 0), label, font=font)
        width, height = bb[2] - bb[0], bb[3] - bb[1]
        draw.text(((full - width) / 2 - bb[0], 184 * k - height / 2 - bb[1]), label, font=font,
                  fill=(255, 222, 120, 255))
        img = Image.alpha_composite(img, glow(text, 4, 2))

    img.putalpha(Image.composite(img.getchannel("A"), Image.new("L", (full, full), 0), mask))
    return img.resize((size, size), Image.LANCZOS)


def png_entry(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def dib_entry(img):
    """32-bit BGRA bitmap entry (the classic format; some older readers can't decode PNG entries)."""
    w, h = img.size
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)  # height covers the AND mask
    pixels = b"".join(img.crop((0, y, w, y + 1)).tobytes("raw", "BGRA") for y in reversed(range(h)))
    mask_row = b"\0" * (((w + 31) // 32) * 4)  # all opaque; the alpha channel does the work
    return header + pixels + mask_row * h


def write_ico(path, images):
    """Writes an .ico: PNG for 256x256 (as Windows' own icons do), bitmaps for the smaller sizes."""
    entries = [png_entry(img) if img.width >= 256 else dib_entry(img) for img in images]
    data = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for img, entry in zip(images, entries):
        # Width/height 0 means 256. Colour count, reserved, planes, bits per pixel, data size, data offset.
        data += struct.pack("<BBBBHHII", img.width % 256, img.height % 256, 0, 0, 1, 32, len(entry), offset)
        offset += len(entry)
    Path(path).write_bytes(data + b"".join(entries))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "rexglue" / "pacman.ico"
    write_ico(out, [render(size, with_text) for size, with_text in SIZES])
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
