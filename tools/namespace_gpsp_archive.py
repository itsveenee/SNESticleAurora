#!/usr/bin/env python3
"""Namespace every global defined by the pinned gpSP archive as GPSP_*.

AURORA_GPSP_GBA_V2_TFA_BLEND_20260911

This lets Aurora statically link several libretro-derived cores without their
retro_* and libretro-common globals colliding. The archive's own references are
rewritten together with definitions, so this is link-time isolation only.
"""
import argparse
import os
import shutil
import subprocess
import tempfile


def cmd(args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--ranlib", required=True)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--output", required=True)
    a = ap.parse_args()

    if not os.path.isfile(a.raw):
        raise SystemExit(f"missing raw archive: {a.raw}")

    text = cmd([a.nm, "-g", "--defined-only", a.raw])
    symbols = set()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        sym = parts[-1]
        if sym.startswith("GPSP_") or sym in ("_gp", "__gnu_local_gp"):
            continue
        if sym.startswith("__gnu_lto_"):
            continue
        # Only ordinary linker symbols. Skip archive/member decorations.
        if any(c in sym for c in "():"):
            continue
        symbols.add(sym)

    required = {
        "retro_init", "retro_deinit", "retro_load_game", "retro_unload_game",
        "retro_run", "retro_serialize", "retro_serialize_size",
        "retro_unserialize", "retro_get_memory_data", "retro_get_memory_size",
        "retro_set_environment", "retro_set_video_refresh",
        "retro_set_audio_sample_batch", "retro_set_input_poll",
        "retro_set_input_state",
        # AURORA_GPSP_GBA_V2_TFA_BLEND_20260911
        "aurora_tfa_set_storage", "aurora_tfa_reset_protocol",
        "aurora_tfa_active", "aurora_tfa_dirty", "aurora_tfa_clear_dirty"
    }
    missing = sorted(required - symbols)
    if missing:
        raise SystemExit("gpSP archive lacks expected symbols: " + ", ".join(missing))

    os.makedirs(os.path.dirname(a.output), exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="aurora-gpsp-ns-") as td:
        mapping = os.path.join(td, "redefine.txt")
        tmpout = os.path.join(td, "gpsp-namespaced.a")
        with open(mapping, "w", encoding="utf-8") as f:
            for sym in sorted(symbols):
                f.write(f"{sym} GPSP_{sym}\n")
        subprocess.check_call([a.objcopy, f"--redefine-syms={mapping}", a.raw, tmpout])
        subprocess.check_call([a.ranlib, tmpout])
        shutil.copyfile(tmpout, a.output)

    verify = cmd([a.nm, "-g", "--defined-only", a.output])
    for sym in ("GPSP_retro_init", "GPSP_retro_load_game", "GPSP_retro_run",
                "GPSP_retro_serialize", "GPSP_retro_get_memory_data",
                "GPSP_aurora_tfa_set_storage", "GPSP_aurora_tfa_dirty"):
        if sym not in verify:
            raise SystemExit(f"namespaced archive missing {sym}")
    print(f"namespaced {len(symbols)} gpSP globals -> {a.output}")


if __name__ == "__main__":
    main()
