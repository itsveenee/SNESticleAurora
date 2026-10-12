#!/usr/bin/env python3
"""Extract/verify an AURSDBG1 .sndbg file produced by Aurora v34+."""
from pathlib import Path
import struct
import sys
import zlib

HEADER = struct.Struct("<8s16I8I256s")
SECTION = struct.Struct("<8s4I")

if len(sys.argv) < 2:
    raise SystemExit("usage: extract_snes_debug_state.py FILE.sndbg [OUTDIR]")
src = Path(sys.argv[1])
out = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix("")
out.mkdir(parents=True, exist_ok=True)

names = {
    "STATE": "core_state.bin",
    "ROMIMG": "rom_image.sfc",
    "RAWSYS": "snes_system_raw.bin",
    "RAWROM": "snes_rom_object_raw.bin",
    "ASMCTX": "asm_context.txt",
    "SOURCES": "sources.tar.gz",
}

with src.open("rb") as f:
    raw = f.read(HEADER.size)
    if len(raw) != HEADER.size:
        raise SystemExit("truncated header")
    vals = HEADER.unpack(raw)
    magic = vals[0]
    ints = vals[1:17]
    reserved = vals[17:25]
    rom_path = vals[25].split(b"\0", 1)[0].decode("utf-8", "replace")
    (version, endian, header_bytes, section_count, flags,
     rom_crc, raw_crc, rom_bytes, state_bytes, sys_bytes, romobj_bytes,
     frame, line, cpu_pc, spc_pc, source_bytes) = ints
    if magic != b"AURSDBG1":
        raise SystemExit(f"bad magic: {magic!r}")
    if endian != 0x12345678:
        raise SystemExit(f"unsupported endian marker: {endian:08X}")
    if header_bytes != HEADER.size:
        raise SystemExit(f"unexpected header size: {header_bytes} != {HEADER.size}")

    print(f"version={version} sections={section_count} flags=0x{flags:08X}")
    print(f"ROM image CRC32={rom_crc:08X} raw-file CRC32={raw_crc:08X} bytes={rom_bytes}")
    print(f"frame={frame} line={line} CPU PC={cpu_pc:06X} SPC PC={spc_pc:04X}")
    print(f"rom_path={rom_path}")

    for i in range(section_count):
        sh = f.read(SECTION.size)
        if len(sh) != SECTION.size:
            raise SystemExit(f"truncated section header {i}")
        tag_b, secver, nbytes, crc, secflags = SECTION.unpack(sh)
        tag = tag_b.split(b"\0", 1)[0].decode("ascii", "replace")
        data = f.read(nbytes)
        if len(data) != nbytes:
            raise SystemExit(f"truncated section {tag}")
        got = zlib.crc32(data) & 0xFFFFFFFF if data else 0
        if got != crc:
            raise SystemExit(f"CRC mismatch in {tag}: file={crc:08X} got={got:08X}")
        dst = out / names.get(tag, f"{i:02d}_{tag}.bin")
        dst.write_bytes(data)
        print(f"{tag:8s} v{secver} {nbytes:9d} bytes crc={crc:08X} -> {dst}")

    extra = f.read(1)
    if extra:
        print("warning: trailing bytes after declared sections")
