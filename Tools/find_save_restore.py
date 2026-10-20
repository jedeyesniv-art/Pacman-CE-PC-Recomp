"""
find_save_restore.py
Locate the register save/restore helper functions that XenonRecomp's main
TOML config needs, by scanning a decrypted + decompressed XEX base image.

Usage:
    python find_save_restore.py <basefile> [--base 0x82000000]

<basefile> is the plain PE image extracted from default.xex
(e.g. with xextool:  xextool -b default.exe default.xex ).

Byte patterns come from the XenonRecomp README.
"""

import argparse
import struct
import sys

# name -> (first-instruction pattern, expected *second* instruction or None)
# The second instruction lets us tell the real helper apart from coincidental
# matches: each helper is a run of identical ops on consecutive registers.
PATTERNS = {
    "restgprlr_14_address": (bytes.fromhex("e9c1ff68"), bytes.fromhex("e9e1ff70")),  # ld r14,-0x98(r1); ld r15,-0x90(r1)
    "savegprlr_14_address": (bytes.fromhex("f9c1ff68"), bytes.fromhex("f9e1ff70")),  # std r14..; std r15..
    "restfpr_14_address":   (bytes.fromhex("c9ccff70"), bytes.fromhex("c9ecff78")),  # lfd f14,-0x90(r12); lfd f15,-0x88(r12)
    "savefpr_14_address":   (bytes.fromhex("d9ccff70"), bytes.fromhex("d9ecff78")),  # stfd f14..; stfd f15..
    "restvmx_14_address":   (bytes.fromhex("3960fee07dcb60ce"), None),
    "savevmx_14_address":   (bytes.fromhex("3960fee07dcb61ce"), None),
    "restvmx_64_address":   (bytes.fromhex("3960fc00100b60cb"), None),
    "savevmx_64_address":   (bytes.fromhex("3960fc00100b61cb"), None),
}


def read_image_base(data: bytes):
    """Read ImageBase from the PE header, if the file has one."""
    if data[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    image_base = struct.unpack_from("<I", data, pe_off + 0x34)[0]
    return image_base, machine


def find_all(data: bytes, pattern: bytes):
    start = 0
    while True:
        i = data.find(pattern, start)
        if i == -1:
            return
        if i % 4 == 0:  # PowerPC instructions are 4-byte aligned
            yield i
        start = i + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("basefile")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="load address (default: read from PE header, else 0x82000000)")
    args = ap.parse_args()

    data = open(args.basefile, "rb").read()
    print(f"File size: {len(data):,} bytes")

    pe = read_image_base(data)
    if pe:
        print(f"PE header found: ImageBase=0x{pe[0]:08X}, machine=0x{pe[1]:04X} "
              f"({'PowerPC big-endian (Xbox 360)' if pe[1] == 0x1F2 else 'unexpected!'})")
    else:
        print("WARNING: no PE header - is this really the decrypted/decompressed basefile?")

    base = args.base if args.base is not None else (pe[0] if pe else 0x82000000)
    print(f"Using load address 0x{base:08X}\n")

    toml_lines = []
    for name, (pat, second) in PATTERNS.items():
        hits = list(find_all(data, pat))
        confirmed = [h for h in hits if second is None or data[h + 4:h + 8] == second]
        print(f"{name}: {len(hits)} match(es), {len(confirmed)} confirmed")
        for h in hits:
            tag = "  <-- confirmed" if h in confirmed and second is not None else ""
            print(f"    0x{base + h:08X}{tag}")
        if len(confirmed) == 1:
            toml_lines.append(f"{name} = 0x{base + confirmed[0]:08X}")
        else:
            toml_lines.append(f"# {name} = ???   ({len(confirmed)} candidates - check manually)")

    print("\n---- paste into the main TOML ----")
    print("\n".join(toml_lines))


if __name__ == "__main__":
    sys.exit(main())
