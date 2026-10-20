"""
ppcinspect.py - small inspection tool for the Pac-Man CE recomp.

Needs:  pip install capstone

Modes:
  python ppcinspect.py <basefile> fn <addr> [<addr> ...] [--switches config\\switch_tables.toml]
      For each address (e.g. a "Switch case at XXXXXXXX" from the log), report:
        - the .pdata entry covering it (if any)
        - a heuristic function start/end, using the matching [[switch]] entry's
          furthest jump target, and a suggested `functions = [...]` line.

  python ppcinspect.py <basefile> dis <start> <end>
      Disassemble an address range.

  python ppcinspect.py <basefile> words <addr> <count>
      Dump <count> big-endian 32-bit words (e.g. a jump table's data).
"""

import argparse
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
except ImportError:
    sys.exit("Please run:  pip install capstone")

BLR = 0x4E800020

# Range holding the save/restore helpers (savegprlr_14 .. end of restfpr), from pacman.toml
HELPERS_LO, HELPERS_HI = 0x820B8760, 0x820B9700


def ends_function(word, addr):
    if word in (BLR, 0):
        return True
    # unconditional 'b' (opcode 18, AA=0, LK=0) into the restore helpers = epilogue
    if (word >> 26) == 18 and (word & 3) == 0:
        off = word & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
        return HELPERS_LO <= addr + off < HELPERS_HI
    return False


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        assert self.data[pe:pe + 4] == b"PE\0\0", "not a PE basefile"
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.data, pe + 0x34)[0]
        self.sections = {}
        sh = pe + 24 + opt_size
        for i in range(nsec):
            o = sh + i * 40
            name = self.data[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsize, vaddr = struct.unpack_from("<II", self.data, o + 8)
            self.sections[name] = (vaddr, vsize)
        self.pdata = self._read_pdata()
        self.cs = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)

    def word(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def _read_pdata(self):
        if ".pdata" not in self.sections:
            return []
        vaddr, vsize = self.sections[".pdata"]
        out = []
        for o in range(vaddr, vaddr + vsize, 8):
            begin, info = struct.unpack_from(">II", self.data, o)
            if begin == 0:
                continue
            size = ((info >> 8) & 0x3FFFFF) * 4
            out.append((begin, size))
        return out

    def pdata_for(self, addr):
        for begin, size in self.pdata:
            if begin <= addr < begin + size:
                return begin, size
        return None

    def disasm(self, start, end):
        lines = []
        a = start
        while a < end:
            raw = self.data[a - self.base:a - self.base + 4]
            ins = next(self.cs.disasm(raw, a), None)
            text = f"{ins.mnemonic} {ins.op_str}" if ins else f".long 0x{int.from_bytes(raw, 'big'):08X}"
            lines.append(f"  {a:08X}: {raw.hex()}  {text}")
            a += 4
        return "\n".join(lines)


def load_switches(path):
    try:
        import tomllib
    except ImportError:
        sys.exit("Python 3.11+ needed for --switches")
    with open(path, "rb") as f:
        return tomllib.load(f).get("switch", [])


def cmd_fn(img, addrs, switches):
    for addr in addrs:
        print(f"=== {addr:08X} ===")
        pd = img.pdata_for(addr)
        if pd:
            print(f"  .pdata: start {pd[0]:08X}, size 0x{pd[1]:X}, end {pd[0] + pd[1]:08X}")
        else:
            print("  .pdata: no entry covers this address")
            before = [p for p in img.pdata if p[0] <= addr]
            after = [p for p in img.pdata if p[0] > addr]
            if before:
                b = max(before)
                print(f"    nearest entry before: start {b[0]:08X}, size 0x{b[1]:X}, end {b[0] + b[1]:08X}")
            if after:
                n = min(after)
                print(f"    nearest entry after:  start {n[0]:08X}, size 0x{n[1]:X}")

        sw = next((s for s in switches if s["base"] <= addr <= s["base"] + 0x40), None)
        if not sw:
            print("  no matching [[switch]] entry found near this address\n")
            continue
        targets = [t for t in sw["labels"] + [sw.get("default", 0)] if t]
        far = max(targets)
        print(f"  switch base {sw['base']:08X}, r{sw['r']}, {len(sw['labels'])} labels, "
              f"targets {min(targets):08X}..{far:08X}")

        # Heuristic start: pdata if available, else scan back to just after a blr / padding
        if pd:
            start = pd[0]
        else:
            # Walk back until the previous word ends a function: blr, padding,
            # or an epilogue tail-branch into the register-restore helpers.
            a = sw["base"]
            while not ends_function(img.word(a - 4), a - 4):
                a -= 4
            start = a
        # Heuristic end: first blr at or after the furthest jump target
        a = far
        while img.word(a) != BLR:
            a += 4
        end = a + 4
        print(f"  heuristic: start {start:08X}, end {end:08X}")
        print(f"  first instructions:\n{img.disasm(start, start + 12)}")
        print(f"  last instructions:\n{img.disasm(end - 12, end)}")
        print(f"  SUGGESTED: {{ address = 0x{start:08X}, size = 0x{end - start:X} }},\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("basefile")
    ap.add_argument("mode", choices=["fn", "dis", "words", "bytes"])
    ap.add_argument("args", nargs="+")
    ap.add_argument("--switches", default=None)
    a = ap.parse_args()

    img = Image(a.basefile)
    nums = [int(x, 16) for x in a.args]

    if a.mode == "fn":
        cmd_fn(img, nums, load_switches(a.switches) if a.switches else [])
    elif a.mode == "dis":
        print(img.disasm(nums[0], nums[1]))
    elif a.mode == "words":
        for i in range(nums[1]):
            ad = nums[0] + i * 4
            print(f"  {ad:08X}: 0x{img.word(ad):08X}")
    elif a.mode == "bytes":
        for i in range(nums[1]):
            ad = nums[0] + i
            print(f"  {ad:08X}: 0x{img.data[ad - img.base]:02X}")
    print(f"\n(.pdata entries loaded: {len(img.pdata)})")


if __name__ == "__main__":
    main()
