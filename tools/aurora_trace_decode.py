#!/usr/bin/env python3
# Aurora Runtime Trace standalone decoder.
from __future__ import annotations

import argparse
import struct
from pathlib import Path

HDR = struct.Struct("<8sHHHHI40sI")
REC = struct.Struct("<BBHIII")
SCPU = struct.Struct("<IIiiiHHHHBBBB")
SPC = struct.Struct("<IHBBiii12B")

TAG = {
    0x01: "ar", 0x02: "aw", 0x03: "sr", 0x04: "sw",
    0x05: "dq", 0x06: "dw", 0x07: "ko", 0x08: "kf",
    0x09: "br", 0x0A: "mx", 0x0B: "dm", 0x0C: "hm",
    0x0D: "hb", 0x0E: "tg", 0x0F: "cf", 0x10: "ca",
    0x11: "ph", 0xF0: "cx", 0xF1: "sn",
}
SNAP_KIND = {1:"frame>", 2:"frame<", 3:"mdma", 4:"hdma", 5:"apuio", 6:"brr"}
CTX_REASON = {
    0xE1:"frame-checkpoint", 0xE2:"high-signal-checkpoint",
    0xE3:"auto-checkpoint", 0xE4:"phase-checkpoint",
}
PHASE_KIND = {
    0x0101:"frame-enter", 0x0102:"region-ready",
    0x0103:"input-latched", 0x0104:"counters-reset",
    0x0105:"render-begin", 0x0106:"ppu-begin",
    0x0107:"visible-begin", 0x0108:"visible-end",
    0x0109:"render-sync-begin", 0x010A:"render-sync-end",
    0x010B:"ppu-end", 0x010C:"render-end",
    0x010D:"vblank-begin", 0x010E:"nmi-signaled",
    0x010F:"vblank-end", 0x0110:"final-ppu-begin",
    0x0111:"final-ppu-end", 0x0112:"field-advanced",
    0x0113:"spc-sync-begin", 0x0114:"spc-sync-end",
    0x0115:"spc-timers-done", 0x0116:"mix-begin",
    0x0117:"mix-end", 0x0118:"frame-exit",
    0x0119:"frame-counter-incremented",
    0x0120:"midframe-sync-begin", 0x0121:"midframe-sync-end",
    0x0201:"line-enter", 0x0301:"host-returned",
}

def marker_text(v: int) -> str:
    try:
        return v.to_bytes(4, "little").decode("ascii", "replace")
    except Exception:
        return f"0x{v:08x}"

def decode_entry_c(v) -> str:
    seq, pc, frame, line, cycles, a, x, y, s, op, p, sig, e = v
    return (
        f"c q={seq:08x} pc={pc:06x} op={op:02x} "
        f"fr={frame} ln={line} cyc={cycles} "
        f"a={a:04x} x={x:04x} y={y:04x} s={s:04x} "
        f"p={p:02x} sig={sig:02x} e={e:02x}"
    )

def decode_entry_s(v) -> str:
    (seq, pc, op, halt, total, frame, cycles,
     a, x, y, sp, psw, f2, f3, rom, p0, p1, p2, p3) = v
    return (
        f"s q={seq:08x} pc={pc:04x} op={op:02x} halt={halt:02x} "
        f"tot={total} fr={frame} cyc={cycles} "
        f"a={a:02x} x={x:02x} y={y:02x} sp={sp:02x} psw={psw:02x} "
        f"f2={f2:02x} f3={f3:02x} rom={rom} "
        f"ports={p0:02x},{p1:02x},{p2:02x},{p3:02x}"
    )

def decode_file(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < HDR.size:
        print(f"# file too short: {len(data)} bytes")
        return 2

    magic, version, rsz, csz, ssz, base, game_raw, reserved = HDR.unpack_from(data, 0)
    game = game_raw.split(b"\0", 1)[0].decode("utf-8", "replace")
    print(
        f"# trace v{version} game={game!r} base={base:08x} "
        f"marker={marker_text(reserved)!r} bytes={len(data)}"
    )
    if magic[:7] != b"AUR6BIN":
        print("# bad magic")
        return 2
    if rsz != REC.size:
        print(f"# unsupported record size {rsz}")
        return 2

    off = HDR.size
    last_close_fence_end = None
    last_close_fence = None
    last_close_ack = None

    while off + REC.size <= len(data):
        rec_off = off
        t, fl, x, seq, a, b = REC.unpack_from(data, off)
        off += REC.size

        if t == 0x0F:
            last_close_fence_end = off
            last_close_fence = (seq, a, b, x, rec_off)
            print(
                f"cf(pre-close) q={seq:08x} trace_frame={a} "
                f"emu_frame={b} records={x} off={rec_off:#x}"
            )
            continue
        if t == 0x10:
            last_close_ack = (seq, a, b, rec_off)
            print(
                f"ca(post-close) q={seq:08x} fence_q={a:08x} "
                f"trace_frame={b} off={rec_off:#x}"
            )
            continue
        if t == 0xF1:
            need = 2 * csz + 2 * ssz
            if csz != SCPU.size or ssz != SPC.size or off + need > len(data):
                print(f"# truncated snapshot at {rec_off:#x}")
                break
            cs = [SCPU.unpack_from(data, off + i*SCPU.size) for i in range(2)]
            off += 2*SCPU.size
            ss = [SPC.unpack_from(data, off + i*SPC.size) for i in range(2)]
            off += 2*SPC.size
            print(
                f"{SNAP_KIND.get(fl, f'snap{fl}')} q={seq:08x} "
                f"trace_frame={a} emu_frame={b} payload={x:04x}"
            )
            for v in cs: print("  " + decode_entry_c(v))
            for v in ss: print("  " + decode_entry_s(v))
            continue
        if t == 0xF0:
            cn, sn, reason = x, a, b & 0xFF
            need = cn*csz + sn*ssz
            if csz != SCPU.size or ssz != SPC.size or off + need > len(data):
                print(f"# truncated context at {rec_off:#x}")
                break
            print(
                f"cx q={seq:08x} "
                f"reason={CTX_REASON.get(reason, TAG.get(reason, hex(reason)))} "
                f"c={cn} s={sn}"
            )
            for i in range(cn):
                print("  " + decode_entry_c(SCPU.unpack_from(data, off+i*SCPU.size)))
            off += cn*SCPU.size
            for i in range(sn):
                print("  " + decode_entry_s(SPC.unpack_from(data, off+i*SPC.size)))
            off += sn*SPC.size
            continue
        if t == 0x11:
            name = PHASE_KIND.get(x, f"phase-{x:04x}")
            if x == 0x0201:
                print(f"ph q={seq:08x} {name} line={a} cpu_frame={b}")
            else:
                print(
                    f"ph q={seq:08x} {name} x={x:04x} "
                    f"line={a} cpu_frame={b}"
                )
            continue

        label = TAG.get(t, f"{t:02x}")
        edge = ">" if fl & 1 else "<" if fl & 2 else "-"
        print(f"{label}{edge} q={seq:08x} x={x:04x} a={a:08x} b={b:08x}")

    if off < len(data):
        print(f"# trailing {len(data)-off} byte(s) after last complete item")

    ack_end = None
    if last_close_ack is not None:
        aseq, fence_seq, afr, ack_off = last_close_ack
        ack_end = ack_off + REC.size
        print(
            f"# last post-close ACK q={aseq:08x} acknowledges "
            f"fence_q={fence_seq:08x} trace_frame={afr} off={ack_off:#x}"
        )
        print(f"# confirmed visible prefix through ACK: {ack_end} byte(s)")

    if last_close_fence is not None:
        seq, fr, ef, records, fence_off = last_close_fence
        fence_end = last_close_fence_end or len(data)
        print(
            f"# last visible pre-close fence q={seq:08x} trace_frame={fr} "
            f"emu_frame={ef} records={records} off={fence_off:#x}"
        )
        if last_close_ack is not None and last_close_ack[1] == seq:
            tail = len(data) - (ack_end or len(data))
            print("# newest fence has a matching visible post-close ACK")
            if tail:
                print(f"# {tail} byte(s) after that ACK are best-effort tail")
        else:
            tail = len(data) - fence_end
            if tail:
                print(
                    f"# {tail} byte(s) after the newest unacknowledged fence "
                    "are best-effort crash tail"
                )
            print(
                "# newest fence has no later visible post-close ACK; "
                "do not overclaim its fclose result"
            )
    return 0

def main() -> int:
    ap = argparse.ArgumentParser(description="Decode Aurora Runtime Trace capture")
    ap.add_argument("capture", type=Path)
    return decode_file(ap.parse_args().capture)

if __name__ == "__main__":
    raise SystemExit(main())
