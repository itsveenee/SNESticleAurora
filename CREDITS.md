# SNESticle Aurora — Credits and Project Lineage

## SNESticle Aurora

**SNESticle Aurora** is maintained by **itsveenee**.

Aurora begins its own release series at **1.0.0**.

The Aurora project has its own name, releases, experiments and development direction while preserving the history and attribution of the projects on which it is based.

<!-- AURORA_BRANDING_CREDITS_V9_20260821 -->
## Name and branding

The **SNESticle Aurora** name, and **Aurora** when used as the identity of this emulator/project, are project-specific names and branding reserved by **Vinícius Nunes (`@itsveenee`)**. Rights granted under the applicable software licenses concern the code and do **not** grant a license to reuse that project identity for an unofficial fork, modified build, redistributed binary or derivative project. Unless separately authorized by @itsveenee, use a distinct name and branding. Factual provenance references such as **“based on SNESticle Aurora”** remain welcome. See [BRANDING.md](BRANDING.md).

This claim is limited to the **SNESticle Aurora / project-specific Aurora identity**. It does not claim ownership of **SNESticle** standing alone or of third-party names and marks.

## SNESticle Revive

SNESticle Aurora is based on **SNESticle Revive**, maintained by **ReyFxck (Thomas R.)**.

Huge thanks to ReyFxck for the work involved in bringing SNESticle back into active development, modernizing the PS2 project, and creating the base from which Aurora was developed.

Upstream project:

`ReyFxck/SNESticleRevive`

## Original SNESticle

The original **SNESticle** and its original codebase are by **Icer Addis**.

Huge thanks to Icer Addis for creating the original emulator.

Historical copyright notice:

`Copyright (c) 1997-2004 Icer Addis`

## Sharing changes between projects

Changes developed for SNESticle Aurora may be cherry-picked, pulled, adapted or otherwise incorporated into SNESticle Revive or other forks as permitted by the applicable licenses.

Likewise, Aurora may independently incorporate useful work from related projects when appropriate, without implying that the projects share a release schedule, version series or development direction.

## Emulator cores

<!-- AURORA_ALL_CORES_CREDITS_V6_20260824 -->
### QuickNES

The NES integration uses the pinned QuickNES/libretro fork at `src/third_party/quicknes`.

- **Original QuickNES / Nes_Emu:** Shay Green
- **Libretro QuickNES core:** libretro contributors
- **Aurora integration fork:** `itsveenee/QuickNES_Core`
- **Aurora PS2 bridge and integration:** Vinícius Nunes (`@itsveenee`)

Its bundled notices remain authoritative; the top-level GPLv2 mirror is `LICENSES/QuickNES-GPL-2.0.txt`.

### PicoDrive

The experimental Mega Drive / Genesis, Master System / Mark III, Game Gear, 32X and Sega CD integration uses the pinned PicoDrive submodule at `src/third_party/picodrive`.

- **Original/current PicoDrive lineage:** notaz, irixxxx and contributors
- **Libretro core work:** libretro/PicoDrive contributors
- **Aurora PS2 bridge and integration:** Vinícius Nunes (`@itsveenee`)

Sega CD is exposed through path-only `.cue` loading and user-provided regional BIOS files. See `THIRD_PARTY.md` and `LICENSES/PicoDrive-COPYING.txt`.

### Beetle PC Engine Fast

The experimental PC Engine / TurboGrafx-16 HuCard and PC Engine CD integration uses Beetle PC Engine Fast, the libretro port/fork of Mednafen PCE Fast.

- **Beetle PCE Fast / libretro core:** libretro contributors
- **Underlying PCE Fast lineage:** Mednafen and its contributors
- **Aurora integration fork:** `itsveenee/beetle-pce-fast-libretro`
- **Aurora PS2 bridge, integration and optimization:** Vinícius Nunes (`@itsveenee`)

PC Engine CD is exposed through path-only `.cue` loading and a user-provided `syscard3.pce`. See `LICENSES/Beetle-PCE-Fast-GPL-2.0.txt`.

### Gambatte

The embedded Game Boy backend used by Super Game Boy support is **Gambatte**, authored by **Sinamas** and contributors. Aurora provides the SNES/ICD2 bridge and SGB-specific integration.

