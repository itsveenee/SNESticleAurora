# Third-party components

SNESticle Aurora contains or integrates code from third-party projects. Their original license files and source-file notices remain authoritative.

<!-- AURORA_BRANDING_THIRD_PARTY_V9_20260821 -->
Third-party licenses govern their respective code and assets; they do **not** grant rights to use the **SNESticle Aurora** name or the project-specific **Aurora** branding for unofficial forks or redistributed builds. Conversely, Aurora's branding policy does not alter any third-party license. See [BRANDING.md](BRANDING.md).

## Emulator cores

<!-- AURORA_ALL_CORES_LICENSES_V6_20260824 -->
### QuickNES

Aurora integrates the pinned `itsveenee/QuickNES_Core` Git submodule at `src/third_party/quicknes`, based on QuickNES / Nes_Emu by Shay Green and the libretro core maintained by libretro contributors. Preserve all notices in the submodule. Its GNU GPLv2 text is mirrored at `LICENSES/QuickNES-GPL-2.0.txt`.

<!-- AURORA_FCEUMM_FDS_THIRD_PARTY_V1 -->
### FCEUmm (Famicom Disk System re-integration — WIP)

Aurora integrates the pinned `itsveenee/Fceumm-PS2` Git submodule at `src/third_party/fceumm-fds`. The current Aurora re-integration is **experimental and not working yet**. It is deliberately built as an FDS-only embedded core; NES/Famicom cartridge emulation remains on QuickNES.

FCEUmm is distributed under the GNU GPLv2. Preserve the original source-file notices and the submodule's `Copying`; a verbatim mirror of that GPLv2 text is included at `LICENSES/FCEUmm-GPL-2.0.txt`.

The Famicom Disk System BIOS is copyrighted firmware and is not distributed by Aurora. A user-supplied `disksys.rom` is expected in `SNESticle/SYSTEM` for this experimental integration.

### PicoDrive

Aurora builds the pinned `itsveenee/picodrive` submodule at `src/third_party/picodrive`, from the PicoDrive lineage by notaz, irixxxx and contributors. Aurora-owned PS2 bridge code is maintained in the parent repository; all upstream source headers and notices remain authoritative.

The component's `COPYING` is mirrored at `LICENSES/PicoDrive-COPYING.txt`. It contains non-commercial/additional redistribution conditions and a complete-source requirement. Aurora links PicoDrive statically into the same ELF; because those restrictions may be incompatible with GPLv2-only combination/redistribution, do not redistribute a PicoDrive-enabled combined binary unless you have confirmed separate permission or another lawful basis.

Aurora V6 exposes Sega CD only as path-based `.cue`. BIOS files are not distributed. PicoDrive searches `SNESticle/SYSTEM` for these base names with `.bin` (preferred) or `.zip`:

- USA: `us_scd2_9306`, `SegaCDBIOS9303`, `us_scd1_9210`, `bios_CD_U`
- Europe: `eu_mcd2_9306`, `eu_mcd2_9303`, `eu_mcd1_9210`, `bios_CD_E`
- Japan: `jp_mcd2_921222`, `jp_mcd1_9112`, `jp_mcd1_9111`, `bios_CD_J`

### Beetle PC Engine Fast

Aurora integrates the pinned `itsveenee/beetle-pce-fast-libretro` submodule at `src/third_party/beetle-pce-fast`, based on `libretro/beetle-pce-fast-libretro` and Mednafen PCE Fast. Aurora's fork contains PS2-specific integration and optimization; publish its modified source before updating the parent repository's gitlink.

The component's GNU GPLv2 `COPYING` is mirrored verbatim at `LICENSES/Beetle-PCE-Fast-GPL-2.0.txt`. Preserve all original source-file notices.

Aurora V6 exposes HuCard and path-based PC Engine CD `.cue` loading. PC Engine CD requires the user-provided `syscard3.pce` in `SNESticle/SYSTEM`. No firmware is included.

### CD image scope and firmware

The PS2 frontend exposes `.cue` only. CHD is deliberately not registered: enabling libchdr would also link compression libraries that have not been measured safely inside the EE's 32 MiB memory budget. CUE-referenced BIN/audio files remain on storage and are streamed by the cores. Sega CD and PC Engine CD BIOS files are copyrighted firmware supplied by the user and are not part of this repository.

## Other components

Aurora also contains or integrates InfoNES, miniz, libxmp-lite, PS2SDK-related libraries and other components inherited from SNESticle/SNESticle Revive. Refer to each component's bundled license and source-file notices.

### m5x7 font

`assets/font/m5x7.ttf` is the **m5x7** font by **Daniel Linssen**, released under **CC0 1.0 Universal**. Attribution is not required by CC0, but is appreciated by the author.

<!-- AURORA_GAMBATTE_LICENSE_V1_20260907 -->
### Gambatte (Game Boy core for Super Game Boy)

Gambatte is authored by **Sinamas** and contributors and is distributed under the **GNU GPL version 2**. Aurora integrates the pinned `itsveenee/gambatte-libretro` submodule at `src/third_party/gambatte` for the Game Boy CPU/PPU/APU/MBC side of the Super Game Boy bridge. Aurora continues to emulate the SNES and ICD2 side. A license mirror is included at `LICENSES/Gambatte-GPL-2.0.txt`.



<!-- AURORA_GPSP_GBA_V1_20260911 -->
### gpSP (Game Boy Advance)

Aurora integrates the pinned `itsveenee/gpsp` Git submodule at
`src/third_party/gpsp`. gpSP/gameplaySP was originally developed by **Exophase**
and has subsequent work from libretro contributors and other maintainers. The
PS2 build uses gpSP's existing `platform=ps2` MIPS/R5900 dynamic recompiler and
is linked into Aurora as a namespaced static core.

The component is distributed under the GNU GPL version 2 or later as described
by its source headers and `COPYING`. The exact `COPYING` from the pinned
submodule is mirrored at `LICENSES/gpSP-GPL-2.0.txt`; source-file notices in the
submodule remain authoritative.

Aurora does not distribute Nintendo GBA firmware. If the user supplies
`gba_bios.bin` in `SNESticle/SYSTEM`, gpSP's normal auto-BIOS path can use it;
otherwise the core falls back to its bundled open replacement BIOS. Aurora's
staged PS2 build caps gpSP's resident ROM page cache at 8 MiB; this is not a ROM
size limit, because larger cartridges use gpSP's existing file-backed 32 KiB
LRU paging path. Direct `.gba`/`.agb` path loading therefore also avoids a
second full-ROM copy in Aurora.
