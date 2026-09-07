#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil

FILES = [
    "gb.c", "sgb.c", "apu.c", "memory.c", "mbc.c", "timing.c",
    "display.c", "camera.c", "sm83_cpu.c", "joypad.c",
    "save_state.c", "random.c", "rumble.c",
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--stage", required=True)
    a = ap.parse_args()

    src = Path(a.source).resolve()
    dst = Path(a.stage).resolve()
    core = src / "Core"

    if not (src / "LICENSE").is_file():
        raise SystemExit("ERRO: SameBoy LICENSE ausente")
    for name in FILES:
        if not (core / name).is_file():
            raise SystemExit("ERRO: SameBoy Core ausente: " + name)

    if dst.exists():
        shutil.rmtree(dst)
    (dst / "Core").mkdir(parents=True)

    # SameBoy source files include assets from subdirectories such as
    # Core/graphics/*.inc. Preserve the complete Core tree recursively;
    # the PS2 Makefile still compiles only the lean FILES list above.
    shutil.rmtree(dst / "Core")
    shutil.copytree(core, dst / "Core")

    # AURORA_SGB_SCANLINE_BATCH_V4_20260907
    # Patch ONLY the staged SameBoy source. Preserve the pinned submodule.
    display = dst / "Core" / "display.c"
    dt = display.read_text(encoding="utf-8")
    old = """    if (gb->model & GB_MODEL_NO_SFC_BIT) {
        if (gb->icd_pixel_callback) {
            gb->icd_pixel_callback(gb, icd_pixel);
        }
    }
"""
    new = """    if (gb->model & GB_MODEL_NO_SFC_BIT) {
        if (gb->icd_pixel_callback) {
            /* AURORA_SGB_SCANLINE_BATCH_V4_20260907
             * Keep SameBoy's exact per-pixel generation and timing, but avoid
             * crossing the C callback boundary for every LCD pixel. The
             * uint32 screen is scratch storage in NO_SFC mode; only values
             * 0..3 are stored. Aurora consumes the completed row on HReset. */
            if (gb->screen && gb->lcd_x < WIDTH &&
                gb->current_lcd_line < LINES) {
                gb->screen[gb->lcd_x +
                           gb->current_lcd_line * WIDTH] = icd_pixel;
            }
            else {
                /* Fail-safe for a host that did not provide the row buffer. */
                gb->icd_pixel_callback(gb, icd_pixel);
            }
        }
    }
"""
    if old not in dt:
        raise SystemExit("ERRO: SameBoy display.c: callback NO_SFC não reconhecido")
    if dt.count(old) != 1:
        raise SystemExit("ERRO: SameBoy display.c: callback NO_SFC não é único")
    dt = dt.replace(old, new, 1)

    # AURORA_SGB_SCANLINE_BATCH_V4_1_20260907
    # Keep private SameBoy fields inside SameBoy. gbhost.cpp receives only the
    # completed scratch-row pointer and line index once per HReset.
    helper = r"""
/* AURORA_SGB_SCANLINE_BATCH_V4_1_20260907
 * V4 staged-only ABI helper. current_lcd_line has already been advanced when
 * the ICD HReset callback runs, so the completed visible row is line-1. */
void *AuroraSameBoyGetCompletedSGBScanline(GB_gameboy_t *gb, int *pLine)
{
    if (pLine) {
        *pLine = -1;
    }

    if (!gb || !pLine || !gb->screen ||
        gb->current_lcd_line == 0 ||
        gb->current_lcd_line > LINES) {
        return NULL;
    }

    *pLine = (int)gb->current_lcd_line - 1;
    return (void *)(gb->screen + (unsigned)(*pLine) * WIDTH);
}
"""
    if "AuroraSameBoyGetCompletedSGBScanline" not in dt:
        dt += helper

    display.write_text(dt, encoding="utf-8")

    shutil.copy2(src / "LICENSE", dst / "LICENSE")
    (dst / ".aurora-sameboy-stage-v1").write_text(
        "SameBoy staged for Aurora SGB\n", encoding="utf-8"
    )
    print("OK: SameBoy Core staged:", dst)

if __name__ == "__main__":
    main()
