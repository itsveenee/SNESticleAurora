# Third-party licenses — SNESticle Aurora

This directory preserves third-party license texts. **Copyright notices in
source files and in the pinned submodules remain authoritative.** These mirrors
do not replace those notices or change any original license terms.

| Component | Mirrored license | Source of the mirror |
| --- | --- | --- |
| QuickNES (NES) | [GNU GPLv2](QuickNES-GPL-2.0.txt) | `src/third_party/quicknes/LICENSE` |
| FCEUmm (FDS) | [GNU GPLv2](FCEUmm-GPL-2.0.txt) | `src/third_party/fceumm-fds/Copying` |
| PicoDrive (MD/SMS/GG/32X/Sega CD) | [PicoDrive COPYING](PicoDrive-COPYING.txt) | `src/third_party/picodrive/COPYING` |
| Beetle PCE Fast (PCE/PCE-CD) | [GNU GPLv2](Beetle-PCE-Fast-GPL-2.0.txt) | `src/third_party/beetle-pce-fast/COPYING` |
| Gambatte (GB/GBC/SGB backend) | [GNU GPLv2](Gambatte-GPL-2.0.txt) | `src/third_party/gambatte/COPYING` |
| gpSP / gameplaySP (GBA) | [GNU GPLv2 text](gpSP-GPL-2.0.txt) | `src/third_party/gpsp/COPYING` |
| ZSNES DSP-3 HLE (SNES) | [GNU GPLv2](ZSNES-DSP3-GPL-2.0.txt) | The pinned upstream `COPYING` documented in `THIRD_PARTY.md` |

**gpSP:** Original gameplaySP by Exophase; later libretro/gpSP contributors.
Its source headers and `COPYING` govern the terms; do not omit their notices.
The repository describes the component as GPLv2 or later where so stated in
its source headers. The mirrored file reproduces the GPLv2 text itself.

**PicoDrive:** Its `COPYING` contains separate restrictions, including
non-commercial conditions. A statically linked, PicoDrive-enabled combined
ELF may present a GPLv2 compatibility problem; do not assume that a GPLv2
license file grants redistribution rights for the whole binary. See
[`THIRD_PARTY.md`](../THIRD_PARTY.md).

Do not add proprietary BIOS/firmware, ROMs or disc images to this directory.
For lineage and other notices, see [`CREDITS.md`](../CREDITS.md),
[`THIRD_PARTY.md`](../THIRD_PARTY.md) and the repository's root `LICENSE`.
