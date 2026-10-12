#!/usr/bin/env python3
# AURORA_PCE_MIPS_STAGE_V1_20260928
from __future__ import annotations

import argparse
from pathlib import Path
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

EXPECTED_HEAD = "9807e82d2138f3a1576b7e0b316f9736ecc20d96"
EXPECTED_BLOBS = {
    "Makefile.common": "f10aecd5b405dfcb664ac2fe8da2e08f0ea86252",
    "mednafen/pce_fast/huc6280.c": "535df65b8b6e84ef165ef81b8be2fd568f0d47b4",
    "mednafen/pce_fast/huc6280.h": "58878a560274e09f1124bd3fce532d715ab030f4",
    "mednafen/pce_fast/vdc.c": "b2c36daf22656ad34b818e186000752be517afc1",
    "mednafen/pce_fast/psg.c": "7ff88864bc5fcd96c914e08fc058e32339513c44",
    "mednafen/sound/Blip_Buffer.c": "556575f66ed74b6667711c4208f7f01dced4bfe8",
    "mednafen/include/blip/Blip_Buffer.h": "326c3ba39b280d7bfc9b99acb55658b43f901400",
}
MARKER = "AURORA_PCE_MIPS_STAGE_V1_20260928"


def run(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, check=check, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def commit_blob(src: Path, rel: str) -> str:
    out = run(["git", "ls-tree", "HEAD", "--", rel], cwd=src).stdout.strip()
    parts = out.split()
    if len(parts) < 3 or parts[1] != "blob":
        raise SystemExit(f"missing PCE committed source: {rel}")
    return parts[2]


def validate_source(src: Path) -> None:
    if not (src / ".git").exists():
        raise SystemExit(f"PCE source is not a populated submodule: {src}")
    head = run(["git", "rev-parse", "HEAD"], cwd=src).stdout.strip()
    if head != EXPECTED_HEAD:
        raise SystemExit(f"PCE submodule HEAD mismatch: {head} != {EXPECTED_HEAD}")
    # Validate the commit, not the working tree. Old Aurora builds may have
    # left generated/modified build products inside the submodule; staging
    # must be reproducible from the pinned commit and must ignore all of them.
    for rel, expected in EXPECTED_BLOBS.items():
        got = commit_blob(src, rel)
        if got != expected:
            raise SystemExit(f"PCE committed fingerprint mismatch for {rel}: {got} != {expected}")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected one anchor, found {n}")
    return text.replace(old, new, 1)


def replace_define(text: str, name: str, new_line: str) -> str:
    pattern = re.compile(r"^#define\s+" + re.escape(name) + r"\b[^\n]*$", re.M)
    text2, n = pattern.subn(new_line, text, count=1)
    if n != 1:
        raise SystemExit(f"huc6280.c: expected one {name} definition, found {n}")
    return text2


def patch_huc(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    anchor = "struct HuC6280 HuCPU;\n"
    insert = r'''

#ifdef AURORA_PS2_PCE_FAST
/* AURORA_PCE_MIPS_CPU_BMT_V4_20260928
 * Keep HuC6280 timing, event checks, callbacks and state in C.  Only bulk
 * byte movement between pages already certified as plain RAM is delegated
 * to R5900 assembly.  Chunks never cross an 8 KiB MPR page and are limited
 * so every copied byte remains strictly before next_user_event. */
extern void AuroraPceBmtCopyIncPS2(uint8 *dst, const uint8 *src, uint32 count);
extern void AuroraPceBmtCopyDecPS2(uint8 *dst, const uint8 *src, uint32 count);

static INLINE uint32 AuroraPceMinU32(uint32 a, uint32 b)
{
   return a < b ? a : b;
}

static INLINE uint32 AuroraPceBmtEventBudget(const int32 next_user_event)
{
   const int32 delta = next_user_event - HuCPU.timestamp;
   if(delta <= 6)
      return 0;
   return (uint32)(delta - 1) / 6U;
}

static INLINE uint32 AuroraPceBmtLogicalLength(void)
{
   return HuCPU.bmt_length ? (uint32)HuCPU.bmt_length : 0x10000U;
}

static INLINE uint32 AuroraPceBmtFastTII(const int32 next_user_event)
{
   const uint32 src_addr = HuCPU.bmt_src;
   const uint32 dst_addr = HuCPU.bmt_dest;
   const uint32 src_off = src_addr & 0x1FFFU;
   const uint32 dst_off = dst_addr & 0x1FFFU;
   const uint8 *src = HuCPU.FastReadPage[src_addr >> 13];
   uint8 *dst = HuCPU.FastWritePage[dst_addr >> 13];
   uint32 n;

   if(!src || !dst)
      return 0;

   n = AuroraPceBmtLogicalLength();
   n = AuroraPceMinU32(n, 0x2000U - src_off);
   n = AuroraPceMinU32(n, 0x2000U - dst_off);
   n = AuroraPceMinU32(n, AuroraPceBmtEventBudget(next_user_event));
   if(n < 4U)
      return 0;

   AuroraPceBmtCopyIncPS2(dst + dst_off, src + src_off, n);
   HuCPU.bmt_src = (uint16)(HuCPU.bmt_src + n);
   HuCPU.bmt_dest = (uint16)(HuCPU.bmt_dest + n);
   HuCPU.bmt_length = (uint16)(HuCPU.bmt_length - n);
   HuCPU.timestamp += (int32)(n * 6U);
   return n;
}

static INLINE uint32 AuroraPceBmtFastTDD(const int32 next_user_event)
{
   const uint32 src_addr = HuCPU.bmt_src;
   const uint32 dst_addr = HuCPU.bmt_dest;
   const uint32 src_off = src_addr & 0x1FFFU;
   const uint32 dst_off = dst_addr & 0x1FFFU;
   const uint8 *src = HuCPU.FastReadPage[src_addr >> 13];
   uint8 *dst = HuCPU.FastWritePage[dst_addr >> 13];
   uint32 n;

   if(!src || !dst)
      return 0;

   n = AuroraPceBmtLogicalLength();
   n = AuroraPceMinU32(n, src_off + 1U);
   n = AuroraPceMinU32(n, dst_off + 1U);
   n = AuroraPceMinU32(n, AuroraPceBmtEventBudget(next_user_event));
   if(n < 4U)
      return 0;

   AuroraPceBmtCopyDecPS2(dst + dst_off, src + src_off, n);
   HuCPU.bmt_src = (uint16)(HuCPU.bmt_src - n);
   HuCPU.bmt_dest = (uint16)(HuCPU.bmt_dest - n);
   HuCPU.bmt_length = (uint16)(HuCPU.bmt_length - n);
   HuCPU.timestamp += (int32)(n * 6U);
   return n;
}
#else
#define AuroraPceBmtFastTII(next_user_event) 0U
#define AuroraPceBmtFastTDD(next_user_event) 0U
#endif
'''
    text = replace_once(text, anchor, anchor + insert, "huc6280.c HuCPU anchor")

    text = replace_define(
        text, "BMT_TDD",
        "#define BMT_TDD BMT_PREHONK(TDD); do { if(AuroraPceBmtFastTDD(next_user_event) && !HuCPU.bmt_length) break; ADDCYC(6); WrMem(HuCPU.bmt_dest, RdMem(HuCPU.bmt_src)); HuCPU.bmt_src--; HuCPU.bmt_dest--; BMT_HONKHONK(TDD); HuCPU.bmt_length--; } while(HuCPU.bmt_length);"
    )
    text = replace_define(
        text, "BMT_TII",
        "#define BMT_TII BMT_PREHONK(TII); do { if(AuroraPceBmtFastTII(next_user_event) && !HuCPU.bmt_length) break; ADDCYC(6); WrMem(HuCPU.bmt_dest, RdMem(HuCPU.bmt_src)); HuCPU.bmt_src++; HuCPU.bmt_dest++; BMT_HONKHONK(TII); HuCPU.bmt_length--; } while(HuCPU.bmt_length);"
    )
    path.write_text(text, encoding="utf-8")


# AURORA_V25_4_BMT_OUT_OF_LINE_20261010: stage-only, no submodule edits.
def patch_v25_bmt_profile(path: Path) -> None:
    s = path.read_text(encoding='utf-8')
    if 'AURORA_V25_PCE_BMT_STAGE_20261010' in s:
        raise SystemExit('v25.4: staged input already includes v25 instrumentation')
    origin='struct HuC6280 HuCPU;\n'
    if s.count(origin) != 1:
        raise SystemExit('v25.4: unique HuCPU declaration not found')
    s=s.replace(origin, origin + '\n#ifdef AURORA_PS2_PCE_FAST\n/* AURORA_V25_PCE_BMT_STAGE_20261010: host-only measurements. */\nstatic uint32 aurora_v25_tii_calls, aurora_v25_tii_bytes;\nstatic uint32 aurora_v25_tdd_calls, aurora_v25_tdd_bytes;\nstatic uint32 aurora_v25_tii_samples, aurora_v25_tii_ticks;\nstatic uint32 aurora_v25_tdd_samples, aurora_v25_tdd_ticks;\nstatic INLINE uint32 AuroraV25HostCount(void)\n{\n    uint32 v;\n    __asm__ __volatile__("mfc0 %0,$9" : "=r" (v) : : "memory");\n    return v;\n}\nvoid AuroraV25PceBmtSnapshot(uint32 *out)\n{\n    if(!out) return;\n    out[0]=aurora_v25_tii_calls; out[1]=aurora_v25_tii_bytes;\n    out[2]=aurora_v25_tdd_calls; out[3]=aurora_v25_tdd_bytes;\n    out[4]=aurora_v25_tii_samples; out[5]=aurora_v25_tii_ticks;\n    out[6]=aurora_v25_tdd_samples; out[7]=aurora_v25_tdd_ticks;\n}\nstatic INLINE void AuroraV25PceBmtReset(void)\n{\n    aurora_v25_tii_calls=aurora_v25_tii_bytes=0;\n    aurora_v25_tdd_calls=aurora_v25_tdd_bytes=0;\n    aurora_v25_tii_samples=aurora_v25_tii_ticks=0;\n    aurora_v25_tdd_samples=aurora_v25_tdd_ticks=0;\n}\n/* AURORA_V25_4_BMT_OUT_OF_LINE_20261010\n * Keep all telemetry out of the large, inlined HuC6280 interpreter.\n * The sole change to its call site is the callee\'s identity; exact arguments,\n * byte-copy semantics, events, and emulated timestamp stay unchanged.\n */\nextern void AuroraPceBmtCopyIncPS2(uint8 *, const uint8 *, uint32);\nextern void AuroraPceBmtCopyDecPS2(uint8 *, const uint8 *, uint32);\nstatic __attribute__((noinline,noclone)) void\nAuroraV25PceBmtMeasuredInc(uint8 *dst, const uint8 *src, uint32 n)\n{\n    const uint32 sampled = ((aurora_v25_tii_calls & 63U) == 0U);\n    const uint32 before = sampled ? AuroraV25HostCount() : 0U;\n    AuroraPceBmtCopyIncPS2(dst, src, n);\n    ++aurora_v25_tii_calls;\n    aurora_v25_tii_bytes += n;\n    if(sampled)\n    {\n        aurora_v25_tii_ticks += AuroraV25HostCount() - before;\n        ++aurora_v25_tii_samples;\n    }\n}\nstatic __attribute__((noinline,noclone)) void\nAuroraV25PceBmtMeasuredDec(uint8 *dst, const uint8 *src, uint32 n)\n{\n    const uint32 sampled = ((aurora_v25_tdd_calls & 63U) == 0U);\n    const uint32 before = sampled ? AuroraV25HostCount() : 0U;\n    AuroraPceBmtCopyDecPS2(dst, src, n);\n    ++aurora_v25_tdd_calls;\n    aurora_v25_tdd_bytes += n;\n    if(sampled)\n    {\n        aurora_v25_tdd_ticks += AuroraV25HostCount() - before;\n        ++aurora_v25_tdd_samples;\n    }\n}\n#endif\n', 1)
    for which, call, wrapper in (
       ('tii','AuroraPceBmtCopyIncPS2(dst + dst_off, src + src_off, n);',
        'AuroraV25PceBmtMeasuredInc(dst + dst_off, src + src_off, n);'),
       ('tdd','AuroraPceBmtCopyDecPS2(dst + dst_off, src + src_off, n);',
        'AuroraV25PceBmtMeasuredDec(dst + dst_off, src + src_off, n);'),
    ):
        if s.count(call)!=1:
            raise SystemExit('v25.4: unknown BMT '+which+' callsite')
        s=s.replace(call,wrapper,1)
    m=re.search(r'(?m)^void HuC6280_Reset\s*\(\s*void\s*\)\s*\{',s)
    if not m:
        raise SystemExit('v25.4: HuC6280_Reset anchor missing')
    s=s[:m.end()]+'\n#ifdef AURORA_PS2_PCE_FAST\n   AuroraV25PceBmtReset();\n#endif'+s[m.end():]
    path.write_text(s, encoding='utf-8')

def patch_blip(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    include_anchor = '#include <math.h>\n'
    decl = r'''

#ifdef AURORA_PS2_PCE_FAST
/* AURORA_PCE_MIPS_AUDIO_BLIP_V2_20260928
 * Pure sample-reader kernel: raw delta buffer, interleaved output stride,
 * high-pass accumulator and exact 16-bit clipping. Buffer removal/memmove
 * remains in the original C code. */
extern int32_t AuroraPceBlipReadStereoPS2(
      const int32_t *buffer, int16_t *out, int32_t count,
      int32_t bass_shift, int32_t reader_accum);
typedef char AuroraPceBlipLongMustBe32[(sizeof(blip_long) == 4) ? 1 : -1];
typedef char AuroraPceBlipSampleMustBe16[(sizeof(blip_sample_t) == 2) ? 1 : -1];
#endif
'''
    text = replace_once(text, include_anchor, include_anchor + decl,
                        "Blip_Buffer.c include anchor")

    pattern = re.compile(
        r"long Blip_Buffer_read_samples\(Blip_Buffer\* bbuf, blip_sample_t\* out,\n"
        r"\s+long max_samples\)\n\{.*?\n\}\n\nvoid Blip_Buffer_mix_samples",
        re.S,
    )
    replacement = r'''long Blip_Buffer_read_samples(Blip_Buffer* bbuf, blip_sample_t* out,
                              long max_samples)
{
   long count = Blip_Buffer_samples_avail(bbuf);
   if (count > max_samples)
      count = max_samples;

   if (count)
   {
#ifdef AURORA_PS2_PCE_FAST
      bbuf->reader_accum = AuroraPceBlipReadStereoPS2(
            (const int32_t *)bbuf->buffer,
            (int16_t *)out,
            (int32_t)count,
            (int32_t)BLIP_READER_BASS(*bbuf),
            (int32_t)bbuf->reader_accum);
#else
      int const bass = BLIP_READER_BASS(*bbuf);
      BLIP_READER_BEGIN(reader, *bbuf);

      blip_long n;

      for (n = count; n; --n)
      {
         blip_long s = BLIP_READER_READ(reader);
         if ((blip_sample_t) s != s)
            s = 0x7FFF - (s >> 24);
         *out = (blip_sample_t) s;
         out += 2;
         BLIP_READER_NEXT(reader, bass);
      }

      BLIP_READER_END(reader, *bbuf);
#endif
      Blip_Buffer_remove_samples(bbuf, count);
   }
   return count;
}

void Blip_Buffer_mix_samples'''
    text2, n = pattern.subn(replacement, text, count=1)
    if n != 1:
        raise SystemExit(f"Blip_Buffer.c: read_samples replacement count={n}")
    path.write_text(text2, encoding="utf-8")



def patch_vdc_sprite_clear(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    include_anchor = '#include "../state_helpers.h"\n'
    decl = (
        "\n#ifdef AURORA_PS2_PCE_FAST\n"
        "/* AURORA_PCE_SPR_CLEAR128_STAGE_V16_20260929\n"
        " * Only the sprite-line zero clear uses the new R5900 kernel.\n"
        " * Revision-v2 DrawBG remains C. */\n"
        "extern void AuroraPceClearSpriteLinePS2(uint32 *dst, uint32 count);\n"
        "#endif\n"
    )
    text = replace_once(
        text, include_anchor, include_anchor + decl,
        "vdc.c sprite-clear declaration anchor"
    )

    old = (
        "   MDFN_FastU32MemsetM8(\n"
        "      (uint32 *)spr_linebuf, 0, ((end + 3) >> 1) & ~1);\n"
    )
    new = (
        "#ifdef AURORA_PS2_PCE_FAST\n"
        "   AuroraPceClearSpriteLinePS2(\n"
        "      (uint32 *)spr_linebuf, "
        "(uint32)(((end + 3) >> 1) & ~1));\n"
        "#else\n"
        "   MDFN_FastU32MemsetM8(\n"
        "      (uint32 *)spr_linebuf, 0, ((end + 3) >> 1) & ~1);\n"
        "#endif\n"
    )
    text = replace_once(
        text, old, new, "vdc.c sprite-line zero clear"
    )
    path.write_text(text, encoding="utf-8")


def patch_vdc(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    include_anchor = '#include "../state_helpers.h"\n'
    decl = r'''

#ifdef AURORA_PS2_PCE_FAST
/* AURORA_PCE_MIPS_GFX_BG_V3_20260928
 * One assembly iteration performs the same BAT lookup and emits the same
 * packed eight indexed pixels as the existing Aurora C fast path. */
extern void AuroraPceDrawBgTilesPS2(
      const uint16 *bat_base, const uint64 *cg_base, uint64 *target,
      uint32 tile_count, uint32 bat_boom, uint32 bat_width_mask,
      const uint64 *cg_mask, const uint64 *color_lut);
#endif
'''
    text = replace_once(text, include_anchor, include_anchor + decl,
                        "vdc.c include anchor")

    old = r'''#ifdef AURORA_PS2_PCE_FAST
      {
         /* AURORA_PCE_EXPERIMENTAL_V7
          * DrawBG emits one uint64 == eight indexed pixels per iteration. */
         const unsigned aurora_tile_count = (count + 7U) >> 3;
         x = (int)aurora_tile_count;

         while(x--)
         {
            const uint16 bat = BAT_Base[bat_boom];
            const uint64 color_or = cblock_exlut[bat >> 12];

            *target64 = (CG_Base[(bat & 0xFFF) * 8] & cg_mask) | color_or;

            bat_boom = (bat_boom + 1) & bat_width_mask;
            target64++;
         }
      }
#else'''
    new = r'''#ifdef AURORA_PS2_PCE_FAST
      {
         const unsigned aurora_tile_count = (count + 7U) >> 3;
         AuroraPceDrawBgTilesPS2(
               BAT_Base, CG_Base, target64,
               aurora_tile_count, (uint32)bat_boom, (uint32)bat_width_mask,
               &cg_mask, cblock_exlut);
      }
#else'''
    text = replace_once(text, old, new, "vdc.c DrawBG fast path")
    path.write_text(text, encoding="utf-8")



def patch_multicore_profiler(stage: Path, profiler_header: Path) -> None:
    """AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008: diagnostic-only."""
    if not profiler_header.is_file():
        raise SystemExit(f"missing Aurora external profiler header: {profiler_header}")
    shutil.copy2(profiler_header, stage / "aurora_external_profiler.h")

    def rep(rel, old, new, label, count=1):
        p = stage / rel
        s = p.read_text(encoding="utf-8")
        n = s.count(old)
        if n != count:
            raise SystemExit(f"PCE profiler {label}: expected {count}, found {n}")
        p.write_text(s.replace(old, new, count), encoding="utf-8", newline="\n")

    # libretro frame + Emulate hierarchy.
    rep("libretro.c", "#include <libretro.h>\n",
        "#include <libretro.h>\n#include \"aurora_external_profiler.h\"\n",
        "libretro include")
    rep("libretro.c", "static void Emulate(EmulateSpecStruct *espec)\n{\n INPUT_Frame();\n",
        "static void Emulate(EmulateSpecStruct *espec)\n{\n AURORA_EXT_BEGIN(_aur_pce_emu_total);\n { AURORA_EXT_BEGIN(_aur_pce_t); INPUT_Frame(); AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_EMU_INPUT_FRAME, _aur_pce_t, 1); }\n",
        "Emulate entry")
    rep("libretro.c", " VDC_RunFrame(espec, false);\n",
        " { AURORA_EXT_BEGIN(_aur_pce_t); VDC_RunFrame(espec, false); AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_VDC_FRAME, _aur_pce_t, 1); }\n",
        "VDC frame")
    rep("libretro.c", "  PCECD_Run(HuCPU.timestamp * 3);\n",
        "  { AURORA_EXT_BEGIN(_aur_pce_t); PCECD_Run(HuCPU.timestamp * 3); AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_EMU_CD_FINAL, _aur_pce_t, 1); }\n",
        "final CD run", 1)
    rep("libretro.c", " PCEFast_PSG_EndFrame(psg, HuCPU.timestamp);\n",
        " { AURORA_EXT_BEGIN(_aur_pce_t); PCEFast_PSG_EndFrame(psg, HuCPU.timestamp); AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_EMU_PSG_END, _aur_pce_t, 1); }\n",
        "PSG end")
    rep("libretro.c", " {\n  uint8_t y;\n  /* sound_buf holds interleaved stereo int16 samples;",
        " {\n  AURORA_EXT_BEGIN(_aur_pce_blip);\n  uint8_t y;\n  /* sound_buf holds interleaved stereo int16 samples;",
        "Blip begin")
    rep("libretro.c", " }\n\n INPUT_FixTS();\n\n HuC6280_ResetTS();",
        "  AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_EMU_BLIP_READ, _aur_pce_blip, espec->SoundBufSize);\n }\n\n INPUT_FixTS();\n\n HuC6280_ResetTS();",
        "Blip end")
    rep("libretro.c", " if(PCE_IsCD)\n  PCECD_ResetTS();\n}\n\nint HuC_StateAction",
        " if(PCE_IsCD)\n  PCECD_ResetTS();\n AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_EMULATE_TOTAL, _aur_pce_emu_total, 1);\n}\n\nint HuC_StateAction",
        "Emulate end")
    rep("libretro.c", "   update_input();\n",
        "   { AURORA_EXT_BEGIN(_aur_pce_t); update_input(); AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_RETRO_INPUT, _aur_pce_t, 1); }\n",
        "retro input")
    # PS2 branch has the first exact video callback form.
    rep("libretro.c", "   video_cb(surf->pixels + surf->pitch * spec.DisplayRect.y,\n         video_width, video_height, surf->pitch * 2);\n",
        "   { AURORA_EXT_BEGIN(_aur_pce_t);\n     video_cb(surf->pixels + surf->pitch * spec.DisplayRect.y,\n         video_width, video_height, surf->pitch * 2);\n     AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_RETRO_VIDEO_CB, _aur_pce_t, 1); }\n",
        "retro video", 1)
    rep("libretro.c", "   audio_batch_cb(spec.SoundBuf, spec.SoundBufSize);\n",
        "   { AURORA_EXT_BEGIN(_aur_pce_t); audio_batch_cb(spec.SoundBuf, spec.SoundBufSize); AURORA_EXT_END(AURORA_EXT_CORE_PCE, AURORA_PCE_RETRO_AUDIO_CB, _aur_pce_t, spec.SoundBufSize); }\n",
        "retro audio")

    # VDC: aggregate hot scanline children locally, emit once per emulated frame.
    p = stage / "mednafen/pce_fast/vdc.c"
    s = p.read_text(encoding="utf-8")
    s = s.replace('#include "../state_helpers.h"\n', '#include "../state_helpers.h"\n#include "../../aurora_external_profiler.h"\n', 1)
    start = "void VDC_RunFrame(EmulateSpecStruct *espec, bool IsHES)\n{\n   bool fc_vrm;"
    repl = """void VDC_RunFrame(EmulateSpecStruct *espec, bool IsHES)\n{\n   AURORA_EXT_ACCUM_DECL(aur_cpu, aur_cpu_n);\n   AURORA_EXT_ACCUM_DECL(aur_bg, aur_bg_n);\n   AURORA_EXT_ACCUM_DECL(aur_spr, aur_spr_n);\n   AURORA_EXT_ACCUM_DECL(aur_mix, aur_mix_n);\n   AURORA_EXT_ACCUM_DECL(aur_over, aur_over_n);\n   AURORA_EXT_ACCUM_DECL(aur_dma, aur_dma_n);\n   AURORA_EXT_ACCUM_DECL(aur_sat, aur_sat_n);\n   AURORA_EXT_ACCUM_DECL(aur_cd, aur_cd_n);\n   bool fc_vrm;"""
    if s.count(start) != 1: raise SystemExit("PCE profiler VDC entry anchor")
    s = s.replace(start, repl, 1)

    # Restrict rewriting to VDC_RunFrame. DoDMA(), for example, occurs in
    # other functions too and must never reference these frame-local counters.
    _vdc_a = s.find("void VDC_RunFrame(EmulateSpecStruct *espec, bool IsHES)\n")
    _vdc_b = s.find("\nvoid VDC_Reset(void)", _vdc_a)
    if _vdc_a < 0 or _vdc_b < 0:
        raise SystemExit("PCE profiler VDC region")
    _vdc = s[_vdc_a:_vdc_b]
    def timed_call(text, call, acc, cnt, expected=1):
        n=text.count(call)
        if n != expected:
            raise SystemExit("PCE profiler VDC call %r: expected %d, found %d" % (call,expected,n))
        # AURORA_EXT_ACCUM expands to do { ... } while (0), therefore the
        # invocation itself needs a trailing semicolon.  The original call's
        # semicolon remains inside __VA_ARGS__ and is harmless.
        block="AURORA_EXT_ACCUM(aur_"+acc+", aur_"+cnt+", " + call.strip() + ");"
        return text.replace(call, block, expected)
    _vdc = timed_call(_vdc, "DoDMA(vdc);", "dma", "dma_n")
    _vdc = timed_call(_vdc, "DoSATDMA(vdc);", "sat", "sat_n")
    _vdc = timed_call(_vdc, "HuC6280_Run(line_leadin1);", "cpu", "cpu_n")
    _vdc = timed_call(_vdc, "HuC6280_Run(2);", "cpu", "cpu_n")
    _vdc = timed_call(_vdc, "HuC6280_Run(455 - line_leadin1 - 2);", "cpu", "cpu_n")
    _vdc = timed_call(_vdc, "DrawBG(vdc, end - start + (vdc->BG_XOffset & 7), bg_linebuf);", "bg", "bg_n")
    _vdc = timed_call(_vdc, "AuroraEvaluateSpriteOverflowOnly(vdc);", "spr", "spr_n")
    _vdc = timed_call(_vdc, "DrawSprites(vdc, end - start, spr_linebuf + 0x20);", "spr", "spr_n")
    for call in ("MixBGSPR(width, bg_linebuf + (vdc->BG_XOffset & 7) + source_offset, spr_linebuf + 0x20 + source_offset, target_ptr16 + target_offset);",
                 "MixBGOnly(width, bg_linebuf + (vdc->BG_XOffset & 7) + source_offset, target_ptr16 + target_offset);",
                 "MixSPROnly(width, spr_linebuf + 0x20 + source_offset, target_ptr16 + target_offset);",
                 "MixNone(width, target_ptr16 + target_offset);"):
        _vdc = timed_call(_vdc, call, "mix", "mix_n")
    # Two distinct TRUE-overscan branches exist in VDC_RunFrame: burst mode
    # and out-of-active-display.  They are mutually exclusive per line but
    # both must be instrumented.
    _vdc = timed_call(_vdc, "DrawOverscan(vdc, target_ptr16, DisplayRect, true, 0, 0);", "over", "over_n", 2)
    _vdc = timed_call(_vdc, "DrawOverscan(vdc, target_ptr16, DisplayRect, false, target_offset, target_offset + width);", "over", "over_n")
    _vdc = timed_call(_vdc, "PCECD_Run(HuCPU.timestamp * 3);", "cd", "cd_n")
    s = s[:_vdc_a] + _vdc + s[_vdc_b:]
    tail = "\tDisplayRect->w = defined_width[vce.dot_clock];\n}\n\nvoid VDC_Reset"
    emit = """\tDisplayRect->w = defined_width[vce.dot_clock];\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_CPU,aur_cpu,aur_cpu_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_BG,aur_bg,aur_bg_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_SPR,aur_spr,aur_spr_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_MIX,aur_mix,aur_mix_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_OVERSCAN,aur_over,aur_over_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_DMA,aur_dma,aur_dma_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_SATDMA,aur_sat,aur_sat_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_CD_SYNC,aur_cd,aur_cd_n);\n}\n\nvoid VDC_Reset"""
    if s.count(tail) != 1: raise SystemExit("PCE profiler VDC exit anchor")
    s=s.replace(tail,emit,1)
    p.write_text(s,encoding="utf-8",newline="\n")

    # PSG aggregate; preserve the PS2 early fast-return path by recording there.
    p=stage/"mednafen/pce_fast/psg.c"; s=p.read_text(encoding="utf-8")
    s=s.replace('#include "psg.h"\n', '#include "psg.h"\n#include "../../aurora_external_profiler.h"\n',1)
    s=s.replace("static void PCEFast_PSG_Update(PCEFast_PSG *psg, int32 timestamp)\n{\n   int32 run_time",
                "static void PCEFast_PSG_Update(PCEFast_PSG *psg, int32 timestamp)\n{\n   AURORA_EXT_BEGIN(_aur_psg_total);\n   int32 run_time",1)
    s=s.replace("      psg->lastts = timestamp;\n      return;\n", "      psg->lastts = timestamp;\n      AURORA_EXT_END(AURORA_EXT_CORE_PCE,AURORA_PCE_PSG_UPDATE,_aur_psg_total,6);\n      return;\n",1)
    s=s.replace("      psg->lastts = running_timestamp;\n   }\n}\n\nvoid PCEFast_PSG_EndFrame",
                "      psg->lastts = running_timestamp;\n   }\n   AURORA_EXT_END(AURORA_EXT_CORE_PCE,AURORA_PCE_PSG_UPDATE,_aur_psg_total,6);\n}\n\nvoid PCEFast_PSG_EndFrame",1)
    p.write_text(s,encoding="utf-8",newline="\n")

    # CD total + nested drive/ADPCM/fader drill-down.
    p=stage/"mednafen/pce_fast/pcecd.c"; s=p.read_text(encoding="utf-8")
    s=s.replace('#include "pcecd.h"\n', '#include "pcecd.h"\n#include "../../aurora_external_profiler.h"\n',1)
    s=s.replace("void PCECD_Run(uint32 in_timestamp)\n{\n   int32 clocks",
                "void PCECD_Run(uint32 in_timestamp)\n{\n   AURORA_EXT_BEGIN(_aur_cd_total);\n   AURORA_EXT_ACCUM_DECL(_aur_cd_fader, _aur_cd_fader_n);\n   AURORA_EXT_ACCUM_DECL(_aur_cd_adpcm, _aur_cd_adpcm_n);\n   int32 clocks",1)
    s=s.replace("      Fader_Run(chunk_clocks);\n", "      AURORA_EXT_ACCUM(_aur_cd_fader, _aur_cd_fader_n, Fader_Run(chunk_clocks));\n",1)
    s=s.replace("         ADPCM_Run(chunk_clocks, running_ts);\n", "         AURORA_EXT_ACCUM(_aur_cd_adpcm, _aur_cd_adpcm_n, ADPCM_Run(chunk_clocks, running_ts));\n",1)
    s=s.replace("      ADPCM_Run(chunk_clocks, running_ts);\n", "      AURORA_EXT_ACCUM(_aur_cd_adpcm, _aur_cd_adpcm_n, ADPCM_Run(chunk_clocks, running_ts));\n",1)
    s=s.replace("   lastts = in_timestamp;\n}\n\nvoid PCECD_ResetTS",
                "   lastts = in_timestamp;\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_CD_FADER,_aur_cd_fader,_aur_cd_fader_n);\n   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_CD_ADPCM,_aur_cd_adpcm,_aur_cd_adpcm_n);\n   AURORA_EXT_END(AURORA_EXT_CORE_PCE,AURORA_PCE_CD_RUN,_aur_cd_total,1);\n}\n\nvoid PCECD_ResetTS",1)
    p.write_text(s,encoding="utf-8",newline="\n")

    p=stage/"mednafen/pce_fast/pcecd_drive.c"; s=p.read_text(encoding="utf-8")
    # pcecd_drive.c includes pcecd_drive.h near the top.
    inc='#include "pcecd_drive.h"\n'
    if inc not in s: raise SystemExit("PCE profiler drive include anchor")
    s=s.replace(inc,inc+'#include "../../aurora_external_profiler.h"\n',1)
    s=s.replace("uint32 PCECD_Drive_Run(pcecd_drive_timestamp_t system_timestamp)\n{\n   int32 run_time",
                "uint32 PCECD_Drive_Run(pcecd_drive_timestamp_t system_timestamp)\n{\n   AURORA_EXT_BEGIN(_aur_drive);\n   int32 run_time",1)
    s=s.replace("   return(next_time);\n}\n\nvoid PCECD_Drive_SetLog",
                "   AURORA_EXT_END(AURORA_EXT_CORE_PCE,AURORA_PCE_CD_DRIVE,_aur_drive,1);\n   return(next_time);\n}\n\nvoid PCECD_Drive_SetLog",1)
    p.write_text(s,encoding="utf-8",newline="\n")


def patch_pce_sprite_work_profiler(stage: Path) -> None:
    """AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_20261008: diagnostic-only."""
    p = stage / "mednafen/pce_fast/vdc.c"
    s = p.read_text(encoding="utf-8")
    if "AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_20261008" in s:
        return
    if "AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008" not in (stage / "aurora_external_profiler.h").read_text(encoding="utf-8"):
        raise SystemExit("PCE sprite profiler requires v3.48 external header")

    macro_anchor = """static INLINE bool AuroraSpriteChunkHasZero(uint64 v)\n{\n"""
    if s.count(macro_anchor) != 1:
        raise SystemExit("PCE sprite profiler helper anchor missing/ambiguous")

    macro_block = r"""/* AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_20261008
 * Diagnostic-only sprite-pressure telemetry.  All counters are frame-local
 * aggregates and are emitted once at VDC_RunFrame end.  No emulated state or
 * renderer policy is changed. */
#if AURORA_FRONTEND_PROFILER
static uint32 aur_pce_spr_prep_cycles, aur_pce_spr_prep_calls;
static uint32 aur_pce_spr_clear_cycles, aur_pce_spr_clear_calls;
static uint32 aur_pce_spr_draw_cycles, aur_pce_spr_draw_calls;
static uint32 aur_pce_spr_lines, aur_pce_spr_nonempty_lines;
static uint32 aur_pce_spr_line_units, aur_pce_spr_native_selected;
static uint32 aur_pce_spr_post_limit, aur_pce_spr_source_sprites;
static uint32 aur_pce_spr_overflow_lines, aur_pce_spr_ge8_lines;
static uint32 aur_pce_spr_ge12_lines, aur_pce_spr_ge16_lines;
static uint32 aur_pce_spr_max_units, aur_pce_spr_max_sources;
static uint32 aur_pce_spr_offscreen, aur_pce_spr_partial_clip;
static uint32 aur_pce_spr_drawn_units, aur_pce_spr_zero_chunks;
static uint32 aur_pce_spr_opaque_chunks, aur_pce_spr_mixed_chunks;
static uint32 aur_pce_spr_sprite0_units, aur_pce_spr_written_pixels;

#define AURORA_PCE_SPR_RESET() do { \
   aur_pce_spr_prep_cycles = aur_pce_spr_prep_calls = 0; \
   aur_pce_spr_clear_cycles = aur_pce_spr_clear_calls = 0; \
   aur_pce_spr_draw_cycles = aur_pce_spr_draw_calls = 0; \
   aur_pce_spr_lines = aur_pce_spr_nonempty_lines = 0; \
   aur_pce_spr_line_units = aur_pce_spr_native_selected = 0; \
   aur_pce_spr_post_limit = aur_pce_spr_source_sprites = 0; \
   aur_pce_spr_overflow_lines = aur_pce_spr_ge8_lines = 0; \
   aur_pce_spr_ge12_lines = aur_pce_spr_ge16_lines = 0; \
   aur_pce_spr_max_units = aur_pce_spr_max_sources = 0; \
   aur_pce_spr_offscreen = aur_pce_spr_partial_clip = 0; \
   aur_pce_spr_drawn_units = aur_pce_spr_zero_chunks = 0; \
   aur_pce_spr_opaque_chunks = aur_pce_spr_mixed_chunks = 0; \
   aur_pce_spr_sprite0_units = aur_pce_spr_written_pixels = 0; \
} while(0)
#define AURORA_PCE_SPR_PHASE_BEGIN(name_) \
   uint32 name_ = AuroraExternalProfilerCycle()
#define AURORA_PCE_SPR_PHASE_END(name_, cyc_, calls_) do { \
   (cyc_) += (uint32)(AuroraExternalProfilerCycle() - (name_)); \
   ++(calls_); \
} while(0)
#define AURORA_PCE_SPR_INC(v_) (++(v_))
#define AURORA_PCE_SPR_ADD(v_, n_) ((v_) += (uint32)(n_))
#define AURORA_PCE_SPR_MAX(v_, n_) do { \
   const uint32 _aur_n = (uint32)(n_); \
   if (_aur_n > (v_)) (v_) = _aur_n; \
} while(0)
#define AURORA_PCE_SPR_LINE(units_, selected_) do { \
   const uint32 _aur_u = (uint32)(units_); \
   ++aur_pce_spr_lines; \
   aur_pce_spr_line_units += _aur_u; \
   aur_pce_spr_native_selected += (uint32)(selected_); \
   if (_aur_u > 16u) ++aur_pce_spr_overflow_lines; \
   if (_aur_u >= 8u) ++aur_pce_spr_ge8_lines; \
   if (_aur_u >= 12u) ++aur_pce_spr_ge12_lines; \
   if (_aur_u >= 16u) ++aur_pce_spr_ge16_lines; \
   if (_aur_u > aur_pce_spr_max_units) aur_pce_spr_max_units = _aur_u; \
} while(0)
#define AURORA_PCE_SPR_EMIT() do { \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_SPR_PREP,aur_pce_spr_prep_cycles,aur_pce_spr_prep_calls); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_SPR_CLEAR,aur_pce_spr_clear_cycles,aur_pce_spr_clear_calls); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_SPR_DRAW,aur_pce_spr_draw_cycles,aur_pce_spr_draw_calls); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_LINES,0,aur_pce_spr_lines); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_NONEMPTY_LINES,0,aur_pce_spr_nonempty_lines); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_LINE_UNITS,0,aur_pce_spr_line_units); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_NATIVE_SELECTED,0,aur_pce_spr_native_selected); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_POST_LIMIT,0,aur_pce_spr_post_limit); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_SOURCE_SPRITES,0,aur_pce_spr_source_sprites); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_OVERFLOW_LINES,0,aur_pce_spr_overflow_lines); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_GE8_LINES,0,aur_pce_spr_ge8_lines); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_GE12_LINES,0,aur_pce_spr_ge12_lines); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_GE16_LINES,0,aur_pce_spr_ge16_lines); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_MAX_UNITS,0,aur_pce_spr_max_units); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_MAX_SOURCES,0,aur_pce_spr_max_sources); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_OFFSCREEN,0,aur_pce_spr_offscreen); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_PARTIAL_CLIP,0,aur_pce_spr_partial_clip); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_DRAWN_UNITS,0,aur_pce_spr_drawn_units); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_ZERO_CHUNKS,0,aur_pce_spr_zero_chunks); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_OPAQUE_CHUNKS,0,aur_pce_spr_opaque_chunks); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_MIXED_CHUNKS,0,aur_pce_spr_mixed_chunks); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_SPRITE0_UNITS,0,aur_pce_spr_sprite0_units); \
   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_SPR_WORK_WRITTEN_PIXELS,0,aur_pce_spr_written_pixels); \
} while(0)
#else
#define AURORA_PCE_SPR_RESET() do { } while(0)
#define AURORA_PCE_SPR_PHASE_BEGIN(name_) do { } while(0)
#define AURORA_PCE_SPR_PHASE_END(name_, cyc_, calls_) do { } while(0)
#define AURORA_PCE_SPR_INC(v_) do { } while(0)
#define AURORA_PCE_SPR_ADD(v_, n_) do { } while(0)
#define AURORA_PCE_SPR_MAX(v_, n_) do { } while(0)
#define AURORA_PCE_SPR_LINE(units_, selected_) do { } while(0)
#define AURORA_PCE_SPR_EMIT() do { } while(0)
#endif

"""
    s = s.replace(macro_anchor, macro_block + macro_anchor, 1)

    # Chunk transparency classification + exact nonzero line-buffer writes.
    old = """   if(!packed)\n      return;\n\n   if(!AuroraSpriteChunkHasZero(packed))\n   {\n"""
    new = """   if(!packed)\n   {\n      AURORA_PCE_SPR_INC(aur_pce_spr_zero_chunks);\n      return;\n   }\n\n   if(!AuroraSpriteChunkHasZero(packed))\n   {\n      AURORA_PCE_SPR_INC(aur_pce_spr_opaque_chunks);\n      AURORA_PCE_SPR_ADD(aur_pce_spr_written_pixels, 8);\n"""
    if s.count(old) != 2:
        raise SystemExit("PCE sprite profiler chunk opaque anchors: expected 2")
    s = s.replace(old, new, 2)

    old = """   else\n   {\n      int x;\n      for(x = 0; x < 8; ++x)\n      {\n         const uint32 raw_pixel = src[x];\n         if(raw_pixel)\n            dst[x] = raw_pixel | prio_or;\n      }\n   }\n}\n\nstatic INLINE void AuroraDrawSprite8Reverse"""
    new = """   else\n   {\n      int x;\n      AURORA_PCE_SPR_INC(aur_pce_spr_mixed_chunks);\n      for(x = 0; x < 8; ++x)\n      {\n         const uint32 raw_pixel = src[x];\n         if(raw_pixel)\n         {\n            AURORA_PCE_SPR_INC(aur_pce_spr_written_pixels);\n            dst[x] = raw_pixel | prio_or;\n         }\n      }\n   }\n}\n\nstatic INLINE void AuroraDrawSprite8Reverse"""
    if s.count(old) != 1:
        raise SystemExit("PCE sprite profiler forward mixed anchor")
    s = s.replace(old, new, 1)

    old = """   else\n   {\n      int x;\n      for(x = 0; x < 8; ++x)\n      {\n         const uint32 raw_pixel = src[7 - x];\n         if(raw_pixel)\n            dst[x] = raw_pixel | prio_or;\n      }\n   }\n}\n#endif\n\nstatic INLINE void RebuildSATCache"""
    new = """   else\n   {\n      int x;\n      AURORA_PCE_SPR_INC(aur_pce_spr_mixed_chunks);\n      for(x = 0; x < 8; ++x)\n      {\n         const uint32 raw_pixel = src[7 - x];\n         if(raw_pixel)\n         {\n            AURORA_PCE_SPR_INC(aur_pce_spr_written_pixels);\n            dst[x] = raw_pixel | prio_or;\n         }\n      }\n   }\n}\n#endif\n\nstatic INLINE void RebuildSATCache"""
    if s.count(old) != 1:
        raise SystemExit("PCE sprite profiler reverse mixed anchor")
    s = s.replace(old, new, 1)

    # Reset frame aggregates after v3.48's local timing declarations.
    reset_anchor = """   AURORA_EXT_ACCUM_DECL(aur_cd, aur_cd_n);\n   bool fc_vrm;"""
    reset_new = """   AURORA_EXT_ACCUM_DECL(aur_cd, aur_cd_n);\n   AURORA_PCE_SPR_RESET();\n   bool fc_vrm;"""
    if s.count(reset_anchor) != 1:
        raise SystemExit("PCE sprite profiler VDC reset anchor")
    s = s.replace(reset_anchor, reset_new, 1)

    # DrawSprites local prep timer and per-line member count.
    draw_entry = """static NO_INLINE void DrawSprites(vdc_t *vdc, const int32 end, uint16 *spr_linebuf)\n{\n   int i;\n   int active_sprites = 0;\n   SPRLE SpriteList[64 * 2];\n"""
    draw_entry_new = draw_entry + """   AURORA_PCE_SPR_PHASE_BEGIN(_aur_spr_prep_t);\n#if AURORA_FRONTEND_PROFILER\n   uint32 aurora_prof_line_units = 0;\n#endif\n"""
    if s.count(draw_entry) != 1:
        raise SystemExit("PCE sprite profiler DrawSprites entry")
    s = s.replace(draw_entry, draw_entry_new, 1)

    cached = """      const unsigned cached_count = vdc->aurora_spr_line_count[line];\n      unsigned render_count = cached_count;\n"""
    cached_new = cached + """#if AURORA_FRONTEND_PROFILER\n      aurora_prof_line_units = cached_count;\n#endif\n"""
    if s.count(cached) != 1:
        raise SystemExit("PCE sprite profiler cached-count anchor")
    s = s.replace(cached, cached_new, 1)

    generic = """         if(y_offset < height)\n         {\n            if(active_sprites == 16)\n"""
    generic_new = """         if(y_offset < height)\n         {\n#if AURORA_FRONTEND_PROFILER\n            ++aurora_prof_line_units;\n#endif\n            if(active_sprites == 16)\n"""
    if s.count(generic) != 1:
        raise SystemExit("PCE sprite profiler generic membership anchor")
    s = s.replace(generic, generic_new, 1)

    limiter_anchor = """#ifdef AURORA_PS2_PCE_FAST\n   /* AURORA_V15_MULTICORE_SPRITE_LIMIT_20260824: filter only after native overflow detection/list build. */\n"""
    limiter_new = """   AURORA_PCE_SPR_LINE(aurora_prof_line_units, active_sprites);\n\n#ifdef AURORA_PS2_PCE_FAST\n   /* AURORA_V15_MULTICORE_SPRITE_LIMIT_20260824: filter only after native overflow detection/list build. */\n"""
    if s.count(limiter_anchor) != 1:
        raise SystemExit("PCE sprite profiler limiter anchor")
    s = s.replace(limiter_anchor, limiter_new, 1)

    # AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_FIXED_20261008
    # Close PREP, derive distinct original SAT sprites, then time the actual
    # PS2 clear block installed earlier by patch_vdc_sprite_clear().  The
    # MDFN clear survives only as the non-PS2 fallback, so anchoring on it
    # alone is incorrect for Aurora's staged PCE source.
    clear_anchor = """#ifdef AURORA_PS2_PCE_FAST\n   AuroraPceClearSpriteLinePS2(\n      (uint32 *)spr_linebuf, (uint32)(((end + 3) >> 1) & ~1));\n#else\n   MDFN_FastU32MemsetM8(\n      (uint32 *)spr_linebuf, 0, ((end + 3) >> 1) & ~1);\n#endif\n\n   if(!active_sprites)\n      return;\n\n   for(i = active_sprites - 1; i >= 0; --i)\n"""
    clear_new = r"""   AURORA_PCE_SPR_ADD(aur_pce_spr_post_limit, active_sprites);
#if AURORA_FRONTEND_PROFILER
   if(active_sprites > 0)
   {
      uint32 aur_sources = 0;
      int aur_last_source = -1;
      int aur_i;
      ++aur_pce_spr_nonempty_lines;
      for(aur_i = 0; aur_i < active_sprites; ++aur_i)
      {
         const int aur_source = (int)SpriteList[aur_i].source_index;
         if(aur_source != aur_last_source)
         {
            ++aur_sources;
            aur_last_source = aur_source;
         }
      }
      aur_pce_spr_source_sprites += aur_sources;
      if(aur_sources > aur_pce_spr_max_sources)
         aur_pce_spr_max_sources = aur_sources;
   }
#endif
   AURORA_PCE_SPR_PHASE_END(_aur_spr_prep_t, aur_pce_spr_prep_cycles, aur_pce_spr_prep_calls);

   AURORA_PCE_SPR_PHASE_BEGIN(_aur_spr_clear_t);
#ifdef AURORA_PS2_PCE_FAST
   AuroraPceClearSpriteLinePS2(
      (uint32 *)spr_linebuf, (uint32)(((end + 3) >> 1) & ~1));
#else
   MDFN_FastU32MemsetM8(
      (uint32 *)spr_linebuf, 0, ((end + 3) >> 1) & ~1);
#endif
   AURORA_PCE_SPR_PHASE_END(_aur_spr_clear_t, aur_pce_spr_clear_cycles, aur_pce_spr_clear_calls);

   if(!active_sprites)
      return;

   AURORA_PCE_SPR_PHASE_BEGIN(_aur_spr_draw_t);
   for(i = active_sprites - 1; i >= 0; --i)
"""
    if s.count(clear_anchor) != 1:
        raise SystemExit("PCE sprite profiler PREP/CLEAR/DRAW anchor")
    s = s.replace(clear_anchor, clear_new, 1)

    # Fully-offscreen and partial clipping telemetry.
    off_anchor = """      if(pos <= -16 || pos >= end)\n         continue;\n#else\n"""
    off_new = """      if(pos <= -16 || pos >= end)\n      {\n         AURORA_PCE_SPR_INC(aur_pce_spr_offscreen);\n         continue;\n      }\n#else\n"""
    if s.count(off_anchor) != 1:
        raise SystemExit("PCE sprite profiler offscreen anchor")
    s = s.replace(off_anchor, off_new, 1)

    post_skip = """#endif\n\n      dest_pix = &spr_linebuf[pos];\n"""
    post_skip_new = """#endif\n\n      AURORA_PCE_SPR_INC(aur_pce_spr_drawn_units);\n      if(pos < 0 || pos + 16 > end)\n         AURORA_PCE_SPR_INC(aur_pce_spr_partial_clip);\n\n      dest_pix = &spr_linebuf[pos];\n"""
    if s.count(post_skip) != 1:
        raise SystemExit("PCE sprite profiler drawn-unit anchor")
    s = s.replace(post_skip, post_skip_new, 1)

    sprite0 = """      if((SpriteList[i].flags & SPRF_SPRITE0) && (vdc->CR & 0x01))\n      {\n         /* Keep collision-IRQ ordering and bounds exactly scalar. */\n"""
    sprite0_new = """      if((SpriteList[i].flags & SPRF_SPRITE0) && (vdc->CR & 0x01))\n      {\n         AURORA_PCE_SPR_INC(aur_pce_spr_sprite0_units);\n         /* Keep collision-IRQ ordering and bounds exactly scalar. */\n"""
    if s.count(sprite0) != 1:
        raise SystemExit("PCE sprite profiler sprite0 anchor")
    s = s.replace(sprite0, sprite0_new, 1)

    pix = """               if(((uint32)pos + x) >= (uint32)end)\n                  continue;\n\n               if(dest_pix[x] & 0x100)\n"""
    pix_new = """               if(((uint32)pos + x) >= (uint32)end)\n                  continue;\n\n               AURORA_PCE_SPR_INC(aur_pce_spr_written_pixels);\n               if(dest_pix[x] & 0x100)\n"""
    if s.count(pix) != 1:
        raise SystemExit("PCE sprite profiler sprite0 pixel anchor")
    s = s.replace(pix, pix_new, 1)

    # Close DRAW phase at the exact DrawSprites function end.
    draw_end = """      }\n   }\n}\n\nstatic INLINE void MixBGSPR"""
    draw_end_new = """      }\n   }\n   AURORA_PCE_SPR_PHASE_END(_aur_spr_draw_t, aur_pce_spr_draw_cycles, aur_pce_spr_draw_calls);\n}\n\nstatic INLINE void MixBGSPR"""
    if s.count(draw_end) != 1:
        raise SystemExit("PCE sprite profiler DrawSprites end anchor")
    s = s.replace(draw_end, draw_end_new, 1)

    # Emit the frame-local aggregate next to v3.48's VDC child records.
    emit_anchor = """   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_CD_SYNC,aur_cd,aur_cd_n);\n}\n\nvoid VDC_Reset"""
    emit_new = """   AURORA_EXT_RECORD(AURORA_EXT_CORE_PCE,AURORA_PCE_VDC_CD_SYNC,aur_cd,aur_cd_n);\n   AURORA_PCE_SPR_EMIT();\n}\n\nvoid VDC_Reset"""
    if s.count(emit_anchor) != 1:
        raise SystemExit("PCE sprite profiler frame emit anchor")
    s = s.replace(emit_anchor, emit_new, 1)

    p.write_text(s, encoding="utf-8", newline="\n")

# AURORA_PCE_V3_1_LOCAL_STAGE_OVERLAY_V1_20261008
# Export HEAD first, then overlay ONLY vetted local C sources (not the whole
# dirty submodule). The original PS2 MIPS BMT/sprite-clear passes still run
# on top of them, preserving the existing assembly build pipeline.
def aurora_overlay_v31_local_cpu_vdc(src: Path, stage: Path) -> None:
    paths = ("mednafen/pce_fast/vdc.c", "mednafen/pce_fast/huc6280.c", "libretro.c") # AURORA_PCE_PAD6_V1_20261009
    changed = {}
    content = {}
    for rel in paths:
        path = src / rel
        if not path.is_file():
            raise SystemExit("[PCE stage v3.1] missing source: " + rel)
        worktree = path.read_bytes()
        committed = subprocess.check_output(["git", "show", "HEAD:" + rel], cwd=src)
        if b"\r\n" in worktree:
            raise SystemExit("[PCE stage v3.1] unexpected CRLF: " + rel)
        code = worktree.decode("utf-8")
        changed[rel] = (worktree != committed)
        if changed[rel]:
            if rel.endswith("/vdc.c"):
                if "AURORA_PCE_MIX_BRANCHLESS_V1_20261008" not in code:
                    raise SystemExit("[PCE stage v3.1] unknown dirty vdc.c")
            elif rel.endswith("/huc6280.c"):
                if ("AURORA_PCE_V3_DIAGNOSTIC_20261008" not in code or
                    "void AuroraPceV3DumpCpu(FILE *fp)" not in code):
                    raise SystemExit("[PCE stage v3.1] unknown dirty huc6280.c")
            elif rel == "libretro.c":
                # Only overlay the six-button extension, not arbitrary dirty core code.
                if ("AURORA_PCE_PAD6_V1_20261009" not in code or
                    "void PCE_AuroraSet6Button(int enabled)" not in code):
                    raise SystemExit("[PCE stage v3.1] unknown dirty libretro.c")
        content[rel] = code

    vdc, cpu = content[paths[0]], content[paths[1]]
    v3_vdc = "AURORA_PCE_V3_DIAGNOSTIC_20261008" in vdc
    v3_cpu = "AURORA_PCE_V3_DIAGNOSTIC_20261008" in cpu
    if v3_vdc != v3_cpu:
        raise SystemExit("[PCE stage v3.1] partial CPU/VDC v3; abort")
    if v3_vdc:
        vdc_required = ("AURORA_PCE_MIX_BRANCHLESS_V1_20261008",
                        "AURORA_PCE_EMPTYLINE_V2_20261008",
                        "void AuroraPceV3WriteDiag(FILE *fp)",
                        "spr_empty_returns", "mix_empty_calls")
        cpu_required = ("void AuroraPceV3DumpCpu(FILE *fp)",
                        "V3_CPU(opcodes)", "V3_CPU(read_direct)")
        if (any(s not in vdc for s in vdc_required) or
                any(s not in cpu for s in cpu_required)):
            raise SystemExit("[PCE stage v3.1] incomplete cumulative v1/v2/v3")
    for rel in paths:
        if changed[rel]:
            (stage / rel).write_text(content[rel], encoding="utf-8")
            print("[PCE stage v3.1] local source overlay:", rel)
        else:
            print("[PCE stage v3.1] pinned HEAD source:", rel)
    if v3_vdc:
        print("[PCE stage v3.1] diagnostic exports will be namespaced")

def patch_v31_saigo_masked_bg(path: Path) -> None:
    # AURORA_V31_SAIGO_MASKED_BG_ZERO_20261011
    # Called after local source overlays, so the real profiler code is audited.
    text = path.read_text(encoding='utf-8')
    head = 'AURORA_PCE_MIX_BRANCHLESS_V1_20261008'
    tail = 'static void MixBGOnly('
    before = 'aurora_colors[(bg & 0x0F) ? bg : 0]'
    after = 'aurora_colors[bg & (0U - (uint32)((bg & 0x0FU) != 0U))]'
    stamp = '/* AURORA_V31_SAIGO_MASKED_BG_ZERO_20261011: three no-sprite BG paths; A/B reversible */\n'
    h = text.find(head)
    if h < 0 or text.find(head, h + len(head)) != -1:
        raise SystemExit('v31: real overlay AURORA_PCE_MIX_BRANCHLESS marker not unique')
    t = text.find(tail, h)
    if t < 0:
        raise SystemExit('v31: real overlay MixBGOnly boundary missing')
    region = text[h:t]
    prerequisites = (
        'if(s_AuroraPceNoSelectedSprites)',
        'AURORA_PCE_EMPTYLINE_V2_20261008',
        'if(!(spr_lo | spr_hi))',
        'const uint16 *const aurora_colors = vce.color_table_cache;',
        'AURORA_PCE_MIX_BRANCHLESS',
    )
    for item in prerequisites:
        if item not in region:
            raise SystemExit('v31: changed real PCE overlay, missing: ' + item)
    original = region.count(before)
    modified = region.count(after)
    if original == 0 and modified == 3 and region.count(stamp) == 1:
        return  # Idempotent producer, not incremental patching.
    if original != 3 or modified != 0 or stamp in region:
        raise SystemExit('v31: real-overlay BG expressions unexpected: ref=' + str(original) +
                         ' candidate=' + str(modified) +
                         '. Expected 3 occurrences inside original MixBGSPR. No files changed.')
    # Strong scope proof: original expressions occur only on two exact
    # empty-line fast paths and the eight-pixel empty-sprite chunk.
    s1=region.index('if(s_AuroraPceNoSelectedSprites)')
    s2=region.index('AURORA_PCE_EMPTYLINE_V2_20261008')
    s3=region.index('if(!(spr_lo | spr_hi))')
    if not (s1 < s2 < s3):
        raise SystemExit('v31: original three no-sprite paths reordered; abort')
    if any(part.count(before)!=1 for part in (
          region[s1:s2], region[s2:s3], region[s3:])):
        raise SystemExit('v31: a palette transform escaped its intended no-sprite scope')
    updated=region.replace(before,after)
    declaration='const uint16 *const aurora_colors = vce.color_table_cache;'
    updated=updated.replace(declaration, declaration+'\n    '+stamp, 1)
    if updated.count(after)!=3 or updated.count(before)!=0:
        raise SystemExit('v31: replacement integrity failed')
    path.write_text(text[:h]+updated+text[t:], encoding='utf-8')

def patch_v34_pce_jr(path: Path) -> None:
    import re
    src = path.read_text(encoding="utf-8")
    tag = "AURORA_V34_PCE_JR_SINGLE_INCREMENT_20261011"
    start = src.find("#define JR(cond)")
    if start < 0 or src.count("#define JR(cond)") != 1:
        raise SystemExit("v34 PCE: JR(cond) not uniquely found")
    stop = src.find("\n#define BRA", start)
    if stop < 0:
        raise SystemExit("v34 PCE: JR/BRA boundary not found")
    old = src[start:stop]
    if tag in src:
        prev = src.rfind(tag, 0, start)
        if (src.count(tag) != 1 or prev < start - 150
                or "   IncPC();" not in old
                or "const int32 disp = (int8)RdAtPC();" not in old
                or "disp = 1 + (int8)RdAtPC()" in old):
            raise SystemExit("v34 PCE: already patched JR divergent")
        return
    # Literal invariant audit before rewriting; fail instead of patching a
    # different cycle/IRQ/PC implementation.
    canon = re.sub(r"[ \t\r\n\\]+", "", old)
    required = ("#defineJR(cond){if(cond){int32disp;"
                "disp=1+(int8)RdAtPC();ADDCYC(2);"
                "HU_PC+=disp;}elseIncPC();}")
    if canon != required:
        raise SystemExit("v34 PCE: unexpected JR macro after local overlays: " + canon[:350])
    new = chr(10).join([
       "/* AURORA_V34_PCE_JR_SINGLE_INCREMENT_20261011 -- conditional branch PC fast path */",
       "#define JR(cond) " + chr(92),
       "{ " + chr(92),
       "   if(cond) " + chr(92),
       "   { " + chr(92),
       "      const int32 disp = (int8)RdAtPC(); " + chr(92),
       "      ADDCYC(2); " + chr(92),
       "      HU_PC += disp; " + chr(92),
       "   } " + chr(92),
       "   IncPC(); " + chr(92),
       "}",
       "",
    ])
    path.write_text(src[:start]+new+src[stop:],encoding="utf-8")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--stage", type=Path)
    ap.add_argument("--asm", type=Path)
    ap.add_argument("--profiler-header", type=Path)
    ap.add_argument("--check-source", action="store_true")
    args = ap.parse_args()

    src = args.source.resolve()
    validate_source(src)
    if args.check_source:
        print(f"[PCE stage] source OK: {EXPECTED_HEAD}")
        return 0

    if args.stage is None or args.asm is None or args.profiler_header is None:
        ap.error("--stage, --asm and --profiler-header are required unless --check-source is used")
    asm = args.asm.resolve()
    if not asm.is_file() or "AURORA_PCE_MIPS_HOTPATHS_V4_20260928" not in asm.read_text(encoding="utf-8"):
        raise SystemExit(f"missing or unrecognized PCE MIPS source: {asm}")

    stage = args.stage.resolve()
    stage.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=stage.name + ".tmp-", dir=stage.parent))
    try:
        # Export exactly HEAD. Never copy the submodule working tree, so local
        # objects/.d files or unrelated dirty files cannot leak into Continuous.
        # run() is text-oriented, so use check_output for the binary tar stream.
        tar_bytes = subprocess.check_output(
            ["git", "archive", "--format=tar", "HEAD"], cwd=src)
        with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as tf:
            tf.extractall(tmp)
        # AURORA_ASM_AUDIT_V11_PCE_SELECTIVE_C_20260929
        # Final conservative PCE policy after comparing the current MIPS
        # kernels with the last pre-MIPS build.
        #
        # Keep BMT assembly: C still owns timing/event/page certification;
        # assembly only copies already-certified plain RAM bytes and removes
        # the old per-byte RdMem/WrMem hot-path overhead.
        #
        # Keep Blip and VDC in the pinned C source instead of replacing their
        # already -O3/-funroll-loops loops with scalar assembly.  We do not
        # recreate stale C here: the pristine pinned Beetle commit is exported
        # first, so all existing Aurora C fast paths remain exactly intact.
        # AURORA_PCE_V3_1_LOCAL_STAGE_OVERLAY_V1_20261008:
        # Keep only vetted C overlays; retain the existing MIPS passes.
        aurora_overlay_v31_local_cpu_vdc(src, tmp)
        patch_huc(tmp / "mednafen/pce_fast/huc6280.c")
        patch_v25_bmt_profile(tmp / "mednafen/pce_fast/huc6280.c")
        patch_vdc_sprite_clear(tmp / "mednafen/pce_fast/vdc.c")
        patch_v31_saigo_masked_bg(tmp / "mednafen/pce_fast/vdc.c")
        patch_v34_pce_jr(tmp / "mednafen/pce_fast/huc6280.c")
        patch_multicore_profiler(tmp, args.profiler_header.resolve())
        patch_pce_sprite_work_profiler(tmp)
        asm_dst = tmp / "mednafen/pce_fast/aurora_pce_hotpaths_ps2.S"
        shutil.copy2(asm, asm_dst)
        (tmp / ".aurora-pce-mips-stage-v1").write_text(
            f"{MARKER}\nsource={EXPECTED_HEAD}\n", encoding="utf-8")
        if stage.exists():
            shutil.rmtree(stage)
        os.replace(tmp, stage)
    except BaseException:
        if tmp.exists():
            shutil.rmtree(tmp, ignore_errors=True)
        raise

    print(f"[PCE stage] prepared pristine build-tree copy: {stage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
