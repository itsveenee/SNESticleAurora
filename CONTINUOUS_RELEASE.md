<!-- AURORA_V8_RELEASE_CREDITS_20260915 -->
Automatically built from the latest commit on `main`.

The continuous package includes the checked-out source tree, manifest, checksums and third-party notices together with the experimental PS2 ELF. Current integrated/WIP cores include **QuickNES**, **FCEUmm/FDS**, **PicoDrive**, **Beetle PCE Fast**, **Gambatte** and **gpSP**.

## Credits and project lineage

- **SNESticle Aurora** is maintained by **Vinícius Nunes (`@itsveenee`)**.
- Aurora is based on **SNESticle Revive**, maintained by **ReyFxck (Thomas R.)**, whose modernization of the PS2 project is the base from which Aurora developed.
- The original **SNESticle** and its original codebase are by **Icer Addis** (`Copyright (c) 1997-2004 Icer Addis`).
- **QuickNES / Nes_Emu:** Shay Green; libretro QuickNES contributors; Aurora integration fork `itsveenee/QuickNES_Core`.
- **FCEUmm / FCEU lineage:** FCEU/FCE Ultra, FCEUX/FCEUmm and libretro contributors; Aurora integration fork `itsveenee/Fceumm-PS2`.
- **PicoDrive:** notaz, irixxxx and contributors; libretro/PicoDrive contributors.
- **Beetle PCE Fast:** libretro contributors, based on the Mednafen PCE Fast lineage.
- **Gambatte:** Sinamas and contributors.
- **gpSP / gameplaySP:** Exophase; later gpSP/libretro contributors.
- **PS2SDK and PS2 homebrew-toolchain contributors** provide the platform SDK, libraries and build infrastructure used by Aurora.

See [`CREDITS.md`](CREDITS.md) for the fuller project-specific acknowledgements and provenance.

## Third-party notices and licenses

The original source-file notices and each component's own license remain authoritative. The repository mirrors the relevant top-level notices under [`LICENSES/`](LICENSES/):

| Component | Notice / license mirrored by Aurora |
| --- | --- |
| QuickNES | `LICENSES/QuickNES-GPL-2.0.txt` — GNU GPLv2 |
| FCEUmm | `LICENSES/FCEUmm-GPL-2.0.txt` — GNU GPLv2 |
| PicoDrive | `LICENSES/PicoDrive-COPYING.txt` — PicoDrive's own COPYING, including additional/separate redistribution terms |
| Beetle PCE Fast | `LICENSES/Beetle-PCE-Fast-GPL-2.0.txt` — GNU GPLv2 |
| Gambatte | `LICENSES/Gambatte-GPL-2.0.txt` — GNU GPLv2 |
| gpSP | `LICENSES/gpSP-GPL-2.0.txt` — gpSP source/COPYING describes GNU GPLv2-or-later |
| ZSNES DSP-3 HLE | `LICENSES/ZSNES-DSP3-GPL-2.0.txt` — GNU GPLv2 |

**Important:** PicoDrive's `COPYING` contains additional/non-commercial redistribution conditions. Because Aurora links PicoDrive statically into the combined ELF, do not redistribute a PicoDrive-enabled combined binary unless you have confirmed separate permission or another lawful basis. See [`THIRD_PARTY.md`](THIRD_PARTY.md) for the repository's current component-by-component notices.

No Nintendo/Sega/NEC BIOS or firmware files, ROMs, disc images, or other copyrighted game content are distributed by Aurora. User-supplied firmware is required where documented.

Contact me on **Discord:** @itsveenee

The only file you need to play is **SNESticle.elf**.

* Please **DO NOT DISTRIBUTE THIS ELF:** it is still highly experimental and **updated almost daily**. Please do not upload or spread buggy builds across websites. **Share the link below to this GitHub page instead.**

* Por favor, **NÃO DISTRIBUA ESTE ELF:** ele ainda está em um estado altamente experimental e é **atualizado quase todos os dias**. Não faça upload e não espalhe versões com bugs em sites. **Compartilhe o link abaixo desta página do GitHub se precisar.**

* Por favor, **NO DISTRIBUYAS ESTE ELF:** todavía se encuentra en un estado altamente experimental y **se actualiza casi a diario**. Por favor, no subas ni difundas versiones con errores en sitios web. **Comparte el enlace abajo de esta página de GitHub si lo necesitas.**

https://github.com/itsveenee/SNESticleAurora/releases/tag/continuous

