# Game Boy Advance / gpSP integration

This directory contains the Aurora-owned `GpSPSystem` adapter for the pinned
`itsveenee/gpsp` submodule at `src/third_party/gpsp`.

The integration deliberately mirrors the standalone Gambatte architecture:
Aurora owns the browser, PS2 input, GS presentation, audio mixer, SRAM and
savestate UI. gpSP is embedded as a namespaced static core and is driven one
frame at a time through its stable core API; it is **not** exposed as a
RetroArch-style frontend.

## Controls

- Cross: GBA A
- Square: GBA B
- Circle: Turbo A
- Triangle: Turbo B
- L1 / R1: GBA L / R
- D-pad, Start and Select: direct mapping

Turbo timing comes from gpSP's native Turbo A/B implementation (`period=4`).

## Video

The complete 240x160 GBA frame is nearest-neighbour fitted into Aurora's
256x240 logical gameplay canvas. The PS2 core's `USE_XBGR1555_FORMAT` output
is decoded explicitly before writing Aurora RGBA8 pixels. Aspect ratio is preserved, nothing is cropped,
and the unused rows/columns are cleared to opaque black every new video frame.
This keeps Aurora's existing PS2/CRT presentation path and its 240p/interlaced
mode handling intact.

## Audio / BIOS / saves

Audio is requested at 32768 Hz and passed as stereo PCM to Aurora's
`CMixBuffer`. `SNESticle/SYSTEM/gba_bios.bin` is used automatically when a
valid official BIOS is present; otherwise the gpSP built-in open BIOS is used.
The first integration exposes gpSP's standard 128 KiB libretro save-RAM window
to Aurora's save system and gpSP's native serialization to Aurora savestates.

Aurora's staged PS2 gpSP build uses an 8 MiB resident ROM page cache. This is
not a cartridge-size ceiling: gpSP keeps larger cartridge files open and pages
32 KiB blocks through its LRU cache, so 16/32 MiB GBA ROMs remain supported
without consuming the corresponding amount of PS2 RAM. `.gba` and `.agb` stay
path-loaded directly; compressed GBA ROMs remain intentionally unregistered so
Aurora never duplicates a large cartridge in its generic ROM buffer.


## Turbo File Advance + interframe blending (v2)

<!-- AURORA_GPSP_GBA_V2_TFA_BLEND_20260911 -->
Aurora emulates the Sammy/ASCII **Turbo File Advance** only when the raw GBA
image CRC32 exactly matches a supported clean dump:

- `9746EF12` — Derby Stallion Advance (Japan)
- `E7FC81D0` — RPG Tsukuru Advance (Japan)

The accessory follows the Turbo File GB byte protocol over GBA Normal 8-bit
external-clock SIO. It implements commands 10/20/22/23/24/30/40 and the TFA-only
`0x34` 64-byte block-fill command. One physical-style 2 MiB image is shared by
both games at `GBA/ASCII Turbo File Advance.tfa`: banks 00-7F are the 1 MiB
internal flash and banks 80-FF are the inserted 1 MiB card. The accessory image
is persistent media, not savestate payload.

Interframe blending is enabled by default for Aurora GBA. gpSP already has an
optimized one-pass 50:50 mixer with pointer swapping instead of a full-frame
history memcpy. Two 240x161x16-bit auxiliary buffers cost about 151 KiB; the
mix itself touches roughly 225 KiB of pixel data per displayed frame (~13.1
MiB/s near 60 fps), which is small compared with GBA emulation/rendering work.
The staged PS2 build also fixes gpSP's averaging LSB mask from RGB565 `0x0821`
to XBGR1555 `0x0421`, matching the actual `platform=ps2` framebuffer format.


## PS2 video/runtime + RTC/state baseline (v13)

<!-- AURORA_GPSP_GBA_V13_SAFE_PERF_20260911 -->
The v13 baseline replaces v3-v12 and is intended to be applied directly over v2.
The white/slow/wide report came from v1, so v2's Turbo File Advance work is
preserved while the base frontend is tightened. V13 also corrects standalone
Gambatte GB/GBC presentation without changing emulated framebuffer contents:
480i/1080i use centred integer 2x2 square-pixel presentation, while 240p uses
a dedicated uniform PCRTC pixel width instead of uneven resampling. Dynamic SGB
keeps its existing SNES/TV presentation. V13 also initializes each shared
Gambatte target surface to opaque black once per session and disables the
full-width fast-clear assumption only for interlaced square-pixel GB/GBC, so
neither logical margins nor the new physical side bars can expose stale pixels.
The interlaced 2x2 transform is transient and wraps only the handheld game
`PolyRect`; it is restored before Aurora draws overlays, status text or modals.

- GBA/gpSP changes remain cumulative:

- The staged PS2 core caps gpSP's resident ROM page cache at 8 MiB instead of
  16 MiB. This is not a cartridge-size limit: larger 16/32 MiB ROMs use the
  core's existing 32 KiB LRU/file-backed paging path.
- gpSP's PS2 `USE_XBGR1555_FORMAT` framebuffer is native 0BGR555
  (B10-14/G5-9/R0-4). Aurora preserves the working V1/V2 final 32-bit output;
  the bridge names/placement are clarified rather than flipping extraction alone.
- Aurora's 256x240 gameplay raster is presented as 4:3. A native 3:2 GBA image
  therefore uses a 256x213 logical viewport, centered with opaque black bars.
- nearest-neighbour X/Y source indices are cached; repeated vertical rows are
  copied after conversion, and no 64-bit division remains in the per-pixel
  frame hot loop.
- the staged gpSP framebuffer starts black; post-process/history buffers also
  start black before the first blended frame.
- interframe blending remains ON by default, retaining v2's PS2 15-bit-safe
  averaging mask; v13 keeps the history/output initial fill black.
- gpSP PCM uses Aurora's `AudMixBuffer::OutputLibretroInterleaved()` path
  directly instead of V1's temporary left/right deinterleave buffers.
- Shoulder turbo is PS2-local: **R2+L1 = Turbo L** and **R2+L2 = Turbo R**.
- **Safe Frameskip** now reaches gpSP's own scanline skip when Aurora passes a
  NULL render target. CPU/timers/audio still run, while interframe-blending
  history is invalidated and reseeded on the next visible frame so stale
  ghosting cannot appear after a skipped frame.
- **Color Correction** is enabled by default. At staging time its upstream
  RGB555-index/RGB565-output table is permuted into native BGR555-index and
  BGR555-output order, so runtime remains a direct lookup; brightness is not altered.
  Aurora transports the chord requests through private virtual L3/R3 signals;
  staged gpSP applies its native default 2-frames-on/2-frames-off cadence and
  serializes both shoulder-turbo counters in savestates.
- RTC remains auto-detected per cartridge and uses the PS2/system wall clock
  (`gpsp_rtc_time_source=system`).
- gpSP serialize/unserialize is wired into Aurora's compressed/CRC'd state
  pipeline as GBA state-system id 11. Turbo File Advance stays external
  physical media and is deliberately not rolled back with a state.
- gpSP's libretro input-bitmask capability is enabled, so normal gameplay input
  is sampled in one packed callback instead of repeated scalar button queries.
- the PS2 interframe blend processes two native 0BGR555 pixels per 32-bit operation;
  the packed formula is bit-exact to v2's 0x0421 scalar rounded average.
- the PS2 dynarec uses 4 MiB ROM + 512 KiB RAM translation caches. Together
  with the 8 MiB ROM page-cache cap, this is still below v1/v2's worst-case
  resident-cache envelope while reducing code-cache churn.
- newly emitted dynarec code uses `SyncDCache(start,end)` for the changed data
  range while retaining gpSP's full instruction-cache invalidate.
- the native 240->256 horizontal scaler uses the exact 15->16 mapping and tiny
  32-entry channel LUTs, reducing colour conversions without changing pixels.


## Hardware correction v14

<!-- AURORA_GPSP_GBA_V14_VISIBLE_BIOS_SQUARE_20260911 -->
V14 is incremental over an already-applied v13. Hardware testing showed smooth
GBA audio but a white/stale picture. The cause was frontend presentation, not
gpSP execution: GBA fell through Aurora's generic/SNES `MainLoopProcess()` path,
which executes the system but does not upload a generic RGBA `CRenderSurface` to
`_OutTex`. GBA now has an explicit software-video branch, parallel to standalone
Gambatte, and calls `TextureUpload()` on every presented frame. Safe Frameskip
still passes a NULL target and intentionally skips both core video work and the
upload for discarded frames.

The frontend also now returns `gpsp_boot_mode=bios` instead of `game`.
`gpsp_bios=auto` is retained: `SYSTEM/gba_bios.bin` is used when valid and gpSP
falls back to its built-in BIOS otherwise. A real Nintendo boot animation
therefore requires the official `gba_bios.bin`; the execution path itself no
longer bypasses BIOS startup.

GBA video remains native 240x160 and is centered at (8,40) inside Aurora's
256x240 software canvas. Physical aspect is handled by the v13 square-pixel
backend: exact 2x2 (480x320) in the 640x480 interlaced source framebuffer and
uniform PCRTC pixel width in 240p. No 240->256 or 160->213 framebuffer scaling
is performed. The gpSP PS2 framebuffer is native 0BGR555 and is expanded to
Aurora's established 0xAABBGGRR software-surface packing, matching the existing
Gambatte/SGB path.
