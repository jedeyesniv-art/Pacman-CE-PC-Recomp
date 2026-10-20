"""Wrap a PNG in a Windows .ico (one PNG-compressed entry, supported since Windows Vista).

Usage: png_to_ico.py <input.png> <output.ico>
Used by rexglue/CMakeLists.txt to make the exe icon from the owner's game_icon.png at build time.
"""

import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    png = open(sys.argv[1], "rb").read()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{sys.argv[1]}: not a PNG file")
    width, height = struct.unpack(">II", png[16:24])
    if width > 256 or height > 256:
        sys.exit(f"{sys.argv[1]}: {width}x{height} is larger than an icon can be (256x256)")

    header = struct.pack("<HHH", 0, 1, 1)  # reserved, type 1 = icon, one image
    # Width/height 0 means 256. Colour count, reserved, planes, bits per pixel, data size, data offset.
    entry = struct.pack("<BBBBHHII", width % 256, height % 256, 0, 0, 1, 32, len(png), 6 + 16)
    with open(sys.argv[2], "wb") as out:
        out.write(header + entry + png)


if __name__ == "__main__":
    main()
