#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Aurora SGB Gambatte staged-source preparer.
# AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907:
# the pinned Gambatte fork itself is authoritative; staging only keeps build
# products out of the submodule working tree.

from pathlib import Path
import argparse
import shutil
import subprocess
import sys

STAGE_MARK = 'AURORA_SGB_GAMBATTE_STAGE_V7_FINAL_REALBOOT_SCALAR_20260908'  # AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
STAMP_NAME = '.aurora-gambatte-stage-v3'


def die(msg):
    print("ERRO:", msg, file=sys.stderr)
    raise SystemExit(1)


def run(cmd, cwd=None, capture=False):
    print("+", " ".join(str(x) for x in cmd))
    kw = dict(cwd=str(cwd) if cwd else None, check=False, text=True)
    if capture:
        kw.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p = subprocess.run(cmd, **kw)
    if p.returncode != 0:
        if capture:
            if p.stdout:
                print(p.stdout, file=sys.stderr)
            if p.stderr:
                print(p.stderr, file=sys.stderr)
        die("comando falhou (%d): %s" % (p.returncode, " ".join(cmd)))
    return p.stdout.strip() if capture else ""


def read(path):
    if not path.is_file():
        die("arquivo ausente: %s" % path)
    return path.read_text(encoding="utf-8")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    print("OK:", path)


def replace_once(text, old, new, label):
    if new in text:
        print("OK:", label, "já aplicado")
        return text
    n = text.count(old)
    if n != 1:
        die("%s: esperado exatamente 1 anchor antigo, encontrado %d" % (label, n))
    print("APPLY:", label)
    return text.replace(old, new, 1)


def patch_staged_gambatte(stage):
    # Public GB API -----------------------------------------------------------
    p = stage / "libgambatte/include/gambatte.h"
    s = read(p)

    old = '''\tlong runFor(gambatte::video_pixel_t *videoBuf, int pitch,
\t\t\tgambatte::uint_least32_t *soundBuf, std::size_t soundBufSize, unsigned &samples);
'''
    new = '''\tlong runFor(gambatte::video_pixel_t *videoBuf, int pitch,
\t\t\tgambatte::uint_least32_t *soundBuf, std::size_t soundBufSize, unsigned &samples);

   /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907
    * External-SFC clock entry point. Returns actual normal-GB clocks used;
    * Aurora keeps any instruction-boundary overshoot as scheduler credit. */
   unsigned long runForClocks(gambatte::video_pixel_t *videoBuf, int pitch,
         gambatte::uint_least32_t *soundBuf, std::size_t soundBufSize,
         unsigned long clocks, unsigned &samples);
'''
    s = replace_once(s, old, new, "gambatte.h runForClocks")

    old = '''\t/** Sets the callback used for getting input state. */
\tvoid setInputGetter(InputGetter *getInput);
'''
    new = '''\t/** Sets the callback used for getting input state. */
\tvoid setInputGetter(InputGetter *getInput);

   /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907
    * FF00/P14/P15 bridge used only by Aurora's external SGB ICD. */
   typedef unsigned char (*SgbJoypCallback)(
         void *userdata, unsigned char p14p15, bool write);
   void setSgbJoypCallback(SgbJoypCallback callback, void *userdata);

   bool savedataDirty() const;
   void clearSavedataDirty();
'''
    s = replace_once(s, old, new, "gambatte.h SGB/dirty API")
    write(p, s)

    # CPU real-progress accounting + forwarders ------------------------------
    p = stage / "libgambatte/src/cpu.h"
    s = read(p)

    old = '''\tlong runFor(unsigned long cycles);
\tvoid setStatePtrs(SaveState &state);
'''
    new = '''\tlong runFor(unsigned long cycles);
   unsigned long lastRunCycles() const { return lastRunCycles_; }
\tvoid setStatePtrs(SaveState &state);
'''
    s = replace_once(s, old, new, "cpu.h lastRunCycles")

    old = '''\tvoid setInputGetter(InputGetter *getInput) {
\t\tmem_.setInputGetter(getInput);
\t}
'''
    new = '''\tvoid setInputGetter(InputGetter *getInput) {
\t\tmem_.setInputGetter(getInput);
\t}

   void setSgbJoypCallback(
         unsigned char (*callback)(void *, unsigned char, bool),
         void *userdata) {
      mem_.setSgbJoypCallback(callback, userdata);
   }

   bool savedataDirty() const { return mem_.savedataDirty(); }
   void clearSavedataDirty() { mem_.clearSavedataDirty(); }
'''
    s = replace_once(s, old, new, "cpu.h SGB forwarders")

    old = '''\tunsigned long cycleCounter_;
\tunsigned short pc_;
'''
    new = '''\tunsigned long cycleCounter_;
   unsigned long lastRunCycles_; /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907 */
\tunsigned short pc_;
'''
    s = replace_once(s, old, new, "cpu.h progress member")
    write(p, s)

    p = stage / "libgambatte/src/cpu.cpp"
    s = read(p)

    old = ''', cycleCounter_(0)
, pc_(0x100)
'''
    new = ''', cycleCounter_(0)
, lastRunCycles_(0) /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907 */
, pc_(0x100)
'''
    s = replace_once(s, old, new, "cpu.cpp constructor")

    old = '''long CPU::runFor(unsigned long const cycles) {
\tprocess(cycles);

\tlong const csb = mem_.cyclesSinceBlit(cycleCounter_);

\tif (cycleCounter_ & 0x80000000)
\t\tcycleCounter_ = mem_.resetCounters(cycleCounter_);

\treturn csb;
}
'''
    new = '''long CPU::runFor(unsigned long const cycles) {
   /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907
    * process() may stop at frame completion and instructions may overshoot
    * the requested endpoint. Capture the real delta before counter rebasing. */
   unsigned long const before = cycleCounter_;
\tprocess(cycles);
   lastRunCycles_ = cycleCounter_ - before;

\tlong const csb = mem_.cyclesSinceBlit(cycleCounter_);

\tif (cycleCounter_ & 0x80000000)
\t\tcycleCounter_ = mem_.resetCounters(cycleCounter_);

\treturn csb;
}
'''
    s = replace_once(s, old, new, "cpu.cpp progress accounting")
    write(p, s)

    # Memory: JOYP bridge + cheap save dirty flag ----------------------------
    p = stage / "libgambatte/src/gambatte-memory.h"
    s = read(p)

    old = '''\tvoid write(unsigned p, unsigned data, unsigned long cc) {
\t\tif (cart_.wmem(p >> 12)) {
\t\t\tcart_.wmem(p >> 12)[p] = data;
\t\t} else
\t\t\tnontrivial_write(p, data, cc);
\t}
'''
    new = '''\tvoid write(unsigned p, unsigned data, unsigned long cc) {
      unsigned char *const wm = cart_.wmem(p >> 12);

      /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907
       * Mark battery/RTC writes where they happen instead of CRC-scanning
       * the complete save area on every frontend dirty query. */
      if (wm) {
         if (p >= 0xA000 && p < 0xC000 && wm[p] != (unsigned char)data)
            saveDataDirty_ = true;
         wm[p] = data;
      } else {
         if (p >= 0xA000 && p < 0xC000)
            saveDataDirty_ = true;
\t\t\tnontrivial_write(p, data, cc);
      }
\t}
'''
    s = replace_once(s, old, new, "memory.h dirty write path")

    old = '''\tvoid setInputGetter(InputGetter *getInput) { getInput_ = getInput; }
#ifdef HAVE_NETWORK
'''
    new = '''\tvoid setInputGetter(InputGetter *getInput) { getInput_ = getInput; }
   void setSgbJoypCallback(
         unsigned char (*callback)(void *, unsigned char, bool),
         void *userdata) {
      sgbJoypCallback_ = callback;
      sgbJoypUser_ = userdata;
   }
   bool savedataDirty() const { return saveDataDirty_; }
   void clearSavedataDirty() { saveDataDirty_ = false; }
#ifdef HAVE_NETWORK
'''
    s = replace_once(s, old, new, "memory.h SGB setters")

    old = '''\tInputGetter *getInput_;
\tunsigned long divLastUpdate_;
'''
    new = '''\tInputGetter *getInput_;
   unsigned char (*sgbJoypCallback_)(void *, unsigned char, bool);
   void *sgbJoypUser_;
   bool saveDataDirty_;
\tunsigned long divLastUpdate_;
'''
    s = replace_once(s, old, new, "memory.h SGB members")
    write(p, s)

    p = stage / "libgambatte/src/gambatte-memory.cpp"
    s = read(p)

    old = '''   getInput_(0)
#ifdef HAVE_NETWORK
, serial_io_(0)
#endif
, divLastUpdate_(0)
'''
    new = '''   getInput_(0)
, sgbJoypCallback_(0) /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907 */
, sgbJoypUser_(0)
, saveDataDirty_(false)
#ifdef HAVE_NETWORK
, serial_io_(0)
#endif
, divLastUpdate_(0)
'''
    s = replace_once(s, old, new, "memory.cpp constructor")

    old = '''void Memory::updateInput() {
\tunsigned state = 0xF;

\tif ((ioamhram_[0x100] & 0x30) != 0x30 && getInput_) {
\t\tunsigned input = (*getInput_)();
\t\tunsigned dpad_state = ~input >> 4;
\t\tunsigned button_state = ~input;
\t\tif (!(ioamhram_[0x100] & 0x10))
\t\t\tstate &= dpad_state;
\t\tif (!(ioamhram_[0x100] & 0x20))
\t\t\tstate &= button_state;
\t}

\tif (state != 0xF && (ioamhram_[0x100] & 0xF) == 0xF)
\t\tintreq_.flagIrq(0x10);

\tioamhram_[0x100] = (ioamhram_[0x100] & -0x10u) | state;
}
'''
    new = '''void Memory::updateInput() {
\tunsigned state = 0xF;

   /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907
    * ICD owns the low nibble in SGB mode. Reads are pure; packet parser state
    * advances only on actual FF00 writes below. */
   if (sgbJoypCallback_) {
      state = sgbJoypCallback_(
            sgbJoypUser_, ioamhram_[0x100] & 0x30, false) & 0x0F;
   } else if ((ioamhram_[0x100] & 0x30) != 0x30 && getInput_) {
\t\tunsigned input = (*getInput_)();
\t\tunsigned dpad_state = ~input >> 4;
\t\tunsigned button_state = ~input;
\t\tif (!(ioamhram_[0x100] & 0x10))
\t\t\tstate &= dpad_state;
\t\tif (!(ioamhram_[0x100] & 0x20))
\t\t\tstate &= button_state;
\t}

\tif (state != 0xF && (ioamhram_[0x100] & 0xF) == 0xF)
\t\tintreq_.flagIrq(0x10);

\tioamhram_[0x100] = (ioamhram_[0x100] & -0x10u) | state;
}
'''
    s = replace_once(s, old, new, "memory.cpp JOYP read")

    old = '''\tcase 0x00:
\t\tif ((data ^ ioamhram_[0x100]) & 0x30) {
\t\t\tioamhram_[0x100] = (ioamhram_[0x100] & ~0x30u) | (data & 0x30);
\t\t\tupdateInput();
\t\t}

\t\treturn;
'''
    new = '''\tcase 0x00:
      /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907
       * SGB packet bits are the sequence of P14/P15 writes. Report every FF00
       * write; coalescing equal selector values loses protocol semantics. */
      if (sgbJoypCallback_) {
         unsigned const oldLow = ioamhram_[0x100] & 0x0F;
         unsigned const state = sgbJoypCallback_(
               sgbJoypUser_, data & 0x30, true) & 0x0F;
         ioamhram_[0x100] =
               (ioamhram_[0x100] & ~0x3Fu) | (data & 0x30) | state;
         if (state != 0x0F && oldLow == 0x0F)
            intreq_.flagIrq(0x10);
      } else if ((data ^ ioamhram_[0x100]) & 0x30) {
\t\t\tioamhram_[0x100] = (ioamhram_[0x100] & ~0x30u) | (data & 0x30);
\t\t\tupdateInput();
\t\t}

\t\treturn;
'''
    s = replace_once(s, old, new, "memory.cpp JOYP write")
    write(p, s)

    # GB implementation ------------------------------------------------------
    p = stage / "libgambatte/src/gambatte.cpp"
    s = read(p)

    old = '''long GB::runFor(gambatte::video_pixel_t *const videoBuf, const int pitch,
\t\t\tgambatte::uint_least32_t *const soundBuf, std::size_t soundBufSize, unsigned &samples) {
\t
\tp_->cpu.setVideoBuffer(videoBuf, pitch);
\tp_->cpu.setSoundBuffer(soundBuf, soundBufSize);
\tconst long cyclesSinceBlit = p_->cpu.runFor(samples * 2);
\tsamples = p_->cpu.fillSoundBuffer();
\t
\treturn cyclesSinceBlit < 0 ? cyclesSinceBlit : static_cast<long>(samples) - (cyclesSinceBlit >> 1);
}
'''
    new = '''long GB::runFor(gambatte::video_pixel_t *const videoBuf, const int pitch,
\t\t\tgambatte::uint_least32_t *const soundBuf, std::size_t soundBufSize, unsigned &samples) {
\t
\tp_->cpu.setVideoBuffer(videoBuf, pitch);
\tp_->cpu.setSoundBuffer(soundBuf, soundBufSize);
\tconst long cyclesSinceBlit = p_->cpu.runFor(samples * 2);
\tsamples = p_->cpu.fillSoundBuffer();
\t
\treturn cyclesSinceBlit < 0 ? cyclesSinceBlit : static_cast<long>(samples) - (cyclesSinceBlit >> 1);
}

/* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907 */
unsigned long GB::runForClocks(
      gambatte::video_pixel_t *const videoBuf, const int pitch,
      gambatte::uint_least32_t *const soundBuf, std::size_t soundBufSize,
      const unsigned long clocks, unsigned &samples) {
   p_->cpu.setVideoBuffer(videoBuf, pitch);
   p_->cpu.setSoundBuffer(soundBuf, soundBufSize);
   p_->cpu.runFor(clocks);
   samples = p_->cpu.fillSoundBuffer();
   return p_->cpu.lastRunCycles();
}
'''
    s = replace_once(s, old, new, "gambatte.cpp runForClocks")

    old = '''void GB::setInputGetter(InputGetter *getInput) {
\tp_->cpu.setInputGetter(getInput);
}
'''
    new = '''void GB::setInputGetter(InputGetter *getInput) {
\tp_->cpu.setInputGetter(getInput);
}

void GB::setSgbJoypCallback(SgbJoypCallback callback, void *userdata) {
   p_->cpu.setSgbJoypCallback(callback, userdata);
}

bool GB::savedataDirty() const {
   return p_->cpu.savedataDirty();
}

void GB::clearSavedataDirty() {
   p_->cpu.clearSavedataDirty();
}
'''
    s = replace_once(s, old, new, "gambatte.cpp SGB API")

    old = '''   if (!failed) {
      p_->gbaCgbMode = flags & GBA_CGB;
      p_->full_init();
      p_->stateNo = 1;
   }
'''
    new = '''   if (!failed) {
      p_->gbaCgbMode = flags & GBA_CGB;
      p_->full_init();
      p_->cpu.clearSavedataDirty(); /* AURORA_SGB_JOYP_BRIDGE_V1_1_20260907 */
      p_->stateNo = 1;
   }
'''
    s = replace_once(s, old, new, "gambatte.cpp clear dirty on load")
    write(p, s)

    # AURORA_SGB_GAMBATTE_LINKFIX_V1_1_1_20260907
    # Aurora calls gambatte::GB directly; the generic libretro
    # frontend must not enter this multi-core executable.
    # AURORA_SGB_GAMBATTE_VIDEO_PERF_FIX_V1_1_3_20260907
    # Persistent video buffer. PPUFrameBuf::setBuf() resets fbline_, therefore
    # runForClocks() must never reattach the framebuffer on every tiny grant.
    p = stage / "libgambatte/include/gambatte.h"
    s = read(p)
    old = '''   void setSgbJoypCallback(SgbJoypCallback callback, void *userdata);

   bool savedataDirty() const;
'''
    new = '''   void setSgbJoypCallback(SgbJoypCallback callback, void *userdata);

   /* AURORA_SGB_GAMBATTE_VIDEO_PERF_FIX_V1_1_3_20260907 */
   void setSgbVideoBuffer(gambatte::video_pixel_t *videoBuf, int pitch);

   bool savedataDirty() const;
'''
    s = replace_once(s, old, new, "gambatte.h persistent video API")
    write(p, s)

    p = stage / "libgambatte/src/gambatte.cpp"
    s = read(p)
    old = '''unsigned long GB::runForClocks(
      gambatte::video_pixel_t *const videoBuf, const int pitch,
      gambatte::uint_least32_t *const soundBuf, std::size_t soundBufSize,
      const unsigned long clocks, unsigned &samples) {
   p_->cpu.setVideoBuffer(videoBuf, pitch);
   p_->cpu.setSoundBuffer(soundBuf, soundBufSize);
   p_->cpu.runFor(clocks);
   samples = p_->cpu.fillSoundBuffer();
   return p_->cpu.lastRunCycles();
}
'''
    new = '''unsigned long GB::runForClocks(
      gambatte::video_pixel_t *const videoBuf, const int pitch,
      gambatte::uint_least32_t *const soundBuf, std::size_t soundBufSize,
      const unsigned long clocks, unsigned &samples) {
   /* AURORA_SGB_GAMBATTE_VIDEO_PERF_FIX_V1_1_3_20260907
    * Video is attached persistently by setSgbVideoBuffer(). Reattaching here
    * resets PPUFrameBuf::fbline_ to the null scratch row mid-scanline. */
   (void)videoBuf;
   (void)pitch;
   p_->cpu.setSoundBuffer(soundBuf, soundBufSize);
   p_->cpu.runFor(clocks);
   samples = p_->cpu.fillSoundBuffer();
   return p_->cpu.lastRunCycles();
}
'''
    s = replace_once(s, old, new, "gambatte.cpp persistent runForClocks video")

    old = '''void GB::setSgbJoypCallback(SgbJoypCallback callback, void *userdata) {
   p_->cpu.setSgbJoypCallback(callback, userdata);
}

bool GB::savedataDirty() const {
'''
    new = '''void GB::setSgbJoypCallback(SgbJoypCallback callback, void *userdata) {
   p_->cpu.setSgbJoypCallback(callback, userdata);
}

void GB::setSgbVideoBuffer(gambatte::video_pixel_t *videoBuf, int pitch) {
   p_->cpu.setVideoBuffer(videoBuf, pitch);
}

bool GB::savedataDirty() const {
'''
    s = replace_once(s, old, new, "gambatte.cpp persistent video setter")
    write(p, s)

    # AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907
    # bsnes-plus/Gambatte contract:
    # callback occurs AFTER LY advances. Aurora then commits the PREVIOUS
    # completed 8-line tile row from the persistent 160x144 framebuffer.

    p = stage / "libgambatte/src/video.cpp"
    s = read(p)

    old = """#include <string>

namespace gambatte
{
"""
    new = """#include <string>

/* AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907
 * Mirrors the bsnes-plus Gambatte scanline callback phase: report the
 * NEW LY after doLyCountEvent(), not an in-progress/completed row pointer. */
extern "C" void AuroraGambatteSgbNewLy(unsigned line);

namespace gambatte
{
"""
    s = replace_once(s, old, new, "video.cpp bsnes-plus LY callback declaration")

    old = """      case LY_COUNT:
         ppu_.doLyCountEvent();
         eventTimes_.set<LY_COUNT>(ppu_.lyCounter().time());
         break;
"""
    new = """      case LY_COUNT:
         ppu_.doLyCountEvent();
         eventTimes_.set<LY_COUNT>(ppu_.lyCounter().time());

         /* AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907
          * The callback receives the newly-entered LY. When this crosses
          * an 8-line boundary, Aurora packs the previous complete block. */
         AuroraGambatteSgbNewLy(ppu_.lyCounter().ly());
         break;
"""
    s = replace_once(s, old, new, "video.cpp post-LY bsnes-plus callback")
    write(p, s)

    # AURORA_SGB_GAMBATTE_RUMBLE_LINKFIX_V1_1_2_20260907
    # libretro.cpp used to provide cartridge_set_rumble(). The direct
    # Aurora GBHost has no Gambatte frontend rumble endpoint, so keep
    # MBC5 banking semantics and discard only the rumble side effect.
    p = stage / "libgambatte/src/mem/cartridge.cpp"
    s = read(p)
    old = "extern void cartridge_set_rumble(unsigned active);\n"
    new = (
        "static inline void cartridge_set_rumble(unsigned active) {\n"
        "   (void)active;\n"
        "} /* AURORA_SGB_GAMBATTE_RUMBLE_LINKFIX_V1_1_2_20260907 */\n"
    )
    s = replace_once(
        s, old, new,
        "cartridge.cpp MBC5 rumble no-op")
    write(p, s)

    p = stage / "Makefile.common"
    s = read(p)
    old = (
        "\t$(CORE_DIR)/video/sprite_mapper.cpp \\\n"
        "\t$(CORE_DIR)/../libretro/libretro.cpp\n"
    )
    new = "\t$(CORE_DIR)/video/sprite_mapper.cpp\n"
    s = replace_once(
        s, old, new,
        "Makefile.common remove generic libretro frontend")
    write(p, s)


def prepare_stage(source, stage):
    source = source.resolve()
    stage = stage.resolve()
    head = run(["git", "rev-parse", "HEAD"], cwd=source, capture=True)
    signature = STAGE_MARK + "\n" + head + "\n"
    stamp = stage / STAMP_NAME

    if stamp.is_file() and stamp.read_text(encoding="utf-8") == signature:
        print("[ Gambatte stage ] up to date:", head[:12])
        return

    if stage.exists():
        shutil.rmtree(stage)

    print("[ Gambatte stage ] copying clean source:", head[:12])
    shutil.copytree(
        source, stage,
        ignore=shutil.ignore_patterns(
            ".git", "*.o", "*.a", "*.so", "*.dll", "*.dylib"))

    # AURORA_SGB_CLASSIC_RGB32_V1_20260908
    # Keep the pinned Gambatte checkout pristine, but make the staged PS2
    # archive use Gambatte's normal u32 RGB framebuffer like bsnes-plus /
    # bsnes-classic. This changes only the generated build-tree copy.
    p = stage / "Makefile.libretro"
    s = read(p)
    old = (
        "   PLATFORM_DEFINES := -DPS2 -DVIDEO_SGB_SHADE8 "
        "# AURORA_SGB_GAMBATTE_SHADE8_JOYP_SYNC_PERF_V3_20260908\n"
    )
    new = (
        "   PLATFORM_DEFINES := -DPS2 "
        "# AURORA_SGB_CLASSIC_RGB32_V1_20260908\n"
    )
    s = replace_once(
        s, old, new,
        "Makefile.libretro classic RGB32 SGB framebuffer")
    write(p, s)

    # AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
    # Historical Gambatte/bsnes DMG tile gather, staged only for PS2.
    p = stage / "libgambatte/src/video/ppu.cpp"
    s = read(p)
    old = """\t\t\t\t/* DMG fast path: a precomputed expansion of
\t\t\t\t * bgPalette[0..3] (slot 0 of bgPaletteExpanded)
\t\t\t\t * turns the 8-dependent-load gather (gcc 13 -O3
\t\t\t\t * refuses to vectorise -- SSE2 has no u32 gather,
\t\t\t\t * so the missed log shows
\t\t\t\t * \"no vectype for stmt: _ = bgPalette[_];\" at every
\t\t\t\t * call site) into two 16-byte memcpys.  The
\t\t\t\t * expansion is rebuilt by LCD on every BG palette
\t\t\t\t * write via refreshBgPaletteExpansion(0). */
\t\t\t\t{
\t\t\t\t\tunsigned const lo = ntileword & 0xFF;
\t\t\t\t\tunsigned const hi = ntileword >> 8;
\t\t\t\t\tstd::memcpy(&dst[0], p.bgPaletteExpanded[0][lo], 4 * sizeof(video_pixel_t));
\t\t\t\t\tstd::memcpy(&dst[4], p.bgPaletteExpanded[0][hi], 4 * sizeof(video_pixel_t));
\t\t\t\t}
"""
    new = """\t\t\t\t/* AURORA_SGB_CLASSIC_SCALAR_PPU_V7_FINAL_20260908 */
\t\t\t\tdst[0] = p.bgPalette[ ntileword & 0x0003       ];
\t\t\t\tdst[1] = p.bgPalette[(ntileword & 0x000C) >>  2];
\t\t\t\tdst[2] = p.bgPalette[(ntileword & 0x0030) >>  4];
\t\t\t\tdst[3] = p.bgPalette[(ntileword & 0x00C0) >>  6];
\t\t\t\tdst[4] = p.bgPalette[(ntileword & 0x0300) >>  8];
\t\t\t\tdst[5] = p.bgPalette[(ntileword & 0x0C00) >> 10];
\t\t\t\tdst[6] = p.bgPalette[(ntileword & 0x3000) >> 12];
\t\t\t\tdst[7] = p.bgPalette[ ntileword           >> 14];
"""
    s = replace_once(s, old, new, "ppu.cpp historical scalar DMG gather")
    write(p, s)

    # RGB32 + scalar gather are staging policy; pinned submodule stays pristine.
    stamp.write_text(signature, encoding="utf-8")
    print("[ Gambatte stage ] real-boot/classic scalar fork staged:", STAGE_MARK)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prepare", action="store_true")
    ap.add_argument("--source")
    ap.add_argument("--stage")
    args = ap.parse_args()
    if not args.prepare or not args.source or not args.stage:
        die("uso: --prepare --source <gambatte> --stage <build/gambatte-src>")
    prepare_stage(Path(args.source), Path(args.stage))


if __name__ == "__main__":
    main()
