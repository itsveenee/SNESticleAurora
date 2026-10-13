# SNESticle Aurora — In development

**SNESticle Aurora** is a PlayStation 2 emulator fork maintained by **@itsveenee**, focused on 240p CRT output, real PS2 hardware, accuracy and compatibility improvements, and other hardware-specific or experimental ideas.

SNESticle Aurora is based on **SNESticle Revive by @ReyFxck (Thomas R.)**, whose work brought SNESticle back into active development, and ultimately on the original **SNESticle by Icer Addis**. Huge thanks to @ReyFxck for his work on SNESticle Revive and for providing the foundation from which Aurora was created and Icer Addis for creating the original SNESticle and its codebase.

NES emulation through **QuickNES** and **FCEUmm** (for Disk System only) is based on: the **QuickNES core originally by Shay Green**, with the libretro core maintained by **libretro contributors**, using `itsveenee/QuickNES_Core` as a pinned Git submodule for its PS2 integration (see [THIRD_PARTY.md](THIRD_PARTY.md) and `LICENSES/QuickNES-GPL-2.0.txt`); **FCEUmm**, using the pinned `itsveenee/Fceumm-PS2` Git submodule at `src/third_party/fceumm-fds` (FDS firmware is **not included**: users must provide `disksys.rom` in `SNESticle/SYSTEM`) (see [THIRD_PARTY.md](THIRD_PARTY.md) and `LICENSES/FCEUmm-GPL-2.0.txt`).

Mega Drive, Master System, Game Gear and Sega CD emulation through **PicoDrive** is based on the emulator originally by **notaz**, with current PicoDrive/libretro work by **irixxxx and other contributors**. For CD games, users must provide a matching regional BIOS in `SNESticle/SYSTEM`. See [THIRD_PARTY.md](THIRD_PARTY.md) and `LICENSES/PicoDrive-COPYING.txt`.

PC Engine / TurboGrafx-16 emulation through **Beetle PC Engine Fast** is based on the libretro port/fork of **Mednafen PCE Fast**, maintained by libretro and Mednafen contributors. Aurora uses `itsveenee/beetle-pce-fast-libretro` with PS2-specific integration and optimization. For CD games, firmware must be user-supplied in `SNESticle/SYSTEM`. See [THIRD_PARTY.md](THIRD_PARTY.md) and `LICENSES/Beetle-PCE-Fast-GPL-2.0.txt`.

Game Boy and Color emulation use **Gambatte**, an open-source Game Boy / Game Boy Color emulator originally developed by **Sinamas**, with subsequent development and maintenance by **additional contributors**. Firmware must be user-supplied in `SNESticle/SYSTEM`. See THIRD_PARTY.md and LICENSES/Gambatte-GPL-2.0.txt for attribution, licensing information, and additional details.

Game Boy Advance emulation uses the pinned **gpSP** fork at `src/third_party/gpsp`, based on gameplaySP by **Exophase** and later libretro contributors. Aurora embeds its existing PS2 dynarec target behind an `Emu::System` adapter. Firmware must be user-supplied in `SNESticle/SYSTEM`. See [THIRD_PARTY.md](THIRD_PARTY.md) and `LICENSES/gpSP-GPL-2.0.txt`.

SNESticle Aurora code covered by the GPL remains under GNU GPLv2; separately licensed third-party components remain under their own terms. **Code license and project branding are separate.** The applicable software licenses grant rights in the code; they **do not grant permission to use the SNESticle Aurora name** or the project-specific **Aurora** identity/branding for an unofficial fork, modified build, redistributed binary, or derivative project. Unless separately authorized by **@itsveenee**, use a distinct project/product name and distinct branding. Factual attribution such as “based on SNESticle Aurora” remains welcome. See [BRANDING.md](BRANDING.md).

Project lineage and attribution are documented in [CREDITS.md](CREDITS.md). See LICENSE, [BRANDING.md](BRANDING.md), and the third-party license files for licensing details.

Keep in mind: this project uses **AI-generated code**, but the changes are tested by me, and I'm a Human according to reCAPTCHA.


## Building from Git

Clone SNESticle Aurora together with its pinned third-party cores:

```bash
git clone --recurse-submodules https://github.com/itsveenee/SNESticleAurora.git
cd SNESticleAurora
git submodule update --init --recursive
make
```

If the repository was cloned without `--recurse-submodules`, run
`git submodule update --init --recursive` afterwards.

## BIOS and firmware
* Aurora creates `SYSTEM` under the active SNESticle data root. Firmware and BIOS files are **not included**.
* For PC Engine CD, place `syscard3.pce` in `SYSTEM`.
* For Sega CD, place the corresponding region BIOS (use PicoDrive as a reference) in `SYSTEM`.
* For GBC and GBA, place `cgb_boot.bin` and `gba_bios.bin` in `SYSTEM`.
* For Famicom Disk System, place `disksys.rom` in `SYSTEM`.

## What's new?

#### Emulation:
* Support added for more NES mappers: 13, 16, 18, 27, 48, 64, 65, 67, 68, 72, 77, 80, 82, 92, 96, 99, 101, 105, 118, 119, 151, 153, 155, 157, 158, 159, 162, 163, 164, 185, 188, 210, 216, and 552. Every licensed NES and Famicom game and most of the bootleg and unlicensed games will boot now.
* Famicom Disk System (firmware not included), press L2+TRIANGLE to change the disk side.
* Famiclone audio option for NES games (swap duty cycles, a known hardware bug in some Famiclones you can intentionally turn on).
* Changed SRAM and RAM initialization for both NES and SNES. This will fix all the very few games that rely on specific initial values to work properly.
* Fixes and improvements for the 240p display modes, improved screen positioning and overscan settings for each system and graphical resolution/modes.
* Dedicated turbo buttons for NES, GB, GBC, PCE, GG and SMS games (circle and triangle).
* Turbo input (hold R2+ANY BUTTON).
* In-game soft reset (L2+SELECT).
* SNES and MD mouse emulation.
* ASCII Turbo File for Famicom, Super Famicom, Game Boy and Game Boy Advance (accessory for many ASCII games).
* Battle Box for Famicom (accessory for Armadillo).
* Arkanoid Pad for Famicom (accessory for Arkanoid and Arkanoid II).
* Famicom microphone (L2+START).
* NES Zapper / Famicom Light Gun (X to shoot, L2+SQUARE to simulate shooting away from the screen).
* PC Engine 6-button pad *(don't use it with 2-button games!)*
* PC Engine alternative video modes (Ninja Spirits, Aoi Blink, Toumaden, Puyo Puyo and more).
* Region select (all consoles).

#### User interface:
* Save SRAM and states to USB.
* Browse SRAM and state files.
* Confirmation prompt for saving and loading states.
* Faster UI navigation.
* Many options to enable emulation hacks and compatibility modes (exchange accuracy for performance or vice-versa).
* Option to reload the emulator's .elf (very useful for upgrading and testing new builds).

 *(**NOTE**: to find the options above, go to the "Settings menu" and change the pages with the circle and square buttons.)*

#### To be added:
* More light guns support (SNES, MD, SMS).
* SMS paddle controller.
* SMB connection and MMCE support *(current implementations do not work).*
* 32X emulation *(currently disabled).*
* Other stupid *(or not-so-stupid)* ideas I might come up with. Thanks!

## Emulation progress

#### Fixed:
* Pilotwings (SNES) and Secret of Mana (SNES) mode 7 rendering, also fixes other games that rely on it.
* Secret of Mana (SNES) mode 5 rendering, also fixes other games that rely on it.
* Addams Family (SNES) graphical glitches and timing issues fixed.
* Sunset Riders (SNES) graphical glitches fixed.
* Super Mario All-Stars (SNES) graphical glitches fixed.
* Sonic Blast Man (SNES) wrong colors fixed.
* Ys (SMS) graphical glitches fixed.
* J.R.R. Tolkien's The Lord of the Rings (SNES) black screen fixed.
* Kishin Douji Zenki: Tenchi Meidou (SNES) black screen fixed.
* The Lost Vikings 1 and 2 (SNES) black screen fixed.
* Blackthorne (SNES) black screen fixed.
* Magical Pop'n (SNES) freeze fixed.
* Accele Brid (SNES) freeze fixed.
* Killer Instinct (SNES) freeze fixed.
* Super Mario Advance (GBA) freeze fixed.
* Many other graphical glitches and inaccuracies on many games and emulated systems were fixed.

#### To be fixed:
* Top Gear (SNES) performance *(in progress, safe frameskip helps a lot).*
* Speedy Gonzales in Los Gatos Banditos (SNES) performance *(in progress, safe frameskip won't help much).*
* SNES SA-1 Emulation (experimental state, unplayable).
* SNES FX1 and FX2 emulation (experimental state, unplayable).
* PC Engine CD and Sega CD performance (experimental state, many games are playable).
* Krazy Creatures (NES) minor graphical glitches.
* Any other games with performance or graphical issues.

## Special thanks
* Pavel (@eXo12): invaluable beta testing, feedbacks and motivation which helped me immensely. Thanks!
* Aurora: it's a secret to everybody.
