#!/usr/bin/env python3
"""Stage the pinned gpSP tree and inject Aurora-only PS2 hooks.

The git submodule remains read-only/clean.  The stage is rebuilt only when the
source tree hash or this staging script changes.
"""
from __future__ import annotations
import argparse
import hashlib
import os
import re
import shutil
from pathlib import Path

# AURORA_TFA_RESYNC_R2_TURBO_OR_V1_20260913

VERSION = "AURORA_TFA_RESYNC_R2_TURBO_OR_V1_20260913_GPSP_STAGE"

TFA_H = r'''#ifndef AURORA_TFA_H
#define AURORA_TFA_H

#include "common.h"
#include <stdbool.h>

#define AURORA_TFA_BYTES (0x200000U)

void aurora_tfa_set_storage(u8 *data, u32 bytes);
void aurora_tfa_reset_protocol(void);
void aurora_tfa_transport_reset(void); /* AURORA_TFA_TRANSPORT_RESET_V1 */
bool aurora_tfa_active(void);
u8 aurora_tfa_transfer(u8 out);
bool aurora_tfa_dirty(void);
void aurora_tfa_clear_dirty(void);

#endif
'''

TFA_C = r'''/* Aurora Turbo File Advance bridge.
 * Protocol documented by Shonumi/Dan Docs; intentionally mirrors Aurora's
 * Turbo File GB implementation.  TFA adds command 0x34 (64-byte block fill).
 */
#include "common.h"
#include "aurora_tfa.h"
#include <string.h>

#define TFA_BANK_BYTES 0x2000U

typedef enum {
  TFA_WAIT_SYNC = 0,
  TFA_PACKET_BODY,
  TFA_PACKET_END,
  TFA_DATA_RESPONSE
} tfa_state_t;

static u8 *tfa_data = NULL;
static u32 tfa_bytes = 0;
static bool tfa_is_dirty = false;
static tfa_state_t tfa_state = TFA_WAIT_SYNC;
static u32 tfa_counter = 0;
static u8 tfa_command = 0;
static u8 tfa_device_status = 0x03;
static u8 tfa_card_status = 0x05;
static u16 tfa_bank = 0;
static bool tfa_sync1 = false;
static bool tfa_sync2 = false;
static u8 tfa_packet[70];
static u8 tfa_out[70];
static u32 tfa_out_length = 0;
static u32 tfa_out_pos = 0;

void aurora_tfa_reset_protocol(void)
{
  /* AURORA_TFA_TRANSPORT_RESET_V1
   * FSM e scheduler serial formam um único dispositivo lógico. */
  aurora_tfa_transport_reset();
  tfa_state = TFA_WAIT_SYNC;
  tfa_counter = 0;
  tfa_command = 0;
  tfa_device_status = 0x03;
  tfa_card_status = 0x05; /* Aurora models the 1 MiB card as inserted. */
  tfa_bank = 0;
  tfa_sync1 = false;
  tfa_sync2 = false;
  tfa_out_length = 0;
  tfa_out_pos = 0;
  memset(tfa_packet, 0, sizeof(tfa_packet));
  memset(tfa_out, 0, sizeof(tfa_out));
}

void aurora_tfa_set_storage(u8 *data, u32 bytes)
{
  tfa_data = (data && bytes == AURORA_TFA_BYTES) ? data : NULL;
  tfa_bytes = tfa_data ? bytes : 0;
  tfa_is_dirty = false;
  aurora_tfa_reset_protocol();
}

bool aurora_tfa_active(void)
{
  return tfa_data && tfa_bytes == AURORA_TFA_BYTES;
}

bool aurora_tfa_dirty(void) { return aurora_tfa_active() && tfa_is_dirty; }
void aurora_tfa_clear_dirty(void) { tfa_is_dirty = false; }

static u32 tfa_body_final_counter(u8 command)
{
  switch (command) {
    case 0x10: return 2U;  /* 5A cmd checksum */
    case 0x20: return 3U;  /* 5A cmd param checksum */
    case 0x22:
    case 0x23: return 4U;  /* AURORA_TFA_PROTOCOL_FIX_V3_20260913: bankHi bankLo checksum */
    case 0x24: return 2U;
    case 0x30: return 68U; /* 5A cmd offHi offLo + 64 data + checksum */
    case 0x34: return 5U;  /* TFA: offHi offLo fill + checksum */
    case 0x40: return 4U;  /* 5A cmd offHi offLo checksum */
    default:   return 2U;
  }
}

static void tfa_finish_response(u32 n_without_checksum)
{
  u32 i;
  u8 sum = 0x5bU; /* 0x100 - second-sync response (0xA5). */
  if (n_without_checksum + 1U > sizeof(tfa_out)) {
    tfa_out_length = 0;
    return;
  }
  for (i = 0; i < n_without_checksum; ++i)
    sum = (u8)(sum - tfa_out[i]);
  tfa_out[n_without_checksum] = sum;
  tfa_out_length = n_without_checksum + 1U;
  tfa_out_pos = 0;
}

static void tfa_build_short(u8 command)
{
  tfa_out[0] = command;
  tfa_out[1] = 0x00;
  tfa_out[2] = tfa_device_status;
  tfa_finish_response(3U);
}

static u32 tfa_storage_offset(void)
{
  return (u32)tfa_bank * TFA_BANK_BYTES +
         ((u32)tfa_packet[2] & 0x1fU) * 256U +
         (u32)tfa_packet[3];
}

static void tfa_process_command(void)
{
  u32 i, off;
  if (tfa_packet[0] != 0x5aU) {
    tfa_build_short(tfa_command ? tfa_command : 0x10U);
    return;
  }

  switch (tfa_command) {
    case 0x10:
      tfa_out[0] = 0x10;
      tfa_out[1] = 0x00;
      tfa_out[2] = tfa_device_status;
      tfa_out[3] = tfa_card_status;
      tfa_out[4] = (u8)((tfa_bank >> 7) & 0x01U);
      tfa_out[5] = (u8)(tfa_bank & 0x7fU);
      tfa_out[6] = 0x00;
      tfa_out[7] = 0x00;
      tfa_finish_response(8U);
      break;

    case 0x20:
      tfa_build_short(0x20);
      break;

    case 0x22:
    case 0x23:
      tfa_bank = (u16)((((u16)tfa_packet[2] & 0x01U) << 7) |
                       ((u16)tfa_packet[3] & 0x7fU));
      tfa_device_status |= 0x08U;
      tfa_build_short(tfa_command);
      break;

    case 0x24:
      tfa_build_short(0x24);
      break;

    case 0x30:
      off = tfa_storage_offset();
      if (off <= AURORA_TFA_BYTES - 64U) {
        for (i = 0; i < 64U; ++i) {
          u8 v = tfa_packet[4U + i];
          if (tfa_data[off + i] != v) {
            tfa_data[off + i] = v;
            tfa_is_dirty = true;
          }
        }
      }
      tfa_build_short(0x30);
      break;

    case 0x34: /* Turbo File Advance-only Block Write. */
      off = tfa_storage_offset();
      if (off <= AURORA_TFA_BYTES - 64U) {
        u8 v = tfa_packet[4];
        for (i = 0; i < 64U; ++i) {
          if (tfa_data[off + i] != v) {
            tfa_data[off + i] = v;
            tfa_is_dirty = true;
          }
        }
      }
      tfa_build_short(0x34);
      break;

    case 0x40:
      tfa_out[0] = 0x40;
      tfa_out[1] = 0x00;
      tfa_out[2] = tfa_device_status;
      off = tfa_storage_offset();
      for (i = 0; i < 64U; ++i)
        tfa_out[3U + i] = (off <= AURORA_TFA_BYTES - 64U)
                            ? tfa_data[off + i] : 0xffU;
      tfa_finish_response(67U);
      break;

    default:
      tfa_build_short(tfa_command);
      break;
  }
}

static u8 tfa_begin_sync(void)
{
  /* AURORA_TFA_COMMAND_RESYNC_V1
   * Reinicia somente a transação incompleta. Banco/status persistem. */
  tfa_state = TFA_PACKET_BODY;
  tfa_counter = 0;
  tfa_command = 0;
  tfa_sync1 = false;
  tfa_sync2 = false;
  tfa_out_length = 0;
  tfa_out_pos = 0;
  memset(tfa_packet, 0, sizeof(tfa_packet));
  memset(tfa_out, 0, sizeof(tfa_out));
  return 0xc6U;
}

u8 aurora_tfa_transfer(u8 out)
{
  u8 in = 0x00;
  if (!aurora_tfa_active())
    return 0xffU;

  /* AURORA_TFA_COMMAND_RESYNC_V1
   * 0x6C pode reiniciar recepção/packet-end após retry do jogo.
   * Não roube 0x6C enquanto uma resposta já construída é lida. */
  if (out == 0x6cU && tfa_state != TFA_DATA_RESPONSE)
    return tfa_begin_sync();

  switch (tfa_state) {
    case TFA_WAIT_SYNC:
      if (out == 0x6cU) {
        in = 0xc6U;
        tfa_state = TFA_PACKET_BODY;
        tfa_counter = 0;
        tfa_command = 0;
        memset(tfa_packet, 0, sizeof(tfa_packet));
      }
      break;

    case TFA_PACKET_BODY:
      if (tfa_counter < sizeof(tfa_packet))
        tfa_packet[tfa_counter] = out;
      if (tfa_counter == 1U)
        tfa_command = out;
      if (tfa_counter >= 1U &&
          tfa_counter == tfa_body_final_counter(tfa_command)) {
        tfa_process_command();
        tfa_state = TFA_PACKET_END;
        tfa_sync1 = tfa_sync2 = false;
      }
      ++tfa_counter;
      break;

    case TFA_PACKET_END:
      if (out == 0xf1U) {
        in = 0xe7U;
        tfa_sync1 = true;
      } else if (out == 0x7eU && tfa_sync1) {
        /* AURORA_TFA_PROTOCOL_FIX_V3_20260913: ignore stray 0x7E until the required 0xF1 arrived. */
        in = 0xa5U;
        tfa_sync2 = true;
      }
      if (tfa_sync1 && tfa_sync2) {
        tfa_state = TFA_DATA_RESPONSE;
        tfa_out_pos = 0;
      }
      break;

    case TFA_DATA_RESPONSE:
      in = (tfa_out_pos < tfa_out_length) ? tfa_out[tfa_out_pos++] : 0xffU;
      if (tfa_out_pos >= tfa_out_length) {
        tfa_state = TFA_WAIT_SYNC;
        tfa_counter = 0;
      }
      break;
  }
  return in;
}
'''


def tree_hash(root: Path, self_path: Path) -> str:
    h = hashlib.sha256()
    h.update(VERSION.encode())
    h.update(self_path.read_bytes())
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(root)
        if ".git" in rel.parts or p.suffix in (".o", ".a", ".d", ".so"):
            continue
        h.update(str(rel).encode())
        h.update(p.read_bytes())
    return h.hexdigest()


def patch_once(path: Path, old: str, new: str, count: int = 1):
    s = path.read_text(encoding="utf-8")
    if old not in s:
        raise SystemExit(f"stage anchor missing in {path}: {old[:100]!r}")
    path.write_text(s.replace(old, new, count), encoding="utf-8", newline="\n")



def patch_multicore_profiler(stage: Path, profiler_header: Path) -> None:
    """AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008: diagnostic-only."""
    if not profiler_header.is_file():
        raise SystemExit(f"missing Aurora external profiler header: {profiler_header}")
    shutil.copy2(profiler_header, stage / "aurora_external_profiler.h")

    def rep(rel, old, new, label, count=1):
        p=stage/rel; s=p.read_text(encoding="utf-8"); n=s.count(old)
        if n != count: raise SystemExit(f"GBA profiler {label}: expected {count}, found {n}")
        p.write_text(s.replace(old,new,count),encoding="utf-8",newline="\n")

    # Ensure the staged PS2 core sees the same global profiler switch.
    rep("Makefile", "\tCFLAGS += -D_EE -I$(PS2SDK)/ee/include/ -I$(PS2SDK)/common/include/\n",
        "\tCFLAGS += -D_EE -I$(PS2SDK)/ee/include/ -I$(PS2SDK)/common/include/\n\tCFLAGS += -DAURORA_FRONTEND_PROFILER=$(AURORA_FRONTEND_PROFILER)\n",
        "Makefile profiler flag")

    # Top-level frame partition + audio/video subchildren.
    rep("libretro/libretro.c", '#include "../common.h"\n', '#include "../common.h"\n#include "../aurora_external_profiler.h"\n', "libretro include")
    rep("libretro/libretro.c", "   input_poll_cb();\n   update_input();\n",
        "   { AURORA_EXT_BEGIN(_aur_gba_t); input_poll_cb(); update_input(); AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_FRAME_INPUT,_aur_gba_t,1); }\n",
        "frame input")
    rep("libretro/libretro.c", "   #ifdef HAVE_DYNAREC\n   if (dynarec_enable)\n      execute_arm_translate(execute_cycles);\n   else\n   #endif\n   {\n      /* Sticky bits only used in interpreter */\n      clear_gamepak_stickybits();\n      execute_arm(execute_cycles);\n   }\n",
        "   { AURORA_EXT_BEGIN(_aur_gba_t);\n   #ifdef HAVE_DYNAREC\n   if (dynarec_enable)\n      execute_arm_translate(execute_cycles);\n   else\n   #endif\n   {\n      /* Sticky bits only used in interpreter */\n      clear_gamepak_stickybits();\n      execute_arm(execute_cycles);\n   }\n   AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_CPU_EXEC,_aur_gba_t,1); }\n",
        "CPU execute")
    rep("libretro/libretro.c", "   audio_run();\n   video_run();\n",
        "   { AURORA_EXT_BEGIN(_aur_gba_t); audio_run(); AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_AUDIO_RUN,_aur_gba_t,1); }\n   { AURORA_EXT_BEGIN(_aur_gba_t); video_run(); AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_VIDEO_RUN,_aur_gba_t,1); }\n",
        "audio/video")
    rep("libretro/libretro.c", "   switch (serial_mode) {\n",
        "   AURORA_EXT_BEGIN(_aur_gba_serial);\n   switch (serial_mode) {\n", "serial begin")
    rep("libretro/libretro.c", "   };\n\n   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)",
        "   };\n   AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_SERIAL_FRAME,_aur_gba_serial,1);\n\n   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)", "serial end")
    rep("libretro/libretro.c", "   samples_produced = sound_read_samples(audio_sample_buffer, samples_to_read);\n",
        "   { AURORA_EXT_BEGIN(_aur_gba_t); samples_produced = sound_read_samples(audio_sample_buffer, samples_to_read); AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_AUDIO_READ,_aur_gba_t,samples_produced); }\n",
        "audio read")
    rep("libretro/libretro.c", "      video_post_process();\n      gba_screen_pixels_buf = gba_processed_pixels;\n",
        "      { AURORA_EXT_BEGIN(_aur_gba_t); video_post_process(); AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_VIDEO_POSTPROCESS,_aur_gba_t,240u*160u); }\n      gba_screen_pixels_buf = gba_processed_pixels;\n",
        "video postprocess")

    # update_gba: nested partition inside CPU_EXEC; collect children locally.
    p=stage/"main.c"; s=p.read_text(encoding="utf-8")
    s=s.replace('#include "common.h"\n', '#include "common.h"\n#include "aurora_external_profiler.h"\n',1)
    entry="""u32 function_cc update_gba(int remaining_cycles)\n{\n  u32 changed_pc = 0;\n  u32 frame_complete = 0;\n  irq_type irq_raised = IRQ_NONE;\n  int dma_cycles;\n"""
    entry2="""u32 function_cc update_gba(int remaining_cycles)\n{\n  AURORA_EXT_BEGIN(_aur_update_total);\n  AURORA_EXT_ACCUM_DECL(_aur_timers, _aur_timers_n);\n  AURORA_EXT_ACCUM_DECL(_aur_scan, _aur_scan_n);\n  AURORA_EXT_ACCUM_DECL(_aur_sound, _aur_sound_n);\n  u32 changed_pc = 0;\n  u32 frame_complete = 0;\n  irq_type irq_raised = IRQ_NONE;\n  int dma_cycles;\n"""
    if s.count(entry)!=1: raise SystemExit("GBA profiler update_gba entry")
    s=s.replace(entry,entry2,1)
    s=s.replace("    dma_cycles = update_timers(&irq_raised, completed_cycles);\n",
                "    AURORA_EXT_ACCUM(_aur_timers, _aur_timers_n, dma_cycles = update_timers(&irq_raised, completed_cycles));\n",1)
    s=s.replace("          update_scanline();\n",
                "          AURORA_EXT_ACCUM(_aur_scan, _aur_scan_n, update_scanline());\n",1)
    s=s.replace("          render_gbc_sound();\n",
                "          AURORA_EXT_ACCUM(_aur_sound, _aur_sound_n, render_gbc_sound());\n",1)
    ret="  return (execute_cycles - dma_cycles) | changed_pc | frame_complete;\n}\n\nvoid reset_gba"
    ret2="""  AURORA_EXT_RECORD(AURORA_EXT_CORE_GBA,AURORA_GBA_UPDATE_TIMERS,_aur_timers,_aur_timers_n);\n  AURORA_EXT_RECORD(AURORA_EXT_CORE_GBA,AURORA_GBA_SCANLINE_TOTAL,_aur_scan,_aur_scan_n);\n  AURORA_EXT_RECORD(AURORA_EXT_CORE_GBA,AURORA_GBA_SOUND_FRAME_FLUSH,_aur_sound,_aur_sound_n);\n  AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_UPDATE_GBA_TOTAL,_aur_update_total,1);\n  return (execute_cycles - dma_cycles) | changed_pc | frame_complete;\n}\n\nvoid reset_gba"""
    if s.count(ret)!=1: raise SystemExit("GBA profiler update_gba exit")
    s=s.replace(ret,ret2,1); p.write_text(s,encoding="utf-8",newline="\n")

    # All DMA sources (immediate, timer/sound, HBlank, VBlank) converge here.
    p=stage/"gba_memory.c"; s=p.read_text(encoding="utf-8")
    s=s.replace('#include "common.h"\n', '#include "common.h"\n#include "aurora_external_profiler.h"\n',1)
    s=s.replace("cpu_alert_type dma_transfer(unsigned dma_chan, int *usedcycles)\n{\n  dma_transfer_type *dmach",
                "cpu_alert_type dma_transfer(unsigned dma_chan, int *usedcycles)\n{\n  AURORA_EXT_BEGIN(_aur_dma);\n  dma_transfer_type *dmach",1)
    tail="""  return ret;\n}\n\n// Be sure to do this after loading ROMs."""
    tail2="""#if AURORA_FRONTEND_PROFILER\n  {\n    uint32_t _aur_dma_cycles = AuroraExternalProfilerCycle() - _aur_dma;\n    AURORA_EXT_RECORD(AURORA_EXT_CORE_GBA,AURORA_GBA_DMA_TOTAL,_aur_dma_cycles,dmach->length);\n    AURORA_EXT_RECORD(AURORA_EXT_CORE_GBA,AURORA_GBA_DMA_CH0 + dma_chan,_aur_dma_cycles,dmach->length);\n  }\n#endif\n  return ret;\n}\n\n// Be sure to do this after loading ROMs."""
    if s.count(tail)!=1: raise SystemExit("GBA profiler DMA exit")
    s=s.replace(tail,tail2,1); p.write_text(s,encoding="utf-8",newline="\n")

    # Raster drill-down.  These are nested diagnostics; RASTER_WINDOW is the
    # partition child of SCANLINE_TOTAL, while layer buckets may overlap through
    # OBJ-window recursion and are therefore reported as relation=nested.
    p=stage/"video.cc"; s=p.read_text(encoding="utf-8")
    s=s.replace('#include "common.h"\n', '#include "common.h"\n#include "aurora_external_profiler.h"\n',1)
    def wrap_region(text,start,end,bucket,label):
        a=text.find(start); b=text.find(end,a+len(start))
        if a<0 or b<0: raise SystemExit("GBA profiler raster region "+label)
        reg=text[a:b]
        brace=reg.find("{\n")
        close=reg.rfind("\n}")
        if brace<0 or close<0: raise SystemExit("GBA profiler braces "+label)
        reg=reg[:brace+2]+"  AURORA_EXT_BEGIN(_aur_raster);\n"+reg[brace+2:close]+"\n  AURORA_EXT_END(AURORA_EXT_CORE_GBA,"+bucket+",_aur_raster,1);"+reg[close:]
        return text[:a]+reg+text[b:]
    s=wrap_region(s,"static void render_scanline_text(u32 layer,\n","static inline u8 lookup_pix_8bpp(","AURORA_GBA_RASTER_TEXT_BG","text")
    s=wrap_region(s,"static void render_scanline_affine(u32 layer,\n","template<rendtype rdmode, typename buftype, unsigned mode, typename pixfmt>","AURORA_GBA_RASTER_AFFINE_BG","affine")
    s=wrap_region(s,"void render_scanline_objs(\n","// Goes through the object list in the OAM", "AURORA_GBA_RASTER_OBJ","obj")
    # Top-level window compositor.
    if "void render_scanline_window(" in s:
        s=wrap_region(s,"void render_scanline_window(","static const u8 active_layers[] = {","AURORA_GBA_RASTER_WINDOW","window")
    # render_w_effects() has early returns. Time its stable callsites instead
    # of the body, so all exits are covered exactly once.
    for call in (
        "      render_w_effects(start, end, scanline, enable_flags, &tile_mode_renderers);",
        "      render_w_effects(start, end, scanline, enable_flags, &bitmap_mode_renderers);"
    ):
        if s.count(call) != 1:
            raise SystemExit("GBA profiler effects call anchor")
        inner = call.strip()
        s=s.replace(call, "      { AURORA_EXT_BEGIN(_aur_raster); " + inner +
            " AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_RASTER_EFFECTS,_aur_raster,end-start); }", 1)

    # Bitmap modes 3/4/5 select exactly one of these BG renderers. This keeps
    # bitmap BG separate from the OBJ work in bitmap_render_layers().
    for call in (
        "          rd->affine_render(start, end, scanline, palette_ram_converted);",
        "          rd->blit_render(start, end, scanline, palette_ram_converted);",
        "          rd->scale_render(start, end, scanline, palette_ram_converted);"
    ):
        if s.count(call) != 1:
            raise SystemExit("GBA profiler bitmap call anchor")
        inner = call.strip()
        s=s.replace(call, "          { AURORA_EXT_BEGIN(_aur_raster); " + inner +
            " AURORA_EXT_END(AURORA_EXT_CORE_GBA,AURORA_GBA_RASTER_BITMAP_BG,_aur_raster,end-start); }", 1)
    p.write_text(s,encoding="utf-8",newline="\n")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--profiler-header", required=True)
    a = ap.parse_args()
    src = Path(a.source).resolve()
    stage = Path(a.stage).resolve()
    me = Path(__file__).resolve()
    profiler_header = Path(a.profiler_header).resolve()
    if not profiler_header.is_file():
        raise SystemExit(f"missing profiler header: {profiler_header}")
    if not (src / "Makefile").is_file() or not (src / "serial.c").is_file():
        raise SystemExit(f"invalid gpSP source tree: {src}")

    digest = tree_hash(src, me)
    _ph = hashlib.sha256()
    _ph.update(digest.encode("ascii"))
    _ph.update(profiler_header.read_bytes())
    digest = _ph.hexdigest()
    stamp = stage / ".aurora-gpsp-stage-v16"
    if stamp.is_file() and stamp.read_text().strip() == digest and \
       (stage / "aurora_tfa.c").is_file():
        print(f"[ gpSP stage ] up-to-date: {stage}")
        return

    if stage.exists():
        shutil.rmtree(stage)
    shutil.copytree(src, stage, ignore=shutil.ignore_patterns(
        ".git", "*.o", "*.a", "*.so", "*.d", "build"))

    (stage / "aurora_tfa.h").write_text(TFA_H, encoding="utf-8", newline="\n")
    (stage / "aurora_tfa.c").write_text(TFA_C, encoding="utf-8", newline="\n")

    # Compile the Aurora accessory implementation into the raw core archive.
    mf = stage / "Makefile.common"
    patch_once(mf,
        "             $(CORE_DIR)/serial.c \\\n",
        "             $(CORE_DIR)/serial.c \\\n             $(CORE_DIR)/aurora_tfa.c \\\n")

    # Route TFA through Normal 8-bit external-clock SIO. Aurora's frontend
    # keeps gpsp_serial=disabled, so no RFU/link-cable mode can steal it.
    ser = stage / "serial.c"
    patch_once(ser, '#include "common.h"\n',
                    '#include "common.h"\n#include "aurora_tfa.h"\n')

    # AURORA_FDS_BUBBLE_TFA_TRANSPORT_V5_20260913_TFA_EXTERNAL_CLOCK
    patch_once(
        ser,
        "static u32 serial_irq_cycles = 0;\n",
        "static u32 serial_irq_cycles = 0;\n"
        "/* AURORA_FDS_BUBBLE_TFA_TRANSPORT_V5_20260913_TFA_EXTERNAL_CLOCK */\n"
        "static u8 aurora_tfa_pending_rx = 0xffU;\n"
        "static bool aurora_tfa_pending_valid = false;\n"
        "\n"
        "void aurora_tfa_transport_reset(void)\n"
        "{\n"
        "  /* AURORA_TFA_TRANSPORT_RESET_V1 */\n"
        "  serial_irq_cycles = 0;\n"
        "  aurora_tfa_pending_rx = 0xffU;\n"
        "  aurora_tfa_pending_valid = false;\n"
        "}\n",
    )

    old = """  case SERIAL_MODE_NORMAL:\n    // For connected Wireless devices\n    if (serial_mode == SERIAL_MODE_RFU) {\n"""
    new = """  case SERIAL_MODE_NORMAL:\n    /* AURORA_FDS_BUBBLE_TFA_TRANSPORT_V5_20260913_TFA_EXTERNAL_CLOCK\n     * TFA supplies the clock in Normal 8-bit slave mode. Calculate the\n     * peripheral response now, but expose it only when the byte completes. */\n    if (aurora_tfa_active()) {\n      if ((newval & 0x0080) && !(newval & 0x0001) &&\n          !(newval & 0x1000) && !serial_irq_cycles) {\n        aurora_tfa_pending_rx =\n          aurora_tfa_transfer((u8)(read_ioreg(REG_SIODATA8) & 0xffU));\n        aurora_tfa_pending_valid = true;\n        serial_irq_cycles = CLOCK_CYC_256KHZ_8BIT;\n      }\n    }\n    // For connected Wireless devices\n    else if (serial_mode == SERIAL_MODE_RFU) {\n"""
    patch_once(ser, old, new)

    old_complete = """    case SERIAL_MODE_NORMAL:\n      // Clear the send bit, signal data is ready.\n      // Set the device busy bit, to perform the weird SO/SI handshake.\n      write_ioreg(REG_SIOCNT, (read_ioreg(REG_SIOCNT) & ~0x80) | 0x04);\n      // Return if IRQs are enabled.\n      return read_ioreg(REG_SIOCNT) & 0x4000;\n"""
    new_complete = """    case SERIAL_MODE_NORMAL:\n      if (aurora_tfa_active() && aurora_tfa_pending_valid) {\n        /* External-clock TFA completion: make RX visible now, clear START,\n         * and preserve SI instead of forcing gpSP's generic bit-2 state. */\n        write_ioreg(REG_SIODATA8, (u16)aurora_tfa_pending_rx);\n        aurora_tfa_pending_valid = false;\n        write_ioreg(REG_SIOCNT, read_ioreg(REG_SIOCNT) & ~0x0080);\n        return read_ioreg(REG_SIOCNT) & 0x4000;\n      }\n      write_ioreg(REG_SIOCNT, (read_ioreg(REG_SIOCNT) & ~0x80) | 0x04);\n      return read_ioreg(REG_SIOCNT) & 0x4000;\n"""
    patch_once(ser, old_complete, new_complete)

    # gpSP's generic mix constant is for RGB565 (channel LSBs 11,5,0).
    # platform=ps2 uses XBGR1555 (10,5,0), so use 0x0421 there.
    lr = stage / "libretro/libretro.c"
    mix_fn = "static void video_post_process_mix(void)\n"
    mix_macro = """/* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911\n * Frame-mix carry-safe averaging mask must follow the actual renderer format. */\n#ifdef USE_XBGR1555_FORMAT\n#define AURORA_GPSP_FRAME_MIX_LSB_MASK 0x0421U\n#else\n#define AURORA_GPSP_FRAME_MIX_LSB_MASK 0x0821U\n#endif\n\n"""
    patch_once(lr, mix_fn, mix_macro + mix_fn)
    s = lr.read_text(encoding="utf-8")
    n = s.count("((rgb_curr ^ rgb_prev) & 0x821)")
    if n != 2:
        raise SystemExit(f"expected two gpSP frame-mix masks, found {n}")
    s = s.replace("((rgb_curr ^ rgb_prev) & 0x821)",
                  "((rgb_curr ^ rgb_prev) & AURORA_GPSP_FRAME_MIX_LSB_MASK)")
    lr.write_text(s, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_V13_STAGE_SAFE_PERF_20260911
    # gpSP allocates the 240x160 16-bit screen buffer but upstream does not
    # guarantee an initial fill before the first frontend-visible callback.
    # Make first boot/reload deterministic (black) instead of exposing stale
    # heap rows while the GBA renderer is still reaching its first full frame.
    s = lr.read_text(encoding="utf-8")
    fb_anchor = "   /* gba_screen_pixels is dereferenced unconditionally by both the\n"
    fb_init = (
        "   if (gba_screen_pixels)\n"
        "      memset(gba_screen_pixels, 0, GBA_SCREEN_BUFFER_SIZE); "+
        "/* AURORA_GPSP_GBA_V13_STAGE_SAFE_PERF_20260911 */\n\n"
    )
    if "AURORA_GPSP_GBA_V13_STAGE_SAFE_PERF_20260911" not in s:
        if fb_anchor not in s:
            raise SystemExit("gpSP framebuffer-init anchor missing")
        s = s.replace(fb_anchor, fb_init + fb_anchor, 1)

        # Frame mixing is ON by default; start history/output at black,
        # not 0xFFFF (white), so the first blended frame cannot flash white.
        proc = "      memset(gba_processed_pixels, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);"
        prev = "         memset(gba_screen_pixels_prev, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);"
        if proc not in s or prev not in s:
            raise SystemExit("gpSP post-process init anchors missing")
        s = s.replace(proc,
                      "      memset(gba_processed_pixels, 0, GBA_SCREEN_BUFFER_SIZE); " +
                      "/* AURORA_GPSP_GBA_V13_STAGE_SAFE_PERF_20260911 */", 1)
        s = s.replace(prev,
                      "         memset(gba_screen_pixels_prev, 0, GBA_SCREEN_BUFFER_SIZE); " +
                      "/* AURORA_GPSP_GBA_V13_STAGE_SAFE_PERF_20260911 */", 1)
        lr.write_text(s, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_V13_STAGE_SAFE_PERF_20260911
    # ROM_BUFFER_SIZE is a resident LRU cache, not a ROM-size limit. Keep
    # 4 MiB on PS2 and let gpSP page larger carts in 32 KiB blocks from the
    # still-open ROM file. The extra 4 MiB of headroom matters on the 32 MiB
    # EE, especially for TFA titles and transition-heavy games.
    mfps2 = stage / "Makefile"
    ms = mfps2.read_text(encoding="utf-8")
    cache_old = "-DPS2 -DUSE_XBGR1555_FORMAT -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=16"
    cache_new = "-DPS2 -DUSE_XBGR1555_FORMAT -DSMALL_TRANSLATION_CACHE -DROM_BUFFER_SIZE=4"
    if cache_new not in ms:
        if cache_old not in ms:
            raise SystemExit("gpSP PS2 ROM cache anchor missing")
        ms = ms.replace(cache_old, cache_new, 1)
        mfps2.write_text(ms, encoding="utf-8", newline="\n")

    # AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_LAZY_ALLOC_STAGE
    # gpSP's MIPS translation caches used to be emitted as permanent .bss in
    # the final Aurora ELF. That reserves 2 MiB + 384 KiB even while MD/PCE
    # are running. Enable gpSP's existing lazy MMAP_JIT_CACHE lifetime on PS2:
    # allocate at retro_init(), release at retro_deinit(). Cache sizes and the
    # dynarec itself stay unchanged while GBA is active.
    ms = mfps2.read_text(encoding="utf-8")
    mmap_old = ("\tHAVE_DYNAREC = 1\n"
                "\tCPU_ARCH := mips\n"
                "\tSTATIC_LINKING = 1\n")
    mmap_new = ("\tHAVE_DYNAREC = 1\n"
                "\tMMAP_JIT_CACHE = 1 # AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_LAZY_ALLOC_STAGE\n"
                "\tCPU_ARCH := mips\n"
                "\tSTATIC_LINKING = 1\n")
    if mmap_new not in ms:
        if mmap_old not in ms:
            raise SystemExit("gpSP PS2 MMAP_JIT_CACHE anchor missing")
        ms = ms.replace(mmap_old, mmap_new, 1)
        mfps2.write_text(ms, encoding="utf-8", newline="\n")

    mm = stage / "memmap.c"
    mms = mm.read_text(encoding="utf-8")

    # Upstream's dormant MIPS mmap branch has a stale third parameter in the
    # validator declaration although _VALIDATE_BLOCK_FN supplies two. It was
    # invisible while PS2 used static .bss; make it compile before enabling it.
    sig_old = "bool validate_addr_section_mips(void *ptr, unsigned size, unsigned max_offset_mb) {"
    sig_new = "bool validate_addr_section_mips(void *ptr, unsigned size) { /* AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_LAZY_ALLOC_STAGE */"
    if sig_new not in mms:
        if sig_old not in mms:
            raise SystemExit("gpSP MIPS JIT validator anchor missing")
        mms = mms.replace(sig_old, sig_new, 1)

    ps2_backend_mark = "AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_LAZY_ALLOC_STAGE_PS2_ALLOC"
    if ps2_backend_mark not in mms:
        posix_anchor = "\n#else\n\n\t#include <sys/mman.h>\n"
        ps2_backend = r'''
#elif defined(PS2)

    /* AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_LAZY_ALLOC_STAGE_PS2_ALLOC
     * EE RAM is executable; cache coherency is handled by platform_cache_sync.
     * Keep one 64-byte-aligned contiguous block so the existing gpSP split
     * (ROM cache followed by RAM cache) remains unchanged. */
    #include <malloc.h>
    #include <stdlib.h>

    void *map_jit_block(unsigned size) {
        void *p = memalign(64, size);
        if (!p)
            return 0;
        if (!_VALIDATE_BLOCK_FN(p, size)) {
            free(p);
            return 0;
        }
        return p;
    }

    void unmap_jit_block(void *bufptr, unsigned size) {
        (void)size;
        free(bufptr);
    }

#else

	#include <sys/mman.h>
'''
        if mms.count(posix_anchor) != 1:
            raise SystemExit("gpSP memmap POSIX backend anchor missing/ambiguous")
        mms = mms.replace(posix_anchor, ps2_backend, 1)

    mm.write_text(mms, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_V13_SHOULDER_TURBO_CORE_20260911
    # Aurora maps the two physical PS2 chords to private virtual L3/R3 signals.
    # Consume those here and pulse the real GBA L/R bits with the same turbo
    # period/pulse-width engine already used by Turbo A/B. The new counters
    # are part of gpSP's input savestate so pulse phase survives save/load.
    inp = stage / "input.c"
    ins = inp.read_text(encoding="utf-8")
    turbo_core_mark = "AURORA_GPSP_GBA_V13_SHOULDER_TURBO_CORE_20260911"
    if turbo_core_mark not in ins:
        old = "unsigned turbo_b_counter   = 0;\n"
        new = (old +
               "static unsigned aurora_turbo_l_counter = 0; /* " + turbo_core_mark + " */\n" +
               "static unsigned aurora_turbo_r_counter = 0;\n")
        if old not in ins:
            raise SystemExit("gpSP turbo-counter anchor missing")
        ins = ins.replace(old, new, 1)

        old = "   bool turbo_a     = false;\n   bool turbo_b     = false;\n"
        new = (old +
               "   bool aurora_turbo_l = false; /* " + turbo_core_mark + " */\n" +
               "   bool aurora_turbo_r = false;\n")
        if old not in ins:
            raise SystemExit("gpSP turbo-local anchor missing")
        ins = ins.replace(old, new, 1)

        old = ("      turbo_a = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_X));\n"
               "      turbo_b = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y));\n")
        new = (old +
               "      aurora_turbo_l = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_L3));\n" +
               "      aurora_turbo_r = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R3));\n")
        if old not in ins:
            raise SystemExit("gpSP bitmask turbo-input anchor missing")
        ins = ins.replace(old, new, 1)

        old = ("      turbo_a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);\n"
               "      turbo_b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);\n")
        new = (old +
               "      aurora_turbo_l = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);\n" +
               "      aurora_turbo_r = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);\n")
        if old not in ins:
            raise SystemExit("gpSP scalar turbo-input anchor missing")
        ins = ins.replace(old, new, 1)

        old = "   else\n      turbo_b_counter = 0;\n\n   // GBP keypad detection hack"
        new = """   else
      turbo_b_counter = 0;

   /* AURORA_GPSP_GBA_V13_SHOULDER_TURBO_CORE_20260911
    * A turbo request owns its shoulder while active. For L this suppresses
    * the continuous L1->GBA-L bit before applying the pulse. R is treated the
    * same way if R1 happens to be held with the R2+L2 turbo chord. */
   if (aurora_turbo_l)
   {
      new_key &= ~BUTTON_L;
      new_key |= (aurora_turbo_l_counter < turbo_pulse_width) ? BUTTON_L : 0;
      if (++aurora_turbo_l_counter >= turbo_period)
         aurora_turbo_l_counter = 0;
   }
   else
      aurora_turbo_l_counter = 0;

   if (aurora_turbo_r)
   {
      new_key &= ~BUTTON_R;
      new_key |= (aurora_turbo_r_counter < turbo_pulse_width) ? BUTTON_R : 0;
      if (++aurora_turbo_r_counter >= turbo_period)
         aurora_turbo_r_counter = 0;
   }
   else
      aurora_turbo_r_counter = 0;

   // GBP keypad detection hack"""
        if old not in ins:
            raise SystemExit("gpSP turbo-handler insertion anchor missing")
        ins = ins.replace(old, new, 1)

        old = ("  turbo_a_counter   = bson_read_int32(p, \"turbo-a\", &v) ? v : 0;\n"
               "  turbo_b_counter   = bson_read_int32(p, \"turbo-b\", &v) ? v : 0;\n"
               "  gbp_keypad_frames = bson_read_int32(p, \"gbp-frames\", &v) ? v : 0;\n")
        new = ("  turbo_a_counter   = bson_read_int32(p, \"turbo-a\", &v) ? v : 0;\n"
               "  turbo_b_counter   = bson_read_int32(p, \"turbo-b\", &v) ? v : 0;\n"
               "  aurora_turbo_l_counter = bson_read_int32(p, \"turbo-l\", &v) ? v : 0; /* " + turbo_core_mark + " */\n"
               "  aurora_turbo_r_counter = bson_read_int32(p, \"turbo-r\", &v) ? v : 0;\n"
               "  gbp_keypad_frames = bson_read_int32(p, \"gbp-frames\", &v) ? v : 0;\n")
        if old not in ins:
            raise SystemExit("gpSP turbo state-read anchor missing")
        ins = ins.replace(old, new, 1)

        old = ("  bson_write_int32(dst, \"turbo-a\", turbo_a_counter);\n"
               "  bson_write_int32(dst, \"turbo-b\", turbo_b_counter);\n"
               "  bson_write_int32(dst, \"gbp-frames\", gbp_keypad_frames);\n")
        new = ("  bson_write_int32(dst, \"turbo-a\", turbo_a_counter);\n"
               "  bson_write_int32(dst, \"turbo-b\", turbo_b_counter);\n"
               "  bson_write_int32(dst, \"turbo-l\", aurora_turbo_l_counter); /* " + turbo_core_mark + " */\n"
               "  bson_write_int32(dst, \"turbo-r\", aurora_turbo_r_counter);\n"
               "  bson_write_int32(dst, \"gbp-frames\", gbp_keypad_frames);\n")
        if old not in ins:
            raise SystemExit("gpSP turbo state-write anchor missing")
        ins = ins.replace(old, new, 1)
        inp.write_text(ins, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_V13_CORE_SAFE_PERF_20260911
    # Safe PS2 performance pass. These changes do not alter GBA timing,
    # serial/TFA semantics, save data, RTC or frame count.

    # 1) V21 memory recovery. These MIPS JIT buffers live in the linked ELF
    # even while another emulator core is active. Keep PS2 at upstream SMALL:
    # 2 MiB ROM + 384 KiB RAM. V13's 4 MiB + 512 KiB permanently consumed
    # another 2176 KiB and matches the cross-core "not enough memory" regression.
    # Prefer occasional gpSP JIT turnover over starving GBC/MD/PCE/CD loaders.
    cfg = stage / "gpsp_config.h"
    cs = cfg.read_text(encoding="utf-8")
    jit_old = ("#if defined(SMALL_TRANSLATION_CACHE)\n"
               "  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)\n"
               "  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)\n"
               "#else\n"
               "  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)\n"
               "  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)\n"
               "#endif")
    jit_new = ("/* AURORA_GPSP_GBA_V13_CORE_SAFE_PERF_20260911 */\n"
               "#if defined(PS2)\n"
               "  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)\n"
               "  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)\n"
               "#elif defined(SMALL_TRANSLATION_CACHE)\n"
               "  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)\n"
               "  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)\n"
               "#else\n"
               "  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)\n"
               "  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)\n"
               "#endif")
    if "AURORA_GPSP_GBA_V13_CORE_SAFE_PERF_20260911" not in cs:
        if jit_old not in cs:
            raise SystemExit("gpSP JIT cache-size anchor missing")
        cs = cs.replace(jit_old, jit_new, 1)
        cfg.write_text(cs, encoding="utf-8", newline="\n")

    # 2) V21 dynarec correctness. The MIPS emitter can backpatch code that was
    # emitted before the current [baseaddr,endptr) tail. A range-only D-cache
    # writeback can therefore leave a modified older cache line dirty while the
    # full I-cache invalidation exposes stale memory. Preserve upstream PS2's
    # full D-cache writeback + full I-cache invalidate.
    cpu = stage / "cpu_threaded.c"
    cps = cpu.read_text(encoding="utf-8")
    sync_old = ("#elif defined(PS2)\n"
                "  void platform_cache_sync(void *baseaddr, void *endptr) {\n"
                "    FlushCache(0);   // Dcache flush\n"
                "    FlushCache(2);   // Icache invalidate\n"
                "  }")
    sync_new = ("#elif defined(PS2)\n"
                "  void platform_cache_sync(void *baseaddr, void *endptr) {\n"
                "    /* AURORA_GPSP_GBA_V21_DYNAREC_FULL_CACHE_SYNC_20260912 */\n"
                "    (void)baseaddr;\n"
                "    (void)endptr;\n"
                "    FlushCache(0);   /* full D-cache writeback */\n"
                "    FlushCache(2);   /* full I-cache invalidate */\n"
                "  }")
    if sync_new not in cps:
        if sync_old not in cps:
            raise SystemExit("gpSP PS2 dynarec cache-sync anchor missing")
        cps = cps.replace(sync_old, sync_new, 1)
        cpu.write_text(cps, encoding="utf-8", newline="\n")

    # 3) V2's scalar XBGR1555 blend is correct. For PS2 only, average two
    # adjacent 16-bit pixels in one 32-bit operation. 0x7BDE clears each
    # 5-bit channel LSB and bit15, so there is no carry/borrow between channels
    # or between the two packed pixels. The expression is bit-exact to V2's
    # (a+b+((a^b)&0x0421))>>1 result. Constant-size memcpy becomes an lw/sw
    # under the PS2 -O3 build while avoiding strict-aliasing UB in the C source.
    lr = stage / "libretro/libretro.c"
    ls = lr.read_text(encoding="utf-8")
    mix_old = ("   for (i = 0; i < npx; i++)\n"
               "   {\n"
               "      uint16_t rgb_curr = src_curr[i];\n"
               "      uint16_t rgb_prev = src_prev[i];\n"
               "\n"
               "      /* Mix colours:\n"
               "       *   http://blargg.8bitalley.com/info/rgb_mixing.html */\n"
               "      dst[i] = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & AURORA_GPSP_FRAME_MIX_LSB_MASK)) >> 1;\n"
               "   }")
    mix_new = ("#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n"
               "   /* AURORA_GPSP_GBA_V13_CORE_SAFE_PERF_20260911 */\n"
               "   for (i = 0; i + 1 < npx; i += 2)\n"
               "   {\n"
               "      uint32_t rgb_curr, rgb_prev, rgb_mix;\n"
               "      memcpy(&rgb_curr, src_curr + i, sizeof(rgb_curr));\n"
               "      memcpy(&rgb_prev, src_prev + i, sizeof(rgb_prev));\n"
               "      rgb_mix = (rgb_curr | rgb_prev) -\n"
               "         (((rgb_curr ^ rgb_prev) & 0x7BDE7BDEU) >> 1);\n"
               "      memcpy(dst + i, &rgb_mix, sizeof(rgb_mix));\n"
               "   }\n"
               "   if (i < npx)\n"
               "   {\n"
               "      uint16_t rgb_curr = src_curr[i];\n"
               "      uint16_t rgb_prev = src_prev[i];\n"
               "      dst[i] = (rgb_curr + rgb_prev +\n"
               "         ((rgb_curr ^ rgb_prev) & AURORA_GPSP_FRAME_MIX_LSB_MASK)) >> 1;\n"
               "   }\n"
               "#else\n"
               "   for (i = 0; i < npx; i++)\n"
               "   {\n"
               "      uint16_t rgb_curr = src_curr[i];\n"
               "      uint16_t rgb_prev = src_prev[i];\n"
               "      dst[i] = (rgb_curr + rgb_prev +\n"
               "         ((rgb_curr ^ rgb_prev) & AURORA_GPSP_FRAME_MIX_LSB_MASK)) >> 1;\n"
               "   }\n"
               "#endif")
    if "0x7BDE7BDEU" not in ls:
        if mix_old not in ls:
            raise SystemExit("gpSP scalar frame-mix loop anchor missing")
        ls = ls.replace(mix_old, mix_new, 1)
        lr.write_text(ls, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_V13_POSTFX_SAFE_FRAMESKIP_CC_20260911
    # Safe Frameskip + interframe-blending correctness and PS2 colour correction.
    lr = stage / "libretro/libretro.c"
    ls = lr.read_text(encoding="utf-8")

    # 1) Export a tiny Aurora hook. The archive namespace pass turns this into
    # GPSP_aurora_set_frontend_skip(), so the PS2 adapter can map Aurora's
    # one-shot NULL render target into gpSP's native renderer skip while still
    # running CPU, timers and audio in retro_run().
    fs_mark = "AURORA_GPSP_GBA_V13_SAFE_FRAMESKIP_BRIDGE_20260911"
    if fs_mark not in ls:
        old = "u32 skip_next_frame                          = 0;\n"
        new = (old +
               "static u32 aurora_frontend_skip = 0; /* " + fs_mark + " */\n" +
               "void aurora_set_frontend_skip(unsigned skip)\n" +
               "{\n" +
               "   /* Latch this separately: retro_run() resets skip_next_frame\n" +
               "    * before evaluating the core's own frameskip policy. */\n" +
               "   aurora_frontend_skip = skip ? 1U : 0U;\n" +
               "}\n")
        if old not in ls:
            raise SystemExit("gpSP skip_next_frame anchor missing")
        ls = ls.replace(old, new, 1)

        # retro_run() unconditionally clears skip_next_frame before its native
        # frameskip switch. Consume Aurora's one-shot request only AFTER that
        # switch, but before CPU execution begins. OR semantics also remain
        # correct if native gpSP frameskip is ever enabled in the future.
        run_anchor = ("         default:\n"
                      "            skip_next_frame = 0;\n"
                      "            break;\n"
                      "      }\n"
                      "   }\n\n"
                      "   /* If frameskip settings have changed, update\n")
        run_repl = ("         default:\n"
                    "            skip_next_frame = 0;\n"
                    "            break;\n"
                    "      }\n"
                    "   }\n\n"
                    "   if (aurora_frontend_skip)\n"
                    "      skip_next_frame = 1; /* " + fs_mark + " */\n"
                    "   aurora_frontend_skip = 0; /* one-shot */\n\n"
                    "   /* If frameskip settings have changed, update\n")
        if run_anchor not in ls:
            raise SystemExit("gpSP retro_run frameskip decision anchor missing")
        ls = ls.replace(run_anchor, run_repl, 1)

    # 2) Interframe blending history must represent adjacent rendered frames.
    # A skipped gpSP frame does not render scanlines at all. Mark history stale;
    # on the next visible frame seed previous=current before post-processing.
    # mix(current,current) == current; the existing pointer swap then makes the
    # just-rendered frame the new history for subsequent visible frames.
    hist_mark = "AURORA_GPSP_GBA_V13_FRAMESKIP_BLEND_RESYNC_20260911"
    if hist_mark not in ls:
        old = "static bool post_process_mix = false;\n"
        new = old + "static bool aurora_mix_history_valid = false; /* " + hist_mark + " */\n"
        if old not in ls:
            raise SystemExit("gpSP post-process history anchor missing")
        ls = ls.replace(old, new, 1)

        old = "static void init_post_processing(void)\n{\n   video_post_process = NULL;\n"
        new = ("static void init_post_processing(void)\n{\n"
               "   video_post_process = NULL;\n"
               "   aurora_mix_history_valid = false; /* " + hist_mark + " */\n")
        if old not in ls:
            raise SystemExit("gpSP init_post_processing anchor missing")
        ls = ls.replace(old, new, 1)

        old = ("   if (skip_next_frame)\n"
               "   {\n"
               "      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,\n"
               "            GBA_SCREEN_PITCH * 2);\n"
               "      return;\n"
               "   }\n")
        new = ("   if (skip_next_frame)\n"
               "   {\n"
               "      if (post_process_mix)\n"
               "         aurora_mix_history_valid = false; /* " + hist_mark + " */\n"
               "      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,\n"
               "            GBA_SCREEN_PITCH * 2);\n"
               "      return;\n"
               "   }\n")
        if old not in ls:
            raise SystemExit("gpSP video_run skip anchor missing")
        ls = ls.replace(old, new, 1)

        old = ("   if (video_post_process)\n"
               "   {\n"
               "      video_post_process();\n"
               "      gba_screen_pixels_buf = gba_processed_pixels;\n"
               "   }\n")
        new = ("   if (video_post_process)\n"
               "   {\n"
               "      if (post_process_mix && !aurora_mix_history_valid &&\n"
               "          gba_screen_pixels_prev)\n"
               "      {\n"
               "         memcpy(gba_screen_pixels_prev, gba_screen_pixels,\n"
               "                GBA_SCREEN_BUFFER_SIZE); /* " + hist_mark + " */\n"
               "         aurora_mix_history_valid = true;\n"
               "      }\n"
               "      video_post_process();\n"
               "      gba_screen_pixels_buf = gba_processed_pixels;\n"
               "   }\n")
        if old not in ls:
            raise SystemExit("gpSP video_run post-process anchor missing")
        ls = ls.replace(old, new, 1)

        # Reset and savestate restore jump to a non-adjacent video timeline.
        # The framebuffer/history is not part of the gpSP savestate, so never
        # blend the first post-reset/post-restore frame with stale pixels.
        reset_old = ("void retro_reset(void)\n"
                     "{\n"
                     "   reset_gba();\n"
                     "}\n")
        reset_new = ("void retro_reset(void)\n"
                     "{\n"
                     "   reset_gba();\n"
                     "   aurora_mix_history_valid = false; /* " + hist_mark + " */\n"
                     "}\n")
        if reset_old not in ls:
            raise SystemExit("gpSP retro_reset blend-history anchor missing")
        ls = ls.replace(reset_old, reset_new, 1)

        unser_old = ("bool retro_unserialize(const void* data, size_t size)\n"
                     "{\n"
                     "   if (size != GBA_STATE_MEM_SIZE)\n"
                     "      return false;\n\n"
                     "   return gba_load_state(data);\n"
                     "}\n")
        unser_new = ("bool retro_unserialize(const void* data, size_t size)\n"
                     "{\n"
                     "   bool ok;\n"
                     "   if (size != GBA_STATE_MEM_SIZE)\n"
                     "      return false;\n\n"
                     "   ok = gba_load_state(data);\n"
                     "   if (ok)\n"
                     "      aurora_mix_history_valid = false; /* " + hist_mark + " */\n"
                     "   return ok;\n"
                     "}\n")
        if unser_old not in ls:
            raise SystemExit("gpSP retro_unserialize blend-history anchor missing")
        ls = ls.replace(unser_old, unser_new, 1)

    # 3) gpSP's generated LUT uses an RGB555 index (R high, B low) and
    # RGB565 output. PS2 USE_XBGR1555_FORMAT keeps the native GBA/PS2 0BGR555
    # framebuffer (B high, R low). Reorder the entire LUT offline below so a
    # native BGR555 pixel can remain a direct array index and the result is
    # already BGR555 too: zero extra channel-swaps in the EE hot loop.
    cc_mark = "AURORA_GPSP_GBA_V13_PS2_COLOR_CORRECTION_20260911"
    if cc_mark not in ls:
        old = "static void video_post_process_cc(void)\n"
        helper = ("/* " + cc_mark + " */\n"
                  "static inline uint16_t aurora_gpsp_color_correct(uint16_t c)\n"
                  "{\n"
                  "#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n"
                  "   return gba_cc_lut[c & 0x7FFFU];\n"
                  "#else\n"
                  "   return gba_cc_lut[((c & 0xFFC0U) >> 1) | (c & 0x1FU)];\n"
                  "#endif\n"
                  "}\n\n" + old)
        if old not in ls:
            raise SystemExit("gpSP colour-correction function anchor missing")
        ls = ls.replace(old, helper, 1)

        old1 = "      dst[i] = gba_cc_lut[((src_color & 0xFFC0) >> 1) | (src_color & 0x1F)];"
        new1 = "      dst[i] = aurora_gpsp_color_correct(src_color);"
        old2 = "      dst[i] = gba_cc_lut[((rgb_mix & 0xFFC0) >> 1) | (rgb_mix & 0x1F)];"
        new2 = "      dst[i] = aurora_gpsp_color_correct(rgb_mix);"
        if old1 not in ls or old2 not in ls:
            raise SystemExit("gpSP colour-correction LUT index anchors missing")
        ls = ls.replace(old1, new1, 1).replace(old2, new2, 1)

        # Color Correction + Interframe Blending is the default V13 path.
        # Keep the V8-derived two-pixel PS2 blend there too; only the cheap average
        # is packed, while each resulting pixel still goes through the exact
        # gpSP colour-correction LUT independently.
        cc_mix_old = ("   for (i = 0; i < npx; i++)\n"
                      "   {\n"
                      "      uint16_t rgb_curr = src_curr[i];\n"
                      "      uint16_t rgb_prev = src_prev[i];\n"
                      "\n"
                      "      uint16_t rgb_mix  = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & AURORA_GPSP_FRAME_MIX_LSB_MASK)) >> 1;\n"
                      "\n"
                      "      /* Convert colour to RGB555 and perform lookup */\n"
                      "      dst[i] = aurora_gpsp_color_correct(rgb_mix);\n"
                      "   }")
        cc_mix_new = ("#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n"
                      "   /* AURORA_GPSP_GBA_V13_PS2_CC_MIX_PACKED_20260911 */\n"
                      "   for (i = 0; i + 1 < npx; i += 2)\n"
                      "   {\n"
                      "      uint32_t rgb_curr, rgb_prev, rgb_mix;\n"
                      "      memcpy(&rgb_curr, src_curr + i, sizeof(rgb_curr));\n"
                      "      memcpy(&rgb_prev, src_prev + i, sizeof(rgb_prev));\n"
                      "      rgb_mix = (rgb_curr | rgb_prev) -\n"
                      "         (((rgb_curr ^ rgb_prev) & 0x7BDE7BDEU) >> 1);\n"
                      "      dst[i + 0] = aurora_gpsp_color_correct((uint16_t)(rgb_mix & 0xFFFFU));\n"
                      "      dst[i + 1] = aurora_gpsp_color_correct((uint16_t)(rgb_mix >> 16));\n"
                      "   }\n"
                      "   if (i < npx)\n"
                      "   {\n"
                      "      uint16_t rgb_curr = src_curr[i];\n"
                      "      uint16_t rgb_prev = src_prev[i];\n"
                      "      uint16_t rgb_mix = (rgb_curr + rgb_prev +\n"
                      "         ((rgb_curr ^ rgb_prev) & AURORA_GPSP_FRAME_MIX_LSB_MASK)) >> 1;\n"
                      "      dst[i] = aurora_gpsp_color_correct(rgb_mix);\n"
                      "   }\n"
                      "#else\n" + cc_mix_old + "\n#endif")
        if cc_mix_old not in ls:
            raise SystemExit("gpSP colour-correction+mix loop anchor missing")
        ls = ls.replace(cc_mix_old, cc_mix_new, 1)

    lr.write_text(ls, encoding="utf-8", newline="\n")

    lut = stage / "gba_cc_lut.c"
    luts = lut.read_text(encoding="utf-8")
    if "AURORA_GPSP_GBA_V13_PS2_COLOR_CORRECTION_20260911" not in luts:
        head = "const u16 gba_cc_lut[] = {"
        if head not in luts:
            raise SystemExit("gpSP colour-correction LUT array anchor missing")
        prefix, body = luts.split(head, 1)
        arr, suffix = body.split("};", 1)
        values = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{4})", arr)]
        if len(values) != 32768:
            raise SystemExit("gpSP colour-correction LUT expected 32768 entries, got %u" % len(values))

        # Upstream table: index = RGB555 (R10:G5:B0), value = RGB565.
        # PS2 renderer:       pixel = BGR555 (B10:G5:R0).
        # Build a table whose ARRAY POSITION is native BGR555 and whose VALUE
        # is native BGR555. This corrects both directions once at build time.
        native = [0] * 32768
        for bgr in range(32768):
            r = bgr & 31
            g = (bgr >> 5) & 31
            b = (bgr >> 10) & 31
            rgb_index = (r << 10) | (g << 5) | b
            v = values[rgb_index]
            rr = (v >> 11) & 31
            gg = (v >> 6) & 31
            bb = v & 31
            native[bgr] = (bb << 10) | (gg << 5) | rr

        lines = []
        for i in range(0, 32768, 5):
            chunk = native[i:i + 5]
            text = ",".join(" 0x%04x" % v for v in chunk)
            if i + len(chunk) < 32768:
                text += ","
            lines.append(text)
        arr = "\n" + "\n".join(lines) + "\n"
        luts = (prefix + "/* AURORA_GPSP_GBA_V13_PS2_COLOR_CORRECTION_20260911: "
                "offline RGB555/RGB565 -> native BGR555-index/native BGR555-output permutation. */\n" +
                head + arr + "};" + suffix)
        lut.write_text(luts, encoding="utf-8", newline="\n")


    # AURORA_GPSP_GBA_V14_SPRITE_PERF_20260911
    # Renderer-only optimization pass.  The cache is refreshed exclusively
    # inside order_obj(), i.e. at the exact same OAM_UPDATED boundary gpSP
    # already uses to rebuild per-scanline OBJ lists.  No GBA timing, sprite
    # limits, window semantics, blending rules or dynarec behavior are changed.
    vid = stage / "video.cc"
    vs = vid.read_text(encoding="utf-8")
    v14_mark = "AURORA_GPSP_GBA_V14_SPRITE_PERF_20260911"

    def v14_replace(text, old, new, label, count=1):
        if old not in text:
            raise SystemExit("gpSP V14 anchor missing (%s)" % label)
        return text.replace(old, new, count)

    def v14_region(text, start_mark, end_mark, replacement, label):
        a = text.find(start_mark)
        if a < 0:
            raise SystemExit("gpSP V14 region start missing (%s)" % label)
        b = text.find(end_mark, a)
        if b < 0:
            raise SystemExit("gpSP V14 region end missing (%s)" % label)
        if text.find(start_mark, a + len(start_mark)) >= 0:
            raise SystemExit("gpSP V14 region start is not unique (%s)" % label)
        return text[:a] + replacement + "\n\n" + text[b:]

    if v14_mark not in vs:
        # 1) Compact decoded-OBJ cache.  obj_w/obj_h remain the *base* GBA
        # dimensions; affine double-size is still applied exactly where the
        # original renderer applied it.  Affine matrix words live in OAM too,
        # therefore the existing OAM_UPDATED contract also makes caching them
        # safe (all CPU/DMA writes to OAM set that flag).
        old = """typedef struct {
  s32 obj_x, obj_y;
  s32 obj_w, obj_h;
  u32 attr1, attr2;
  bool is_double;
} t_sprite;
"""
        new = """typedef struct {
  s16 obj_x, obj_y;
  s16 aff_dx, aff_dy, aff_dmx, aff_dmy;
  u16 attr0, attr1, attr2;
  u8 obj_w, obj_h;
} t_sprite;

/* AURORA_GPSP_GBA_V14_SPRITE_PERF_20260911
 * Decoded once whenever order_obj() observes OAM_UPDATED, then reused by
 * every scanline.  20 bytes x 128 = 2.5 KiB, avoiding repeated endian swaps,
 * shape/size table lookups, X sign-extension and affine-matrix loads. */
static t_sprite obj_cache[128];
"""
        vs = v14_replace(vs, old, new, "compact OBJ cache type")

        # Small hot-loop cleanup: make palette invariance explicit.  GCC often
        # hoists this already, but doing it in source guarantees the sprite
        # inner loops contain no repeated palette-base formation.
        old = """  } else {
    // Only 32 bits (8 pixels * 4 bits)
    for (u32 i = start; i < end; i++, dest_ptr++) {
      u32 selb = hflip ? (3-i/2) : i/2;
      u32 seln = hflip ? ((i & 1) ^ 1) : (i & 1);
      u8 pval = (tile_ptr[selb] >> (seln * 4)) & 0xF;
      const u16 *subpal = &pal[palette];
"""
        new = """  } else {
    // Only 32 bits (8 pixels * 4 bits)
    const u16 *subpal = &pal[palette];
    for (u32 i = start; i < end; i++, dest_ptr++) {
      u32 selb = hflip ? (3-i/2) : i/2;
      u32 seln = hflip ? ((i & 1) ^ 1) : (i & 1);
      u8 pval = (tile_ptr[selb] >> (seln * 4)) & 0xF;
"""
        vs = v14_replace(vs, old, new, "partial OBJ tile subpalette")

        old = """  } else {
    u32 tilepix = eswap32(*(u32*)tile_ptr);
    if (tilepix) {   // Can skip all pixels if the row is just transparent
      for (u32 i = 0; i < 8; i++, dest_ptr++) {
        u8 pval = (hflip ? (tilepix >> ((7-i)*4)) : (tilepix >> (i*4))) & 0xF;
        const u16 *subpal = &pal[palette];
"""
        new = """  } else {
    u32 tilepix = eswap32(*(u32*)tile_ptr);
    const u16 *subpal = &pal[palette];
    if (tilepix) {   // Can skip all pixels if the row is just transparent
      for (u32 i = 0; i < 8; i++, dest_ptr++) {
        u8 pval = (hflip ? (tilepix >> ((7-i)*4)) : (tilepix >> (i*4))) & 0xF;
"""
        vs = v14_replace(vs, old, new, "full OBJ tile subpalette")

        old = """  u32 px_attr = px_comb | palette | 0x100;  // Combine flags + high palette bit

  u8 pval = 0;
  u32 mctr = 0;
"""
        new = """  u32 px_attr = px_comb | palette | 0x100;  // Combine flags + high palette bit
  const u16 *subpal = &pal[palette];

  u8 pval = 0;
  u32 mctr = 0;
"""
        vs = v14_replace(vs, old, new, "mosaic OBJ subpalette", 1)
        old = """    // Write the pixel value as required
    const u16 *subpal = &pal[palette];
    if (pval) {
"""
        new = """    // Write the pixel value as required
    if (pval) {
"""
        vs = v14_replace(vs, old, new, "mosaic OBJ inner subpalette", 1)

        # 2) Affine OBJ: preserve the full rotation path, but the dy==0
        # specialization has a scanline-constant source Y.  Hoist Y tile-row
        # address formation out of the pixel loop and consume affine values
        # already decoded by order_obj().
        affine_start = """template <typename stype, rendtype rdtype, bool mosaic, bool is8bpp, bool rotate>
static void render_affine_object("""
        affine_end = "// Renders a single sprite on the current scanline."
        affine_new = r"""template <typename stype, rendtype rdtype, bool mosaic, bool is8bpp, bool rotate>
static void render_affine_object(
  const t_sprite *obji,
  u32 start, u32 end, stype *dst_ptr, u32 mosv, u32 mosh,
  u32 base_tile, u32 pxcomb, u16 palette, const u16 *palptr,
  s32 vcount, bool obj1dmap
) {
  // Tile size in bytes for each mode
  const u32 tile_bsize = is8bpp ? tile_size_8bpp : tile_size_4bpp;
  const u32 tile_bwidth = is8bpp ? tile_width_8bpp : tile_width_4bpp;

  // Affine parameters are decoded once from OAM by order_obj().
  const s32 dx  = obji->aff_dx;
  const s32 dy  = obji->aff_dy;
  const s32 dmx = obji->aff_dmx;
  const s32 dmy = obji->aff_dmy;
  const bool is_double = (obji->attr0 & 0x0200) != 0;

  // Object dimensions and boundaries
  const u32 obj_dimw = obji->obj_w;
  const u32 obj_dimh = obji->obj_h;
  s32 middle_x = is_double ? obji->obj_w : (obji->obj_w / 2);
  const s32 middle_y = is_double ? obji->obj_h : (obji->obj_h / 2);
  const s32 obj_width  = is_double ? obji->obj_w * 2 : obji->obj_w;
  const s32 obj_height = is_double ? obji->obj_h * 2 : obji->obj_h;

  if (mosaic)
    vcount -= vcount % mosv;
  const s32 y_delta = vcount - (obji->obj_y + middle_y);

  if (obji->obj_x < (signed)start)
    middle_x -= (start - obji->obj_x);
  s32 source_x = (obj_dimw << 7) + (y_delta * dmx) - (middle_x * dx);
  s32 source_y = (obj_dimh << 7) + (y_delta * dmy) - (middle_x * dy);

  // Retain upstream's early rejection.
  if (!rotate && ((u32)(source_y >> 8)) >= (u32)obj_height)
    return;

  const u32 d_start = MAX((signed)start, obji->obj_x);
  const u32 d_end   = MIN((signed)end,   obji->obj_x + obj_width);
  u32 cnt = d_end - d_start;
  dst_ptr += d_start;

  const u32 tile_pitch = obj1dmap ? (obj_dimw / 8) * tile_bsize : 1024;
  const u32 px_attr = pxcomb | palette | 0x100;

  // In the no-rotation specialization source_y never changes.  The original
  // loop therefore recomputed an identical tile-row term for every pixel.
  u32 fixed_pixel_y = 0;
  u32 fixed_y_tile_off = 0;
  if (!rotate) {
    fixed_pixel_y = (u32)(source_y >> 8);
    // This is equivalent to the original per-pixel source bounds check:
    // with dy==0 an out-of-range Y can never become valid later in the row.
    if (fixed_pixel_y >= obj_dimh)
      return;
    fixed_y_tile_off =
      ((fixed_pixel_y >> 3) * tile_pitch) +
      ((fixed_pixel_y & 0x7) * tile_bwidth);
  }

  // Skip output pixels until their source position enters the sprite.
  while (cnt) {
    const u32 pixel_x = (u32)(source_x >> 8);
    if (!rotate) {
      if (pixel_x < obj_dimw)
        break;
    } else {
      const u32 pixel_y = (u32)(source_y >> 8);
      if (pixel_x < obj_dimw && pixel_y < obj_dimh)
        break;
    }

    dst_ptr++;
    source_x += dx;
    if (rotate)
      source_y += dy;
    cnt--;
  }

  u8 pixval = 0;
  u32 mctr = 0;
  for (u32 i = 0; i < cnt; i++) {
    const u32 pixel_x = (u32)(source_x >> 8);
    const u32 pixel_y = rotate ? (u32)(source_y >> 8) : fixed_pixel_y;

    if (pixel_x >= obj_dimw || (rotate && pixel_y >= obj_dimh))
      return;

    if (!mosaic || !mctr) {
      const u32 y_tile_off = rotate
        ? ((pixel_y >> 3) * tile_pitch) + ((pixel_y & 0x7) * tile_bwidth)
        : fixed_y_tile_off;

      if (is8bpp) {
        const u32 tile_off =
          base_tile +
          y_tile_off +
          ((pixel_x >> 3) * tile_bsize) +
          (pixel_x & 0x7);

        pixval = vram[0x10000 + (tile_off & 0x7FFF)];
      } else {
        const u32 tile_off =
          base_tile +
          y_tile_off +
          ((pixel_x >> 3) * tile_bsize) +
          ((pixel_x >> 1) & 0x3);

        const u8 pixpair = vram[0x10000 + (tile_off & 0x7FFF)];
        pixval = (pixel_x & 1) ? (pixpair >> 4) : (pixpair & 0xF);
      }
      mctr = mosh;
    }
    if (mosaic)
      mctr--;

    if (pixval) {
      if (rdtype == FULLCOLOR)
        *dst_ptr = palptr[pixval | palette];
      else if (rdtype == INDXCOLOR)
        *dst_ptr = pixval | px_attr;
      else if (rdtype == STCKCOLOR) {
        if (*dst_ptr & 0x100)
          *dst_ptr = pixval | px_attr | ((*dst_ptr) & 0xFFFF0000);
        else
          *dst_ptr = pixval | px_attr | ((*dst_ptr) << 16);
      }
      else if (rdtype == PIXCOPY)
        *dst_ptr = dst_ptr[240];
    }

    dst_ptr++;
    source_x += dx;
    if (rotate)
      source_y += dy;
  }
}"""
        vs = v14_region(vs, affine_start, affine_end, affine_new, "affine OBJ renderer")

        # 3) Sprite dispatcher: VCOUNT, DISPCNT OBJ mapping and MOSAIC are
        # scanline invariants.  Receive them from render_scanline_objs instead
        # of rereading global ioreg state once (or more) for every object.
        sprite_start = """template <typename stype, rendtype rdtype, bool is8bpp, bool mosaic>
inline static void render_sprite("""
        sprite_end = "// Renders objects on a scanline for a given priority."
        sprite_new = r"""template <typename stype, rendtype rdtype, bool is8bpp, bool mosaic>
inline static void render_sprite(
  const t_sprite *obji, u32 start, u32 end, stype *scanline,
  u32 pxcomb, const u16* palptr, s32 vcount, bool obj1dmap, u32 mosaic_reg
) {
  const bool is_affine = (obji->attr0 & 0x0100) != 0;
  const u32 msk = is8bpp && !obj1dmap ? 0x3FE : 0x3FF;
  const u32 base_tile = (obji->attr2 & msk) * 32;

  const u32 mosv = (mosaic ? (mosaic_reg >> 12) & 0xF : 0) + 1;
  const u32 mosh = (mosaic ? (mosaic_reg >>  8) & 0xF : 0) + 1;

  // Objects use the higher palette part in 4bpp mode.
  const u16 pal = is8bpp ? 0 : ((obji->attr2 >> 8) & 0xF0);

  if (is_affine) {
    if (obji->aff_dy == 0)
      render_affine_object<stype, rdtype, mosaic, is8bpp, false>(
        obji, start, end, scanline, mosv, mosh,
        base_tile, pxcomb, pal, palptr, vcount, obj1dmap);
    else
      render_affine_object<stype, rdtype, mosaic, is8bpp, true>(
        obji, start, end, scanline, mosv, mosh,
        base_tile, pxcomb, pal, palptr, vcount, obj1dmap);
  } else {
    if (obji->obj_x >= (signed)end || obji->obj_x + obji->obj_w <= (signed)start)
      return;

    const bool hflip = (obji->attr1 & 0x1000) != 0;
    const bool vflip = (obji->attr1 & 0x2000) != 0;

    u32 voffset = vflip ? obji->obj_y + obji->obj_h - vcount - 1
                        : vcount - obji->obj_y;
    if (mosaic)
      voffset -= voffset % mosv;

    const u32 tile_bsize  = is8bpp ? tile_size_8bpp : tile_size_4bpp;
    const u32 tile_bwidth = is8bpp ? tile_width_8bpp : tile_width_4bpp;
    const u32 obj_pitch = obj1dmap ? (obji->obj_w / 8) * tile_bsize : 1024;
    const u32 hflip_off = hflip ? ((obji->obj_w / 8) - 1) * tile_bsize : 0;

    const u32 tile_offset =
      base_tile +
      (voffset / 8) * obj_pitch +
      (voffset % 8) * tile_bwidth +
      hflip_off;

    const s32 obj_x_offset = obji->obj_x - (s32)start;
    const u32 clipped_width =
      obj_x_offset >= 0 ? obji->obj_w : obji->obj_w + obj_x_offset;
    const u32 max_range =
      obj_x_offset >= 0 ? end - obji->obj_x : end - start;
    const u32 max_draw = MIN(max_range, clipped_width);

    if (mosaic && mosh > 1) {
      if (hflip)
        render_object_mosaic<stype, rdtype, is8bpp, true>(
          obj_x_offset, max_draw, &scanline[start], tile_offset,
          mosh, pxcomb, pal, palptr);
      else
        render_object_mosaic<stype, rdtype, is8bpp, false>(
          obj_x_offset, max_draw, &scanline[start], tile_offset,
          mosh, pxcomb, pal, palptr);
    } else {
      if (hflip)
        render_object<stype, rdtype, is8bpp, true>(
          obj_x_offset, max_draw, &scanline[start], tile_offset,
          pxcomb, pal, palptr);
      else
        render_object<stype, rdtype, is8bpp, false>(
          obj_x_offset, max_draw, &scanline[start], tile_offset,
          pxcomb, pal, palptr);
    }
  }
}"""
        vs = v14_region(vs, sprite_start, sprite_end, sprite_new, "OBJ dispatcher")

        # 4) Per-priority scanline pass: consume decoded OBJ metadata directly.
        # color_flags(4), MOSAIC, OBJ mapping and WINOUT do not change while
        # update_scanline() is executing, so read once per pass rather than per
        # sprite.  Window segmentation is deliberately left untouched.
        scan_start = """template <typename stype, rendtype rdtype>
void render_scanline_objs("""
        scan_end = "// Goes through the object list in the OAM"
        scan_new = r"""template <typename stype, rendtype rdtype>
void render_scanline_objs(
  u32 priority, u32 start, u32 end, void *raw_ptr, const u16* palptr
) {
  stype *scanline = (stype*)raw_ptr;
  const s32 vcount = read_ioreg(REG_VCOUNT);
  const u32 obj_color_flags = color_flags(4);
  const u32 mosaic_reg = read_ioreg(REG_MOSAIC);
  const bool obj1dmap = (read_ioreg(REG_DISPCNT) & 0x40) != 0;
  const u32 obj_enable =
    (rdtype == PIXCOPY) ? (read_ioreg(REG_WINOUT) >> 8) : 0;

  const u32 objcnt = obj_priority_count[priority][vcount];
  const u8 *objlist = obj_priority_list[priority][vcount];

  // Render all visible objects for this priority, back to front.
  for (s32 objn = (s32)objcnt - 1; objn >= 0; objn--) {
    const u32 objoff = objlist[objn];
    const t_sprite *obji = &obj_cache[objoff];
    const u16 obj_attr0 = obji->attr0;
    const bool is_affine = (obj_attr0 & 0x0100) != 0;
    const bool is_trans =
      ((obj_attr0 >> 10) & 0x3) == OBJ_MOD_SEMITRAN;
    const bool is_double = (obj_attr0 & 0x0200) != 0;

    const s32 obj_maxw =
      (is_affine && is_double) ? obji->obj_w * 2 : obji->obj_w;

    if (obji->obj_x >= (signed)end ||
        obji->obj_x + obj_maxw <= (signed)start)
      continue;

    const bool forcebld = is_trans && rdtype != FULLCOLOR;

    if (rdtype == PIXCOPY) {
      const u32 sec_start = MAX((signed)start, obji->obj_x);
      const u32 sec_end   = MIN((signed)end, obji->obj_x + obj_maxw);
      u16 *tmp_ptr = (u16*)&scanline[GBA_SCREEN_PITCH];
      render_scanline_conditional(sec_start, sec_end, tmp_ptr, obj_enable);
    }

    const u32 pxcomb = (forcebld ? 0x800 : 0) | obj_color_flags;
    const bool emosaic = (obj_attr0 & 0x1000) != 0;
    const bool is_8bpp = (obj_attr0 & 0x2000) != 0;
    const bool mosaic_active = emosaic && (mosaic_reg & 0xFF00);

    if (mosaic_active) {
      if (is_8bpp)
        render_sprite<stype, rdtype, true, true>(
          obji, start, end, scanline, pxcomb, palptr,
          vcount, obj1dmap, mosaic_reg);
      else
        render_sprite<stype, rdtype, false, true>(
          obji, start, end, scanline, pxcomb, palptr,
          vcount, obj1dmap, mosaic_reg);
    } else {
      if (is_8bpp)
        render_sprite<stype, rdtype, true, false>(
          obji, start, end, scanline, pxcomb, palptr,
          vcount, obj1dmap, mosaic_reg);
      else
        render_sprite<stype, rdtype, false, false>(
          obji, start, end, scanline, pxcomb, palptr,
          vcount, obj1dmap, mosaic_reg);
    }
  }
}"""
        vs = v14_region(vs, scan_start, scan_end, scan_new, "OBJ scanline pass")

        # 5) order_obj remains the sole invalidation/rebuild point, but now it
        # stores the already-decoded values it was computing anyway.  The
        # hardware cycle-limit logic and row lists are intentionally byte-for-
        # byte equivalent in structure to upstream.
        order_start = "static void order_obj(u32 video_mode)\n"
        order_end = "u32 layer_order[16];"
        order_new = r"""static void order_obj(u32 video_mode)
{
  u32 obj_num;
  u32 row;
  t_oam *oam_base = (t_oam*)oam_ram;
  u16 rend_cycles[160];

  const bool hblank_free = read_ioreg(REG_DISPCNT) & 0x20;
  const u16 max_rend_cycles = !sprite_limit ? REND_CYC_MAX :
                               hblank_free  ? REND_CYC_REDUCED :
                                              REND_CYC_SCANLINE;

  memset(obj_priority_count, 0, sizeof(obj_priority_count));
  memset(obj_alpha_count, 0, sizeof(obj_alpha_count));
  memset(rend_cycles, 0, sizeof(rend_cycles));

  for (obj_num = 0; obj_num < 128; obj_num++)
  {
    t_oam *oam_ptr = &oam_base[obj_num];
    const u16 obj_attr0 = eswap16(oam_ptr->attr0);

    // Bit 9 disables regular sprites (that is, non-affine ones).
    if ((obj_attr0 & 0x0300) == 0x0200)
      continue;

    const u16 obj_shape = obj_attr0 >> 14;
    const u32 obj_mode = (obj_attr0 >> 10) & 0x03;

    if ((obj_shape == 0x3) || (obj_mode == OBJ_MOD_INVALID))
      continue;

    const u16 obj_attr2 = eswap16(oam_ptr->attr2);

    // On bitmap modes, objs 0-511 are not usable.
    if ((video_mode >= 3) && (!(obj_attr2 & 0x200)))
      continue;

    const u16 obj_attr1 = eswap16(oam_ptr->attr1);
    const u16 obj_size = obj_attr1 >> 14;
    const s32 obj_base_height = obj_dim_table[obj_shape][obj_size][1];
    const s32 obj_base_width  = obj_dim_table[obj_shape][obj_size][0];
    s32 obj_height = obj_base_height;
    s32 obj_width  = obj_base_width;
    s32 obj_y = obj_attr0 & 0xFF;

    if (obj_y > 160)
      obj_y -= 256;

    if (obj_attr0 & 0x0200)
    {
      obj_height *= 2;
      obj_width *= 2;
    }

    if (((obj_y + obj_height) > 0) && (obj_y < 160))
    {
      const s32 obj_x = (s32)(obj_attr1 << 23) >> 23;

      if (((obj_x + obj_width) > 0) && (obj_x < 240))
      {
        const bool is_affine = (obj_attr0 & 0x0100) != 0;
        t_sprite *cached = &obj_cache[obj_num];

        cached->obj_x = (s16)obj_x;
        cached->obj_y = (s16)obj_y;
        cached->obj_w = (u8)obj_base_width;
        cached->obj_h = (u8)obj_base_height;
        cached->attr0 = obj_attr0;
        cached->attr1 = obj_attr1;
        cached->attr2 = obj_attr2;

        if (is_affine) {
          const u32 pnum = (obj_attr1 >> 9) & 0x1F;
          const t_affp *affp_base = (const t_affp*)oam_ram;
          const t_affp *affp = &affp_base[pnum];
          cached->aff_dx  = (s16)eswap16(affp->dx);
          cached->aff_dmx = (s16)eswap16(affp->dmx);
          cached->aff_dy  = (s16)eswap16(affp->dy);
          cached->aff_dmy = (s16)eswap16(affp->dmy);
        } else {
          cached->aff_dx = cached->aff_dmx =
          cached->aff_dy = cached->aff_dmy = 0;
        }

        u32 obj_priority = (obj_attr2 >> 10) & 0x03;
        const u32 starty = MAX(obj_y, 0);
        const u32 endy   = MIN(obj_y + obj_height, 160);
        const u16 cyccnt = is_affine ? (10 + obj_width * 2) : obj_width;

        switch (obj_mode) {
        case OBJ_MOD_SEMITRAN:
          for (row = starty; row < endy; row++)
          {
            if (rend_cycles[row] < max_rend_cycles) {
              const u32 cur_cnt = obj_priority_count[obj_priority][row];
              obj_priority_list[obj_priority][row][cur_cnt] = obj_num;
              obj_priority_count[obj_priority][row] = cur_cnt + 1;
              rend_cycles[row] += cyccnt;
              obj_alpha_count[row] = 1;
            }
          }
          break;

        case OBJ_MOD_WINDOW:
          obj_priority = 4;
          /* fallthrough */
        case OBJ_MOD_NORMAL:
          for (row = starty; row < endy; row++)
          {
            if (rend_cycles[row] < max_rend_cycles) {
              const u32 cur_cnt = obj_priority_count[obj_priority][row];
              obj_priority_list[obj_priority][row][cur_cnt] = obj_num;
              obj_priority_count[obj_priority][row] = cur_cnt + 1;
              rend_cycles[row] += cyccnt;
            }
          }
          break;
        };
      }
    }
  }
}"""
        vs = v14_region(vs, order_start, order_end, order_new, "OBJ ordering/cache rebuild")

        # Audit the transformed source before committing it to the stage.
        required = (
            "static t_sprite obj_cache[128];",
            "const u32 obj_color_flags = color_flags(4);",
            "fixed_y_tile_off",
            "cached->aff_dy",
            "vcount, obj1dmap, mosaic_reg",
        )
        for token in required:
            if token not in vs:
                raise SystemExit("gpSP V14 post-patch audit failed: missing %r" % token)

        forbidden = (
            "const t_affp *affp = &affp_base[pnum];\n\n    if (affp->dy == 0)",
            "s32 vcount = read_ioreg(REG_VCOUNT);\n  bool obj1dmap = read_ioreg(REG_DISPCNT) & 0x40;",
        )
        for token in forbidden:
            if token in vs:
                raise SystemExit("gpSP V14 post-patch audit failed: stale hot-path code remains")

        vid.write_text(vs, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_V15_ACCURACY_PERF_20260912
    # Accuracy + performance pass:
    #   - TFA external-clock completion must not inherit gpSP's RFU/GBP SI handshake.
    #   - Cache the last affine BG tile pointer inside one scanline.

    # 1) Turbo File Advance SIO completion.
    ser = stage / "serial.c"
    ss = ser.read_text(encoding="utf-8")
    v15_tfa_mark = "AURORA_GPSP_GBA_V15_TFA_EXTERNAL_CLOCK_20260912"

    if v15_tfa_mark not in ss and "AURORA_FDS_BUBBLE_TFA_TRANSPORT_V5_20260913_TFA_EXTERNAL_CLOCK" not in ss:
        tfa_start_old = """      if ((newval & 0x0080) && !(newval & 0x0001) &&
          !(newval & 0x1000) && !serial_irq_cycles) {
        u8 in = aurora_tfa_transfer((u8)(read_ioreg(REG_SIODATA8) & 0xffU));
"""
        tfa_start_new = """      if ((newval & 0x0080) && !(newval & 0x0001) &&
          !(newval & 0x1000) && !serial_irq_cycles) {
        /* AURORA_GPSP_GBA_V15_TFA_EXTERNAL_CLOCK_20260912
         * TFA is a Normal-8 external-clock device. SI must not inherit the
         * RFU/GBP busy handshake between byte transfers. */
        newval &= ~0x0004U;
        u8 in = aurora_tfa_transfer((u8)(read_ioreg(REG_SIODATA8) & 0xffU));
"""
        if tfa_start_old not in ss:
            raise SystemExit("gpSP V15 TFA start anchor missing")
        ss = ss.replace(tfa_start_old, tfa_start_new, 1)

        normal_done_old = """    case SERIAL_MODE_NORMAL:
      // Clear the send bit, signal data is ready.
      // Set the device busy bit, to perform the weird SO/SI handshake.
      write_ioreg(REG_SIOCNT, (read_ioreg(REG_SIOCNT) & ~0x80) | 0x04);
      // Return if IRQs are enabled.
      return read_ioreg(REG_SIOCNT) & 0x4000;
"""
        normal_done_new = """    case SERIAL_MODE_NORMAL:
      /* AURORA_GPSP_GBA_V15_TFA_EXTERNAL_CLOCK_20260912
       * The generic path below intentionally raises SI for RFU/GBP's
       * SO/SI handshake. Turbo File Advance does not use that handshake:
       * complete the externally-clocked byte by clearing START and stale SI. */
      if (aurora_tfa_active())
        write_ioreg(REG_SIOCNT,
                    read_ioreg(REG_SIOCNT) & ~(0x0080U | 0x0004U));
      else {
        // Clear the send bit, signal data is ready.
        // Set the device busy bit, to perform the weird SO/SI handshake.
        write_ioreg(REG_SIOCNT, (read_ioreg(REG_SIOCNT) & ~0x80) | 0x04);
      }
      // Return if IRQs are enabled.
      return read_ioreg(REG_SIOCNT) & 0x4000;
"""
        if normal_done_old not in ss:
            raise SystemExit("gpSP V15 Normal-SIO completion anchor missing")
        ss = ss.replace(normal_done_old, normal_done_new, 1)

        if ss.count(v15_tfa_mark) < 2:
            raise SystemExit("gpSP V15 TFA post-patch audit failed")
        ser.write_text(ss, encoding="utf-8", newline="\n")

    # 2) Affine BG last-tile cache.
    vid = stage / "video.cc"
    vs = vid.read_text(encoding="utf-8")
    v15_aff_mark = "AURORA_GPSP_GBA_V15_AFFINE_TILE_CACHE_20260912"

    if v15_aff_mark not in vs:
        helper_anchor = """static inline u8 lookup_pix_8bpp(
  u32 px, u32 py, const u8 *tile_base, const u8 *map_base, u32 map_size
) {
  // Pitch represents the log2(number of tiles per row) (from 16 to 128)
  u32 map_pitch = map_size + 4;
  // Given coords (px,py) in the background space, find the tile.
  u32 mapoff = (px / 8) + ((py / 8) << map_pitch);
  // Each tile is 8x8, so 64 bytes each.
  const u8 *tile_ptr = &tile_base[map_base[mapoff] * tile_size_8bpp];
  // Read the 8bit color within the tile.
  return tile_ptr[(px % 8) + ((py % 8) * 8)];
}
"""
        helper_new = helper_anchor + """
/* AURORA_GPSP_GBA_V15_AFFINE_TILE_CACHE_20260912
 * The renderer is not interleaved with CPU/DMA while this scanline call runs,
 * so repeated samples from one 8x8 affine tile can safely reuse its pointer. */
static inline u8 lookup_pix_8bpp_cached(
  u32 px, u32 py, const u8 *tile_base, const u8 *map_base, u32 map_pitch,
  u32 *cached_mapoff, const u8 **cached_tile
) {
  const u32 mapoff = (px >> 3) + ((py >> 3) << map_pitch);
  const u8 *tile_ptr = *cached_tile;

  if (mapoff != *cached_mapoff) {
    *cached_mapoff = mapoff;
    tile_ptr = &tile_base[((u32)map_base[mapoff]) << 6];
    *cached_tile = tile_ptr;
  }

  return tile_ptr[(px & 7U) + ((py & 7U) << 3)];
}
"""
        if helper_anchor not in vs:
            raise SystemExit("gpSP V15 affine helper anchor missing")
        vs = vs.replace(helper_anchor, helper_new, 1)

        cache_anchor = """  // Maps are squared, four sizes available (128x128 to 1024x1024)
  u32 width_height = 128 << map_size;

  // Horizontal mosaic effect.
"""
        cache_new = """  // Maps are squared, four sizes available (128x128 to 1024x1024)
  u32 width_height = 128 << map_size;

  /* AURORA_GPSP_GBA_V15_AFFINE_TILE_CACHE_20260912 */
  const u32 map_pitch = map_size + 4;
  u32 cached_mapoff = ~0U;
  const u8 *cached_tile = NULL;

  // Horizontal mosaic effect.
"""
        if cache_anchor not in vs:
            raise SystemExit("gpSP V15 affine cache-state anchor missing")
        vs = vs.replace(cache_anchor, cache_new, 1)

        old_call = "lookup_pix_8bpp(pix_x, pix_y, tile_base, map_base, map_size)"
        new_call = ("lookup_pix_8bpp_cached(pix_x, pix_y, tile_base, map_base, "
                    "map_pitch, &cached_mapoff, &cached_tile)")
        n_calls = vs.count(old_call)
        if n_calls != 4:
            raise SystemExit("gpSP V15 affine-call audit failed: expected 4, found %d" % n_calls)
        vs = vs.replace(old_call, new_call)

        if vs.count(v15_aff_mark) < 2 or vs.count(new_call) != 4:
            raise SystemExit("gpSP V15 affine post-patch audit failed")
        vid.write_text(vs, encoding="utf-8", newline="\n")


    # ------------------------------------------------------------------
    # V21: large-ROM pager correctness. TFA SIO remains owned by V15.
    # ------------------------------------------------------------------
    mem = stage / "gba_memory.c"
    ms = mem.read_text(encoding="utf-8")
    pager_mark = "AURORA_GPSP_GBA_V21_PAGER_IO_20260912"
    if pager_mark not in ms:
        page_old = (
            "  filestream_seek(gamepak_file_large, file_index * (32 * 1024), SEEK_SET);\n"
            "  {\n"
            "    u32 read_len = (u32)filestream_read(gamepak_file_large, swap_location, (32 * 1024));\n"
            "    if (read_len < (32 * 1024))\n"
            "      memset(swap_location + read_len, 0xFF, (32 * 1024) - read_len);\n"
            "  }\n"
        )
        page_new = (
            "  /* " + pager_mark + "\n"
            "   * Large carts are demand-paged in 32 KiB units. A failed seek or\n"
            "   * negative read must never be cast to u32 and mistaken for a full\n"
            "   * page; map deterministic open-cart data instead. */\n"
            "  if (!gamepak_file_large ||\n"
            "      filestream_seek(gamepak_file_large,\n"
            "                      (int64_t)file_index * (32 * 1024), SEEK_SET) < 0)\n"
            "  {\n"
            "    memset(swap_location, 0xFF, (32 * 1024));\n"
            "  }\n"
            "  else\n"
            "  {\n"
            "    int64_t got = filestream_read(gamepak_file_large,\n"
            "                                  swap_location, (32 * 1024));\n"
            "    if (got <= 0)\n"
            "      memset(swap_location, 0xFF, (32 * 1024));\n"
            "    else if (got < (32 * 1024))\n"
            "      memset(swap_location + (u32)got, 0xFF,\n"
            "             (32 * 1024) - (u32)got);\n"
            "  }\n"
        )
        if page_old not in ms:
            raise SystemExit("gpSP V21 32 KiB ROM pager anchor missing")
        ms = ms.replace(page_old, page_new, 1)
        mem.write_text(ms, encoding="utf-8", newline="\n")


    # AURORA_PCE_SSF2_FINAL_20260913_GPSP_RESIDENT_FINAL_BEGIN
    # AURORA_PCE_SSF2_FINAL_R4_CLEANUP_20260913_UPSTREAM_WORKRAM
    # Keep gpSP's pinned MIPS IWRAM/VRAM/EWRAM layout unchanged.  R4 narrows
    # the cross-core memory fix to gpSP's own MMAP_JIT_CACHE mechanism only.

    # Make the existing lazy JIT real on PS2 and make Aurora's final
    # --gc-sections useful inside gpSP object files.  The upstream .S rule also
    # includes CFLAGS, and CXXFLAGS is derived from CFLAGS, so one PS2 CFLAGS
    # line covers C, C++ and preprocessed MIPS assembly consistently.
    _fmk = stage / "Makefile"
    _fmks = _fmk.read_text(encoding="utf-8")
    _fmks, _fn = re.subn(r"^MMAP_JIT_CACHE\s*[:?+]?=\s*.*$",
                          "MMAP_JIT_CACHE := 1", _fmks,
                          count=1, flags=re.MULTILINE)
    if _fn != 1:
        raise SystemExit("gpSP FINAL: MMAP_JIT_CACHE assignment missing")

    _fflags = "CFLAGS += -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables # AURORA_PCE_SSF2_FINAL_20260913_GPSP_GC_SECTIONS\n"
    if "AURORA_PCE_SSF2_FINAL_20260913_GPSP_GC_SECTIONS" not in _fmks:
        _fps2 = "else ifeq ($(platform), ps2)\n"
        _fpos = _fmks.find(_fps2)
        if _fpos < 0:
            raise SystemExit("gpSP FINAL: PS2 Makefile branch missing")
        _fpos += len(_fps2)
        _fmks = _fmks[:_fpos] + _fflags + _fmks[_fpos:]
    _fmk.write_text(_fmks, encoding="utf-8", newline="\n")
    # AURORA_PCE_SSF2_FINAL_20260913_GPSP_RESIDENT_FINAL_END
    # AURORA_PCE_SSF2_FINAL_R3_20260913_MMAP_HARDEN_BEGIN
    _r3_mk = stage / "Makefile"
    _r3_ms = _r3_mk.read_text(encoding="utf-8")
    _r3_gate = "ifeq ($(MMAP_JIT_CACHE), 1)\n"
    _r3_force = """# AURORA_PCE_SSF2_FINAL_R3_20260913_MMAP_EXACT_PS2
ifeq ($(platform), ps2)
override MMAP_JIT_CACHE := 1
endif

ifeq ($(MMAP_JIT_CACHE), 1)
"""
    if "AURORA_PCE_SSF2_FINAL_R3_20260913_MMAP_EXACT_PS2" not in _r3_ms:
        if _r3_ms.count(_r3_gate) != 1:
            raise SystemExit("gpSP R3: MMAP_JIT_CACHE gate missing/ambiguous")
        _r3_ms = _r3_ms.replace(_r3_gate, _r3_force, 1)

    # Also normalise the known malformed Aurora PS2 assignment when present.
    # This is not relied upon for correctness (the override above is), but it
    # prevents the bad value from surviving in the staged Makefile.
    _r3_ms = re.sub(
        r'^(\s*)MMAP_JIT_CACHE\s*=\s*1\s+#\s*AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_LAZY_ALLOC_STAGE\s*$',
        r'\1MMAP_JIT_CACHE := 1 # AURORA_PCE_SSF2_FINAL_R3_20260913_MMAP_NORMALIZED',
        _r3_ms, count=1, flags=re.M)

    if "override MMAP_JIT_CACHE := 1" not in _r3_ms:
        raise SystemExit("gpSP R3: exact PS2 MMAP override was not installed")
    _r3_mk.write_text(_r3_ms, encoding="utf-8", newline="\n")
    # AURORA_PCE_SSF2_FINAL_R3_20260913_MMAP_HARDEN_END
    # AURORA_GPSP_RAMONLY_ZIP_V2_20260913
    _ram_gm = stage / "gba_memory.c"
    _ram_s = _ram_gm.read_text(encoding="utf-8")
    _ram_marker = "AURORA_GPSP_RAMONLY_ZIP_V2_20260913"
    if _ram_marker not in _ram_s:
        _ram_anchor = "static s32 load_gamepak_raw(const char *name)\n"
        if _ram_s.count(_ram_anchor) != 1:
            raise SystemExit("gpSP RAM-only ZIP: load_gamepak_raw anchor missing/ambiguous")
        _ram_helper = r'''/* AURORA_GPSP_RAMONLY_ZIP_V2_20260913
 * Frontend-owned memory source for compressed cartridges.
 * This intentionally has NO file/path fallback: every ROM page must fit the
 * core's existing buffers while retro_load_game() consumes info->data. */
static s32 load_gamepak_raw_memory(const void *data, size_t data_size)
{
  const u8 *src = (const u8 *)data;
  u32 source_size, raw_size, buf_blocks, rom_blocks;
  unsigned i, j;

  if (!src || data_size == 0 || data_size > (size_t)0x02000000U)
    return -1;

  source_size = (u32)data_size;
  raw_size = (source_size + 0x7FFFU) & ~0x7FFFU;
  if (raw_size < source_size || raw_size > 0x02000000U)
    return -1;

  if (gamepak_file_large)
  {
    filestream_close(gamepak_file_large);
    gamepak_file_large = NULL;
  }
  if (gamepak_mini_rom)
  {
    free(gamepak_mini_rom);
    gamepak_mini_rom = NULL;
  }
  gamepak_mini_materialized = false;

  gamepak_file_blocks = raw_size >> 15;
  gamepak_mirror_1m = (raw_size == 0x00100000U);
  gamepak_size = gamepak_mirror_1m ? 0x00400000U : raw_size;

  /* Preserve gpSP's special 1 MiB cartridge mirroring without storage I/O. */
  if (gamepak_mirror_1m)
  {
    u32 map_blocks = gamepak_size >> 15;
    u32 phyn;
    u32 first_copy;

    gamepak_mini_rom = (u8 *)malloc(gamepak_size);
    if (!gamepak_mini_rom)
      return -1;

    memset(gamepak_mini_rom, 0xFF, gamepak_size);
    memcpy(gamepak_mini_rom, src, source_size);
    memcpy(gamepak_mini_rom + 0x100000U, gamepak_mini_rom, 0x100000U);
    memcpy(gamepak_mini_rom + 0x200000U, gamepak_mini_rom, 0x100000U);
    memcpy(gamepak_mini_rom + 0x300000U, gamepak_mini_rom, 0x100000U);

    if (!gamepak_buffer_count || !gamepak_buffers[0])
    {
      free(gamepak_mini_rom);
      gamepak_mini_rom = NULL;
      return -1;
    }
    first_copy = gamepak_buffer_blocksize < 0x100000U
                   ? gamepak_buffer_blocksize : 0x100000U;
    memcpy(gamepak_buffers[0], gamepak_mini_rom, first_copy);

    map_null(read, 0x8000000, 0xD000000);
    for (phyn = 0; phyn < map_blocks; phyn++)
    {
      u8 *blkptr = &gamepak_mini_rom[32U * 1024U * phyn];
      map_rom_entry(read, phyn, blkptr, map_blocks);
    }
    update_gpio_romregs();
    gamepak_mirror_1m = false;
    gamepak_mini_materialized = true;
    return 0;
  }

  buf_blocks = (raw_size + gamepak_buffer_blocksize - 1U) /
               gamepak_buffer_blocksize;

  /* There is deliberately no memory->file spill and no path-backed pager.
   * If the complete ROM cannot fit now, the frontend reports a clean ZIP
   * too-large-for-RAM error. */
  if (!buf_blocks || buf_blocks > gamepak_buffer_count)
    return -1;

  rom_blocks = gamepak_size >> 15;
  map_null(read, 0x8000000, 0xD000000);

  for (i = 0; i < buf_blocks; i++)
  {
    u32 off = (u32)i * gamepak_buffer_blocksize;
    u32 take = 0;
    if (off < source_size)
    {
      take = source_size - off;
      if (take > gamepak_buffer_blocksize)
        take = gamepak_buffer_blocksize;
      memcpy(gamepak_buffers[i], src + off, take);
    }
    if (take < gamepak_buffer_blocksize)
      memset(gamepak_buffers[i] + take, 0xFF,
             gamepak_buffer_blocksize - take);

    for (j = 0; j < 32 && i * 32 + j < gamepak_file_blocks; j++)
    {
      u32 phyn = i * 32 + j;
      u8 *blkptr = &gamepak_buffers[i][32U * 1024U * j];
      u32 entry = evict_gamepak_page();
      gamepak_blk_queue[entry].phy_rom = phyn;
      map_rom_entry(read, phyn, blkptr, rom_blocks);
    }
  }

  update_gpio_romregs();
  return 0;
}

'''
        _ram_s = _ram_s.replace(_ram_anchor, _ram_helper + _ram_anchor, 1)
        _ram_old = "if (load_gamepak_raw(name))"
        if _ram_s.count(_ram_old) != 1:
            raise SystemExit("gpSP RAM-only ZIP: load_gamepak call anchor missing/ambiguous")
        _ram_new = "if ((info && info->data && info->size) ?\n" \
                   "      load_gamepak_raw_memory(info->data, info->size) :\n" \
                   "      load_gamepak_raw(name))"
        _ram_s = _ram_s.replace(_ram_old, _ram_new, 1)
        _ram_gm.write_text(_ram_s, encoding="utf-8", newline="\n")

    # AURORA_GPSP_GBA_MARIO_BIOS_LATCH_V1_20260913
    # GBA protected-BIOS reads outside 00000000h..00003FFFh expose the last
    # BIOS-prefetch word, not arbitrary current bus data. gpSP models that
    # latch with REG_BUS_VALUE. reset_gba() calls init_memory() before
    # init_cpu(), and init_cpu() clears the value memory init placed there.
    # Restore the post-startup/SoftReset value after that clear. Actual BIOS
    # execution is unaffected, and existing SWI/IRQ paths still update the
    # latch later as before.
    _bios_cpu = stage / "cpu.cc"
    _bios_s = _bios_cpu.read_text(encoding="utf-8")
    _bios_anchor = (
        "  // Initialize CPU registers\n"
        "  memset(reg, 0, REG_USERDEF * sizeof(u32));\n"
        "  memset(reg_mode, 0, sizeof(reg_mode));\n"
    )
    _bios_new = (
        "  // Initialize CPU registers\n"
        "  memset(reg, 0, REG_USERDEF * sizeof(u32));\n"
        "  /* AURORA_GPSP_GBA_MARIO_BIOS_LATCH_V1_20260913: preserve protected-BIOS latch across reset. */\n"
        "  reg[REG_BUS_VALUE] = 0xe129f000;\n"
        "  memset(reg_mode, 0, sizeof(reg_mode));\n"
    )
    if _bios_s.count(_bios_anchor) != 1:
        raise SystemExit("gpSP Mario/BIOS latch: init_cpu anchor missing/ambiguous")
    _bios_s = _bios_s.replace(_bios_anchor, _bios_new, 1)
    _bios_cpu.write_text(_bios_s, encoding="utf-8", newline="\n")

    # AURORA_GPSP_PROTECTED_BIOS_LATCH_V2_20260913
    # Dedicated GBA protected-BIOS prefetch latch.
    #
    # REG_BUS_VALUE is ordinary open-bus state and changes during normal game
    # execution. GBA BIOS protection instead returns the last prefetched BIOS
    # instruction when PC is outside 00000000h..00003FFFh.
    _pb_mem = stage / "gba_memory.c"
    _pb_cpu = stage / "cpu.cc"

    _pb_ms = _pb_mem.read_text(encoding="utf-8")
    _pb_decl = "u32 aurora_bios_protected_latch = 0xe129f000U;"
    if _pb_decl not in _pb_ms:
        _pb_inc = '#include "streams/file_stream.h"\n'
        if _pb_ms.count(_pb_inc) != 1:
            raise SystemExit(
                "gpSP protected BIOS latch: gba_memory include anchor "
                "missing/ambiguous")
        _pb_ms = _pb_ms.replace(
            _pb_inc,
            _pb_inc +
            "\n/* AURORA_GPSP_PROTECTED_BIOS_LATCH_V2_20260913: separate from ordinary open-bus state. */\n" +
            _pb_decl + "\n",
            1)

    _pb_old_read = (
        "reg[REG_BUS_VALUE] >> ((address & 0x03) << 3)"
    )
    _pb_new_read = (
        "aurora_bios_protected_latch >> ((address & 0x03) << 3)"
    )
    # AURORA_FDS_SAVE_GPSP_BUILD_FIX_V4_20260913_GPSP_BIOS_ANCHOR
    # REG_BUS_VALUE has legitimate users outside BIOS. Restrict this edit
    # to read_memory()'s BIOS branch instead of counting the whole file.
    if _pb_new_read not in _pb_ms:
        _pb_tag = "/* BIOS */"
        _pb_a = _pb_ms.find(_pb_tag)
        _pb_b = _pb_ms.find("case 0x02:", _pb_a + 1) if _pb_a >= 0 else -1
        if _pb_a < 0 or _pb_b <= _pb_a:
            raise SystemExit(
                "gpSP protected BIOS latch: BIOS block not found")
        _pb_chunk = _pb_ms[_pb_a:_pb_b]
        if _pb_old_read not in _pb_chunk:
            raise SystemExit(
                "gpSP protected BIOS latch: protected read missing in BIOS")
        _pb_chunk = _pb_chunk.replace(
            _pb_old_read, _pb_new_read, 1)
        _pb_ms = _pb_ms[:_pb_a] + _pb_chunk + _pb_ms[_pb_b:]

    _pb_mem.write_text(
        _pb_ms, encoding="utf-8", newline="\n")

    _pb_cs = _pb_cpu.read_text(encoding="utf-8")

    _pb_extern = "  extern u32 aurora_bios_protected_latch;\n"
    if _pb_extern not in _pb_cs:
        _pb_inc = '  #include "cpu_instrument.h"\n'
        if _pb_cs.count(_pb_inc) != 1:
            raise SystemExit(
                "gpSP protected BIOS latch: cpu extern anchor "
                "missing/ambiguous")
        _pb_cs = _pb_cs.replace(
            _pb_inc, _pb_inc + _pb_extern, 1)

    _pb_init = "  aurora_bios_protected_latch = 0xe129f000U;\n"
    if _pb_init not in _pb_cs:
        _pb_v2 = "  reg[REG_BUS_VALUE] = 0xe129f000;\n"
        if _pb_v2 in _pb_cs:
            if _pb_cs.count(_pb_v2) != 1:
                raise SystemExit(
                    "gpSP protected BIOS latch: V2 reset anchor ambiguous")
            _pb_cs = _pb_cs.replace(
                _pb_v2, _pb_v2 + _pb_init, 1)
        else:
            _pb_zero = (
                "  memset(reg, 0, REG_USERDEF * sizeof(u32));\n"
            )
            if _pb_cs.count(_pb_zero) != 1:
                raise SystemExit(
                    "gpSP protected BIOS latch: init_cpu memset anchor "
                    "missing/ambiguous")
            _pb_cs = _pb_cs.replace(
                _pb_zero,
                _pb_zero +
                "  reg[REG_BUS_VALUE] = 0xe129f000;\n" +
                _pb_init,
                1)

    _pb_irq = "    reg[REG_BUS_VALUE] = 0xe55ec002;\n"
    _pb_irq_latch = (
        "    aurora_bios_protected_latch = 0xe55ec002U;\n"
    )
    if _pb_irq_latch not in _pb_cs:
        if _pb_cs.count(_pb_irq) != 1:
            raise SystemExit(
                "gpSP protected BIOS latch: IRQ anchor "
                "missing/ambiguous")
        _pb_cs = _pb_cs.replace(
            _pb_irq, _pb_irq + _pb_irq_latch, 1)

    _pb_swi = (
        "reg[REG_BUS_VALUE] = 0xe3a02004;  "
        "// After SWI, we read bios[0xE4]"
    )
    _pb_swi_latch = (
        "aurora_bios_protected_latch = 0xe3a02004U;"
    )
    if _pb_swi_latch not in _pb_cs:
        _pb_count = _pb_cs.count(_pb_swi)
        if _pb_count != 2:
            raise SystemExit(
                "gpSP protected BIOS latch: expected ARM+THUMB SWI "
                f"anchors, found {_pb_count}")
        _pb_cs = _pb_cs.replace(
            _pb_swi,
            _pb_swi + "\n"
            "             " + _pb_swi_latch,
        )

    _pb_cpu.write_text(
        _pb_cs, encoding="utf-8", newline="\n")


    # AURORA_GBA_DYNAREC_DEFERRED_EXIT_TEARDOWN_V7_20260916
    #
    # cpu_threaded.c grows deferred_exits with realloc() while resolving
    # external JIT branch exits. The existing ROM/RAM JIT buffers are freed
    # at retro_deinit(), but this independent worklist historically kept its
    # high-water allocation across games. Free it only at full dynarec
    # teardown: there is no per-frame cost and no churn inside one game.
    _v7_cpu = stage / "cpu_threaded.c"
    _v7_cps = _v7_cpu.read_text(encoding="utf-8")
    _v7_mark = "AURORA_GBA_DYNAREC_DEFERRED_EXIT_TEARDOWN_V7_20260916"
    if _v7_mark not in _v7_cps:
        _v7_old = """void deinit_dynarec_caches(void)
{
  rom_translation_ptr = NULL;
  ram_translation_ptr = NULL;
  last_rom_translation_ptr = NULL;
  last_ram_translation_ptr = NULL;

  ram_block_tag = INITIAL_TOP_TAG;
  rom_cache_watermark = INITIAL_ROM_WATERMARK;
  bios_swi_entrypoint = NULL;

  iwram_code_min = ~0U;
  iwram_code_max = 0U;
  ewram_code_min = ~0U;
  ewram_code_max = 0U;
  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
}
"""
        _v7_new = """void deinit_dynarec_caches(void)
{
  /* AURORA_GBA_DYNAREC_DEFERRED_EXIT_TEARDOWN_V7_20260916
   * The deferred branch-exit vector is independent of the executable JIT
   * mapping. It may grow with realloc() during a game, so full content
   * teardown must release that high-water allocation as well. */
  free(deferred_exits);
  deferred_exits = NULL;
  deferred_exit_count = 0;
  deferred_exit_capacity = 0;
  translate_depth = 0;

  rom_translation_ptr = NULL;
  ram_translation_ptr = NULL;
  last_rom_translation_ptr = NULL;
  last_ram_translation_ptr = NULL;

  ram_block_tag = INITIAL_TOP_TAG;
  rom_cache_watermark = INITIAL_ROM_WATERMARK;
  bios_swi_entrypoint = NULL;

  iwram_code_min = ~0U;
  iwram_code_max = 0U;
  ewram_code_min = ~0U;
  ewram_code_max = 0U;
  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
}
"""
        if _v7_cps.count(_v7_old) != 1:
            raise SystemExit(
                "gpSP V7: expected one deinit_dynarec_caches lifecycle anchor, found %d"
                % _v7_cps.count(_v7_old)
            )
        _v7_cps = _v7_cps.replace(_v7_old, _v7_new, 1)

    for _v7_tok in (
        _v7_mark,
        "free(deferred_exits);",
        "deferred_exits = NULL;",
        "deferred_exit_count = 0;",
        "deferred_exit_capacity = 0;",
        "translate_depth = 0;",
    ):
        if _v7_tok not in _v7_cps:
            raise SystemExit("gpSP V7 post-patch audit failed: missing " + _v7_tok)

    _v7_cpu.write_text(_v7_cps, encoding="utf-8", newline="\n")


    # AURORA_GPSP_GBA_V25_PS2_MIPS_POSTFX_AUDIO_20260929
    # Safe output-side MIPS pass. Keep emulated CPU/PPU/audio-generation timing
    # completely untouched: these leaf helpers only transform already-rendered
    # pixels or already-mixed host audio samples.
    _v25_mark = "AURORA_GPSP_GBA_V25_PS2_MIPS_POSTFX_AUDIO_20260929"
    _v25_asm_path = stage / "aurora_gpsp_ps2_hotpaths.S"
    # AURORA_GPSP_GBA_V26_NUMERIC_GPR_FIX_20260929: numeric GPRs only; fixes EE/R5900 alias/overlap regression.
    _v25_asm = "/* Aurora gpSP PS2 output hot paths.\n * AURORA_GPSP_GBA_V26_NUMERIC_GPR_FIX_20260929\n *\n * PS2/R5900 leaf functions using numeric GPR names only.\n * No callee-saved register, stack pointer or global pointer is touched.\n * Video routines process exactly the native 240*160 GBA framebuffer.\n */\n\n    .text\n    .align 4\n    .set noreorder\n    .set noat\n\n/* void aurora_gpsp_ps2_cc_asm(u16 *dst, const u16 *src, const u16 *lut)\n * $4=dst, $5=src, $6=lut\n * $8=count, $9=packed source, $10/$11=scratch\n */\n    .type aurora_gpsp_ps2_cc_asm, %function\n    .global aurora_gpsp_ps2_cc_asm\naurora_gpsp_ps2_cc_asm:\n    addiu   $8, $0, 9600\n1:\n    lw      $9, 0($5)\n    andi    $10, $9, 0x7fff\n    srl     $11, $9, 16\n    andi    $11, $11, 0x7fff\n    sll     $10, $10, 1\n    sll     $11, $11, 1\n    addu    $10, $6, $10\n    addu    $11, $6, $11\n    lhu     $10, 0($10)\n    lhu     $11, 0($11)\n    sll     $11, $11, 16\n    or      $10, $10, $11\n    sw      $10, 0($4)\n\n    lw      $9, 4($5)\n    andi    $10, $9, 0x7fff\n    srl     $11, $9, 16\n    andi    $11, $11, 0x7fff\n    sll     $10, $10, 1\n    sll     $11, $11, 1\n    addu    $10, $6, $10\n    addu    $11, $6, $11\n    lhu     $10, 0($10)\n    lhu     $11, 0($11)\n    sll     $11, $11, 16\n    or      $10, $10, $11\n    sw      $10, 4($4)\n\n    addiu   $4, $4, 8\n    addiu   $5, $5, 8\n    addiu   $8, $8, -1\n    bnez    $8, 1b\n     nop\n    jr      $31\n     nop\n    .size aurora_gpsp_ps2_cc_asm, .-aurora_gpsp_ps2_cc_asm\n\n\n/* void aurora_gpsp_ps2_mix_asm(u16 *dst, const u16 *curr, const u16 *prev)\n * $4=dst, $5=curr, $6=prev\n * $8=count, $9/$10=packed inputs, $11=diff, $12=result, $15=mask\n * Exact packed form used by Aurora's PS2 C path:\n *   (a | b) - (((a ^ b) & 0x7BDE7BDE) >> 1)\n */\n    .type aurora_gpsp_ps2_mix_asm, %function\n    .global aurora_gpsp_ps2_mix_asm\naurora_gpsp_ps2_mix_asm:\n    addiu   $8, $0, 9600\n    lui     $15, 0x7bde\n    ori     $15, $15, 0x7bde\n2:\n    lw      $9, 0($5)\n    lw      $10, 0($6)\n    xor     $11, $9, $10\n    and     $11, $11, $15\n    srl     $11, $11, 1\n    or      $12, $9, $10\n    subu    $12, $12, $11\n    sw      $12, 0($4)\n\n    lw      $9, 4($5)\n    lw      $10, 4($6)\n    xor     $11, $9, $10\n    and     $11, $11, $15\n    srl     $11, $11, 1\n    or      $12, $9, $10\n    subu    $12, $12, $11\n    sw      $12, 4($4)\n\n    addiu   $4, $4, 8\n    addiu   $5, $5, 8\n    addiu   $6, $6, 8\n    addiu   $8, $8, -1\n    bnez    $8, 2b\n     nop\n    jr      $31\n     nop\n    .size aurora_gpsp_ps2_mix_asm, .-aurora_gpsp_ps2_mix_asm\n\n\n/* void aurora_gpsp_ps2_cc_mix_asm(\n *     u16 *dst, const u16 *curr, const u16 *prev, const u16 *lut)\n * $4=dst, $5=curr, $6=prev, $7=lut\n * $8=count, $9/$10=inputs, $11=diff, $12=blend,\n * $13/$14=LUT scratch, $15=mask\n */\n    .type aurora_gpsp_ps2_cc_mix_asm, %function\n    .global aurora_gpsp_ps2_cc_mix_asm\naurora_gpsp_ps2_cc_mix_asm:\n    addiu   $8, $0, 9600\n    lui     $15, 0x7bde\n    ori     $15, $15, 0x7bde\n3:\n    lw      $9, 0($5)\n    lw      $10, 0($6)\n    xor     $11, $9, $10\n    and     $11, $11, $15\n    srl     $11, $11, 1\n    or      $12, $9, $10\n    subu    $12, $12, $11\n\n    andi    $13, $12, 0x7fff\n    srl     $14, $12, 16\n    andi    $14, $14, 0x7fff\n    sll     $13, $13, 1\n    sll     $14, $14, 1\n    addu    $13, $7, $13\n    addu    $14, $7, $14\n    lhu     $13, 0($13)\n    lhu     $14, 0($14)\n    sll     $14, $14, 16\n    or      $13, $13, $14\n    sw      $13, 0($4)\n\n    lw      $9, 4($5)\n    lw      $10, 4($6)\n    xor     $11, $9, $10\n    and     $11, $11, $15\n    srl     $11, $11, 1\n    or      $12, $9, $10\n    subu    $12, $12, $11\n\n    andi    $13, $12, 0x7fff\n    srl     $14, $12, 16\n    andi    $14, $14, 0x7fff\n    sll     $13, $13, 1\n    sll     $14, $14, 1\n    addu    $13, $7, $13\n    addu    $14, $7, $14\n    lhu     $13, 0($13)\n    lhu     $14, 0($14)\n    sll     $14, $14, 16\n    or      $13, $13, $14\n    sw      $13, 4($4)\n\n    addiu   $4, $4, 8\n    addiu   $5, $5, 8\n    addiu   $6, $6, 8\n    addiu   $8, $8, -1\n    bnez    $8, 3b\n     nop\n    jr      $31\n     nop\n    .size aurora_gpsp_ps2_cc_mix_asm, .-aurora_gpsp_ps2_cc_mix_asm\n\n\n/* void aurora_gpsp_ps2_sound_drain_asm(s16 *out, s32 *src, u32 samples)\n * $4=out, $5=src, $6=count\n * $9=sample, $10=compare scratch, $11=+32767, $12=-32768\n * Only final ring drain/clear/clamp is performed here; synthesis/timing remains C.\n */\n    .type aurora_gpsp_ps2_sound_drain_asm, %function\n    .global aurora_gpsp_ps2_sound_drain_asm\naurora_gpsp_ps2_sound_drain_asm:\n    beqz    $6, 9f\n     nop\n    addiu   $11, $0, 32767\n    lui     $12, 0xffff\n    ori     $12, $12, 0x8000\n4:\n    lw      $9, 0($5)\n    sw      $0, 0($5)\n\n    slt     $10, $11, $9\n    beqz    $10, 5f\n     nop\n    addu    $9, $11, $0\n5:\n    slt     $10, $9, $12\n    beqz    $10, 6f\n     nop\n    addu    $9, $12, $0\n6:\n    sh      $9, 0($4)\n    addiu   $4, $4, 2\n    addiu   $5, $5, 4\n    addiu   $6, $6, -1\n    bnez    $6, 4b\n     nop\n9:\n    jr      $31\n     nop\n    .size aurora_gpsp_ps2_sound_drain_asm, .-aurora_gpsp_ps2_sound_drain_asm\n\n    .set at\n    .set reorder\n"
    _v25_asm_path.write_text(_v25_asm, encoding="utf-8", newline="\n")

    # Compile the helper only for the PS2 staged core. Upper-case .S matches
    # gpSP's existing preprocessed-assembly rule and mips/mips_stub.S.
    _v25_mfc = stage / "Makefile.common"
    _v25_mfs = _v25_mfc.read_text(encoding="utf-8")
    _v25_src_line = "  SOURCES_ASM += $(CORE_DIR)/aurora_gpsp_ps2_hotpaths.S # " + _v25_mark
    if _v25_src_line not in _v25_mfs:
        _v25_mf_anchor = "SOURCES_ASM := $(CORE_DIR)/bios_data.S\n"
        if _v25_mfs.count(_v25_mf_anchor) != 1:
            raise SystemExit("gpSP V25: Makefile.common ASM anchor missing/ambiguous")
        _v25_mfs = _v25_mfs.replace(
            _v25_mf_anchor,
            _v25_mf_anchor +
            "\nifeq ($(platform), ps2)\n" +
            _v25_src_line + "\n" +
            "endif\n",
            1,
        )
    _v25_mfc.write_text(_v25_mfs, encoding="utf-8", newline="\n")

    # Video: retain every existing C/C++ loop. Assembly is an early PS2-only
    # fast path; if an allocator ever returns a non-word-aligned framebuffer,
    # execution transparently falls back to the current implementation.
    _v25_lr = stage / "libretro/libretro.c"
    _v25_ls = _v25_lr.read_text(encoding="utf-8")
    _v25_proto_anchor = "static void video_post_process_cc(void)\n"
    if _v25_ls.count(_v25_proto_anchor) != 1:
        raise SystemExit("gpSP V25: video_post_process_cc anchor missing/ambiguous")

    _v25_protos = '#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n/* AURORA_GPSP_GBA_V25_PS2_MIPS_POSTFX_AUDIO_20260929 */\nextern void aurora_gpsp_ps2_cc_asm(\n      u16 *dst, const u16 *src, const u16 *lut);\nextern void aurora_gpsp_ps2_mix_asm(\n      u16 *dst, const u16 *curr, const u16 *prev);\nextern void aurora_gpsp_ps2_cc_mix_asm(\n      u16 *dst, const u16 *curr, const u16 *prev, const u16 *lut);\n\nstatic inline bool aurora_gpsp_ps2_word_aligned(const void *p)\n{\n   return (((uintptr_t)p) & 3U) == 0;\n}\n#endif\n\n'
    if _v25_mark not in _v25_ls:
        _v25_ls = _v25_ls.replace(
            _v25_proto_anchor, _v25_protos + _v25_proto_anchor, 1)

    def _v25_video_region(text, start, end, label):
        a = text.find(start)
        if a < 0:
            raise SystemExit("gpSP V25: missing video region " + label)
        b = text.find(end, a + len(start))
        if b < 0:
            raise SystemExit("gpSP V25: missing video region end " + label)
        return a, b, text[a:b]

    # CC-only: direct native BGR555 LUT. Return after ASM because there is no
    # history-buffer swap in this mode.
    _a, _b, _cc = _v25_video_region(
        _v25_ls,
        "static void video_post_process_cc(void)\n",
        "static void video_post_process_mix(void)\n",
        "cc",
    )
    _cc_decl = "   size_t i;\n"
    if _cc.count(_cc_decl) != 1:
        raise SystemExit("gpSP V25: CC local-declaration anchor missing/ambiguous")
    _cc_fast = '   size_t i;\n\n#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n   if (aurora_gpsp_ps2_word_aligned(src) &&\n       aurora_gpsp_ps2_word_aligned(dst))\n   {\n      aurora_gpsp_ps2_cc_asm(dst, src, gba_cc_lut);\n      return;\n   }\n#endif\n'
    _cc = _cc.replace(_cc_decl, _cc_fast, 1)
    _v25_ls = _v25_ls[:_a] + _cc + _v25_ls[_b:]

    # Mix: jump only over the existing C loop, never over the mandatory
    # current/previous framebuffer pointer swap.
    _a, _b, _mix = _v25_video_region(
        _v25_ls,
        "static void video_post_process_mix(void)\n",
        "static void video_post_process_cc_mix(void)\n",
        "mix",
    )
    _mix_decl = "   size_t i;\n"
    if _mix.count(_mix_decl) != 1:
        raise SystemExit("gpSP V25: MIX local-declaration anchor missing/ambiguous")
    _mix_fast = '   size_t i;\n\n#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n   if (aurora_gpsp_ps2_word_aligned(src_curr) &&\n       aurora_gpsp_ps2_word_aligned(src_prev) &&\n       aurora_gpsp_ps2_word_aligned(dst))\n   {\n      aurora_gpsp_ps2_mix_asm(dst, src_curr, src_prev);\n      goto aurora_gpsp_ps2_mix_done;\n   }\n#endif\n'
    _mix = _mix.replace(_mix_decl, _mix_fast, 1)
    _mix_swap = "   {\n      u16 *t = gba_screen_pixels;\n"
    if _mix.count(_mix_swap) != 1:
        raise SystemExit("gpSP V25: MIX framebuffer-swap anchor missing/ambiguous")
    _mix = _mix.replace(
        _mix_swap,
        "#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n"
        "aurora_gpsp_ps2_mix_done:\n"
        "#endif\n" + _mix_swap,
        1,
    )
    _v25_ls = _v25_ls[:_a] + _mix + _v25_ls[_b:]

    # CC+Mix: same rule, preserving the mandatory pointer swap and exact LUT.
    _a, _b, _ccm = _v25_video_region(
        _v25_ls,
        "static void video_post_process_cc_mix(void)\n",
        "static void init_post_processing(void)\n",
        "cc+mix",
    )
    _ccm_decl = "   size_t i;\n"
    if _ccm.count(_ccm_decl) != 1:
        raise SystemExit("gpSP V25: CC+MIX local-declaration anchor missing/ambiguous")
    _ccm_fast = '   size_t i;\n\n#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n   if (aurora_gpsp_ps2_word_aligned(src_curr) &&\n       aurora_gpsp_ps2_word_aligned(src_prev) &&\n       aurora_gpsp_ps2_word_aligned(dst))\n   {\n      aurora_gpsp_ps2_cc_mix_asm(dst, src_curr, src_prev, gba_cc_lut);\n      goto aurora_gpsp_ps2_cc_mix_done;\n   }\n#endif\n'
    _ccm = _ccm.replace(_ccm_decl, _ccm_fast, 1)
    if _ccm.count(_mix_swap) != 1:
        raise SystemExit("gpSP V25: CC+MIX framebuffer-swap anchor missing/ambiguous")
    _ccm = _ccm.replace(
        _mix_swap,
        "#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n"
        "aurora_gpsp_ps2_cc_mix_done:\n"
        "#endif\n" + _mix_swap,
        1,
    )
    _v25_ls = _v25_ls[:_a] + _ccm + _v25_ls[_b:]
    _v25_lr.write_text(_v25_ls, encoding="utf-8", newline="\n")

    # Audio output drain: split the ring into at most two contiguous spans,
    # then let ASM perform only the exact old clear+clamp+store operation.
    # GBA sound generation, interpolation, FIFO/DMA and sample timing stay C.
    _v25_sound = stage / "sound.c"
    _v25_ss = _v25_sound.read_text(encoding="utf-8")
    _v25_s_start = "u32 sound_read_samples(s16 *out, u32 frames)\n"
    _v25_s_end = "void sound_flush_ring(void)\n"
    _v25_sa = _v25_ss.find(_v25_s_start)
    _v25_sb = _v25_ss.find(_v25_s_end, _v25_sa + len(_v25_s_start))
    if _v25_sa < 0 or _v25_sb < 0:
        raise SystemExit("gpSP V25: sound_read_samples region missing")
    if _v25_ss.count(_v25_s_start) != 1:
        raise SystemExit("gpSP V25: sound_read_samples anchor ambiguous")

    _v25_sound_region = "#if defined(PS2)\nextern void aurora_gpsp_ps2_sound_drain_asm(\n      s16 *out, s32 *src, u32 samples);\n#endif\n\nu32 sound_read_samples(s16 *out, u32 frames)\n{\n   u32 samples_to_read = frames << 1;\n\n   /* Keep the ring invariant explicit before the contiguous PS2 split.\n    * For valid states this is already true; modulo-normalising here is\n    * equivalent to the old per-sample '& BUFFER_SIZE_MASK' access. */\n   sound_buffer_base &= BUFFER_SIZE_MASK;\n\n   u32 samples_available =\n      (gbc_sound_buffer_index - sound_buffer_base) & BUFFER_SIZE_MASK;\n\n   /* The last 512 samples are 'in use', and cannot be read out yet. */\n   samples_available = (samples_available > 512)\n      ? (samples_available - 512) : 0;\n   samples_available = (samples_available >> 1) << 1;\n\n   if (samples_to_read > samples_available)\n      samples_to_read = samples_available;\n\n#if defined(PS2)\n   if (samples_to_read)\n   {\n      u32 first = BUFFER_SIZE - sound_buffer_base;\n      if (first > samples_to_read)\n         first = samples_to_read;\n\n      aurora_gpsp_ps2_sound_drain_asm(\n         out, &sound_buffer[sound_buffer_base], first);\n\n      if (first < samples_to_read)\n         aurora_gpsp_ps2_sound_drain_asm(\n            out + first, sound_buffer, samples_to_read - first);\n   }\n#else\n   {\n      u32 i;\n      for (i = 0; i < samples_to_read; i++)\n      {\n         u32 source_index = (sound_buffer_base + i) & BUFFER_SIZE_MASK;\n         s32 current_sample = sound_buffer[source_index];\n\n         sound_buffer[source_index] = 0;\n\n         if (current_sample > 32767)\n            current_sample = 32767;\n         if (current_sample < -32768)\n            current_sample = -32768;\n\n         out[i] = current_sample;\n      }\n   }\n#endif\n\n   sound_buffer_base += samples_to_read;\n   sound_buffer_base &= BUFFER_SIZE_MASK;\n\n   return (samples_to_read >> 1);\n}\n\n"
    _v25_ss = _v25_ss[:_v25_sa] + _v25_sound_region + _v25_ss[_v25_sb:]
    _v25_sound.write_text(_v25_ss, encoding="utf-8", newline="\n")

    # Stage-level fail-closed audit before the raw gpSP archive may build.
    _v25_asm_check = _v25_asm_path.read_text(encoding="utf-8")
    for _v25_tok in (
        ".global aurora_gpsp_ps2_cc_asm",
        ".global aurora_gpsp_ps2_mix_asm",
        ".global aurora_gpsp_ps2_cc_mix_asm",
        ".global aurora_gpsp_ps2_sound_drain_asm",
        "addiu   $8, $0, 9600",
        "lui     $15, 0x7bde",
    ):
        if _v25_tok not in _v25_asm_check:
            raise SystemExit("gpSP V25 ASM audit failed: missing " + _v25_tok)

    # AURORA_GPSP_GBA_V26_NUMERIC_GPR_FIX_20260929: numeric-register-only ABI audit.  gpSP's own MIPS stub
    # documents $8-$15 as unsaved temporaries and $4-$7 as arguments.
    import re as _v26_re
    _v26_aliases = _v26_re.findall(r"\$[A-Za-z_][A-Za-z0-9_]*", _v25_asm_check)
    if _v26_aliases:
        raise SystemExit("gpSP V26 ASM ABI audit failed: symbolic GPR alias " +
                         ", ".join(sorted(set(_v26_aliases))))
    _v26_regs = {int(_r) for _r in _v26_re.findall(r"\$(\d+)", _v25_asm_check)}
    _v26_allowed = {0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 31}
    if not _v26_regs <= _v26_allowed:
        raise SystemExit("gpSP V26 ASM ABI audit failed: unexpected GPR(s) " +
                         str(sorted(_v26_regs - _v26_allowed)))

    _v25_lra = _v25_lr.read_text(encoding="utf-8")
    _v25_saudit = _v25_sound.read_text(encoding="utf-8")
    for _v25_tok in (
        "aurora_gpsp_ps2_cc_asm(dst, src, gba_cc_lut);",
        "aurora_gpsp_ps2_mix_asm(dst, src_curr, src_prev);",
        "aurora_gpsp_ps2_cc_mix_asm(dst, src_curr, src_prev, gba_cc_lut);",
        "aurora_gpsp_ps2_mix_done:",
        "aurora_gpsp_ps2_cc_mix_done:",
    ):
        if _v25_tok not in _v25_lra:
            raise SystemExit("gpSP V25 video audit failed: missing " + _v25_tok)
    if "aurora_gpsp_ps2_sound_drain_asm(" not in _v25_saudit:
        raise SystemExit("gpSP V25 audio audit failed: ASM bridge missing")

    patch_multicore_profiler(stage, profiler_header)

    # AURORA_UNUSED_GPSP_COLOR_LUT_PRUNE_V1_20261014
    # Aurora's PS2 frontend hard-wires gpsp_color_correction=disabled and
    # rejects dynamic override. Retain GBA frame mixing/RTC/TFA/serial/audio.
    # Apply AFTER the V25 ASM audits and external profiler instrumenter:
    # V25 originally generates unused CC and CC+MIX callsites, so merely
    # dropping gba_cc_lut.c would produce an unresolved global in the ELF.
    _prune_video = stage / "libretro/libretro.c"
    _prune_make = stage / "Makefile.common"
    _prune_asm = stage / "aurora_gpsp_ps2_hotpaths.S"

    def _prune_once(_data, _old, _new, _what):
        _num = _data.count(_old)
        if _num != 1:
            raise SystemExit("gpSP COLOR PRUNE: %s: expected 1 match; got %d" % (_what, _num))
        return _data.replace(_old, _new, 1)

    def _prune_region(_data, _start, _end, _what):
        if _data.count(_start) != 1 or _data.count(_end) != 1:
            raise SystemExit("gpSP COLOR PRUNE: missing/duplicate %s region" % _what)
        _a = _data.index(_start)
        _b = _data.index(_end, _a + len(_start))
        return _data[:_a] + _data[_b:]

    _px = _prune_video.read_text(encoding="utf-8")
    # AURORA_GPSP_LUT_V24_20261014: V13 LUT helper survived V23 CC-region pruning.
    _v24_dead_helper = (
        '/* AURORA_GPSP_GBA_V13_PS2_COLOR_CORRECTION_20260911 */\n'
        'static inline uint16_t aurora_gpsp_color_correct(uint16_t c)\n'
        '{\n'
        '#if defined(PS2) && defined(USE_XBGR1555_FORMAT)\n'
        '   return gba_cc_lut[c & 0x7FFFU];\n'
        '#else\n'
        '   return gba_cc_lut[((c & 0xFFC0U) >> 1) | (c & 0x1FU)];\n'
        '#endif\n'
        '}\n'
        '\n'
    )
    _px = _prune_once(_px, _v24_dead_helper, "", "unused V13 LUT helper")
    _px = _prune_once(_px, '#include "../gba_cc_lut.h"\n', '', 'LUT include')
    # AURORA_GPSP_MIX_V26_BOUNDARY_20261014: retain original V2 mix mask / #defines.
    _px = _prune_region(_px, 'static void video_post_process_cc(void)\n',
                       '/* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911', 'cc-only function')
    _px = _prune_region(_px, 'static void video_post_process_cc_mix(void)\n',
                       'static void init_post_processing(void)\n', 'cc+mix function')
    # Both CC function-pointer uses are removed. Preserve the existing
    # frame-mixing function, framebuffer swap and allocation-failure path.
    _px = _prune_once(_px,
        '      if (post_process_cc && mix_active)\n'
        '         video_post_process = video_post_process_cc_mix;\n'
        '      else if (post_process_cc)\n'
        '         video_post_process = video_post_process_cc;\n'
        '      else if (mix_active)\n'
        '         video_post_process = video_post_process_mix;\n',
        '      if (mix_active)\n'
        '         video_post_process = video_post_process_mix;\n',
        'postprocessing dispatch')
    _px = _prune_region(_px,
                       '   var.key              = "gpsp_color_correction";\n',
                       '   var.key               = "gpsp_frame_mixing";\n',
                       'color setting block')
    # The option is now fixed by construction, not merely convention.
    _px = _prune_once(_px,
         '   var.key               = "gpsp_frame_mixing";\n',
         '   post_process_cc_prev = post_process_cc;\n'
         '   post_process_cc = false; /* PS2: permanently unavailable */\n'
         '   var.key               = "gpsp_frame_mixing";\n',
         'fixed color choice')
    # Two unused declarations are generated for PS2 assembler helpers.
    _px = _prune_once(_px,
        'extern void aurora_gpsp_ps2_cc_asm(\n'
        '      u16 *dst, const u16 *src, const u16 *lut);\n',
        '', 'CC ASM prototype')
    _px = _prune_once(_px,
        'extern void aurora_gpsp_ps2_cc_mix_asm(\n'
        '      u16 *dst, const u16 *curr, const u16 *prev, const u16 *lut);\n',
        '', 'CC+mix ASM prototype')
    # AURORA_GPSP_LUT_V25_ORDER_20261014: all CC consumer functions MUST already be pruned.
    if "aurora_gpsp_color_correct" in _px:
        raise SystemExit("gpSP LUT V25: leftover color-correction consumer")
    for _forbidden in ('gba_cc_lut', 'video_post_process_cc_mix',
                       'video_post_process_cc(', 'aurora_gpsp_ps2_cc_asm',
                       'aurora_gpsp_ps2_cc_mix_asm'):
        if _forbidden in _px:
            raise SystemExit('gpSP COLOR PRUNE: surviving code reference: '+_forbidden)
    for _required in ('static void video_post_process_mix(void)',
                      'aurora_gpsp_ps2_mix_asm(', 'gpsp_frame_mixing',
                      'post_process_mix'):
        if _required not in _px:
            raise SystemExit('gpSP COLOR PRUNE: removed a required component: '+_required)
    _prune_video.write_text(_px, encoding="utf-8", newline="\n")

    _pm = _prune_make.read_text(encoding="utf-8")
    _pm = _prune_once(_pm, '             $(CORE_DIR)/gba_cc_lut.c \\\n',
                      '', 'LUT source object')
    if 'gba_cc_lut.c' in _pm:
        raise SystemExit('gpSP COLOR PRUNE: LUT still compiled')
    _prune_make.write_text(_pm, encoding="utf-8", newline="\n")

    # The CC ASM helpers only consume the LUT through an argument, not via
    # a global relocation. Still, remove both unused routines so the
    # .text section preserves *only* frame mixing and sound draining.
    _as = _prune_asm.read_text(encoding="utf-8")
    _as = _prune_region(_as,
        '/* void aurora_gpsp_ps2_cc_asm(',
        '/* void aurora_gpsp_ps2_mix_asm(', 'unused color ASM')
    _as = _prune_region(_as,
        '/* void aurora_gpsp_ps2_cc_mix_asm(',
        '/* void aurora_gpsp_ps2_sound_drain_asm(', 'unused color+mix ASM')
    for _forbidden in ('.global aurora_gpsp_ps2_cc_asm',
                       '.global aurora_gpsp_ps2_cc_mix_asm'):
        if _forbidden in _as:
            raise SystemExit('gpSP COLOR PRUNE: stranded ASM export: '+_forbidden)
    for _required in ('.global aurora_gpsp_ps2_mix_asm',
                      '.global aurora_gpsp_ps2_sound_drain_asm'):
        if _required not in _as:
            raise SystemExit('gpSP COLOR PRUNE: missing required ASM: '+_required)
    _prune_asm.write_text(_as, encoding="utf-8", newline="\n")

    stamp.write_text(digest + "\n", encoding="utf-8")
    print(f"[ gpSP stage ] V21: SMALL JIT memory + 4 MiB paged ROM + TFA pre-reserve (V15 SIO) + pager I/O + full PS2 JIT cache sync: {stage}")


if __name__ == "__main__":
    main()
