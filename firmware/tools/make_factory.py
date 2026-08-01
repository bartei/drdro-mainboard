#!/usr/bin/env python3
"""Merge Intel HEX files into one combined image — no dependencies.

Used to build the drDRO factory image: bootloader (0x08000000) + app (Exec, 0x08020000)
in a single .hex for first-time flashing. On first boot the bootloader finds a valid
image in Exec and runs it directly (no settings blob needed; see dualbank_design.md).

Usage:  make_factory.py <out.hex> <in1.hex> <in2.hex> [...]
Errors out if two inputs overlap (a real layout bug).
"""
import sys


def read_ihex(path):
    """Return {address: byte} for all data records. Handles ext-linear (04) /
    ext-segment (02) addressing; ignores start-address (03/05) and EOF (01)."""
    mem = {}
    base = 0
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line[0] != ":":
                continue
            data = bytes.fromhex(line[1:])
            count, addr_hi, addr_lo, rectype = data[0], data[1], data[2], data[3]
            payload = data[4:4 + count]
            # checksum check
            if (sum(data) & 0xFF) != 0:
                sys.exit(f"{path}: bad record checksum: {line}")
            if rectype == 0x00:                       # data
                addr = base + (addr_hi << 8) + addr_lo
                for i, b in enumerate(payload):
                    mem[addr + i] = b
            elif rectype == 0x04:                     # extended linear address
                base = (payload[0] << 8 | payload[1]) << 16
            elif rectype == 0x02:                     # extended segment address
                base = (payload[0] << 8 | payload[1]) << 4
            elif rectype in (0x01,):                  # EOF
                break
            # 0x03/0x05 (start address) ignored — bootloader sets the entry point
    return mem


def write_ihex(path, mem):
    def rec(count, addr, rectype, payload):
        body = bytes([count, (addr >> 8) & 0xFF, addr & 0xFF, rectype]) + payload
        chk = (-sum(body)) & 0xFF
        return ":" + body.hex().upper() + f"{chk:02X}"

    # Emit contiguous byte-runs as <=16-byte data records, re-emitting an
    # extended-linear-address record whenever the upper 16 bits of the address change.
    lines = []
    cur_upper = None
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        upper = start >> 16
        if upper != cur_upper:
            lines.append(rec(2, 0, 0x04, bytes([(upper >> 8) & 0xFF, upper & 0xFF])))
            cur_upper = upper
        chunk = [mem[start]]
        a = start
        while (len(chunk) < 16 and i + len(chunk) < len(addrs)
               and addrs[i + len(chunk)] == a + 1 and ((a + 1) >> 16) == upper):
            a += 1
            chunk.append(mem[a])
        lines.append(rec(len(chunk), start & 0xFFFF, 0x00, bytes(chunk)))
        i += len(chunk)
    lines.append(":00000001FF")                        # EOF
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: make_factory.py <out.hex> <in1.hex> <in2.hex> [...]")
    out, ins = sys.argv[1], sys.argv[2:]
    merged = {}
    for path in ins:
        m = read_ihex(path)
        overlap = merged.keys() & m.keys()
        if overlap:
            lo = min(overlap)
            sys.exit(f"overlap at 0x{lo:08X} merging {path} — check the flash layout.")
        merged.update(m)
    write_ihex(out, merged)
    lo, hi = min(merged), max(merged)
    print(f"wrote {out}: {len(merged)} bytes, 0x{lo:08X}..0x{hi:08X}")


if __name__ == "__main__":
    main()
