


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "console.h"
#include "snppu.h"
#include "snppurender.h"
#include "snppuchrcache.h"
#include "rendersurface.h"
#include "snmask.h"
#include "prof.h"
#include "sndbglog.h"
#if AURORA_C4_PROFILER
#include "platform/ps2/system/aurora_c4_profiler.h"
#endif
#include "platform/ps2/system/aurora_snes_cost_profiler.h" /* AURORA_SNES_PPU_BREAKDOWN_V2_20260920 */
#include "platform/ps2/system/aurora_c4_profiler.h" /* AURORA_C4_CHR_SPLIT_PROFILER_V8_3_20261004 */
/* AURORA_C4_CHR_SPLIT_PROFILER_V8_3_4_REPAIR_20261004: wrapper-local reconstruction; diagnostic-only. */
#if AURORA_SMK_PROFILER
#include "platform/ps2/system/aurora_smk_profiler.h"
#endif
#if CODE_PLATFORM == CODE_PS2
#include "ps2mem.h" /* AURORA_DKC_SPR_LUT_MIRROR_V23_20260928_INCLUDE */
#endif

#define SNPPU_BGPLANE_SIZE 48
#define SNPPURENDER_CHR64 (TRUE)

/* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
/* AURORA_SNES_DEEP_BG_SCOPE_FIX_V3_14_5_20261006 */

static void _FetchMode7(Uint8 *pLine, SnesPPU *pPPU, Int32 iLine, SNMaskT *pPriority, SNMaskT *pOpaque);
static void _BuildMode7ExtBG(Uint8 *pLineBG2, SNMaskT *pPriority,
	SNMaskT *pOpaque, const Uint8 *pLineBG1);

#if AURORA_FRONTEND_PROFILER
/* AURORA_SNES_MICROSCOPE_V3_27_20261006 */
/* AURORA_CHR4_LOCALITY_MICROSCOPE_V3_24_20261006
 * Runs only after the already-timed BGCHR record at the callsite.  It therefore
 * does not contaminate SNES_BG_CHR measured decode/total.  Source-row math
 * mirrors the active CHR4 path.
 *
 * AURORA_CHR4_REUSE_MICROSCOPE_V3_28_20261007
 * The second lens correlates the previous OPAQUE row only.  This models a
 * persistent one-entry register cache that survives transparent rows, while
 * adjacent_* separately reports the easier no-gap case.  The pixel key adds
 * uPal&7 because the 4bpp color LUT duplicates entries 0..7 at 8..15 and bit 3
 * is priority.
 *
 * AURORA_CHR4_PATH_BG_MICROSCOPE_V3_29_20261007
 * Split the same correlation by BG0..3 and the actual 4bpp execution family.
 * Mode 5/6 use a two-character N/N+1 pair, so their decode key contains BOTH
 * resolved source-row addresses plus H-flip.  MAIN/SUB stay separate because
 * their hires packing semantics differ. */
enum
{
    AURORA_CHR4_PATH_NOOFFSET = 0,
    AURORA_CHR4_PATH_OFFSET = 1,
    AURORA_CHR4_PATH_MODE5_MAIN = 2,
    AURORA_CHR4_PATH_MODE5_SUB = 3,
    AURORA_CHR4_PATH_MODE6_MAIN = 4,
    AURORA_CHR4_PATH_MODE6_SUB = 5,
    AURORA_CHR4_PATH_COUNT = 6
};

static _INLINE Uint32 _AuroraChr4MainPath(Uint32 uBGMode, Bool bOffset)
{
    if (uBGMode == 5u) return AURORA_CHR4_PATH_MODE5_MAIN;
    if (uBGMode == 6u) return AURORA_CHR4_PATH_MODE6_MAIN;
    return bOffset ? AURORA_CHR4_PATH_OFFSET : AURORA_CHR4_PATH_NOOFFSET;
}

static _INLINE Uint32 _AuroraChr4SubPath(Uint32 uBGMode)
{
    return uBGMode == 5u
        ? AURORA_CHR4_PATH_MODE5_SUB : AURORA_CHR4_PATH_MODE6_SUB;
}

static _INLINE void _AuroraProfileCHR4Locality(
    const SnesBGInfoT *pBGInfo, const SnesRenderTileT *pTiles,
    Uint32 nTiles, Int32 iLine, const Uint8 *pMask,
    Uint32 uBGIndex, Uint32 uPathIndex, Bool bOffset)
{
    if (!pBGInfo || !pTiles || !pMask || nTiles == 0u ||
        uBGIndex >= 4u || uPathIndex >= AURORA_CHR4_PATH_COUNT)
        return;

    Int32 iSampleLine = iLine;
    if (pBGInfo->uMosaic > 0u)
    {
        const Int32 nMosaic = (Int32)pBGInfo->uMosaic + 1;
        iSampleLine /= nMosaic;
        iSampleLine *= nMosaic;
    }
    /* Match _FetchCHR_64: offset-per-tile paths pass uScrollY=0 and rely
     * on each tile's uOffsetY. */
    const Uint32 uScrollY = bOffset ? 0u :
        ((pBGInfo->uScrollY + (Uint32)iSampleLine) & 7u);
    const Bool bMode56 = uPathIndex >= AURORA_CHR4_PATH_MODE5_MAIN;

    Uint32 uOpaque = 0;
    Uint32 uPalReuse = 0;
    Uint32 uPalRuns = 0;
    Uint32 uFlipSame = 0;
    Uint32 uFlipChanges = 0;
    Uint32 uRowSame = 0;
    Uint32 uRowPlus16 = 0;
    Uint32 uRowMinus16 = 0;
    Uint32 uRowOther = 0;
    Uint32 uPrevRow = 0;
    Uint32 uPrevFlip = 0;
    Uint32 uLastOpaquePal = 0;
    Bool bHavePrev = FALSE;
    Bool bHaveOpaquePal = FALSE;

    Uint32 uOpaqueTransitions = 0;
    Uint32 uAdjacentOpaqueTransitions = 0;
    Uint32 uPalIndexReuse = 0;
    Uint32 uDecodeKeyReuse = 0;
    Uint32 uRowSameFlipChanged = 0;
    Uint32 uPixelKeyReuse = 0;
    Uint32 uAdjacentPalIndexReuse = 0;
    Uint32 uAdjacentDecodeKeyReuse = 0;
    Uint32 uAdjacentPixelKeyReuse = 0;
    Uint32 uPrevOpaqueRow0 = 0;
    Uint32 uPrevOpaqueRow1 = 0;
    Uint32 uPrevOpaqueHFlip = 0;
    Uint32 uPrevOpaquePalIndex = 0;
    Bool bHaveOpaqueKey = FALSE;
    Bool bPrevPhysicalOpaque = FALSE;

    for (Uint32 i = 0; i < nTiles; ++i)
    {
        const Uint32 uFlip = (Uint32)pTiles[i].uFlip & 3u;
        const Uint32 uHFlip = uFlip & 1u;
        Uint32 uRow0;
        Uint32 uRow1 = 0u;

        if (bMode56)
        {
            const Uint32 uTile0 = (Uint32)pTiles[i].uTile & 0x03ffu;
            const Uint32 uTile1 = (uTile0 + 1u) & 0x03ffu;
            Uint32 uSampleRow = (uScrollY + (Uint32)pTiles[i].uOffsetY) & 7u;
            if (uFlip & 2u)
                uSampleRow ^= 7u;
            /* Match _FetchCHR4Mode5_64 exactly: mask character base first,
             * then add the selected source row. */
            uRow0 = ((pBGInfo->uChrAddr + uTile0 * 16u) &
                SNPPU_VRAM_WORD_MASK) + uSampleRow;
            uRow1 = ((pBGInfo->uChrAddr + uTile1 * 16u) &
                SNPPU_VRAM_WORD_MASK) + uSampleRow;
        }
        else
        {
            const Uint32 uFlipXor = (uFlip & 2u) ? 7u : 0u;
            uRow0 = (pBGInfo->uChrAddr +
                ((Uint32)pTiles[i].uTile << 4) +
                ((uScrollY + (Uint32)pTiles[i].uOffsetY) ^ uFlipXor)) &
                SNPPU_VRAM_WORD_MASK;
        }

        if (bHavePrev)
        {
            if (uFlip == uPrevFlip) ++uFlipSame;
            else ++uFlipChanges;

            const Uint32 uDelta = (uRow0 - uPrevRow) & SNPPU_VRAM_WORD_MASK;
            if (uDelta == 0u) ++uRowSame;
            else if (uDelta == 16u) ++uRowPlus16;
            else if (uDelta == (SNPPU_VRAM_WORD_MASK + 1u - 16u)) ++uRowMinus16;
            else ++uRowOther;
        }
        bHavePrev = TRUE;
        uPrevFlip = uFlip;
        uPrevRow = uRow0;

        if (pMask[i] != 0u)
        {
            const Uint32 uPal = (Uint32)pTiles[i].uPal;
            const Uint32 uPalIndex = uPal & 7u;

            ++uOpaque;
            if (!bHaveOpaquePal || uPal != uLastOpaquePal)
                ++uPalRuns;
            else
                ++uPalReuse;
            bHaveOpaquePal = TRUE;
            uLastOpaquePal = uPal;

            if (bHaveOpaqueKey)
            {
                const Bool bSamePalIndex =
                    uPalIndex == uPrevOpaquePalIndex;
                const Bool bSameRows = uRow0 == uPrevOpaqueRow0 &&
                    (!bMode56 || uRow1 == uPrevOpaqueRow1);
                const Bool bSameHFlip = uHFlip == uPrevOpaqueHFlip;
                ++uOpaqueTransitions;
                if (bSamePalIndex) ++uPalIndexReuse;
                if (bSameRows && bSameHFlip) ++uDecodeKeyReuse;
                else if (bSameRows) ++uRowSameFlipChanged;
                if (bSameRows && bSameHFlip && bSamePalIndex)
                    ++uPixelKeyReuse;

                if (bPrevPhysicalOpaque)
                {
                    ++uAdjacentOpaqueTransitions;
                    if (bSamePalIndex) ++uAdjacentPalIndexReuse;
                    if (bSameRows && bSameHFlip)
                        ++uAdjacentDecodeKeyReuse;
                    if (bSameRows && bSameHFlip && bSamePalIndex)
                        ++uAdjacentPixelKeyReuse;
                }
            }

            bHaveOpaqueKey = TRUE;
            uPrevOpaqueRow0 = uRow0;
            uPrevOpaqueRow1 = uRow1;
            uPrevOpaqueHFlip = uHFlip;
            uPrevOpaquePalIndex = uPalIndex;
            bPrevPhysicalOpaque = TRUE;
        }
        else
        {
            bPrevPhysicalOpaque = FALSE;
        }
    }

    AuroraFrontendProfilerSnesChr4LocalityRecord(
        nTiles, uOpaque, uPalReuse, uPalRuns,
        uFlipSame, uFlipChanges,
        uRowSame, uRowPlus16, uRowMinus16, uRowOther);
    AuroraFrontendProfilerSnesChr4ReuseRecord(
        uBGIndex, uPathIndex, nTiles, uOpaque,
        uOpaqueTransitions, uAdjacentOpaqueTransitions,
        uPalIndexReuse, uDecodeKeyReuse,
        uRowSameFlipChanged, uPixelKeyReuse,
        uAdjacentPalIndexReuse, uAdjacentDecodeKeyReuse,
        uAdjacentPixelKeyReuse);
}
#endif

#if SNPPU_OBJ_CACHE || SNPPU_BG_CACHE
static SnesPPUChrCacheT _SnesPPU_ChrCache _ALIGN(64);
#endif

void SnesPPUInvalidateChrCache(Uint32 uWordAddress, Uint32 nWords)
{
#if SNPPU_OBJ_CACHE || SNPPU_BG_CACHE
	Uint32 nInvalidated = SnesPPUChrCacheInvalidateRange(
		&_SnesPPU_ChrCache, uWordAddress, nWords);
#if SNDBG_LOG
	g_DbgChrCacheInvalidations += nInvalidated;
#else
	(void)nInvalidated;
#endif
#else
	(void)uWordAddress;
	(void)nWords;
#endif
}

#if  !SNPPURENDER_CHR64

static Uint32 _SnesPPU_Tile2PalLookup[16]=
{
0x00000000 + 0x04040404 * 0,
0x00000000 + 0x04040404 * 1,
0x00000000 + 0x04040404 * 2,
0x00000000 + 0x04040404 * 3,
0x00000000 + 0x04040404 * 4,
0x00000000 + 0x04040404 * 5,
0x00000000 + 0x04040404 * 6,
0x00000000 + 0x04040404 * 7,
0x00000000 + 0x04040404 * 0,
0x00000000 + 0x04040404 * 1,
0x00000000 + 0x04040404 * 2,
0x00000000 + 0x04040404 * 3,
0x00000000 + 0x04040404 * 4,
0x00000000 + 0x04040404 * 5,
0x00000000 + 0x04040404 * 6,
0x00000000 + 0x04040404 * 7,
};



static Uint32 _SnesPPU_Tile4PalLookup[16]=
{
0x00000000 + 0x10101010 * 0,
0x00000000 + 0x10101010 * 1,
0x00000000 + 0x10101010 * 2,
0x00000000 + 0x10101010 * 3,
0x00000000 + 0x10101010 * 4,
0x00000000 + 0x10101010 * 5,
0x00000000 + 0x10101010 * 6,
0x00000000 + 0x10101010 * 7,
0x00000000 + 0x10101010 * 0,
0x00000000 + 0x10101010 * 1,
0x00000000 + 0x10101010 * 2,
0x00000000 + 0x10101010 * 3,
0x00000000 + 0x10101010 * 4,
0x00000000 + 0x10101010 * 5,
0x00000000 + 0x10101010 * 6,
0x00000000 + 0x10101010 * 7
};

#else

static Uint64 _SnesPPU_Tile2PalLookup64[4][16]=
{
	{
		0x00000000 + 0x0404040404040404 * 0,
		0x00000000 + 0x0404040404040404 * 1,
		0x00000000 + 0x0404040404040404 * 2,
		0x00000000 + 0x0404040404040404 * 3,
		0x00000000 + 0x0404040404040404 * 4,
		0x00000000 + 0x0404040404040404 * 5,
		0x00000000 + 0x0404040404040404 * 6,
		0x00000000 + 0x0404040404040404 * 7,
		0x00000000 + 0x0404040404040404 * 0,
		0x00000000 + 0x0404040404040404 * 1,
		0x00000000 + 0x0404040404040404 * 2,
		0x00000000 + 0x0404040404040404 * 3,
		0x00000000 + 0x0404040404040404 * 4,
		0x00000000 + 0x0404040404040404 * 5,
		0x00000000 + 0x0404040404040404 * 6,
		0x00000000 + 0x0404040404040404 * 7,
	},
	{
		0x2020202020202020 + 0x0404040404040404 * 0,
		0x2020202020202020 + 0x0404040404040404 * 1,
		0x2020202020202020 + 0x0404040404040404 * 2,
		0x2020202020202020 + 0x0404040404040404 * 3,
		0x2020202020202020 + 0x0404040404040404 * 4,
		0x2020202020202020 + 0x0404040404040404 * 5,
		0x2020202020202020 + 0x0404040404040404 * 6,
		0x2020202020202020 + 0x0404040404040404 * 7,
		0x2020202020202020 + 0x0404040404040404 * 0,
		0x2020202020202020 + 0x0404040404040404 * 1,
		0x2020202020202020 + 0x0404040404040404 * 2,
		0x2020202020202020 + 0x0404040404040404 * 3,
		0x2020202020202020 + 0x0404040404040404 * 4,
		0x2020202020202020 + 0x0404040404040404 * 5,
		0x2020202020202020 + 0x0404040404040404 * 6,
		0x2020202020202020 + 0x0404040404040404 * 7,
	},
	{
		0x4040404040404040 + 0x0404040404040404 * 0,
		0x4040404040404040 + 0x0404040404040404 * 1,
		0x4040404040404040 + 0x0404040404040404 * 2,
		0x4040404040404040 + 0x0404040404040404 * 3,
		0x4040404040404040 + 0x0404040404040404 * 4,
		0x4040404040404040 + 0x0404040404040404 * 5,
		0x4040404040404040 + 0x0404040404040404 * 6,
		0x4040404040404040 + 0x0404040404040404 * 7,
		0x4040404040404040 + 0x0404040404040404 * 0,
		0x4040404040404040 + 0x0404040404040404 * 1,
		0x4040404040404040 + 0x0404040404040404 * 2,
		0x4040404040404040 + 0x0404040404040404 * 3,
		0x4040404040404040 + 0x0404040404040404 * 4,
		0x4040404040404040 + 0x0404040404040404 * 5,
		0x4040404040404040 + 0x0404040404040404 * 6,
		0x4040404040404040 + 0x0404040404040404 * 7,
	},
	{
		0x6060606060606060 + 0x0404040404040404 * 0,
		0x6060606060606060 + 0x0404040404040404 * 1,
		0x6060606060606060 + 0x0404040404040404 * 2,
		0x6060606060606060 + 0x0404040404040404 * 3,
		0x6060606060606060 + 0x0404040404040404 * 4,
		0x6060606060606060 + 0x0404040404040404 * 5,
		0x6060606060606060 + 0x0404040404040404 * 6,
		0x6060606060606060 + 0x0404040404040404 * 7,
		0x6060606060606060 + 0x0404040404040404 * 0,
		0x6060606060606060 + 0x0404040404040404 * 1,
		0x6060606060606060 + 0x0404040404040404 * 2,
		0x6060606060606060 + 0x0404040404040404 * 3,
		0x6060606060606060 + 0x0404040404040404 * 4,
		0x6060606060606060 + 0x0404040404040404 * 5,
		0x6060606060606060 + 0x0404040404040404 * 6,
		0x6060606060606060 + 0x0404040404040404 * 7,
	},
};



static Uint64 _SnesPPU_Tile4PalLookup64[16]=
{
0x00000000 + 0x1010101010101010 * 0,
0x00000000 + 0x1010101010101010 * 1,
0x00000000 + 0x1010101010101010 * 2,
0x00000000 + 0x1010101010101010 * 3,
0x00000000 + 0x1010101010101010 * 4,
0x00000000 + 0x1010101010101010 * 5,
0x00000000 + 0x1010101010101010 * 6,
0x00000000 + 0x1010101010101010 * 7,
0x00000000 + 0x1010101010101010 * 0,
0x00000000 + 0x1010101010101010 * 1,
0x00000000 + 0x1010101010101010 * 2,
0x00000000 + 0x1010101010101010 * 3,
0x00000000 + 0x1010101010101010 * 4,
0x00000000 + 0x1010101010101010 * 5,
0x00000000 + 0x1010101010101010 * 6,
0x00000000 + 0x1010101010101010 * 7
};

#endif


static Uint32 _SnesPPU_Obj4PalLookup[8]=
{
	0x80808080 + 0x10101010 * 0,
	0x80808080 + 0x10101010 * 1,
	0x80808080 + 0x10101010 * 2,
	0x80808080 + 0x10101010 * 3,
	0x80808080 + 0x10101010 * 4,
	0x80808080 + 0x10101010 * 5,
	0x80808080 + 0x10101010 * 6,
	0x80808080 + 0x10101010 * 7
};




struct SNPPUBg8FlipT
{
	Uint32  uFlipXOR;
	Uint8   *pFlipLookup;
	SnesChrLookupT *pLookup;
	Uint32	pad;
};

static SNPPUBg8FlipT _FlipTable8[4]=
{
	{0, _SnesPPU_HFlipLookup[1], &_SnesPPU_PlaneLookup[0]},
	{0, _SnesPPU_HFlipLookup[0], &_SnesPPU_PlaneLookup[1]},
	{7, _SnesPPU_HFlipLookup[1], &_SnesPPU_PlaneLookup[0]},
	{7, _SnesPPU_HFlipLookup[0], &_SnesPPU_PlaneLookup[1]}
};

/* AURORA_PPU_R5900_DIRECT_BG_V5_20260927
 * Normal PS2 builds use the direct BG path (SNPPU_BG_CACHE=0). Keep all
 * emulated PPU decisions in C++; only the already-decided row expansion and
 * masked copy kernels cross this ABI boundary. The reference C++ paths remain
 * compiled when the switch is disabled or BG cache mode is selected. */
#ifndef AURORA_PPU_R5900_ASM
#define AURORA_PPU_R5900_ASM 1
#endif

#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
/* AURORA_DKC_SPR_LUT_MIRROR_V23_20260928_PTRS
 * Mirror layout is compile-time guarded by snppurender.cpp. */
#define AURORA_DKC_SPR_PLANE0 ((const Uint64 *)(PS2MEM_SCRATCHPAD + 8 * 1024))
#define AURORA_DKC_SPR_PLANE1 ((const Uint64 *)(PS2MEM_SCRATCHPAD + 10 * 1024))
#define AURORA_DKC_SPR_MASKREV ((const Uint8  *)(PS2MEM_SCRATCHPAD + 12 * 1024))

/* AURORA_DKC_CHR_PALETTE_SPR_V27_20260928
 * 12 KiB+256 .. +383 : 4bpp palette rows (16 * u64)
 * 12 KiB+384 .. +895 : four 2bpp palette banks (64 * u64)
 * Both are immutable host-derived LUTs and are refreshed per visible frame.
 */
#define AURORA_DKC_SPR_PAL4 ((const Uint64 *)(PS2MEM_SCRATCHPAD + 12 * 1024 + 256))
#define AURORA_DKC_SPR_PAL2 ((const Uint64 *)(PS2MEM_SCRATCHPAD + 12 * 1024 + 384))

typedef char AuroraDKCSprPal4SizeGuard[
    (sizeof(_SnesPPU_Tile4PalLookup64) == 128) ? 1 : -1];
typedef char AuroraDKCSprPal2SizeGuard[
    (sizeof(_SnesPPU_Tile2PalLookup64) == 512) ? 1 : -1];
typedef char AuroraDKCSprPaletteEndGuard[
    ((12 * 1024 + 384 + sizeof(_SnesPPU_Tile2PalLookup64)) <= 16 * 1024) ? 1 : -1];

void SnesPPURender8RefreshPaletteLutsScratchPS2(void)
{
    memcpy((void *)(PS2MEM_SCRATCHPAD + 12 * 1024 + 256),
           (const void *)&_SnesPPU_Tile4PalLookup64[0],
           sizeof(_SnesPPU_Tile4PalLookup64));
    memcpy((void *)(PS2MEM_SCRATCHPAD + 12 * 1024 + 384),
           (const void *)&_SnesPPU_Tile2PalLookup64[0][0],
           sizeof(_SnesPPU_Tile2PalLookup64));
}

struct AuroraPPUChrDirectAsmCtx
{
	const Uint64 *pPlaneNormal;
	const Uint64 *pPlaneHFlip;
	const Uint8  *pMaskReverse;
	const Uint64 *pPalLookup;
};

typedef char AuroraPPUTileAsmSizeCheck[(sizeof(SnesRenderTileT) == 6) ? 1 : -1];
typedef char AuroraPPUTileAsmPalCheck[(__builtin_offsetof(SnesRenderTileT, uPal) == 2) ? 1 : -1];
typedef char AuroraPPUTileAsmFlipCheck[(__builtin_offsetof(SnesRenderTileT, uFlip) == 3) ? 1 : -1];
typedef char AuroraPPUTileAsmOffsetYCheck[(__builtin_offsetof(SnesRenderTileT, uOffsetY) == 4) ? 1 : -1];
typedef char AuroraPPUCtxAsmSizeCheck[(sizeof(AuroraPPUChrDirectAsmCtx) == 16) ? 1 : -1];

extern "C" void AuroraPPUFetchCHR2Direct64PS2(
	const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles,
	Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask,
	const AuroraPPUChrDirectAsmCtx *pCtx);
extern "C" void AuroraPPUFetchCHR4Direct64PS2(
	const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles,
	Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask,
	const AuroraPPUChrDirectAsmCtx *pCtx);
/* AURORA_C4_BG_CHR_NOOFFSET_R5900_V8_4_FINAL_20261004
 * Ordinary non-OPT BG rows have uOffsetY == 0 by construction in FetchBG().
 * Use leaf R5900 specializations that consume the scanline row directly.
 * The generic kernels remain intact for offset-per-tile and all other cases. */
extern "C" void AuroraPPUFetchCHR2NoOffset64PS2(
	const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles,
	Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask,
	const AuroraPPUChrDirectAsmCtx *pCtx);
extern "C" void AuroraPPUFetchCHR4NoOffset64PS2(
	const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles,
	Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask,
	const AuroraPPUChrDirectAsmCtx *pCtx);
/* AURORA_PPU_R5900_CHR8_V16_20260928
 * 8bpp uses the same direct-row context as 2/4bpp; pPalLookup is unused. */
extern "C" void AuroraPPUFetchCHR8Direct64PS2(
	const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles,
	Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask,
	const AuroraPPUChrDirectAsmCtx *pCtx);
extern "C" void AuroraPPURenderBGDataOPS2(
	Uint8 *pLine8, Uint8 *pSrc8, const Uint8 *pMaskData,
	Uint32 uScrollX, Int32 nTiles, const Uint64 *pMaskLookup);
extern "C" void AuroraPPURenderBGDataPS2(
	Uint8 *pLine8, Uint8 *pSrc8, const Uint8 *pMaskData,
	Uint32 uScrollX, Int32 nTiles, const Uint64 *pMaskLookup);

/* AURORA_LUT_OBJ2_FINAL_V3_41_20261008
 * AURORA_OBJ_POSTFETCH_COMPACT_R5900_V3_40_20261008
 * ObjLine entries are exactly one quadword.  The assembly compactor only
 * removes rows whose renderer-visible opaque byte is zero; _FetchOBJ itself
 * remains untouched and therefore still consumes the physical 34-sliver
 * budget/cache sequence before compaction. */
typedef char AuroraPPUObjCompactSizeCheck[
	(sizeof(SnesRenderObj8T) == 16) ? 1 : -1];
typedef char AuroraPPUObjCompactOpaqueOffsetCheck[
	((__builtin_offsetof(SnesRenderObj8T, uData) + SNPPU_BGPLANE_OPAQUE) == 13) ? 1 : -1];
extern "C" Int32 AuroraPPUCompactOBJTransparentPS2(
	SnesRenderObj8T *pObjLine, Int32 nObjLine);

static const AuroraPPUChrDirectAsmCtx _AuroraPPUChr4DirectAsmCtx =
{
	AURORA_DKC_SPR_PLANE0,
	AURORA_DKC_SPR_PLANE1,
	AURORA_DKC_SPR_MASKREV,
	AURORA_DKC_SPR_PAL4
};
#endif


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////



static void _MosaicBG8(Uint8 *pLine, Int32 nPixels, Uint32 uMosaic)
{
	Int32 nMosaic;

	PROF_ENTER("MosiacBG8");

	switch (uMosaic)
	{
	case 2:
		while (nPixels > 0)
		{
			Uint8 uData0, uData1, uData2, uData3;
			uData0 = pLine[0];
			uData1 = pLine[2];
			uData2 = pLine[4];
			uData3 = pLine[6];
			pLine[1] = uData0;
			pLine[3] = uData1;
			pLine[5] = uData2;
			pLine[7] = uData3;
			pLine+=8;
			nPixels-=8;
		}
		break;


	default:
		while (nPixels > 0)
		{
			Uint8 uData;

			uData = pLine[0];
			nMosaic = uMosaic;

			while (nPixels > 0 && nMosaic > 0)
			{
				pLine[0] = uData;
				pLine++;
				nPixels--;
				nMosaic--;
			}
		}
		break;

	}

	PROF_LEAVE("MosiacBG8");
}


static void _MosaicBGPlanar(Uint8 *pLine, Int32 nTotalPixels, Uint32 uMosaic)
{
	Int32 nMosaic;
	Int32 nPixels=0;

	PROF_ENTER("MosiacBGPlanar");

	switch (uMosaic)
	{
	case 2:
		while (nTotalPixels > 0)
		{
			Uint8 uData;
			uData = pLine[0];
			uData &= 0x55;
			uData|= uData << 1;
			pLine[0] = uData;
			nTotalPixels -= 8;
			pLine++;
		}
		break;

	default:
		while (nPixels < nTotalPixels)
		{
			Uint8 uData;

			uData = (pLine[nPixels/8]>>(nPixels&7)) & 1;
			nMosaic = uMosaic;

			if (uData)
			{
				while (nPixels < nTotalPixels && nMosaic > 0)
				{
					pLine[nPixels/8] |= 1<<(nPixels&7);
					nPixels++;
					nMosaic--;
				}
			} else
			{
				while (nPixels < nTotalPixels && nMosaic > 0)
				{
					pLine[nPixels/8] &= ~(1<<(nPixels&7));
					nPixels++;
					nMosaic--;
				}
			}
		}
	}
	PROF_LEAVE("MosiacBGPlanar");
}




// render 8


void _ClearLine8(Uint8 *pLine8, Uint8 *pLineP, Int32 nPixels, Uint8 uColor, Uint32 uBGMask)
{
    /* AURORA_MEGA_V4_CLEARLINE_MEMSET
     * These are host-side byte planes only. The old loops assigned Uint8 on
     * every iteration, so memset with the same truncated byte is exactly the
     * same state and lets the PS2 libc use its tuned bulk store path. */
    if (nPixels > 0)
    {
        memset(pLine8, uColor, (size_t)nPixels);
        memset(pLineP, (Uint8)uBGMask, (size_t)nPixels);
    }
}

void _ClearLine8(Uint8 *pLine8, Int32 nPixels, Uint8 uColor)
{
    /* AURORA_MEGA_V4_CLEARLINE_MEMSET: byte-exact host-plane clear. */
    if (nPixels > 0)
        memset(pLine8, uColor, (size_t)nPixels);
}

#if !SNPPURENDER_CHR64

static void _FetchCHR2(Uint16 *pVram, Uint32 uBaseAddr, SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask)
{
	SNPPUBg8FlipT *pFlip;

	PROF_ENTER("_FetchCHR2");
	while (nTiles > 0)
	{
		SnesPPUTile2T *pTile2;
		Uint32 uTileAddr;
		Uint32 uPlane0, uPlane1;
		Uint32 uTile0, uTile1;
		Uint32 uMask;
		SnesChrLookupT *pLookup;
		Uint8 *pHFlip;

		pFlip = &_FlipTable8[pTiles->uFlip];

		// calculate tile address
		uTileAddr = (uBaseAddr + pTiles->uTile * 8) & 0x7FFF;

		// get pointer to tile data (y flipped)
		pTile2 = (SnesPPUTile2T *)(pVram + uTileAddr + (uScrollY ^ pFlip->uFlipXOR));

		// get tile plane bits
		uPlane0 = pTile2->uPlane01[0][0];
		uPlane1 = pTile2->uPlane01[0][1];

		pLookup = pFlip->pLookup;
		pHFlip  = pFlip->pFlipLookup;

		uMask = uPlane0 | uPlane1;
		uMask = pHFlip[uMask];

		pMask[ 0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] = (pTiles->uPal & 8) ? uMask : 0;
		pMask++;

		// get palette bits
		uTile0  =
		uTile1  = _SnesPPU_Tile2PalLookup[pTiles->uPal];

		// decode tile
		uTile0 |= (*pLookup)[uPlane0][0] << 0;
		uTile1 |= (*pLookup)[uPlane0][1] << 0;

		uTile0 |= (*pLookup)[uPlane1][0] << 1;
		uTile1 |= (*pLookup)[uPlane1][1] << 1;

		// store tile data
		((Uint32 *)pDest)[0] = uTile0;
		((Uint32 *)pDest)[1] = uTile1;

		pDest+=8;
		pTiles++;
		nTiles--;
	}
	PROF_LEAVE("_FetchCHR2");

}







static void _FetchCHR4(Uint16 *pVram, Uint32 uBaseAddr, SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask)
{
	SNPPUBg8FlipT *pFlip;

	PROF_ENTER("_FetchCHR4");

	while (nTiles > 0)
	{
		SnesPPUTile4T *pTile4;
		Uint32 uTileAddr;
		Uint32 uPlane0, uPlane1, uPlane2, uPlane3;
		Uint32 uTile0, uTile1;
		Uint32 uMask;
		SnesChrLookupT *pLookup;
		Uint8 *pHFlip;

		pFlip = &_FlipTable8[pTiles->uFlip];

		// calculate tile address
		uTileAddr = (uBaseAddr + pTiles->uTile * 16) & 0x7FFF;

		// get pointer to tile data (y flipped)
		pTile4 = (SnesPPUTile4T *)(pVram + uTileAddr + (uScrollY ^ pFlip->uFlipXOR));

		// get tile plane bits
		uPlane0 = pTile4->uPlane01[0][0];
		uPlane1 = pTile4->uPlane01[0][1];
		uPlane2 = pTile4->uPlane23[0][0];
		uPlane3 = pTile4->uPlane23[0][1];

		pLookup = pFlip->pLookup;
		pHFlip  = pFlip->pFlipLookup;


		// create mask
		uMask = uPlane0 | uPlane1 | uPlane2 | uPlane3;
		uMask = pHFlip[uMask];

		pMask[ 0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] = (pTiles->uPal & 8) ? uMask : 0;
		pMask++;

		// get palette bits
		uTile0  =
		uTile1  = _SnesPPU_Tile4PalLookup[pTiles->uPal];

		// decode tile
		uTile0 |= (*pLookup)[uPlane0][0] << 0;
		uTile1 |= (*pLookup)[uPlane0][1] << 0;

		uTile0 |= (*pLookup)[uPlane1][0] << 1;
		uTile1 |= (*pLookup)[uPlane1][1] << 1;

		uTile0 |= (*pLookup)[uPlane2][0] << 2;
		uTile1 |= (*pLookup)[uPlane2][1] << 2;

		uTile0 |= (*pLookup)[uPlane3][0] << 3;
		uTile1 |= (*pLookup)[uPlane3][1] << 3;

		// store tile data
		((Uint32 *)pDest)[0] = uTile0;
		((Uint32 *)pDest)[1] = uTile1;

		pDest+=8;
		pTiles++;
		nTiles--;
	}
	PROF_LEAVE("_FetchCHR4");
}




static void _FetchCHR(Uint8 *pLine, SnesPPU *pPPU, SnesBGInfoT *pBGInfo, struct SnesRenderTileT *pTiles, Int32 nTiles, Int32 iLine,Uint8 *pMask)
{
	Uint32 uScrollY;

	if (pBGInfo->uMosaic > 0)
	{
		iLine /= pBGInfo->uMosaic + 1;
		iLine *= pBGInfo->uMosaic + 1;
	}

	uScrollY = pBGInfo->uScrollY + iLine;
	switch (pBGInfo->uBitDepth)
	{
	case 2:
		// fetch chr (2-bit)
		_FetchCHR2(pPPU->GetVramPtr(0), pBGInfo->uChrAddr, pTiles, nTiles, uScrollY & 7, pLine, pMask);
		break;
	case 4:
		// fetch chr (4-bit)
		_FetchCHR4(pPPU->GetVramPtr(0), pBGInfo->uChrAddr, pTiles, nTiles, uScrollY & 7, pLine, pMask);
		break;
	}

	if (pBGInfo->uMosaic > 0)
	{
		_MosaicBG8(pLine + (pBGInfo->uScrollX & 7), 256, pBGInfo->uMosaic + 1);
	}

}


#else


//
//
//


#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM && !SNPPU_BG_CACHE
static void _FetchCHR2_64(const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask, Uint64 *pPalLookup)
{
	AuroraPPUChrDirectAsmCtx ctx =
	{
		AURORA_DKC_SPR_PLANE0,
		AURORA_DKC_SPR_PLANE1,
		AURORA_DKC_SPR_MASKREV,
		(const Uint64 *)pPalLookup
	};
	PROF_ENTER("_FetchCHR2_64");
#if AURORA_C4_PROFILER
	Uint32 c4ChrStart = 0;
	if (g_AuroraC4ProfilerActive)
		c4ChrStart = ProfCtrGetCycle();
#endif
	AuroraPPUFetchCHR2Direct64PS2(
		pVram, uBaseAddr, pTiles, nTiles, uScrollY, pDest, pMask, &ctx);
#if AURORA_C4_PROFILER
	if (g_AuroraC4ProfilerActive)
	{
		const Uint32 c4ChrCycles =
			(Uint32)(ProfCtrGetCycle() - c4ChrStart);
		AuroraC4ProfilerChrKernel(
			AURORA_C4_CHR2_GENERIC_OFFSET, c4ChrCycles,
			nTiles > 0 ? (Uint32)nTiles : 0u);
	}
#endif
	PROF_LEAVE("_FetchCHR2_64");
}

static void _FetchCHR2NoOffset_64(const Uint16 *pVram, Uint32 uBaseAddr,
	const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY,
	Uint8 *pDest, Uint8 *pMask, Uint64 *pPalLookup)
{
	AuroraPPUChrDirectAsmCtx ctx =
	{
		AURORA_DKC_SPR_PLANE0,
		AURORA_DKC_SPR_PLANE1,
		AURORA_DKC_SPR_MASKREV,
		(const Uint64 *)pPalLookup
	};
	PROF_ENTER("_FetchCHR2NoOffset_64");
#if AURORA_C4_PROFILER
	Uint32 c4ChrStart = 0;
	if (g_AuroraC4ProfilerActive)
		c4ChrStart = ProfCtrGetCycle();
#endif
	AuroraPPUFetchCHR2NoOffset64PS2(
		pVram, uBaseAddr, pTiles, nTiles, uScrollY, pDest, pMask, &ctx);
#if AURORA_C4_PROFILER
	if (g_AuroraC4ProfilerActive)
	{
		const Uint32 c4ChrCycles =
			(Uint32)(ProfCtrGetCycle() - c4ChrStart);
		AuroraC4ProfilerChrKernel(
			AURORA_C4_CHR2_NOOFFSET, c4ChrCycles,
			nTiles > 0 ? (Uint32)nTiles : 0u);
	}
#endif
	PROF_LEAVE("_FetchCHR2NoOffset_64");
}
#else
static void _FetchCHR2_64(const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask, Uint64 *pPalLookup)
{
	const SNPPUBg8FlipT *pFlip;

	PROF_ENTER("_FetchCHR2_64");
	while (nTiles > 0)
	{
		Uint32 uTileAddr, uRowAddr;
		Uint64 uTile0;
		Uint32 uMask;
#if !SNPPU_BG_CACHE
		Uint32 uMaskPal;
#endif

		pFlip = &_FlipTable8[pTiles->uFlip];

		// calculate tile address
		uTileAddr = (uBaseAddr + pTiles->uTile * 8) & 0x7FFF;

		// get pointer to tile data (y flipped)
		uRowAddr = uTileAddr +
			((uScrollY + pTiles->uOffsetY) ^ pFlip->uFlipXOR);

		#if SNPPU_BG_CACHE
		if (SnesPPUChrCacheLookup2(&_SnesPPU_ChrCache, uRowAddr,
			(pTiles->uFlip & 1) != 0, &uTile0, &uMask))
		{
			#if SNDBG_LOG
			g_DbgBGCacheHits++;
			#endif
		}
		else
		{
			const SnesPPUTile2T *pTile2 =
				(const SnesPPUTile2T *)(pVram + uRowAddr);
			const SnesChrLookup64T *pLookup =
				(const SnesChrLookup64T *)&_SnesPPU_PlaneLookup[0];
			Uint32 uPlane0 = pTile2->uPlane01[0][0];
			Uint32 uPlane1 = pTile2->uPlane01[0][1];

			#if SNDBG_LOG
			g_DbgBGCacheMisses++;
			#endif
			uMask = _SnesPPU_HFlipLookup[1][uPlane0 | uPlane1];
			uTile0  = (*pLookup)[uPlane0] << 0;
			uTile0 |= (*pLookup)[uPlane1] << 1;
			SnesPPUChrCacheStore2(&_SnesPPU_ChrCache, uRowAddr,
				uTile0, uMask);
			if (pTiles->uFlip & 1)
				SnesPPUChrCacheFlipRow(&uTile0, &uMask);
		}
		#else
		{
			/* AURORA_SNES_BG_CHR_HOTPATH_V1_20260920
			 * AURORA_SNES_BG_CHR_HOTPATH_V2_REVISED_20260920
			 *
			 * Keep V1's tiny _FlipTable8 indirection. The previous V2
			 * replaced it with per-tile arithmetic and increased register
			 * pressure/spills on MIPS. Reuse V1's existing nonblank branch.
			 * uMask is a bit permutation of the source-plane OR, therefore
			 * zero/nonzero is preserved exactly.
			 */
			const Uint16 uPair01 =
				pVram[uRowAddr & SNPPU_VRAM_WORD_MASK];
			Uint8 *pHFlip = pFlip->pFlipLookup;
			const Uint32 uPlane0 = (Uint32)(uPair01 & 0x00ffu);
			const Uint32 uPlane1 = (Uint32)(uPair01 >> 8);

			uMask = pHFlip[uPlane0 | uPlane1];
			if (uMask)
			{
				const SnesChrLookup64T *pLookup =
					(const SnesChrLookup64T *)pFlip->pLookup;
				const Uint32 uPal = (Uint32)pTiles->uPal;

				uTile0  = (*pLookup)[uPlane0] << 0;
				uTile0 |= (*pLookup)[uPlane1] << 1;
				uTile0 |= pPalLookup[uPal];
				uMaskPal = (uPal & 8u) ? uMask : 0;
			}
			else
			{
				uTile0 = 0;
				uMaskPal = 0;
			}
		}
		#endif

		#if SNPPU_BG_CACHE
		// Cache entries remain palette-independent.
		uTile0 |= pPalLookup[pTiles->uPal];

		pMask[0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] =
			(pTiles->uPal & 8) ? uMask : 0;
		#else
		// Direct path folded palette + priority-mask work into one uPal load.
		pMask[0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] = uMaskPal;
		#endif
		pMask++;

		// store tile data
		((Uint64 *)pDest)[0] = uTile0;

		pDest+=8;
		pTiles++;
		nTiles--;
	}
	PROF_LEAVE("_FetchCHR2_64");

}
#endif









#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM && !SNPPU_BG_CACHE
static void _FetchCHR4_64(const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask)
{
	PROF_ENTER("_FetchCHR4_64");
#if AURORA_C4_PROFILER
	Uint32 c4ChrStart = 0;
	if (g_AuroraC4ProfilerActive)
		c4ChrStart = ProfCtrGetCycle();
#endif
	AuroraPPUFetchCHR4Direct64PS2(
		pVram, uBaseAddr, pTiles, nTiles, uScrollY, pDest, pMask,
		&_AuroraPPUChr4DirectAsmCtx);
#if AURORA_C4_PROFILER
	if (g_AuroraC4ProfilerActive)
	{
		const Uint32 c4ChrCycles =
			(Uint32)(ProfCtrGetCycle() - c4ChrStart);
		AuroraC4ProfilerChrKernel(
			AURORA_C4_CHR4_GENERIC_OFFSET, c4ChrCycles,
			nTiles > 0 ? (Uint32)nTiles : 0u);
	}
#endif
	PROF_LEAVE("_FetchCHR4_64");
}

static void _FetchCHR4NoOffset_64(const Uint16 *pVram, Uint32 uBaseAddr,
	const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY,
	Uint8 *pDest, Uint8 *pMask)
{
	PROF_ENTER("_FetchCHR4NoOffset_64");
#if AURORA_C4_PROFILER
	Uint32 c4ChrStart = 0;
	if (g_AuroraC4ProfilerActive)
		c4ChrStart = ProfCtrGetCycle();
#endif
	AuroraPPUFetchCHR4NoOffset64PS2(
		pVram, uBaseAddr, pTiles, nTiles, uScrollY, pDest, pMask,
		&_AuroraPPUChr4DirectAsmCtx);
#if AURORA_C4_PROFILER
	if (g_AuroraC4ProfilerActive)
	{
		const Uint32 c4ChrCycles =
			(Uint32)(ProfCtrGetCycle() - c4ChrStart);
		AuroraC4ProfilerChrKernel(
			AURORA_C4_CHR4_NOOFFSET, c4ChrCycles,
			nTiles > 0 ? (Uint32)nTiles : 0u);
	}
#endif
#if AURORA_C4_PROFILER
	/* AURORA_C4_CHR4_DATA_MIX_DIAG_V8_3_MIX1_20261004: intentionally AFTER the raw ASM timer/recorder. */
	if (g_AuroraC4ProfilerActive && nTiles > 0)
	{
		Uint32 c4Flip[4]={0,0,0,0};
		Uint32 c4Transparent[4]={0,0,0,0};
		for (Int32 c4i=0;c4i<nTiles;c4i++)
		{
			const Uint32 c4f=(Uint32)(pTiles[c4i].uFlip & 3u);
			++c4Flip[c4f];
			if (pMask[c4i] == 0)
				++c4Transparent[c4f];
		}
		AuroraC4ProfilerChr4Mix(
			c4Flip[0],c4Flip[1],c4Flip[2],c4Flip[3],
			c4Transparent[0],c4Transparent[1],
			c4Transparent[2],c4Transparent[3]);
	}
#endif
	PROF_LEAVE("_FetchCHR4NoOffset_64");
}
#else
static void _FetchCHR4_64(const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask)
{
	const SNPPUBg8FlipT *pFlip;

	PROF_ENTER("_FetchCHR4_64");

	while (nTiles > 0)
	{
		Uint32 uTileAddr, uRowAddr;
		Uint64 uTile0;
		Uint32 uMask;
#if !SNPPU_BG_CACHE
		Uint32 uMaskPal;
#endif

		pFlip = &_FlipTable8[pTiles->uFlip];

		// calculate tile address
		uTileAddr = (uBaseAddr + pTiles->uTile * 16) & 0x7FFF;

		// get pointer to tile data (y flipped)
		uRowAddr = uTileAddr +
			((uScrollY + pTiles->uOffsetY) ^ pFlip->uFlipXOR);

		#if SNPPU_BG_CACHE
		if (SnesPPUChrCacheLookup4(&_SnesPPU_ChrCache, uRowAddr,
			(pTiles->uFlip & 1) != 0, &uTile0, &uMask))
		{
			#if SNDBG_LOG
			g_DbgBGCacheHits++;
			#endif
		}
		else
		{
			const SnesPPUTile4T *pTile4 =
				(const SnesPPUTile4T *)(pVram + uRowAddr);
			const SnesChrLookup64T *pLookup =
				(const SnesChrLookup64T *)&_SnesPPU_PlaneLookup[0];
			Uint32 uPlane0 = pTile4->uPlane01[0][0];
			Uint32 uPlane1 = pTile4->uPlane01[0][1];
			Uint32 uPlane2 = pTile4->uPlane23[0][0];
			Uint32 uPlane3 = pTile4->uPlane23[0][1];

			#if SNDBG_LOG
			g_DbgBGCacheMisses++;
			#endif
			uMask = _SnesPPU_HFlipLookup[1]
				[uPlane0 | uPlane1 | uPlane2 | uPlane3];
			uTile0  = (*pLookup)[uPlane0] << 0;
			uTile0 |= (*pLookup)[uPlane1] << 1;
			uTile0 |= (*pLookup)[uPlane2] << 2;
			uTile0 |= (*pLookup)[uPlane3] << 3;
			SnesPPUChrCacheStore4(&_SnesPPU_ChrCache, uRowAddr,
				uTile0, uMask);
			/* AURORA_BG_HFLIP_MISS_REUSE_V1 */
			if (pTiles->uFlip & 1)
			{
#if SNPPU_CHR_CACHE_HFLIP
				SnesPPUChrCacheLoad4HFlip(
					&_SnesPPU_ChrCache, uRowAddr, &uTile0, &uMask);
#else
				SnesPPUChrCacheFlipRow(&uTile0, &uMask);
#endif
			}
		}
		#else
		{
			/* AURORA_SNES_BG_CHR_HOTPATH_V1_20260920
			 * AURORA_SNES_BG_CHR_HOTPATH_V2_REVISED_20260920
			 *
			 * Same strategy as 2bpp: retain V1's _FlipTable8 lookup and its
			 * single decode branch. No restrict qualifiers, no derived H/V-flip
			 * temporaries, and no second per-tile branch.
			 */
			const Uint16 uPair01 =
				pVram[uRowAddr & SNPPU_VRAM_WORD_MASK];
			const Uint16 uPair23 =
				pVram[(uRowAddr + 8u) & SNPPU_VRAM_WORD_MASK];
			Uint8 *pHFlip = pFlip->pFlipLookup;
			const Uint32 uPlane0 = (Uint32)(uPair01 & 0x00ffu);
			const Uint32 uPlane1 = (Uint32)(uPair01 >> 8);
			const Uint32 uPlane2 = (Uint32)(uPair23 & 0x00ffu);
			const Uint32 uPlane3 = (Uint32)(uPair23 >> 8);

			uMask = pHFlip[uPlane0 | uPlane1 | uPlane2 | uPlane3];
			if (uMask)
			{
				const SnesChrLookup64T *pLookup =
					(const SnesChrLookup64T *)pFlip->pLookup;
				const Uint32 uPal = (Uint32)pTiles->uPal;

				uTile0  = (*pLookup)[uPlane0] << 0;
				uTile0 |= (*pLookup)[uPlane1] << 1;
				uTile0 |= (*pLookup)[uPlane2] << 2;
				uTile0 |= (*pLookup)[uPlane3] << 3;
				uTile0 |= _SnesPPU_Tile4PalLookup64[uPal];
				uMaskPal = (uPal & 8u) ? uMask : 0;
			}
			else
			{
				uTile0 = 0;
				uMaskPal = 0;
			}
		}
		#endif

		#if SNPPU_BG_CACHE
		// Cache entries remain palette-independent.
		uTile0 |= _SnesPPU_Tile4PalLookup64[pTiles->uPal];

		pMask[0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] =
			(pTiles->uPal & 8) ? uMask : 0;
		#else
		// Direct path folded palette + priority-mask work into one uPal load.
		pMask[0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] = uMaskPal;
		#endif
		pMask++;

		// store tile data
		((Uint64 *)pDest)[0] = uTile0;

		pDest+=8;
		pTiles++;
		nTiles--;
	}
	PROF_LEAVE("_FetchCHR4_64");
}
#endif


/* AURORA_ACCURACY_MODE5_HIRES_DECIMATE_V1_1_20260906
 *
 * Aurora's output stays 256 pixels wide, so represent each real 16-sample
 * Mode 5 character pair by one sharp 8-sample line. This is not a generic
 * scaler: it follows the PPU/Snes9x hires converter phases.
 *
 * Normal main-screen Mode 5 samples odd physical columns from N and N+1.
 * H-flip changes the converter to the even source phase, then reverses the
 * complete eight-sample result. Mosaic selects the even phase for the
 * non-flipped main converter as well.
 */

static _INLINE Uint32 _SnesPPUMode5Pack4(Uint64 uData, Bool bOdd)
{
	Uint32 uOut;

	if (bOdd)
		uData >>= 8;

	uOut  = (Uint32)((uData >>  0) & 0xFF) <<  0;
	uOut |= (Uint32)((uData >> 16) & 0xFF) <<  8;
	uOut |= (Uint32)((uData >> 32) & 0xFF) << 16;
	uOut |= (Uint32)((uData >> 48) & 0xFF) << 24;
	return uOut;
}


static _INLINE Uint8 _SnesPPUMode5PackMask4(Uint32 uMask, Bool bOdd)
{
	if (bOdd)
		uMask >>= 1;

	// Compress source mask bits 0,2,4,6 to destination bits 0..3.
	return (Uint8)(
		  ((uMask >> 0) & 1)
		| ((uMask >> 1) & 2)
		| ((uMask >> 2) & 4)
		| ((uMask >> 3) & 8)
	);
}


/* AURORA_MDR_MODE5_CLEAN_V2_1_20260907
 * Aurora outputs 256 logical pixels while Mode 5 carries two physical hires
 * dots per logical position. Keep V1.1's preferred phase whenever it is
 * opaque; only a transparent preferred sample may borrow the opposite phase.
 * This helper is reached only from the existing Mode-5 CHR path.
 */
static _INLINE Uint32 _SnesPPUMode5PreferCoverage4(
    Uint32 uPreferred,
    Uint32 uAlternate)
{
    Uint32 uOut = 0;
    Uint32 uP;

    uP = (uPreferred >> 0) & 0xFF;
    uOut |= (uP ? uP : ((uAlternate >> 0) & 0xFF)) << 0;
    uP = (uPreferred >> 8) & 0xFF;
    uOut |= (uP ? uP : ((uAlternate >> 8) & 0xFF)) << 8;
    uP = (uPreferred >> 16) & 0xFF;
    uOut |= (uP ? uP : ((uAlternate >> 16) & 0xFF)) << 16;
    uP = (uPreferred >> 24) & 0xFF;
    uOut |= (uP ? uP : ((uAlternate >> 24) & 0xFF)) << 24;
    return uOut;
}


static _INLINE Uint32 _SnesPPUMode5Pack4Coverage(
    Uint64 uData,
    Bool bPreferredOdd)
{
    Uint32 uPreferred = _SnesPPUMode5Pack4(uData, bPreferredOdd);
    Uint32 uAlternate = _SnesPPUMode5Pack4(uData, !bPreferredOdd);
    return _SnesPPUMode5PreferCoverage4(uPreferred, uAlternate);
}


static _INLINE Uint8 _SnesPPUMode5PackMask4Coverage(
    Uint32 uMask,
    Bool bPreferredOdd)
{
    return (Uint8)(
        _SnesPPUMode5PackMask4(uMask, bPreferredOdd) |
        _SnesPPUMode5PackMask4(uMask, !bPreferredOdd)
    );
}


/* AURORA_V7_MODE56_HIRES_20260915
 * Snes9x selects odd hires dots for the main BG converter and even dots
 * for the subscreen; H-flip swaps those phases, and mosaic uses even.
 *
 * Aurora's visible carrier is still 256-wide, so keep the established
 * coverage fallback for MAIN to avoid losing thin details. SUB, however,
 * is used as the real color-math source and must carry the exact even phase.
 */
static _INLINE Uint64 _SnesPPUMode5PackPair(
    Uint64 uRow0,
    Uint64 uRow1,
    Bool bHFlip,
    Bool bMosaic,
    Bool bSubscreen)
{
    Bool bPreferredOdd = (Bool)((!bSubscreen && !bMosaic) ^ bHFlip);
    Uint64 uOut;

    /* AURORA_SECRET_OF_MANA_MODE5_MAIN_COVERAGE_V7_1_20260929
     * Native Mode 5/6 contains two physical hires dots per Aurora output
     * sample. Preserve the exact SUB phase, but on MAIN restore the proven
     * coverage-preserving 256px reduction: the opposite phase is consulted
     * only when the preferred sample is transparent. A non-zero preferred
     * sample is never replaced. */
    if (bSubscreen)
    {
        uOut =
            (Uint64)_SnesPPUMode5Pack4(uRow0, bPreferredOdd) |
            ((Uint64)_SnesPPUMode5Pack4(uRow1, bPreferredOdd) << 32);
    }
    else
    {
        uOut =
            (Uint64)_SnesPPUMode5Pack4Coverage(uRow0, bPreferredOdd) |
            ((Uint64)_SnesPPUMode5Pack4Coverage(uRow1, bPreferredOdd) << 32);
    }

    if (bHFlip)
        uOut = SnesPPUChrCacheReverseBytes(uOut);
    return uOut;
}



static _INLINE Uint8 _SnesPPUMode5PackPairMask(
    Uint32 uMask0,
    Uint32 uMask1,
    Bool bHFlip,
    Bool bMosaic,
    Bool bSubscreen)
{
    Bool bPreferredOdd = (Bool)((!bSubscreen && !bMosaic) ^ bHFlip);
    Uint8 uOut;

    /* AURORA_SECRET_OF_MANA_MODE5_MAIN_COVERAGE_V7_1_20260929
     * Opacity must describe the same reduction as the pixel carrier:
     * exact selected phase for SUB, union coverage for MAIN. */
    if (bSubscreen)
    {
        uOut = (Uint8)(
            _SnesPPUMode5PackMask4(uMask0, bPreferredOdd) |
            (_SnesPPUMode5PackMask4(uMask1, bPreferredOdd) << 4)
        );
    }
    else
    {
        uOut = (Uint8)(
            _SnesPPUMode5PackMask4Coverage(uMask0, bPreferredOdd) |
            (_SnesPPUMode5PackMask4Coverage(uMask1, bPreferredOdd) << 4)
        );
    }

    if (bHFlip)
        uOut = SnesPPUChrCacheReverseMask(uOut);
    return uOut;
}



/* AURORA_MODE56_SCRATCH_FINAL_V3_37_20261007
 * Eight-workload FS=0 verdict: retain only the measured Mode5/6 scratch-LUT
 * optimization from v3.31..v3.36. CHR4 adaptive collapse and OBJ transparent
 * elision were rejected by local-cycle A/B data and are intentionally absent. */
static _INLINE void _SnesPPUMode5GetCHR2Row64(
	const Uint16 *pVram,
	Uint32 uRowAddr,
	Uint64 *pData,
	Uint32 *pOpaque)
{
#if SNPPU_BG_CACHE
	if (SnesPPUChrCacheLookup2(
		&_SnesPPU_ChrCache, uRowAddr, FALSE, pData, pOpaque))
	{
#if SNDBG_LOG
		g_DbgBGCacheHits++;
#endif
		return;
	}
#endif

	{
		const SnesPPUTile2T *pTile2 =
			(const SnesPPUTile2T *)(pVram + uRowAddr);
#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
		/* AURORA_MODE56_SCRATCH_LUT_V3_34_20261007 */
		const SnesChrLookup64T *pLookup =
			(const SnesChrLookup64T *)AURORA_DKC_SPR_PLANE0;
		const Uint8 *pMaskLookup = AURORA_DKC_SPR_MASKREV;
#else
		const SnesChrLookup64T *pLookup =
			(const SnesChrLookup64T *)&_SnesPPU_PlaneLookup[0];
		const Uint8 *pMaskLookup = _SnesPPU_HFlipLookup[1];
#endif
		Uint32 uPlane0 = pTile2->uPlane01[0][0];
		Uint32 uPlane1 = pTile2->uPlane01[0][1];

#if SNPPU_BG_CACHE && SNDBG_LOG
		g_DbgBGCacheMisses++;
#endif
		*pOpaque = pMaskLookup[uPlane0 | uPlane1];
		*pData  = (*pLookup)[uPlane0] << 0;
		*pData |= (*pLookup)[uPlane1] << 1;

#if SNPPU_BG_CACHE
		SnesPPUChrCacheStore2(
			&_SnesPPU_ChrCache, uRowAddr, *pData, *pOpaque);
#endif
	}
}


static _INLINE void _SnesPPUMode5GetCHR4Row64(
	const Uint16 *pVram,
	Uint32 uRowAddr,
	Uint64 *pData,
	Uint32 *pOpaque)
{
#if SNPPU_BG_CACHE
	if (SnesPPUChrCacheLookup4(
		&_SnesPPU_ChrCache, uRowAddr, FALSE, pData, pOpaque))
	{
#if SNDBG_LOG
		g_DbgBGCacheHits++;
#endif
		return;
	}
#endif

	{
		const SnesPPUTile4T *pTile4 =
			(const SnesPPUTile4T *)(pVram + uRowAddr);
#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
		/* AURORA_MODE56_SCRATCH_LUT_V3_34_20261007 */
		const SnesChrLookup64T *pLookup =
			(const SnesChrLookup64T *)AURORA_DKC_SPR_PLANE0;
		const Uint8 *pMaskLookup = AURORA_DKC_SPR_MASKREV;
#else
		const SnesChrLookup64T *pLookup =
			(const SnesChrLookup64T *)&_SnesPPU_PlaneLookup[0];
		const Uint8 *pMaskLookup = _SnesPPU_HFlipLookup[1];
#endif
		Uint32 uPlane0 = pTile4->uPlane01[0][0];
		Uint32 uPlane1 = pTile4->uPlane01[0][1];
		Uint32 uPlane2 = pTile4->uPlane23[0][0];
		Uint32 uPlane3 = pTile4->uPlane23[0][1];

#if SNPPU_BG_CACHE && SNDBG_LOG
		g_DbgBGCacheMisses++;
#endif
		*pOpaque = pMaskLookup[
			uPlane0 | uPlane1 | uPlane2 | uPlane3
		];
		*pData  = (*pLookup)[uPlane0] << 0;
		*pData |= (*pLookup)[uPlane1] << 1;
		*pData |= (*pLookup)[uPlane2] << 2;
		*pData |= (*pLookup)[uPlane3] << 3;

#if SNPPU_BG_CACHE
		SnesPPUChrCacheStore4(
			&_SnesPPU_ChrCache, uRowAddr, *pData, *pOpaque);
#endif
	}
}


static void _FetchCHR2Mode5_64(
	const Uint16 *pVram,
	Uint32 uBaseAddr,
	const SnesRenderTileT *pTiles,
	Int32 nTiles,
	Uint32 uScrollY,
	Uint8 *pDest,
	Uint8 *pMask,
	Uint64 *pPalLookup,
	Bool bMosaic,
	Bool bSubscreen)
{
	PROF_ENTER("_FetchCHR2Mode5_64");

	while (nTiles > 0)
	{
		Uint32 uTile0 = pTiles->uTile & 0x03FF;
		Uint32 uTile1 = (uTile0 + 1) & 0x03FF;
		Uint32 uRow = (uScrollY + pTiles->uOffsetY) & 7;
		Uint32 uAddr0;
		Uint32 uAddr1;
		Uint64 uRow0;
		Uint64 uRow1;
		Uint64 uOut;
		Uint32 uMask0;
		Uint32 uMask1;
		Uint8 uOutMask;
		Bool bHFlip = (pTiles->uFlip & 1) != 0;

		if (pTiles->uFlip & 2)
			uRow ^= 7;

		uAddr0 = ((uBaseAddr + uTile0 * 8) & 0x7FFF) + uRow;
		uAddr1 = ((uBaseAddr + uTile1 * 8) & 0x7FFF) + uRow;

		_SnesPPUMode5GetCHR2Row64(
			pVram, uAddr0, &uRow0, &uMask0);
		_SnesPPUMode5GetCHR2Row64(
			pVram, uAddr1, &uRow1, &uMask1);

		uOut = _SnesPPUMode5PackPair(
			uRow0, uRow1, bHFlip, bMosaic, bSubscreen);
		uOutMask = _SnesPPUMode5PackPairMask(
			uMask0, uMask1, bHFlip, bMosaic, bSubscreen);

		uOut |= pPalLookup[pTiles->uPal];

		pMask[0] = uOutMask;
		pMask[SNPPU_BGPLANE_SIZE] =
			(pTiles->uPal & 8) ? uOutMask : 0;
		((Uint64 *)pDest)[0] = uOut;

		pDest += 8;
		pMask++;
		pTiles++;
		nTiles--;
	}

	PROF_LEAVE("_FetchCHR2Mode5_64");
}


static void _FetchCHR4Mode5_64(
	const Uint16 *pVram,
	Uint32 uBaseAddr,
	const SnesRenderTileT *pTiles,
	Int32 nTiles,
	Uint32 uScrollY,
	Uint8 *pDest,
	Uint8 *pMask,
	Bool bMosaic,
	Bool bSubscreen)
{
	PROF_ENTER("_FetchCHR4Mode5_64");

	while (nTiles > 0)
	{
		Uint32 uTile0 = pTiles->uTile & 0x03FF;
		Uint32 uTile1 = (uTile0 + 1) & 0x03FF;
		Uint32 uRow = (uScrollY + pTiles->uOffsetY) & 7;
		Uint32 uAddr0;
		Uint32 uAddr1;
		Uint64 uRow0;
		Uint64 uRow1;
		Uint64 uOut;
		Uint32 uMask0;
		Uint32 uMask1;
		Uint8 uOutMask;
		Bool bHFlip = (pTiles->uFlip & 1) != 0;

		if (pTiles->uFlip & 2)
			uRow ^= 7;

		uAddr0 = ((uBaseAddr + uTile0 * 16) & 0x7FFF) + uRow;
		uAddr1 = ((uBaseAddr + uTile1 * 16) & 0x7FFF) + uRow;

		_SnesPPUMode5GetCHR4Row64(
			pVram, uAddr0, &uRow0, &uMask0);
		_SnesPPUMode5GetCHR4Row64(
			pVram, uAddr1, &uRow1, &uMask1);

		uOut = _SnesPPUMode5PackPair(
			uRow0, uRow1, bHFlip, bMosaic, bSubscreen);
		uOutMask = _SnesPPUMode5PackPairMask(
			uMask0, uMask1, bHFlip, bMosaic, bSubscreen);

#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
		uOut |= AURORA_DKC_SPR_PAL4[pTiles->uPal];
#else
		uOut |= _SnesPPU_Tile4PalLookup64[pTiles->uPal];
#endif

		pMask[0] = uOutMask;
		pMask[SNPPU_BGPLANE_SIZE] =
			(pTiles->uPal & 8) ? uOutMask : 0;
		((Uint64 *)pDest)[0] = uOut;

		pDest += 8;
		pMask++;
		pTiles++;
		nTiles--;
	}

	PROF_LEAVE("_FetchCHR4Mode5_64");
}


#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM && !SNPPU_BG_CACHE
/* AURORA_PPU_R5900_CHR8_V16_20260928
 * Emulated PPU decisions stay in C++; this leaf kernel only expands the
 * already-selected 8bpp VRAM row and writes the same 8-byte tile + masks. */
static void _FetchCHR8_64(const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask)
{
	PROF_ENTER("_FetchCHR8_64");
	AuroraPPUFetchCHR8Direct64PS2(
		pVram, uBaseAddr, pTiles, nTiles, uScrollY, pDest, pMask,
		&_AuroraPPUChr4DirectAsmCtx);
	PROF_LEAVE("_FetchCHR8_64");
}
#else
static void _FetchCHR8_64(const Uint16 *pVram, Uint32 uBaseAddr, const SnesRenderTileT *pTiles, Int32 nTiles, Uint32 uScrollY, Uint8 *pDest, Uint8 *pMask)
{
	SNPPUBg8FlipT *pFlip;

	PROF_ENTER("_FetchCHR8_64");

	while (nTiles > 0)
	{
		SnesPPUTile8T *pTile8;
		Uint32 uTileAddr;
		Uint32 uPlane0, uPlane1, uPlane2, uPlane3;
		Uint32 uPlane4, uPlane5, uPlane6, uPlane7;
		Uint64 uTile0;
		Uint32 uMask;
		SnesChrLookup64T *pLookup;
		Uint8 *pHFlip;

		pFlip = &_FlipTable8[pTiles->uFlip];

		// calculate tile address
		uTileAddr = (uBaseAddr + pTiles->uTile * 32) & 0x7FFF;

		// get pointer to tile data (y flipped)
		pTile8 = (SnesPPUTile8T *)(pVram + uTileAddr + ((uScrollY+pTiles->uOffsetY) ^ pFlip->uFlipXOR));

		// get tile plane bits
		uPlane0 = pTile8->uPlane01[0][0];
		uPlane1 = pTile8->uPlane01[0][1];
		uPlane2 = pTile8->uPlane23[0][0];
		uPlane3 = pTile8->uPlane23[0][1];
		uPlane4 = pTile8->uPlane45[0][0];
		uPlane5 = pTile8->uPlane45[0][1];
		uPlane6 = pTile8->uPlane67[0][0];
		uPlane7 = pTile8->uPlane67[0][1];

		pLookup = (SnesChrLookup64T *)pFlip->pLookup;
		pHFlip  = pFlip->pFlipLookup;

		// create mask
		uMask = uPlane0 | uPlane1 | uPlane2 | uPlane3 | uPlane4 | uPlane5 | uPlane6 | uPlane7;
		uMask = pHFlip[uMask];

		// decode tile
		uTile0  = (*pLookup)[uPlane0] << 0;
		uTile0 |= (*pLookup)[uPlane1] << 1;
		uTile0 |= (*pLookup)[uPlane2] << 2;
		uTile0 |= (*pLookup)[uPlane3] << 3;
		uTile0 |= (*pLookup)[uPlane4] << 4;
		uTile0 |= (*pLookup)[uPlane5] << 5;
		uTile0 |= (*pLookup)[uPlane6] << 6;
		uTile0 |= (*pLookup)[uPlane7] << 7;

		pMask[ 0] = uMask;
		pMask[SNPPU_BGPLANE_SIZE] = (pTiles->uPal & 8) ? uMask : 0;
		pMask++;

		// store tile data
		((Uint64 *)pDest)[0] = uTile0;

		pDest+=8;
		pTiles++;
		nTiles--;
	}
	PROF_LEAVE("_FetchCHR8_64");
}
#endif






static void _FetchCHR_64(Uint8 *pLine, SnesPPU *pPPU, SnesBGInfoT *pBGInfo, struct SnesRenderTileT *pTiles, Int32 nTiles, Int32 iLine,Uint8 *pMask, Bool bOffset, Bool bHiresSubscreen)
{
	Uint32 uScrollY = 0;
	/* AURORA_SNES_SAFE_PERF_V3_20260919
	 * GetRegs()/GetVramPtr() are inline address getters. This helper passes
	 * only the resulting VRAM pointer to its decode routines. */
	const SnesPPURegsT *pRegs = pPPU->GetRegs();
	Uint16 *pVram = pPPU->GetVramPtr(0);
	const Uint8 uBGMode = (Uint8)(pRegs->bgmode & 7);
	const Uint32 uMosaic = pBGInfo->uMosaic;
	const Uint32 uMosaicSize = uMosaic + 1;

	if (uMosaic > 0)
	{
		iLine /= uMosaicSize;
		iLine *= uMosaicSize;
	}

	if (!bOffset)
	{
		uScrollY = pBGInfo->uScrollY + iLine;
	}

	switch (pBGInfo->uBitDepth)
	{
	case 2:
		// Mode 5 decodes the adjacent character pair N/N+1.
		if (uBGMode == 5)
			_FetchCHR2Mode5_64(
				pVram,
				pBGInfo->uChrAddr,
				pTiles,
				nTiles,
				uScrollY & 7,
				pLine,
				pMask,
				AURORA_DKC_SPR_PAL2 + ((Uint32)pBGInfo->uPalBase * 16u),
				(pRegs->mosaic & 0x02) != 0,
				bHiresSubscreen
			);
		else if (!bOffset)
			_FetchCHR2NoOffset_64(
				pVram,
				pBGInfo->uChrAddr,
				pTiles,
				nTiles,
				uScrollY & 7,
				pLine,
				pMask,
				AURORA_DKC_SPR_PAL2 + ((Uint32)pBGInfo->uPalBase * 16u)
			);
		else
			_FetchCHR2_64(
				pVram,
				pBGInfo->uChrAddr,
				pTiles,
				nTiles,
				uScrollY & 7,
				pLine,
				pMask,
				AURORA_DKC_SPR_PAL2 + ((Uint32)pBGInfo->uPalBase * 16u)
			);
		break;
	case 4:
		// Modes 5/6 decode the adjacent hires character pair N/N+1.
		if ((uBGMode == 5) ||
		    (uBGMode == 6))
			_FetchCHR4Mode5_64(
				pVram,
				pBGInfo->uChrAddr,
				pTiles,
				nTiles,
				uScrollY & 7,
				pLine,
				pMask,
				(pRegs->mosaic & 0x01) != 0,
				bHiresSubscreen
			);
		else if (!bOffset)
			_FetchCHR4NoOffset_64(
				pVram,
				pBGInfo->uChrAddr,
				pTiles,
				nTiles,
				uScrollY & 7,
				pLine,
				pMask
			);
		else
			_FetchCHR4_64(
				pVram,
				pBGInfo->uChrAddr,
				pTiles,
				nTiles,
				uScrollY & 7,
				pLine,
				pMask
			);
		break;
	case 8:
		// fetch chr (8-bit)
		_FetchCHR8_64(pVram, pBGInfo->uChrAddr, pTiles, nTiles, uScrollY & 7, pLine, pMask);
		break;
	}

	if (uMosaic > 0)
	{
		_MosaicBG8(pLine + (pBGInfo->uScrollX & 7), 256, uMosaicSize);
	}
}

#endif

//#endif


#if CODE_PLATFORM == CODE_PS2
#if AURORA_PPU_R5900_ASM
static void _RenderBGData_O(Uint8 *pLine8, Uint8 *pSrc8, SNMaskT *pBGMask, Uint32 uScrollX, Int32 nTiles)
{
	AuroraPPURenderBGDataOPS2(
		pLine8, pSrc8, pBGMask->uMask8, uScrollX, nTiles,
		AURORA_DKC_SPR_PLANE1);
}

static void _RenderBGData(Uint8 *pLine8, Uint8 *pSrc8, SNMaskT *pBGMask, Uint32 uScrollX, Int32 nTiles)
{
	AuroraPPURenderBGDataPS2(
		pLine8, pSrc8, pBGMask->uMask8, uScrollX, nTiles,
		AURORA_DKC_SPR_PLANE1);
}
#else
static void _RenderBGData_O(Uint8 *pLine8, Uint8 *pSrc8, SNMaskT *pBGMask, Uint32 uScrollX, Int32 nTiles)
{
	Uint16 *pMaskData;
	SnesChrLookup64T *pLookup64 = (SnesChrLookup64T *)&_SnesPPU_PlaneLookup[1];

	pSrc8    += (uScrollX & 7);
	pMaskData = (Uint16 *)pBGMask->uMask8;

	__asm__ (
		"mtsab      %0,0     \n"
		: : "r" (pSrc8)
		);

	while (nTiles > 0)
	{
		Uint32 uMask;

		// fetch mask half-worde for 16 pixels
		uMask = *pMaskData;
		pMaskData++;

		// write 0 to output
		__asm__ __volatile__ (
			"sq        $0,0x00(%0)     \n"
			:
			: "r" (pLine8)
			: "memory"
			);

		if (uMask)
		{
			/* GCC 15 r5900 TI-mode split: keep 128-bit value in fixed
			 * MMI register and emit `sq` inside asm block (snmaskop.h
			 * pattern). C-level Uint128 store would only emit 1 sd. */
			if (uMask!=0xFFFF)
			{
				Uint64 uMask0,uMask1;

				uMask0 = (*pLookup64)[uMask & 0xFF];
				uMask1 = (*pLookup64)[uMask >> 8];

				__asm__ __volatile__ (
					"lq         $8, 0x00(%2)     \n"
					"lq         $9, 0x10(%2)     \n"
					"qfsrv      $8, $9, $8       \n"
					"pcpyld     $10, %1, %0      \n"
					"pceqb      $10, $10, $0     \n"
					"por        $8, $8, $10      \n"
					"pxor       $8, $8, $10      \n"
					"sq         $8, 0x00(%3)     \n"
					:
					: "r" (uMask0), "r" (uMask1), "r" (pSrc8), "r" (pLine8)
					: "$8", "$9", "$10", "memory"
					);
			} else
			{
				__asm__ __volatile__ (
					"lq         $8, 0x00(%0)     \n"
					"lq         $9, 0x10(%0)     \n"
					"qfsrv      $8, $9, $8       \n"
					"sq         $8, 0x00(%1)     \n"
					:
					: "r" (pSrc8), "r" (pLine8)
					: "$8", "$9", "memory"
					);
			}
		}

		pSrc8+=16;
		pLine8+=16;
		nTiles-=2;
	}
}

static void _RenderBGData(Uint8 *pLine8, Uint8 *pSrc8, SNMaskT *pBGMask, Uint32 uScrollX, Int32 nTiles)
{
	Uint16 *pMaskData;
	SnesChrLookup64T *pLookup64 = (SnesChrLookup64T *)&_SnesPPU_PlaneLookup[1];

	pSrc8    += (uScrollX & 7);
	pMaskData = (Uint16 *)pBGMask->uMask8;

    __asm__ (
    	"mtsab      %0,0     \n"
    	: : "r" (pSrc8)
     );

	while (nTiles > 0)
	{
		Uint32 uMask;

		// fetch mask half-worde for 16 pixels
		uMask = *pMaskData;
        pMaskData++;

		if (uMask)
		{
			/* GCC 15 r5900 TI-mode split: see _RenderBGData_O above. */
			if (uMask!=0xFFFF)
			{
                Uint64 uMask0,uMask1;

                uMask0 = (*pLookup64)[uMask & 0xFF];
                uMask1 = (*pLookup64)[uMask >> 8];

    	        __asm__ __volatile__ (
    	            "lq         $8,  0x00(%2)    \n"
    	            "lq         $9,  0x10(%2)    \n"
    	            "qfsrv      $8,  $9, $8      \n"
    	            "lq         $11, 0x00(%3)    \n"
    	            "pcpyld     $10, %1, %0      \n"
    	            "pceqb      $10, $10, $0     \n"
    	            "pand       $11, $11, $10    \n"
    	            "por        $8,  $8, $10     \n"
    	            "pxor       $8,  $8, $10     \n"
    	            "por        $8,  $8, $11     \n"
    	            "sq         $8,  0x00(%3)    \n"
    	            :
    	            : "r" (uMask0), "r" (uMask1), "r" (pSrc8), "r" (pLine8)
    	            : "$8", "$9", "$10", "$11", "memory"
    	         );
			} else
			{
    	        __asm__ __volatile__ (
    	            "lq         $8, 0x00(%0)     \n"
    	            "lq         $9, 0x10(%0)     \n"
    	            "qfsrv      $8, $9, $8       \n"
    	            "sq         $8, 0x00(%1)     \n"
    	            :
    	            : "r" (pSrc8), "r" (pLine8)
    	            : "$8", "$9", "memory"
    	         );
			}
		}

		pSrc8+=16;
		pLine8+=16;
		nTiles-=2;
	}
}
#endif

#else

#define RENDERBGPIXEL(_y,_x)\
	if (uMask&(1<<(_x))) pLine8[_x] = (_y) >> (_x*8);

static void _RenderBGData(Uint8 *pLine8, Uint8 *pSrc8, SNMaskT *pBGMask, Uint32 uScrollX, Int32 nTiles)
{
	Uint8 *pMaskData;
	Uint32 uShift, uInvShift;

	//memcpy(pLine8, pSrc8 + (uScrollX &7), 256);

	pSrc8    += (uScrollX & 4);
	uShift    = (uScrollX & 3) << 3;
	uInvShift = 32 - uShift;

	pMaskData = pBGMask->uMask8;
	if (uShift == 0)
	{
		while (nTiles > 0)
		{
			Uint32 uMask;

			// fetch mask byte
			uMask = *pMaskData++;

			if (uMask)
			{
				Uint32 t0,t2;
				t0 = ((Uint32 *)pSrc8)[0];
				t2 = ((Uint32 *)pSrc8)[1];
				pSrc8+=8;

				if (uMask==0xFF)
				{
					((Uint32 *)pLine8)[0] = t0;
					((Uint32 *)pLine8)[1] = t2;
					pLine8+=8;
				} else
				{
					if (uMask&(1<<0)) pLine8[0] = t0 >> (0*8);
					if (uMask&(1<<1)) pLine8[1] = t0 >> (1*8);
					if (uMask&(1<<2)) pLine8[2] = t0 >> (2*8);
					if (uMask&(1<<3)) pLine8[3] = t0 >> (3*8);
					if (uMask&(1<<4)) pLine8[4] = t2 >> (0*8);
					if (uMask&(1<<5)) pLine8[5] = t2 >> (1*8);
					if (uMask&(1<<6)) pLine8[6] = t2 >> (2*8);
					if (uMask&(1<<7)) pLine8[7] = t2 >> (3*8);
					pLine8+=8;
				}
			}  else
			{
				pSrc8+=8;
				pLine8+=8;
			}

			nTiles--;
		}
	} else
	{

		while (nTiles > 0)
		{
			Uint32 uMask;
			Uint32 t0,t1,t2,t3;

			// fetch mask byte
			uMask = *pMaskData++;

			if (uMask)
			{
				t0 = ((Uint32 *)pSrc8)[0];
				t1 =
				t2 = ((Uint32 *)pSrc8)[1];
				t3 = ((Uint32 *)pSrc8)[2];
				pSrc8+=8;

				t0 >>= uShift;
				t1 <<= uInvShift;
				t0|=t1;

				t2 >>= uShift;
				t3 <<= uInvShift;
				t2|=t3;

				if (uMask==0xFF)
				{
					((Uint32 *)pLine8)[0] = t0;
					((Uint32 *)pLine8)[1] = t2; pLine8+=8;
				} else
				{
					if (uMask&(1<<0)) pLine8[0] = t0 >> (0*8);
					if (uMask&(1<<1)) pLine8[1] = t0 >> (1*8);
					if (uMask&(1<<2)) pLine8[2] = t0 >> (2*8);
					if (uMask&(1<<3)) pLine8[3] = t0 >> (3*8);
					if (uMask&(1<<4)) pLine8[4] = t2 >> (0*8);
					if (uMask&(1<<5)) pLine8[5] = t2 >> (1*8);
					if (uMask&(1<<6)) pLine8[6] = t2 >> (2*8);
					if (uMask&(1<<7)) pLine8[7] = t2 >> (3*8);
					pLine8+=8;
				}
			}  else
			{
				pSrc8+=8;
				pLine8+=8;
			}

			nTiles--;
		}
	}
}


static void _RenderBGData_O(Uint8 *pLine8, Uint8 *pSrc8, SNMaskT *pBGMask, Uint32 uScrollX, Int32 nTiles)
{
	memset(pLine8, 0, 256);
	_RenderBGData(pLine8, pSrc8, pBGMask, uScrollX, nTiles);
}


#endif


/* AURORA_V4_MODE34_DIRECT_COLOR_20260915
 * Pack Direct Color ownership and the tilemap YYY palette bits into one
 * nibble per output pixel. Bit 3 is ownership; bits 0-2 are YYY.
 * Mosaic must replicate the palette metadata from the same source pixel
 * whose CHR value _MosaicBG8() replicated. */
static void _SnesPPUTrackMode34DirectColor(Uint8 *pAttrib8, Uint8 uShift,
	const SNMaskT *pVisible, const SnesRenderTileT *pTiles,
	Uint32 uScrollX, Uint32 uMosaic)
{
	Uint32 iPixel;
	const Uint8 uKeep = uShift ? 0x0F : 0xF0;
	const Uint32 uFineScroll = uScrollX & 7;
	const Uint32 uMosaicSize = uMosaic + 1;

	/* AURORA_TOPGEAR_MODE34_DIRECT8_V6_20260917
	 * With no mosaic and zero fine scroll, each mask byte corresponds
	 * exactly to one pTiles[] entry. PlaneLookup[1] expands bit N to
	 * byte N, so eight metadata bytes can be selected at once. */
	if (!uMosaic && uFineScroll == 0 &&
	    (uShift == 0 || uShift == 4))
	{
#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
		/* AURORA_DIRECTCOLOR_SCRATCH_LUT_V3_39_20261008
		 * PlaneLookup[1] is already mirrored at SPR+10 KiB on every visible
		 * BeginRender.  This is the exact same immutable 256x64-bit table. */
		const SnesChrLookup64T *pLookup64 =
			(const SnesChrLookup64T *)AURORA_DKC_SPR_PLANE1;
#else
		SnesChrLookup64T *pLookup64 =
			(SnesChrLookup64T *)&_SnesPPU_PlaneLookup[1];
#endif
		Uint64 *pAttrib64 = (Uint64 *)pAttrib8;
		const Uint32 uNibbleMask = uShift ? 0xF0u : 0x0Fu;
		Uint32 iByte;

		for (iByte = 0; iByte < 256 / 8; iByte++)
		{
			Uint8 uMask8 = pVisible->uMask8[iByte];
			if (uMask8)
			{
				Uint64 uVisible = (*pLookup64)[uMask8];
				Uint32 uNibble =
					(0x08u | (pTiles[iByte].uPal & 0x07u)) << uShift;
				Uint64 uSelect = uVisible * uNibbleMask;
				Uint64 uData = uVisible * uNibble;
				pAttrib64[iByte] =
					(pAttrib64[iByte] & ~uSelect) | uData;
			}
		}
		return;
	}

	for (iPixel = 0; iPixel < 256; iPixel++)
	{
		Uint32 uSourceX;
		Uint32 uTileIndex;
		Uint8 uNibble;

		if (!(pVisible->uMask8[iPixel >> 3] & (1u << (iPixel & 7))))
			continue;

		uSourceX = iPixel;
		if (uMosaic)
			uSourceX = (uSourceX / uMosaicSize) * uMosaicSize;
		uTileIndex = (uSourceX + uFineScroll) >> 3;
		uNibble = (Uint8)(0x08 | (pTiles[uTileIndex].uPal & 0x07));
		pAttrib8[iPixel] = (Uint8)((pAttrib8[iPixel] & uKeep) |
			(uNibble << uShift));
	}
}


/* AURORA_V8_MODE7_RELEASE_AUDIT_20260915
 * Mode 7 Direct Color uses only the chunky 8-bit pixel: there are no
 * tilemap palette attributes, so YYY=000. Bit 3 marks Direct Color
 * ownership and bits 0-2 remain clear. */
static void _SnesPPUTrackMode7DirectColor(Uint8 *pAttrib8, Uint8 uShift,
	const SNMaskT *pVisible)
{
	/* AURORA_TOPGEAR_MODE7_DIRECT8_V5_20260917
	 * _SnesPPU_PlaneLookup[1][mask] is eight bytes containing bit0..bit7 as
	 * 0/1. Expand eight visibility bits at once, then use a bytewise select
	 * mask so invisible attribute bytes remain completely untouched. */
	if (uShift == 0 || uShift == 4)
	{
#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
		/* AURORA_DIRECTCOLOR_SCRATCH_LUT_V3_39_20261008
		 * Same PlaneLookup[1] mirror used by the PS2 BG mask kernels. */
		const SnesChrLookup64T *pLookup64 =
			(const SnesChrLookup64T *)AURORA_DKC_SPR_PLANE1;
#else
		SnesChrLookup64T *pLookup64 =
			(SnesChrLookup64T *)&_SnesPPU_PlaneLookup[1];
#endif
		Uint64 *pAttrib64 = (Uint64 *)pAttrib8;
		const Uint32 uNibbleMask = uShift ? 0xF0u : 0x0Fu;
		const Uint32 uDirectBit = uShift ? 0x80u : 0x08u;
		Uint32 iByte;

		for (iByte = 0; iByte < 256 / 8; iByte++)
		{
			const Uint64 uVisible =
				(*pLookup64)[pVisible->uMask8[iByte]];
			const Uint64 uSelect = uVisible * uNibbleMask;
			const Uint64 uDirect = uVisible * uDirectBit;
			pAttrib64[iByte] =
				(pAttrib64[iByte] & ~uSelect) | uDirect;
		}
		return;
	}

	/* Preserve the old helper's semantics for any unexpected private caller. */
	{
		Uint32 iPixel;
		const Uint8 uKeep = uShift ? 0x0F : 0xF0;
		const Uint8 uDirect = (Uint8)(0x08 << uShift);
		for (iPixel = 0; iPixel < 256; iPixel++)
		{
			if (!(pVisible->uMask8[iPixel >> 3] &
			      (1u << (iPixel & 7))))
				continue;
			pAttrib8[iPixel] =
				(Uint8)((pAttrib8[iPixel] & uKeep) | uDirect);
		}
	}
}



/* AURORA_V33_MAGICAL_BG_MASK_PAIR_R5900_20261011
 * Exactly the two original SNMaskSHL(0) operations in a single EE MMI
 * load/store group. TempMask[0] and TempMask[1] are 48-byte rows; only
 * the first 32 bytes of each are read (0/16 and 48/64). Destinations are
 * separate, already-aligned SNMaskT planes. The original code separately
 * calls SNMaskCopy twice in the zero-scroll case, each with two lq/sq.
 * Non-Mode1 or fineX != 0 goes through original SNMaskSHL functions.
 * No cache, no new per-pixel branches, no altered emulated state.
 */
#ifndef AURORA_V33_SNES_MASK_PAIR_FUSED
#define AURORA_V33_SNES_MASK_PAIR_FUSED 1
#endif
#if CODE_PLATFORM == CODE_PS2 && AURORA_V33_SNES_MASK_PAIR_FUSED
static _INLINE void AuroraV33CopyMode1MaskPairPS2(
    SNMaskT *pOpaque, SNMaskT *pPriority, const Uint8 *pTempMask)
{
    __asm__ __volatile__(
        "lq      $8,0x00(%2)\n"
        "lq      $9,0x10(%2)\n"
        "lq     $10,0x30(%2)\n"
        "lq     $11,0x40(%2)\n"
        "sq      $8,0x00(%0)\n"
        "sq      $9,0x10(%0)\n"
        "sq     $10,0x00(%1)\n"
        "sq     $11,0x10(%1)\n"
        : : "r"(pOpaque), "r"(pPriority), "r"(pTempMask)
        : "$8", "$9", "$10", "$11", "memory");
}
#endif

static void _RenderBG8(Uint8 *pLine8, SNMaskT *pLine, SNMaskT *pBGPlane, SNMaskT *pWindow, Uint32 uBitDepth, SNMaskT *pAddSubMask, Uint8 bAddSubMask, SNMaskT *pBGPri, SNMaskT *pExtraMask, Uint32 uPriority, Bool &bRendered, Uint32 uScrollX, SNMaskT *pRenderedMask = NULL)
{
	if (uBitDepth!=0)
	{
#if AURORA_SNES_BG2_FASTPATH
        /* AURORA_V43_SNES_BG2_PRODUCTION_ISOLATED_20261011
         * Exact v41.2 winning candidate, without profiler or A/B probe.
         * Priority 4/7 must execute original code: it clears layer masks.
         * Only the 2bpp fully transparent source BG plane can return.
         */
        if (uBitDepth == 2u && uPriority != 4u && uPriority != 7u)
        {
            const SNMaskT *const pOpacity =
                &pBGPlane[SNPPU_BGPLANE_OPAQUE];
            if (pOpacity->uMask64[0] == 0u &&
                (pOpacity->uMask64[1] |
                 pOpacity->uMask64[2] |
                 pOpacity->uMask64[3]) == 0u)
            {
                if (pBGPri) SNMaskClear(pBGPri);
                if (pRenderedMask) SNMaskClear(pRenderedMask);
                if (!bRendered)
                {
                    memset(pLine8, 0, 256);
                    bRendered = TRUE;
                }
                return;
            }
        }
#endif
		SNMaskT BGMask;		// bits of BG to render
		SNMaskT BGPri;      // bits of BG with high priority

		if (!pBGPri) pBGPri = &BGPri;

		// BGMask = mask & ~window
		// BGMask are the opaque pixels of the background that are not within the clip window
		if (pWindow)
		{
			SNMaskANDN(&BGMask, &pBGPlane[SNPPU_BGPLANE_OPAQUE], pWindow);
		} else
		{
			SNMaskCopy(&BGMask, &pBGPlane[SNPPU_BGPLANE_OPAQUE]);
		}

		if (pExtraMask)
		{
			// hide bg pixels
			SNMaskANDN(&BGMask, &BGMask, pExtraMask);
		}

		// BGPri  = mask  & ~window & bghipriority
		// these are the high priority pixels that need to be rendered
		// they overwrite the lo and hi priority pixels that have already been rendered
		SNMaskAND(pBGPri, &BGMask, &pBGPlane[SNPPU_BGPLANE_PRI]);

		switch (uPriority)
		{
		case 0:
			// lo = 00
			// hi = 01
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], pBGPri, true);
			break;
		case 1:
			// lo = 00 (masked behind bg3hi unless bg2hi)
			SNMaskANDN(&BGMask, &BGMask, &pLine[SNPPU_BGPLANE_LAYER0]);
			SNMaskOR(&BGMask, &BGMask, pBGPri);

			// lo = 00
			// hi = 01
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], pBGPri, true);
			break;
		case 2:
			// lo = 10
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], &BGMask, false);
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER1], &BGMask, true);
			// hi = 11
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], pBGPri, true);
			break;
		case 3:
			{
				SNMaskT temp;
				// temp = bg2hi
				SNMaskAND(&temp, &pLine[SNPPU_BGPLANE_LAYER0], &pLine[SNPPU_BGPLANE_LAYER1]);
				// lo = 00 (masked behind bg2hi unless bg1hi)
				SNMaskANDN(&BGMask, &BGMask, &temp);
				SNMaskOR(&BGMask, &BGMask, pBGPri);
			}

			// lo = 10
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER1], &BGMask, true);
			// hi = 11
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], &BGMask, false);
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], pBGPri, true);
			break;

		case 4:
			// lo = 00
			// hi = 10
			SNMaskClear(&pLine[SNPPU_BGPLANE_LAYER0]);
			SNMaskCopy(&pLine[SNPPU_BGPLANE_LAYER1], pBGPri);
			break;

		case 5:
			// lo = 01
			SNMaskANDN(&BGMask, &BGMask, &pLine[SNPPU_BGPLANE_LAYER1]);
			SNMaskOR(&BGMask, &BGMask, pBGPri);

			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER0], &BGMask, true);

			// hi = 11
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER1], &BGMask, false);
			SNMaskBool(&pLine[SNPPU_BGPLANE_LAYER1], pBGPri, true);
			break;

		case 7:
			// lo = 10
			//SNMaskSet(&pLine[SNPPU_BGPLANE_LAYER0]);
			//SNMaskClear(&pLine[SNPPU_BGPLANE_LAYER1]);
			//SNMaskClear(&pLine[SNPPU_BGPLANE_LAYER0]);
			//SNMaskCopy(&pLine[SNPPU_BGPLANE_LAYER1], pBGPri);
//			SNMaskCopy(&pLine[SNPPU_BGPLANE_LAYER0], &BGMask);

	// contra 3 = happy
			SNMaskClear(&pLine[SNPPU_BGPLANE_LAYER0]);
			SNMaskCopy(&pLine[SNPPU_BGPLANE_LAYER1], pBGPri);
	// bgpri = 0 sadv
//			SNMaskCopy(&pLine[SNPPU_BGPLANE_LAYER0], &BGMask);
//			SNMaskClear(&pLine[SNPPU_BGPLANE_LAYER1]);
			break;
		}

		if (pAddSubMask)
		{
			// set or reset bits of AddSubMask based on pixels that were rendered
			SNMaskBool(pAddSubMask, &BGMask, bAddSubMask ? true : false);
		}

		/* AURORA_V4_MODE34_DIRECT_COLOR_20260915
		 * Expose the exact post-window/post-priority pixel set when BG1
		 * Direct Color needs to retain tile palette metadata. */
		if (pRenderedMask)
			SNMaskCopy(pRenderedMask, &BGMask);


		if (!bRendered)
		{
			// render it
			PROF_ENTER("_RenderBGData_O");
			_RenderBGData_O(pLine8, (Uint8 *)pBGPlane, &BGMask, uScrollX, 32);
			PROF_LEAVE("_RenderBGData_O");
			bRendered = TRUE;
		} else
		{
			// render it
			PROF_ENTER("_RenderBGData");
			_RenderBGData(pLine8, (Uint8 *)pBGPlane, &BGMask, uScrollX, 32);
			PROF_LEAVE("_RenderBGData");
		}

	}
}



static _INLINE void _DecodeOBJRow4(SnesChrLookupT *pLookup, Uint8 *pHFlip,
	Uint32 uPlane0, Uint32 uPlane1, Uint32 uPlane2, Uint32 uPlane3,
	Uint32 *pTile0, Uint32 *pTile1, Uint32 *pOpaque)
{
	Uint32 uTile0 = (*pLookup)[uPlane0][0] << 0;
	Uint32 uTile1 = (*pLookup)[uPlane0][1] << 0;

	uTile0 |= (*pLookup)[uPlane1][0] << 1;
	uTile1 |= (*pLookup)[uPlane1][1] << 1;
	uTile0 |= (*pLookup)[uPlane2][0] << 2;
	uTile1 |= (*pLookup)[uPlane2][1] << 2;
	uTile0 |= (*pLookup)[uPlane3][0] << 3;
	uTile1 |= (*pLookup)[uPlane3][1] << 3;

	*pTile0 = uTile0;
	*pTile1 = uTile1;
	*pOpaque = pHFlip[uPlane0 | uPlane1 | uPlane2 | uPlane3];
}


#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
/* AURORA_DKC_OBJ_HOTPATH_V20_20260930
 * Default OBJ cache stores canonical rows, so cache misses do not need eight
 * 32-bit planar-LUT loads.  BeginRender() already mirrors the exact canonical
 * 256x64-bit plane LUT and normal opacity reverse table into scratchpad for
 * the BG kernels.  Reuse those immutable host-derived copies here: four 64-bit
 * LUT loads produce the same eight indexed pixels, then the existing cache
 * owns canonical/H-flipped representation exactly as before. */
static _INLINE void _DecodeOBJRow4ScratchPS2(
	Uint16 uPair01, Uint16 uPair23, Uint64 *pRowData, Uint32 *pOpaque)
{
	const Uint32 uSource =
		(Uint32)(((uPair01 | uPair23) |
		          ((uPair01 | uPair23) >> 8)) & 0x00ffu);

	if (!uSource)
	{
		*pRowData = 0;
		*pOpaque = 0;
		return;
	}

	const Uint64 *pLookup = AURORA_DKC_SPR_PLANE0;
	Uint64 uRow = pLookup[uPair01 & 0x00ffu];
	uRow |= pLookup[uPair01 >> 8] << 1;
	uRow |= pLookup[uPair23 & 0x00ffu] << 2;
	uRow |= pLookup[uPair23 >> 8] << 3;

	*pRowData = uRow;
	*pOpaque = AURORA_DKC_SPR_MASKREV[uSource];
}
#endif


/* AURORA_OBJ_STAT77_V2_RENDER8_20260915 */
static Int32 _FetchOBJ(SnesRenderObjT *pObjBase, Uint8 *pObjList, Int32 nObjList,
	SnesRenderObj8T *pObjLine, Int32 MaxObj8Line, Int32 iLine,
	Uint32 uBaseAddr, Uint32 uNameSelect, Uint16 *pVram,
	Bool bObjInterlace, Bool bField)
{
	Int32 nObjLine = 0;
#if AURORA_FRONTEND_PROFILER
	/* AURORA_SNES_TARGET_TRAITS_V3_21_1_20261006 */
	Uint32 uAuroraObjCacheHits = 0;
	Uint32 uAuroraObjCacheMisses = 0;
	Uint32 uAuroraObjMissTransparent = 0;
	Uint32 uAuroraObjMissOpaque = 0;
	Uint32 uAuroraObjHFlipTiles = 0;
	Uint32 uAuroraObjSecondTableTiles = 0;
	/* AURORA_OBJ_MICROSCOPE_V3_25_20261006 */
	Uint32 uAuroraObjHitCycles = 0;
	Uint32 uAuroraObjMissCycles = 0;
	Uint32 uAuroraObjHitTransparent = 0;
	Uint32 uAuroraObjHitFullOpaque = 0;
	Uint32 uAuroraObjHitPartial = 0;
	Uint32 uAuroraObjMicMissTransparent = 0;
	Uint32 uAuroraObjMicMissFullOpaque = 0;
	Uint32 uAuroraObjMicMissPartial = 0;
	Uint32 uAuroraObjRowSame = 0;
	Uint32 uAuroraObjRowPlus16 = 0;
	Uint32 uAuroraObjRowMinus16 = 0;
	Uint32 uAuroraObjRowOther = 0;
	Uint32 uAuroraObjPrevRowAddr = 0;
	Bool bAuroraObjHavePrevRow = FALSE;
#endif
#if SNDBG_LOG && SNPPU_OBJ_CACHE
	Uint32 uCacheHits = 0;
	Uint32 uCacheMisses = 0;
#endif
#if SNDBG_DEEP
	Bool bTrace = g_DbgCaptureActive &&
		(iLine == 112 || iLine == 160);
	if (bTrace)
		DLog("[snes-obj-trace] f=%u line=%d selected=%d base/name=%04X/%04X",
			(unsigned)g_DbgCaptureFrameNo, (int)iLine, (int)nObjList,
			(unsigned)uBaseAddr, (unsigned)uNameSelect);
#endif

	// A busca reversa e' intencional: o PPU primeiro seleciona ate' 32 OBJ
	// na ordem iniciada por OAMPRI e depois busca ate' 34 tiles na ordem
	// inversa. _RenderOBJ8 percorre o resultado de volta para preservar a
	// prioridade. Nao inverter este laco como "correcao" de time-over.
	while (--nObjList >= 0)
	{
		SnesRenderObjT *pObj;
		Int32 ObjX, ObjY;
		Uint32 uSize;

        // get pointer to obj
		pObj = pObjBase + pObjList[nObjList];

		uSize = pObj->uWidth;

		// get obj position
		ObjX = pObj->uPosX;
		ObjX<<=32-9;
		ObjX>>=32-9;
		ObjY = (iLine - pObj->uPosY) & 0xFF;
		if (bObjInterlace)
		{
			/* One display line addresses two source rows. Vertical flip is
			 * resolved before field parity; on the flipped field hardware
			 * subtracts the field bit instead of adding it. */
			ObjY <<= 1;
			ObjY ^= pObj->uVXOR;
			if (pObj->uVXOR)
				ObjY -= bField ? 1 : 0;
			else
				ObjY += bField ? 1 : 0;
			ObjY &= 0xFF;
		}
		else
		{
			ObjY ^= pObj->uVXOR;
			ObjY &= pObj->uHeight - 1;
		}

		Uint32 uTileAddr;
		Uint32 uTile0, uTile1, uOpaque;
#if !SNPPU_OBJ_CACHE
		SnesChrLookupT *pLookup;
		Uint8 *pHFlip;
#endif
		Uint32 uTile;

#if !SNPPU_OBJ_CACHE
		if (pObj->bHFlip)
		{
			pLookup = &_SnesPPU_PlaneLookup[1];
			pHFlip  = _SnesPPU_HFlipLookup[0];
		} else
		{
			pLookup = &_SnesPPU_PlaneLookup[0];
			pHFlip  = _SnesPPU_HFlipLookup[1];
		}
#endif

		// SNES OBJ: dentro de um sprite, a COLUNA (nibble baixo) e a LINHA
		// (nibble alto) do numero do tile avancam SEPARADAMENTE, com wrap
		// dentro da tabela de 256 tiles. O bit 8 seleciona a segunda tabela,
		// cujo deslocamento completo vem de OBSEL; ele nao faz parte de uTile.
		// Ex.: um 16x16 no tile $0F usa $0F,$00,$1F,$10. O codigo antigo
		// somava linear (+chry*0x10 e pTile4++), estourando os nibbles e
		// embaralhando sprites grandes que cruzam essas fronteiras (bug
		// classico em Final Fight 2 e afins).
		{
		Bool bSecondTable = (pObj->uTile & 0x100) != 0;
		Uint32 uRow  = ((pObj->uTile >> 4) + (ObjY >> 3)) & 0x0F;
		Uint32 uCol0 = pObj->uTile & 0x0F;
		Uint32 uYoff = ObjY & 7;
		Int32 iTileX;
		Int32 nTileCount;
		Int32 iCol;
		Int32 iColStep;
		/* AURORA_OBJ_PALETTE_HOIST_V1 */
		/* AURORA_OBJ_INVARIANT_HOIST_V3_22_20261006
		 * Immutable for every fetched sliver of this OBJ.  Keeping these in
		 * explicit locals avoids repeated structure loads in the hot tile loop.
		 * uObjRowBase is the exact pre-mask VRAM word base for this tile row. */
		const Uint8 uObjPri = pObj->uPri;
		const Uint8 uObjPal = pObj->uPal;
		const Bool bObjHFlip = pObj->bHFlip;
		const Uint32 uPalette = _SnesPPU_Obj4PalLookup[uObjPal];
		const Uint32 uObjRowBase =
			uBaseAddr + (bSecondTable ? uNameSelect : 0u) + (uRow << 8);

#if SNDBG_DEEP
		if (bTrace)
		{
			Uint32 uTraceHash = 2166136261u;
			Uint32 uFirstAddr = 0;
			Uint32 uLastAddr = 0;
			Int32 iTraceTile;

			for (iTraceTile = 0; iTraceTile < (pObj->uWidth >> 3);
			     iTraceTile++)
			{
				Int32 iSource = _SnesPPUOBJSourceColumn(iTraceTile,
					pObj->uWidth, pObj->bHFlip);
				Uint32 uTraceTile = (uRow << 4) |
					((uCol0 + iSource) & 0x0F);
				Uint32 uTraceAddr = uBaseAddr + uTraceTile * 16;
				Uint32 uWord0;
				Uint32 uWord8;

				if (bSecondTable)
					uTraceAddr += uNameSelect;
				uTraceAddr = (uTraceAddr & 0x7FFF) + uYoff;
				if (!iTraceTile)
					uFirstAddr = uTraceAddr;
				uLastAddr = uTraceAddr;
				uWord0 = pVram[uTraceAddr];
				uWord8 = pVram[uTraceAddr + 8];
				uTraceHash = (uTraceHash ^ uWord0) * 16777619u;
				uTraceHash = (uTraceHash ^ uWord8) * 16777619u;
			}
			DLog("[snes-obj-trace] f=%u line=%d idx=%u xy=%d/%u wh=%u/%u tile/row/yoff=%03X/%X/%u pal/pri/h/vx=%u/%u/%u/%u addr=%04X..%04X vhash=%08X",
				(unsigned)g_DbgCaptureFrameNo, (int)iLine,
				(unsigned)pObjList[nObjList], (int)ObjX,
				(unsigned)pObj->uPosY, (unsigned)pObj->uWidth,
				(unsigned)pObj->uHeight, (unsigned)pObj->uTile,
				(unsigned)uRow, (unsigned)uYoff, (unsigned)pObj->uPal,
				(unsigned)pObj->uPri, (unsigned)pObj->bHFlip,
				(unsigned)pObj->uVXOR, (unsigned)uFirstAddr,
				(unsigned)uLastAddr, (unsigned)uTraceHash);
		}
#endif

		/* AURORA_REVIVE_A06DD_OBJ_HOTPATH_20260829
		 * Recorta a faixa uma vez e elimina teste/source-column por tile. */
		_SnesPPUOBJCountedTileRange(pObj->uPosX, ObjX, pObj->uWidth,
			&iTileX, &nTileCount);
		ObjX += iTileX << 3;
		uSize = (Uint32)nTileCount << 3;
		if (bObjHFlip)
		{
			iCol = (pObj->uWidth >> 3) - 1 - iTileX;
			iColStep = -1;
		}
		else
		{
			iCol = iTileX;
			iColStep = 1;
		}

		while (uSize > 0)
		{
			{
				/* AURORA_OBJ_INVARIANT_HOIST_V3_22_20261006:
				 * row/name-table terms are already in uObjRowBase. */
				uTile = (uCol0 + iCol) & 0x0F;
				uTileAddr = uObjRowBase + (uTile << 4);
				Uint32 uRowAddr = (uTileAddr & 0x7FFF) + uYoff;
#if AURORA_FRONTEND_PROFILER
				if (bObjHFlip)
					++uAuroraObjHFlipTiles;
				if (bSecondTable)
					++uAuroraObjSecondTableTiles;

				/* AURORA_OBJ_MICROSCOPE_V3_25_20261006
				 * Row-locality bookkeeping is outside the timed cache+emit window. */
				if (bAuroraObjHavePrevRow)
				{
					const Uint32 uDelta =
						(uRowAddr - uAuroraObjPrevRowAddr) & SNPPU_VRAM_WORD_MASK;
					if (uDelta == 0u) ++uAuroraObjRowSame;
					else if (uDelta == 16u) ++uAuroraObjRowPlus16;
					else if (uDelta == (SNPPU_VRAM_WORD_MASK + 1u - 16u))
						++uAuroraObjRowMinus16;
					else ++uAuroraObjRowOther;
				}
				bAuroraObjHavePrevRow = TRUE;
				uAuroraObjPrevRowAddr = uRowAddr;
				Bool bAuroraObjCacheHit = FALSE;
				const Uint32 uAuroraObjPathStart = ProfCtrGetCycle();
#endif

#if SNPPU_OBJ_CACHE
				{
					Uint64 uRowData;

					if (SnesPPUChrCacheLookup4(&_SnesPPU_ChrCache,
						uRowAddr, bObjHFlip, &uRowData, &uOpaque))
					{
#if AURORA_FRONTEND_PROFILER
						++uAuroraObjCacheHits;
						bAuroraObjCacheHit = TRUE;
#endif
#if SNDBG_LOG
						uCacheHits++;
#endif
					}
					else
					{
#if AURORA_FRONTEND_PROFILER
						++uAuroraObjCacheMisses;
#endif
#if SNDBG_LOG
						uCacheMisses++;
#endif
#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
						const Uint16 uPair01 = pVram[uRowAddr];
						const Uint16 uPair23 = pVram[uRowAddr + 8];
						_DecodeOBJRow4ScratchPS2(
							uPair01, uPair23, &uRowData, &uOpaque);
#else
						const SnesPPUTile4T *pTile4 =
							(const SnesPPUTile4T *)(pVram + uRowAddr);
						Uint32 uPlane0 = pTile4->uPlane01[0][0];
						Uint32 uPlane1 = pTile4->uPlane01[0][1];
						Uint32 uPlane2 = pTile4->uPlane23[0][0];
						Uint32 uPlane3 = pTile4->uPlane23[0][1];
						_DecodeOBJRow4(&_SnesPPU_PlaneLookup[0],
							_SnesPPU_HFlipLookup[1], uPlane0, uPlane1,
							uPlane2, uPlane3, &uTile0, &uTile1, &uOpaque);
						uRowData = (Uint64)uTile0 | ((Uint64)uTile1 << 32);
#endif
#if AURORA_FRONTEND_PROFILER
						if (uOpaque)
							++uAuroraObjMissOpaque;
						else
							++uAuroraObjMissTransparent;
#endif
						SnesPPUChrCacheStore4(&_SnesPPU_ChrCache,
							uRowAddr, uRowData, uOpaque);
						/* AURORA_HFLIP_MISS_REUSE_V1 */
						if (bObjHFlip)
						{
#if SNPPU_CHR_CACHE_HFLIP
							SnesPPUChrCacheLoad4HFlip(
								&_SnesPPU_ChrCache,
								uRowAddr, &uRowData, &uOpaque);
#else
							SnesPPUChrCacheFlipRow(&uRowData, &uOpaque);
#endif
						}
					}
					uTile0 = (Uint32)uRowData;
					uTile1 = (Uint32)(uRowData >> 32);
				}
#else
				{
					const SnesPPUTile4T *pTile4 =
						(const SnesPPUTile4T *)(pVram + uRowAddr);
					Uint32 uPlane0 = pTile4->uPlane01[0][0];
					Uint32 uPlane1 = pTile4->uPlane01[0][1];
					Uint32 uPlane2 = pTile4->uPlane23[0][0];
					Uint32 uPlane3 = pTile4->uPlane23[0][1];
					_DecodeOBJRow4(pLookup, pHFlip, uPlane0, uPlane1,
						uPlane2, uPlane3, &uTile0, &uTile1, &uOpaque);
#if AURORA_FRONTEND_PROFILER
					++uAuroraObjCacheMisses;
					if (uOpaque)
						++uAuroraObjMissOpaque;
					else
						++uAuroraObjMissTransparent;
#endif
				}
#endif
				uTile0 |= uPalette;
				uTile1 |= uPalette;

				// store tile data
				((Uint32 *)pObjLine->uData)[0] = uTile0;
				((Uint32 *)pObjLine->uData)[1] = uTile1;
				pObjLine->uData[SNPPU_BGPLANE_OPAQUE] = (Uint8)uOpaque;

				// store objline
				pObjLine->uPri  = uObjPri;
				pObjLine->uPal  = uObjPal;
				pObjLine->iPosX = ObjX;
#if AURORA_FRONTEND_PROFILER
				{
					const Uint32 uPathCycles =
						ProfCtrGetCycle() - uAuroraObjPathStart;
					if (bAuroraObjCacheHit)
					{
						uAuroraObjHitCycles += uPathCycles;
						if (uOpaque == 0u) ++uAuroraObjHitTransparent;
						else if ((uOpaque & 0xffu) == 0xffu) ++uAuroraObjHitFullOpaque;
						else ++uAuroraObjHitPartial;
					}
					else
					{
						uAuroraObjMissCycles += uPathCycles;
						if (uOpaque == 0u) ++uAuroraObjMicMissTransparent;
						else if ((uOpaque & 0xffu) == 0xffu) ++uAuroraObjMicMissFullOpaque;
						else ++uAuroraObjMicMissPartial;
					}
				}
#endif
				pObjLine++;
				nObjLine++;
				if (nObjLine >= MaxObj8Line) goto FetchOBJDone;
			}

			/* OBJ fetch always advances left-to-right. H-flip mirrors the
			   source column, not the fetch position; this matters when the
			   hardware stops at the 34-tile scanline limit. */
			ObjX += 8;
			uSize -= 8;
			iCol += iColStep;
		}
		}

		// next obj
//		pObjList++;
//		nObjList--;
	}

FetchOBJDone:
#if AURORA_FRONTEND_PROFILER
	AuroraFrontendProfilerSnesObjMicroscopeRecord(
		uAuroraObjCacheHits, uAuroraObjHitCycles,
		uAuroraObjCacheMisses, uAuroraObjMissCycles,
		uAuroraObjHitTransparent, uAuroraObjHitFullOpaque, uAuroraObjHitPartial,
		uAuroraObjMicMissTransparent, uAuroraObjMicMissFullOpaque,
		uAuroraObjMicMissPartial,
		uAuroraObjRowSame, uAuroraObjRowPlus16,
		uAuroraObjRowMinus16, uAuroraObjRowOther);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_CACHE_HITS,
		0u, uAuroraObjCacheHits);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_CACHE_MISSES,
		0u, uAuroraObjCacheMisses);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_MISS_TRANSPARENT,
		0u, uAuroraObjMissTransparent);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_MISS_OPAQUE,
		0u, uAuroraObjMissOpaque);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_HFLIP_TILES,
		0u, uAuroraObjHFlipTiles);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_SECOND_TABLE_TILES,
		0u, uAuroraObjSecondTableTiles);
#endif
#if SNDBG_LOG && SNPPU_OBJ_CACHE
	g_DbgObjCacheHits += uCacheHits;
	g_DbgObjCacheMisses += uCacheMisses;
#endif
	return nObjLine;
}

static void _ClearLinePlanar(SNMaskT *pPlanes, Int32 nPlanes)
{
	Int32 iPlane;
	for (iPlane=0; iPlane < nPlanes; iPlane++)
	{
		SNMaskClear(&pPlanes[iPlane]);
	}
}



void SnesPPURender::RenderLine8(Int32 iLine, SnesRender8pInfoT *pRenderInfo)
{
	SNMaskT BG3Pri;
	SNMaskT M7BG2MainPri;
	SNMaskT M7BG2SubPri;
	Bool bBG3Pri;
	/* AURORA_MODE7_EXTBG_COMPOSITOR_V1_20260903: EXTBG BG2 main/sub priority masks. */
	SnesBGInfoT	BGInfo[4];
	/* Physical S-PPU time-over capacity: at most 34 OBJ slivers/scanline. */
	/* AURORA_OBJ_POSTFETCH_COMPACT_R5900_V3_40_20261008
	 * Quadword alignment is required only by the post-fetch leaf compactor. */
	SnesRenderObj8T ObjLine[SNPPU_MAXOBJCHR] _ALIGN(16);
	Int32 nObjLine;
	Int32 nObjRenderLine;
	const SnesPPURegsT *pRegs = m_pPPU->GetRegs();
	/* AURORA_SAFE_CODE_PERF_V1_PPU
	 * RenderLine8 is synchronous: these registers cannot change underneath the
	 * scanline. Snapshot fields repeatedly used across helper calls. */
	const Uint8 uBGModeReg = pRegs->bgmode;
	const Uint8 uBGMode = (Uint8)(uBGModeReg & 7);
	/* AURORA_PPU_HIRES_TS_CORRECTNESS_V1_20260929
	 * In Modes 5/6 and SETINI pseudo-hires, TS is not merely a color-math
	 * operand: it supplies alternating visible sub-screen half-pixels. */
	const Bool bHiresSubscreenVisible =
		(uBGMode == 5) || (uBGMode == 6) ||
		((pRegs->setini & SNESPPU_SETINI_PSEUDOHIR) != 0);
	const Bool bMode7ExtBG =
		(uBGMode == 7) && ((pRegs->setini & 0x40) != 0);
	const Uint8 uOBSEL = pRegs->obsel;

	/* Physical OBJ evaluation is independent of Aurora's software layer
	 * mask. RenderLine8 is reached only while not force-blanked, so publish
	 * the sticky STAT77 state for this scanline before host policy can elide
	 * sprite drawing. */
	m_pPPU->SetObjOverflow(
		m_ObjRangeOver[iLine] != 0,
		m_ObjTimeOver[iLine] != 0);

	/* AURORA_V85_SOFTWARE_LAYER_MASK
	 * Mask before fetch/decode so disabled layers really save EE work. */
	const Uint8 uSoftwareLayers = SNPPURenderGetSoftwareLayerMask();
	const Uint8 uSoftwareHacks = SNPPURenderGetSoftwareHackFlags();
	Uint8 tm = (Uint8)(pRegs->tm & _tm & uSoftwareLayers);
	Uint8 tmw = (Uint8)(pRegs->tmw & _tmw & uSoftwareLayers);
	Uint32 cgadsub = (pRegs->cgadsub & 0x3F);
	Uint8 ts = (Uint8)(pRegs->ts & _ts & uSoftwareLayers);
	Uint8 tsw = (Uint8)(pRegs->tsw & _tsw & uSoftwareLayers);
	Uint8 cgwsel = pRegs->cgwsel;
	Uint8 uFetchLayers;
	/* AURORA_SNES_PPU_FOCUS_V3_20260920: diagnostic work counters only. */
#if AURORA_SNES_COST_PROFILER || AURORA_FRONTEND_PROFILER
	Uint32 uAuroraBgMapFetches = 0;
	Uint32 uAuroraBgChrDecodes = 0;
#endif

	if (uSoftwareHacks & SNPPU_HACK_COLOR_MATH_OFF)
		cgadsub = 0;

	if (uSoftwareHacks & SNPPU_HACK_WINDOWS_OFF)
	{
		tmw = 0;
		tsw = 0;
		/* Preserve direct-color/subscreen selection (bits 0-1). */
		cgwsel &= 0x03;
	}
	Bool bRendered;
	const Bool bMode34Direct =
		((cgwsel & 0x01) != 0) && (uBGMode == 3 || uBGMode == 4);
	const Bool bMode7Direct =
		((cgwsel & 0x01) != 0) && (uBGMode == 7);
	const Bool bBG1Direct = bMode34Direct || bMode7Direct;
	Uint8 *pDirectAttrib =
		bBG1Direct ? pRenderInfo->BlendInfo.uAttrib8 : NULL;

	if (pDirectAttrib)
		memset(pDirectAttrib, 0, 256);

	SNMaskT *pMain = pRenderInfo->Main;
	SNMaskT *pSub = pRenderInfo->Sub;
	SNMaskT *pBGWindow = pRenderInfo->BGWindow;
    SNMaskT *pMainAddSubMask = &pRenderInfo->MainAddSubMask;
	/* AURORA_TOPGEAR_NO_COLORMATH_MASKWRITE_V6_20260917
	 * cgadsub==0 explicitly clears MainAddSubMask below. Every BG/OBJ
	 * update would only clear subsets of that zero mask, so suppress
	 * those writes. Nonzero CGADSUB retains the exact original pointer. */
	SNMaskT *pMainAddSubWriteMask =
		cgadsub ? pMainAddSubMask : NULL;
	SNMaskT *pSubAddSubMask = &pRenderInfo->SubAddSubMask;
    Uint8  *pMain8 = pRenderInfo->BlendInfo.uMain8;
    Uint8  *pSub8 = pRenderInfo->BlendInfo.uSub8;

	/* AURORA_MODE7_EXTBG_COMPOSITOR_V1_20260903
	 * Keep BG1/BG2 identities independent in Mode 7 EXTBG. */
	if (uBGMode == 7)
	{
		Uint8 uMode7LayerMask = SNESPPU_MASK_BG1 | SNESPPU_MASK_OBJ;
		if (bMode7ExtBG)
			uMode7LayerMask |= SNESPPU_MASK_BG2;

		tm  &= uMode7LayerMask;
		ts  &= uMode7LayerMask;
		tmw &= uMode7LayerMask;
		tsw &= uMode7LayerMask;
	}

	/* AURORA_PPU_HIRES_TS_CORRECTNESS_V1_20260929
	 * In ordinary 256-dot modes, fixed-color/no-CGADSUB can elide TS.
	 * Hires is the exception because TS itself is visible. */
	if (!bHiresSubscreenVisible && (!(cgwsel & 0x02) || cgadsub == 0))
		ts = 0;
	uFetchLayers = tm | ts;
#if AURORA_C4_PROFILER
	AuroraC4ProfilerPpuLine(uBGMode,
		((pRegs->mosaic & 0x0Fu) != 0) ? TRUE : FALSE,
		((tmw | tsw) != 0 || (cgwsel & 0xF0u) != 0) ? TRUE : FALSE,
		(cgadsub != 0) ? TRUE : FALSE,
		(uBGMode == 5 || uBGMode == 6) ? TRUE : FALSE,
		((pRegs->setini & SNESPPU_SETINI_PSEUDOHIR) != 0) ? TRUE : FALSE);
#endif

#if SNDBG_LOG
	g_DbgBGActiveLayers +=
		((uFetchLayers & SNESPPU_MASK_BG1) ? 1u : 0u) +
		((uFetchLayers & SNESPPU_MASK_BG2) ? 1u : 0u) +
		((uFetchLayers & SNESPPU_MASK_BG3) ? 1u : 0u) +
		((uFetchLayers & SNESPPU_MASK_BG4) ? 1u : 0u);
	Uint32 _tBGInfo = ProfCtrGetCycle();
#endif

	/* PPU detail: BG info */
	AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_OTHER);
	PROF_ENTER("DecodeBGInfo");
	if (uBGMode == 7)
	{
		/* AURORA_MODE7_MIN_BGINFO_V2 / AURORA_MODE7_EXTBG_COMPOSITOR_V1_20260903 */
		BGInfo[0].uBitDepth = 8;
		BGInfo[0].uScrollX = 0;
		BGInfo[0].Priority = 0;
		BGInfo[1].uBitDepth = bMode7ExtBG ? 7 : 0;
		BGInfo[1].uScrollX = 0;
		BGInfo[1].Priority = 4;
		BGInfo[2].uBitDepth = 0;
		BGInfo[3].uBitDepth = 0;
	}
	else
	{
		DecodeBGInfo(BGInfo);
	}
	PROF_LEAVE("DecodeBGInfo");
	AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_OTHER);
#if SNDBG_LOG
	g_TmgCycBGInfo += ProfCtrGetCycle() - _tBGInfo;
#endif

	// fetch obj chr for visible objs
	/* PPU detail: OBJ fetch */
	AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_OBJ_FETCH);
	PROF_ENTER("FetchOBJ");
#if SNDBG_LOG
	Uint32 _tObjA = ProfCtrGetCycle();
#endif
	if (uFetchLayers & SNESPPU_MASK_OBJ)
	{
		Uint8 rotated[SNPPU_MAXOBJ];
		Uint8 *list=m_ObjLine[iLine];
		Int32 count=m_nObjLine[iLine];
		Int32 budget=SNPPURenderGetObjTileBudget();
		/* AURORA_SAFE_HOTPATH_V4: when budget is the physical 34-tile limit, limiter
		   pressure is inactive and the potential cache is intentionally ignored. */
		if (budget<SNPPU_MAXOBJCHR && count>1 &&
		    m_nObjTilePotential[iLine]>(Uint16)budget)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraObjRotateStart = ProfCtrGetCycle();
#endif
			Int32 shift=(Int32)(g_SnesObjLimitFramePhase%(Uint32)count);
			for (Int32 i=0;i<count;i++) rotated[i]=list[(i+shift)%count];
			list=rotated;
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_ROTATE,
				ProfCtrGetCycle() - uAuroraObjRotateStart, (Uint32)count);
#endif
		}
#if AURORA_FRONTEND_PROFILER
		const Uint32 uAuroraObjFetchStart = ProfCtrGetCycle();
#endif
		nObjLine=_FetchOBJ(m_Objs,list,count,ObjLine,budget,iLine,(uOBSEL&7)<<13,
			_SnesPPUOBJNameSelect(uOBSEL),m_pPPU->GetVramPtr(0),
			m_pPPU->IsObjInterlace(), m_pPPU->GetField());
#if AURORA_FRONTEND_PROFILER
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_FETCH_DECODE,
			ProfCtrGetCycle() - uAuroraObjFetchStart, (Uint32)nObjLine);
#endif
	}
	else
		nObjLine = 0;
#if AURORA_C4_PROFILER
	AuroraC4ProfilerObjLine((Uint32)m_nObjLine[iLine],
		(Uint32)m_nObjTilePotential[iLine], (Uint32)nObjLine);
#endif
#if SNDBG_LOG
	{
		Uint32 _dObjFetch = ProfCtrGetCycle() - _tObjA;
		g_TmgCycObj += _dObjFetch;
		g_TmgCycObjFetch += _dObjFetch;
	}
	{
		Bool _objEnabled = ((tm | ts) & SNESPPU_MASK_OBJ) != 0;
		#if SNDBG_DEEP
		Int32 _i;
		Uint32 _opaque = 0;
		for (_i = 0; _i < nObjLine; _i++)
			if (ObjLine[_i].uData[SNPPU_BGPLANE_OPAQUE] != 0) _opaque++;
		#endif

		if (_objEnabled) g_DbgObjEnabledLines++;
		g_DbgObjOamRefs += m_nObjLine[iLine];
		g_DbgObjTiles += nObjLine;
		#if SNDBG_DEEP
		g_DbgObjOpaqueTiles += _opaque;
		if (_objEnabled && nObjLine > 0 && _opaque == 0) g_DbgObjEmptyLines++;
		#endif
		if (m_ObjRangeOver[iLine]) g_DbgObjRangeLimitLines++;
		if (m_ObjTimeOver[iLine]) g_DbgObjLimitLines++;
		g_DbgObjOBSEL = uOBSEL;
		g_DbgObjTM = pRegs->tm;
		g_DbgObjTS = pRegs->ts;
		g_DbgObjPriority = pRegs->oampri.w;
	}
#endif

	/* AURORA_OBJ_POSTFETCH_COMPACT_R5900_V3_40_20261008
	 * IMPORTANT: this is deliberately after every physical-fetch diagnostic.
	 * _FetchOBJ(), cache order, MaxObj8Line/STAT77 accounting and SNDBG physical
	 * counters therefore see the original list.  Only the render-list count is
	 * reduced.  A one-row line is cheaper to leave to the renderer's existing
	 * uOpaque test than to call the compactor. */
	nObjRenderLine = nObjLine;
#if CODE_PLATFORM == CODE_PS2 && AURORA_PPU_R5900_ASM
	if (nObjRenderLine > 1)
	{
#if AURORA_FRONTEND_PROFILER
		const Uint32 uAuroraObjCompactStart = ProfCtrGetCycle();
#endif
		nObjRenderLine = AuroraPPUCompactOBJTransparentPS2(
			ObjLine, nObjRenderLine);
#if AURORA_FRONTEND_PROFILER
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_OBJ_COMPACT,
			ProfCtrGetCycle() - uAuroraObjCompactStart,
			(Uint32)(nObjLine - nObjRenderLine));
#endif
	}
#endif
	PROF_LEAVE("FetchOBJ");
	AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_OBJ_FETCH);

	AURORA_SNES_PPU_LINE(
		(Uint32)uBGMode, (Uint32)m_nObjLine[iLine], (Uint32)nObjLine,
		((tm | ts) & SNESPPU_MASK_OBJ) ? TRUE : FALSE,
		m_ObjRangeOver[iLine] ? TRUE : FALSE,
		m_ObjTimeOver[iLine] ? TRUE : FALSE,
		(tmw | tsw) ? TRUE : FALSE, cgadsub ? TRUE : FALSE,
		ts ? TRUE : FALSE, bHiresSubscreenVisible ? TRUE : FALSE,
		bBG1Direct ? TRUE : FALSE);

	if (uBGMode!=7)
	{
		/* PPU detail: non-Mode7 BG fetch/decode */
		AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_OTHER);
		Int32 iBG;
		Uint8 uBGFlags[4];
		Uint16 *pOffset = NULL;
		Uint32 uOffsetOR = 0;
		/* AURORA_SNES_SAFE_PERF_V9_20260919 / AURORA_SNES_SAFE_PERF_V9_BG_20260919: scanline invariant reused by offset BG paths. */
		const Bool bMode4 = (uBGMode == 4);

		AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_MAP);
		if ((uBGMode==2 || bMode4 || uBGMode==6) &&
		    (uFetchLayers & (SNESPPU_MASK_BG1 | SNESPPU_MASK_BG2)))
		{
			pOffset = pRenderInfo->BGOffset;

			// fetch offsets
#if SNDBG_LOG
			Uint32 _tBGOffset = ProfCtrGetCycle();
#endif
			uOffsetOR  = FetchOffset(&BGInfo[2], pOffset, iLine, pRenderInfo->uBGVramAddr[2], bMode4 ? FALSE : TRUE);
#if SNDBG_LOG
			g_TmgCycBGOffset += ProfCtrGetCycle() - _tBGOffset;
#endif
		}

#if SNDBG_LOG
		Uint32 _tBGMap = ProfCtrGetCycle();
#endif
		for (iBG=0; iBG <= 3; iBG++)
		{
			const Uint8 uLayerMask = (Uint8)(1u << iBG);
			if (!(uFetchLayers & uLayerMask))
			{
				uBGFlags[iBG] = 0;
				continue;
			}
#if AURORA_SNES_COST_PROFILER || AURORA_FRONTEND_PROFILER
			++uAuroraBgMapFetches;
#endif

			/* AURORA_SNES_SAFE_PERF_V9_BG_20260919: compute this active layer's offset bit once. */
			const Uint32 uOffsetMask = 0x2000u << iBG;

			// is offset enabled for this BG layer?
			if (uOffsetOR & uOffsetMask)
			{
				// fetch BGline with offset
				PROF_ENTER("FetchBGOffset");
				uBGFlags[iBG] = FetchBGOffset(&BGInfo[iBG], pRenderInfo->Tiles[iBG], 33, iLine, pOffset, uOffsetMask, bMode4);
				PROF_LEAVE("FetchBGOffset");

				// invalidate cache
				pRenderInfo->uBGVramAddr[iBG] = 0xFFFFFFFF;
			} else
			{
				// fetch line without offset
				PROF_ENTER("FetchBG");
				uBGFlags[iBG] = FetchBG(&BGInfo[iBG], pRenderInfo->Tiles[iBG], 33, iLine, pRenderInfo->uBGVramAddr[iBG]);
				PROF_LEAVE("FetchBG");
			}
		}
#if SNDBG_LOG
		g_TmgCycBGMap += ProfCtrGetCycle() - _tBGMap;
		Uint32 _tBGChr = ProfCtrGetCycle();
#endif
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_MAP);

		AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_CHR);
		PROF_ENTER("BGCHR");
		for (iBG=0; iBG <= 3; iBG++)
		{
			if (uBGFlags[iBG] & SNPPU_BGFLAGS_FETCHCHR)
			{
#if AURORA_SNES_COST_PROFILER || AURORA_FRONTEND_PROFILER
				++uAuroraBgChrDecodes;
#endif
				Uint8 TempMask[2][SNPPU_BGPLANE_SIZE];
#if SNDBG_LOG
				g_DbgBGChrRows += 33;
#endif
				const Uint32 uFineX = BGInfo[iBG].uScrollX & 7;
				const Uint32 uMosaic = BGInfo[iBG].uMosaic;
#if AURORA_FRONTEND_PROFILER
				const Uint32 uAuroraBgChrStart = ProfCtrGetCycle();
#endif

				// fetch bg tile data
				#if SNPPURENDER_CHR64
				_FetchCHR_64((Uint8 *)pRenderInfo->BGPlanes[iBG], m_pPPU, &BGInfo[iBG], pRenderInfo->Tiles[iBG], 33, iLine, TempMask[0], (uBGFlags[iBG]&SNPPU_BGFLAGS_OFFSET), FALSE);
				#else
				_FetchCHR((Uint8 *)pRenderInfo->BGPlanes[iBG], m_pPPU, &BGInfo[iBG], pRenderInfo->Tiles[iBG], 33, iLine, TempMask[0], (uBGFlags[iBG]&SNPPU_BGFLAGS_OFFSET));
				#endif
#if AURORA_FRONTEND_PROFILER
				const Uint32 uAuroraBgChrDecodeEnd = ProfCtrGetCycle();
#endif

				// shift mask based on h-scroll of BG
				/* AURORA_SNES_SAFE_PERF_V9_BG_20260919: identical source value for both mask planes. */
#if CODE_PLATFORM == CODE_PS2 && AURORA_V33_SNES_MASK_PAIR_FUSED
				if (uFineX == 0) /* AURORA_V37_SNES_MASK_PAIR_ZERO_FINE_ALL_BG_20261011 */
				{
					AuroraV33CopyMode1MaskPairPS2(
						&pRenderInfo->BGPlanes[iBG][SNPPU_BGPLANE_OPAQUE],
						&pRenderInfo->BGPlanes[iBG][SNPPU_BGPLANE_PRI], TempMask[0]);
				}
				else
#endif
				{
				SNMaskSHL(&pRenderInfo->BGPlanes[iBG][SNPPU_BGPLANE_OPAQUE], TempMask[0], uFineX);
				SNMaskSHL(&pRenderInfo->BGPlanes[iBG][SNPPU_BGPLANE_PRI], TempMask[1], uFineX);
				}

				if (uMosaic > 0)
				{
					const Uint32 uMosaicSize = uMosaic + 1;
					_MosaicBGPlanar((Uint8 *)&pRenderInfo->BGPlanes[iBG][SNPPU_BGPLANE_OPAQUE], 256, uMosaicSize);
					_MosaicBGPlanar((Uint8 *)&pRenderInfo->BGPlanes[iBG][SNPPU_BGPLANE_PRI], 256, uMosaicSize);
				}
#if AURORA_FRONTEND_PROFILER
				AURORA_SNES_BGCHR_RECORD(
					(Uint32)iBG, (Uint32)BGInfo[iBG].uBitDepth, 33u,
					(uBGFlags[iBG] & SNPPU_BGFLAGS_OFFSET) ? TRUE : FALSE,
					uMosaic > 0 ? TRUE : FALSE, uFineX != 0 ? TRUE : FALSE, FALSE,
					uAuroraBgChrStart, uAuroraBgChrDecodeEnd, ProfCtrGetCycle());
				if (BGInfo[iBG].uBitDepth == 2 || BGInfo[iBG].uBitDepth == 4)
				{
					const Uint32 uAuroraV23TraitsBegin = ProfCtrGetCycle(); /* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009 */
					Uint32 uAuroraOpaqueRows = 0;
					Uint32 uAuroraFlip0OpaqueRows = 0;
					for (Uint32 uAuroraTile = 0; uAuroraTile < 33u; ++uAuroraTile)
					{
						if (TempMask[0][uAuroraTile] == 0) continue;
						++uAuroraOpaqueRows;
						if (pRenderInfo->Tiles[iBG][uAuroraTile].uFlip == 0)
							++uAuroraFlip0OpaqueRows;
					}
					AURORA_SNES_CHR_TRAITS_RECORD((Uint32)BGInfo[iBG].uBitDepth,
						uAuroraOpaqueRows, 33u - uAuroraOpaqueRows,
						uAuroraFlip0OpaqueRows, uAuroraOpaqueRows - uAuroraFlip0OpaqueRows);
					AuroraFrontendProfilerSnesChrTraitsCost(
					    (Uint32)(ProfCtrGetCycle() - uAuroraV23TraitsBegin));
					if (BGInfo[iBG].uBitDepth == 4)
						_AuroraProfileCHR4Locality(&BGInfo[iBG],
							pRenderInfo->Tiles[iBG], 33u, iLine, TempMask[0],
							(Uint32)iBG, _AuroraChr4MainPath((Uint32)uBGMode,
								(uBGFlags[iBG] & SNPPU_BGFLAGS_OFFSET) ? TRUE : FALSE),
							(uBGFlags[iBG] & SNPPU_BGFLAGS_OFFSET) ? TRUE : FALSE);
				}
#endif
			}
		}

#if SNPPURENDER_CHR64
		/* AURORA_V7_MODE56_HIRES_20260915
		 * BG3/BG4 are invisible in modes 5/6, so their line planes are
		 * safe scratch for the exact EVEN subscreen phase:
		 *   Mode 5 BG1 -> plane 2, BG2 -> plane 3
		 *   Mode 6 BG1 -> plane 2
		 * Decode whenever TS actually uses the layer, independent of the
		 * main-plane row cache, so dynamic TS changes cannot expose stale
		 * alternate data. */
		if (uBGMode == 5 || uBGMode == 6)
		{
			for (iBG = 0; iBG < 2; iBG++)
			{
				if (!(ts & (1 << iBG)) || !BGInfo[iBG].uBitDepth)
					continue;
#if AURORA_SNES_COST_PROFILER || AURORA_FRONTEND_PROFILER
				++uAuroraBgChrDecodes;
#endif

				const Int32 iSubPlane = iBG + 2;
				Uint8 SubPhaseMask[2][SNPPU_BGPLANE_SIZE];
				const Uint32 uAuroraHiresFineX = BGInfo[iBG].uScrollX & 7;
				const Uint32 uAuroraHiresMosaic = BGInfo[iBG].uMosaic;
#if AURORA_FRONTEND_PROFILER
				const Uint32 uAuroraHiresBgChrStart = ProfCtrGetCycle();
#endif

				_FetchCHR_64(
					(Uint8 *)pRenderInfo->BGPlanes[iSubPlane],
					m_pPPU,
					&BGInfo[iBG],
					pRenderInfo->Tiles[iBG],
					33,
					iLine,
					SubPhaseMask[0],
					(uBGFlags[iBG] & SNPPU_BGFLAGS_OFFSET),
					TRUE
				);
#if AURORA_FRONTEND_PROFILER
				const Uint32 uAuroraHiresBgChrDecodeEnd = ProfCtrGetCycle();
#endif

				SNMaskSHL(
					&pRenderInfo->BGPlanes[iSubPlane][SNPPU_BGPLANE_OPAQUE],
					SubPhaseMask[0], uAuroraHiresFineX);
				SNMaskSHL(
					&pRenderInfo->BGPlanes[iSubPlane][SNPPU_BGPLANE_PRI],
					SubPhaseMask[1], uAuroraHiresFineX);

				if (uAuroraHiresMosaic > 0)
				{
					_MosaicBGPlanar(
						(Uint8 *)&pRenderInfo->BGPlanes[iSubPlane][SNPPU_BGPLANE_OPAQUE],
						256, uAuroraHiresMosaic + 1);
					_MosaicBGPlanar(
						(Uint8 *)&pRenderInfo->BGPlanes[iSubPlane][SNPPU_BGPLANE_PRI],
						256, uAuroraHiresMosaic + 1);
				}
#if AURORA_FRONTEND_PROFILER
				AURORA_SNES_BGCHR_RECORD(
					(Uint32)iBG, (Uint32)BGInfo[iBG].uBitDepth, 33u,
					(uBGFlags[iBG] & SNPPU_BGFLAGS_OFFSET) ? TRUE : FALSE,
					uAuroraHiresMosaic > 0 ? TRUE : FALSE,
					uAuroraHiresFineX != 0 ? TRUE : FALSE, TRUE,
					uAuroraHiresBgChrStart, uAuroraHiresBgChrDecodeEnd,
					ProfCtrGetCycle());
				if (BGInfo[iBG].uBitDepth == 2 || BGInfo[iBG].uBitDepth == 4)
				{
					const Uint32 uAuroraV23TraitsBegin = ProfCtrGetCycle(); /* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009 */
					Uint32 uAuroraOpaqueRows = 0;
					Uint32 uAuroraFlip0OpaqueRows = 0;
					for (Uint32 uAuroraTile = 0; uAuroraTile < 33u; ++uAuroraTile)
					{
						if (SubPhaseMask[0][uAuroraTile] == 0) continue;
						++uAuroraOpaqueRows;
						if (pRenderInfo->Tiles[iBG][uAuroraTile].uFlip == 0)
							++uAuroraFlip0OpaqueRows;
					}
					AURORA_SNES_CHR_TRAITS_RECORD((Uint32)BGInfo[iBG].uBitDepth,
						uAuroraOpaqueRows, 33u - uAuroraOpaqueRows,
						uAuroraFlip0OpaqueRows, uAuroraOpaqueRows - uAuroraFlip0OpaqueRows);
					AuroraFrontendProfilerSnesChrTraitsCost(
					    (Uint32)(ProfCtrGetCycle() - uAuroraV23TraitsBegin));
					if (BGInfo[iBG].uBitDepth == 4)
						_AuroraProfileCHR4Locality(&BGInfo[iBG],
							pRenderInfo->Tiles[iBG], 33u, iLine, SubPhaseMask[0],
							(Uint32)iBG, _AuroraChr4SubPath((Uint32)uBGMode),
							(uBGFlags[iBG] & SNPPU_BGFLAGS_OFFSET) ? TRUE : FALSE);
				}
#endif
			}
		}
#endif

		PROF_LEAVE("BGCHR");
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_CHR);
#if SNDBG_LOG
		g_TmgCycBGChr += ProfCtrGetCycle() - _tBGChr;
#endif
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_OTHER);
	} else
	{
		// mode 7
		/* PPU detail: Mode 7 transform/fetch */
		AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_MODE7);
		PROF_ENTER("BGMODE7");
#if SNDBG_LOG
		Uint32 _tM7 = ProfCtrGetCycle();
#endif
		if (uFetchLayers & (SNESPPU_MASK_BG1 | SNESPPU_MASK_BG2))
		{
			_FetchMode7((Uint8 *)pRenderInfo->BGPlanes[0], m_pPPU, iLine,
				&pRenderInfo->BGPlanes[0][SNPPU_BGPLANE_PRI],
				&pRenderInfo->BGPlanes[0][SNPPU_BGPLANE_OPAQUE]);

			if (bMode7ExtBG && (uFetchLayers & SNESPPU_MASK_BG2))
				_BuildMode7ExtBG(
					(Uint8 *)pRenderInfo->BGPlanes[1],
					&pRenderInfo->BGPlanes[1][SNPPU_BGPLANE_PRI],
					&pRenderInfo->BGPlanes[1][SNPPU_BGPLANE_OPAQUE],
					(const Uint8 *)pRenderInfo->BGPlanes[0]);

			/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
			{
				Uint32 uM7MosaicSize = (Uint32)(((pRegs->mosaic >> 4) & 0x0F) + 1);
				if (uM7MosaicSize > 1)
				{
					if (pRegs->mosaic & 0x01)
					{
#if AURORA_FRONTEND_PROFILER
						const Uint32 uAuroraM7MosaicStart = ProfCtrGetCycle();
#endif
						_MosaicBG8((Uint8 *)pRenderInfo->BGPlanes[0], 256, uM7MosaicSize);
						_MosaicBGPlanar((Uint8 *)&pRenderInfo->BGPlanes[0][SNPPU_BGPLANE_OPAQUE], 256, uM7MosaicSize);
#if AURORA_FRONTEND_PROFILER
						AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_MOSAIC_BG1,
							ProfCtrGetCycle() - uAuroraM7MosaicStart, 256u);
#endif
					}
					if (bMode7ExtBG && (uFetchLayers & SNESPPU_MASK_BG2) &&
					    (pRegs->mosaic & 0x02))
					{
#if AURORA_FRONTEND_PROFILER
						const Uint32 uAuroraM7MosaicStart = ProfCtrGetCycle();
#endif
						_MosaicBG8((Uint8 *)pRenderInfo->BGPlanes[1], 256, uM7MosaicSize);
						_MosaicBGPlanar((Uint8 *)&pRenderInfo->BGPlanes[1][SNPPU_BGPLANE_PRI], 256, uM7MosaicSize);
						_MosaicBGPlanar((Uint8 *)&pRenderInfo->BGPlanes[1][SNPPU_BGPLANE_OPAQUE], 256, uM7MosaicSize);
#if AURORA_FRONTEND_PROFILER
						AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_MOSAIC_BG2,
							ProfCtrGetCycle() - uAuroraM7MosaicStart, 256u);
#endif
					}
				}
			}
		}
#if SNDBG_LOG
		g_TmgCycM7 += ProfCtrGetCycle() - _tM7;
#endif
		PROF_LEAVE("BGMODE7");
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_MODE7);
	}

	AURORA_SNES_PPU_WORK((Uint32)tm, (Uint32)ts,
		uAuroraBgMapFetches, uAuroraBgChrDecodes);

	/* PPU detail: BG compositor (OBJ draw is nested out below). */
	AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_OTHER);
	PROF_ENTER("RenderBG");
#if SNDBG_LOG
	Uint32 _tBGMain = ProfCtrGetCycle();
#endif

	if (cgadsub & 0x20)
	{
		SNMaskSet(pMainAddSubMask);
	} else
	{
		SNMaskClear(pMainAddSubMask);
	}

	AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_MAIN);
	bBG3Pri = (uBGModeReg & 8) && (tm & SNESPPU_MASK_BG3) && (uBGMode==1);

	// clear main screen
	SNMaskClear(&pMain[SNPPU_BGPLANE_PLANE7]);
	SNMaskClear(&pMain[SNPPU_BGPLANE_LAYER0]);
	SNMaskClear(&pMain[SNPPU_BGPLANE_LAYER1]);
	bRendered=FALSE;

	// render bg layers to main screen
	if (tm & SNESPPU_MASK_BG4)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_MAIN_BG4 = ProfCtrGetCycle();
#endif
			_RenderBG8(pMain8, pMain, pRenderInfo->BGPlanes[3], (tmw&SNESPPU_MASK_BG4) ? &pBGWindow[3] : NULL, BGInfo[3].uBitDepth,  pMainAddSubWriteMask, cgadsub & SNESPPU_MASK_BG4, NULL, NULL, BGInfo[3].Priority, bRendered, BGInfo[3].uScrollX );
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_MAIN_BG4,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_MAIN_BG4, 256u);
#endif
		}
	if (tm & SNESPPU_MASK_BG3)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_MAIN_BG3 = ProfCtrGetCycle();
#endif
			_RenderBG8(pMain8, pMain, pRenderInfo->BGPlanes[2], (tmw&SNESPPU_MASK_BG3) ? &pBGWindow[2] : NULL, BGInfo[2].uBitDepth,  pMainAddSubWriteMask, cgadsub & SNESPPU_MASK_BG3, &BG3Pri, NULL, BGInfo[2].Priority, bRendered, BGInfo[2].uScrollX);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_MAIN_BG3,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_MAIN_BG3, 256u);
#endif
		}
	if (tm & SNESPPU_MASK_BG2)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_MAIN_BG2 = ProfCtrGetCycle();
#endif
			_RenderBG8(pMain8, pMain, pRenderInfo->BGPlanes[1], (tmw&SNESPPU_MASK_BG2) ? &pBGWindow[1] : NULL, BGInfo[1].uBitDepth,  pMainAddSubWriteMask, cgadsub & SNESPPU_MASK_BG2, bMode7ExtBG ? &M7BG2MainPri : NULL, bBG3Pri ? &BG3Pri : NULL, BGInfo[1].Priority, bRendered, BGInfo[1].uScrollX);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_MAIN_BG2,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_MAIN_BG2, 256u);
#endif
		}
	if (tm & SNESPPU_MASK_BG1)
	{
		SNMaskT DirectBG1Mask;
#if AURORA_SMK_PROFILER
		const Bool bSmkMode7Compose = g_AuroraSmkProfilerActive && (uBGMode == 7);
		const Uint32 uSmkMode7ComposeStart = bSmkMode7Compose ? ProfCtrGetCycle() : 0u;
#endif
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_MAIN_BG1 = ProfCtrGetCycle();
#endif
			_RenderBG8(pMain8, pMain, pRenderInfo->BGPlanes[0], (tmw&SNESPPU_MASK_BG1) ? &pBGWindow[0] : NULL, BGInfo[0].uBitDepth,  pMainAddSubWriteMask, cgadsub & SNESPPU_MASK_BG1, NULL, (bMode7ExtBG && (tm & SNESPPU_MASK_BG2)) ? &M7BG2MainPri : (bBG3Pri ? &BG3Pri : NULL), BGInfo[0].Priority, bRendered, BGInfo[0].uScrollX, bBG1Direct ? &DirectBG1Mask : NULL);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_MAIN_BG1,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_MAIN_BG1, 256u);
#endif
		}
#if AURORA_SMK_PROFILER
		if (bSmkMode7Compose)
			AuroraSmkProfilerMode7Compose(0, (Uint32)(ProfCtrGetCycle() - uSmkMode7ComposeStart));
#endif
#if AURORA_FRONTEND_PROFILER
		const Uint32 uAuroraDirectStart = ProfCtrGetCycle();
#endif
		if (bMode34Direct)
			_SnesPPUTrackMode34DirectColor(pDirectAttrib, 0, &DirectBG1Mask,
				pRenderInfo->Tiles[0], BGInfo[0].uScrollX, BGInfo[0].uMosaic);
		else if (bMode7Direct)
			_SnesPPUTrackMode7DirectColor(pDirectAttrib, 0, &DirectBG1Mask);
#if AURORA_FRONTEND_PROFILER
		if (bMode34Direct || bMode7Direct)
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_MAIN_DIRECT,
				ProfCtrGetCycle() - uAuroraDirectStart, 256u);
#endif
	}
	if (!bRendered)
		_ClearLinePlanar((SNMaskT *)pMain8, 8);
#if SNDBG_LOG
	g_TmgCycBGMain += ProfCtrGetCycle() - _tBGMain;
#endif
	if (tm & SNESPPU_MASK_OBJ)
	{
#if SNDBG_LOG
		Uint32 _tObjB = ProfCtrGetCycle();
#endif
		/* PPU detail: main OBJ draw */
		AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_OBJ_MAIN);
		_SnesPPURenderOBJ8(pMain8, pMain, ObjLine, nObjRenderLine,  (tmw&SNESPPU_MASK_OBJ) ? &pBGWindow[4] : NULL, bBG3Pri ? &BG3Pri : NULL,
			pMainAddSubWriteMask, (cgadsub & 0x10) ? 1 : 0, pDirectAttrib, 0);
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_OBJ_MAIN);
#if SNDBG_LOG
		{
			Uint32 _dObjDraw = ProfCtrGetCycle() - _tObjB;
			g_TmgCycObj += _dObjDraw;
			g_TmgCycObjDraw += _dObjDraw;
		}
#endif
	}
	AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_MAIN);

#if CODE_PLATFORM == CODE_PS2
	/* AURORA_DIRECT_MAIN_SUB_ELIDE_V3 */
	if (cgadsub==0 && (cgwsel&0xC0)==0 && m_pPPU->GetIntensity()==15)
	{
		PROF_LEAVE("RenderBG");
		/* PPU detail: close BG before direct-main return */
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_OTHER);
		return;
	}
#endif

#if SNDBG_LOG
	Uint32 _tBGSub = ProfCtrGetCycle();
#endif
	AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_BG_SUB);
	if (cgwsel & 0x02)
	{
		// coloradd/sub subscreen
		SNMaskClear(pSubAddSubMask);		// the bg of the subscreen is not 1/2'd
	} else
	{
		// coloradd/sub fixed color only
		SNMaskSet(pSubAddSubMask);           //all pixels are 1/2!
	}

	// do fancy BG3 priority stuff?
	bBG3Pri = (uBGModeReg & 8) && (ts & SNESPPU_MASK_BG3) && (uBGMode==1);;

	// clear Sub screen
	SNMaskClear(&pSub[SNPPU_BGPLANE_PLANE7]);
	SNMaskClear(&pSub[SNPPU_BGPLANE_LAYER0]);
	SNMaskClear(&pSub[SNPPU_BGPLANE_LAYER1]);
	bRendered = FALSE;

	/* AURORA_V7_MODE56_HIRES_20260915
	 * Exact even-dot background planes for the hires subscreen. */
	SNMaskT *pSubBG1Plane =
		(uBGMode == 5 || uBGMode == 6)
		? pRenderInfo->BGPlanes[2] : pRenderInfo->BGPlanes[0];
	SNMaskT *pSubBG2Plane =
		(uBGMode == 5)
		? pRenderInfo->BGPlanes[3] : pRenderInfo->BGPlanes[1];

	// render bg layers to sub screen
	if (ts & SNESPPU_MASK_BG4)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_SUB_BG4 = ProfCtrGetCycle();
#endif
			_RenderBG8(pSub8, pSub, pRenderInfo->BGPlanes[3], (tsw&SNESPPU_MASK_BG4) ? &pBGWindow[3] : NULL, BGInfo[3].uBitDepth, pSubAddSubMask, 1, NULL, NULL, BGInfo[3].Priority, bRendered, BGInfo[3].uScrollX);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_SUB_BG4,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_SUB_BG4, 256u);
#endif
		}
	if (ts & SNESPPU_MASK_BG3)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_SUB_BG3 = ProfCtrGetCycle();
#endif
			_RenderBG8(pSub8, pSub, pRenderInfo->BGPlanes[2], (tsw&SNESPPU_MASK_BG3) ? &pBGWindow[2] : NULL, BGInfo[2].uBitDepth, pSubAddSubMask, 1, &BG3Pri, NULL, BGInfo[2].Priority, bRendered, BGInfo[2].uScrollX);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_SUB_BG3,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_SUB_BG3, 256u);
#endif
		}
	if (ts & SNESPPU_MASK_BG2)
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_SUB_BG2 = ProfCtrGetCycle();
#endif
			_RenderBG8(pSub8, pSub, pSubBG2Plane, (tsw&SNESPPU_MASK_BG2) ? &pBGWindow[1] : NULL, BGInfo[1].uBitDepth, pSubAddSubMask, 1, bMode7ExtBG ? &M7BG2SubPri : NULL, bBG3Pri ? &BG3Pri : NULL, BGInfo[1].Priority, bRendered, BGInfo[1].uScrollX);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_SUB_BG2,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_SUB_BG2, 256u);
#endif
		}
	if (ts & SNESPPU_MASK_BG1)
	{
		SNMaskT DirectBG1Mask;
#if AURORA_SMK_PROFILER
		const Bool bSmkMode7Compose = g_AuroraSmkProfilerActive && (uBGMode == 7);
		const Uint32 uSmkMode7ComposeStart = bSmkMode7Compose ? ProfCtrGetCycle() : 0u;
#endif
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraDeepStart_BG_SUB_BG1 = ProfCtrGetCycle();
#endif
			_RenderBG8(pSub8, pSub, pSubBG1Plane, (tsw&SNESPPU_MASK_BG1) ? &pBGWindow[0] : NULL, BGInfo[0].uBitDepth, pSubAddSubMask, 1, NULL, (bMode7ExtBG && (ts & SNESPPU_MASK_BG2)) ? &M7BG2SubPri : (bBG3Pri ? &BG3Pri : NULL), BGInfo[0].Priority, bRendered, BGInfo[0].uScrollX, bBG1Direct ? &DirectBG1Mask : NULL);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_SUB_BG1,
				ProfCtrGetCycle() - uAuroraDeepStart_BG_SUB_BG1, 256u);
#endif
		}
#if AURORA_SMK_PROFILER
		if (bSmkMode7Compose)
			AuroraSmkProfilerMode7Compose(1, (Uint32)(ProfCtrGetCycle() - uSmkMode7ComposeStart));
#endif
#if AURORA_FRONTEND_PROFILER
		const Uint32 uAuroraDirectStart = ProfCtrGetCycle();
#endif
		if (bMode34Direct)
			_SnesPPUTrackMode34DirectColor(pDirectAttrib, 4, &DirectBG1Mask,
				pRenderInfo->Tiles[0], BGInfo[0].uScrollX, BGInfo[0].uMosaic);
		else if (bMode7Direct)
			_SnesPPUTrackMode7DirectColor(pDirectAttrib, 4, &DirectBG1Mask);
#if AURORA_FRONTEND_PROFILER
		if (bMode34Direct || bMode7Direct)
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_BG_SUB_DIRECT,
				ProfCtrGetCycle() - uAuroraDirectStart, 256u);
#endif
	}
	if (!bRendered)
		_ClearLinePlanar((SNMaskT *)pSub8, 8);
#if SNDBG_LOG
	g_TmgCycBGSub += ProfCtrGetCycle() - _tBGSub;
#endif
	if (ts & SNESPPU_MASK_OBJ)
	{
#if SNDBG_LOG
		Uint32 _tObjC = ProfCtrGetCycle();
#endif
		/* PPU detail: sub OBJ draw */
		AURORA_SNES_PPU_DETAIL_BEGIN(AURORA_SNES_PPU_DETAIL_OBJ_SUB);
		_SnesPPURenderOBJ8(pSub8, pSub, ObjLine, nObjRenderLine,  (tsw&SNESPPU_MASK_OBJ) ? &pBGWindow[4] : NULL, bBG3Pri ? &BG3Pri : NULL,
			pSubAddSubMask, 4|1, pDirectAttrib, 4);
		AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_OBJ_SUB);
#if SNDBG_LOG
		{
			Uint32 _dObjDraw = ProfCtrGetCycle() - _tObjC;
			g_TmgCycObj += _dObjDraw;
			g_TmgCycObjDraw += _dObjDraw;
		}
#endif
	}

	AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_SUB);
	PROF_LEAVE("RenderBG");
	/* PPU detail: normal BG compositor end */
	AURORA_SNES_PPU_DETAIL_END(AURORA_SNES_PPU_DETAIL_BG_OTHER);
}


void RenderLine8Mode7(Int32 iLine,  SnesRender8pInfoT *pRenderInfo)
{

}










/* AURORA_SAFE_CODE_PERF_V1_MODE7
 * Remove address work whose result is provably discarded/out-of-range while
 * preserving the exact existing repeat/clamp/black rules. VRAM is plain host
 * memory here, so skipped discarded reads have no emulated side effects. */
/* AURORA_MODE7_TILEMAP_CACHE_V2
 * Cache the Mode 7 tilemap byte while consecutive transformed pixels stay
 * in the same 8x8 map tile. VRAM is stable during synchronous RenderLine(). */
/* AURORA_TOPGEAR_MODE7_DY0_FETCH_V4_20260917
 * Exact _FetchMode7_Repeat specialization for DY=0. The transformed Y
 * coordinate is constant, so map row and character row are line invariants.
 *
 * AURORA_TOPGEAR_MODE7_UNITX_V5_20260917:
 * DX=0 and DX=+/-256 have exact integer-X recurrences and avoid the per-pixel
 * fixed-point shift/add. DX=DY=0 collapses to one exact VRAM fetch + memset. */
static _INLINE void _FetchMode7_RepeatDY0(
	Uint8 *pLine, Int32 nPixels, Uint8 *pVram,
	Int32 x, Int32 y, Int32 dx)
{
	const Int32 y2 = (y >> 8) & 0x3FF;
	const Uint32 uTileRow = ((Uint32)y2 >> 3) << 7;
	const Uint32 uChrRow = ((Uint32)y2 & 7u) << 3;
	Uint32 uLastTileAddr = 0xFFFFFFFFu;
	Uint32 uChrBase = 0;

	if (nPixels <= 0)
		return;

	if (dx == 0)
	{
		const Uint32 x2 = (Uint32)(x >> 8) & 0x3FFu;
		const Uint32 uTileAddr = uTileRow | (x2 >> 3);
		const Uint32 uBase = (Uint32)pVram[uTileAddr * 2] << 6;
		const Uint32 uChrAddr = uBase + uChrRow + (x2 & 7u);
		const Uint8 uPixel = pVram[uChrAddr * 2 + 1];
		memset(pLine, uPixel, (size_t)nPixels);
		return;
	}

	if (dx == 256 || dx == -256)
	{
		Int32 x2 = x >> 8;
		const Int32 iStep = (dx > 0) ? 1 : -1;

		while (nPixels > 0)
		{
			const Uint32 uX = (Uint32)x2 & 0x3FFu;
			const Uint32 uTileAddr = uTileRow | (uX >> 3);
			Uint32 uChrAddr;

			x2 += iStep;
			if (uTileAddr != uLastTileAddr)
			{
				uChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
				uLastTileAddr = uTileAddr;
			}

			uChrAddr = uChrBase + uChrRow + (uX & 7u);
			*pLine++ = pVram[uChrAddr * 2 + 1];
			nPixels--;
		}
		return;
	}

	while (nPixels > 0)
	{
		Int32 x2 = x >> 8;
		Uint32 uTileAddr;
		Uint32 uChrAddr;

		x += dx;
		x2 &= 0x3FF;

		uTileAddr = uTileRow | ((Uint32)x2 >> 3);
		if (uTileAddr != uLastTileAddr)
		{
			uChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
			uLastTileAddr = uTileAddr;
		}

		uChrAddr = uChrBase + uChrRow + ((Uint32)x2 & 7u);
		*pLine++ = pVram[uChrAddr * 2 + 1];
		nPixels--;
	}
}


/* AURORA_TOPGEAR_MODE7_UNITX_V5_20260917
 * Repeat-mode specialization for DX=-256/0/+256 while DY is nonzero.
 * Integer X is an exact +/-1/0 recurrence; Y retains the original arithmetic. */
static _INLINE void _FetchMode7_RepeatUnitX(
	Uint8 *pLine, Int32 nPixels, Uint8 *pVram,
	Int32 x, Int32 y, Int32 dx, Int32 dy)
{
	Int32 x2 = x >> 8;
	const Int32 iStep = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
	Uint32 uLastTileAddr = 0xFFFFFFFFu;
	Uint32 uChrBase = 0;

	while (nPixels > 0)
	{
		Int32 y2 = y >> 8;
		const Uint32 uX = (Uint32)x2 & 0x3FFu;
		Uint32 uTileAddr;
		Uint32 uChrAddr;

		y += dy;
		x2 += iStep;
		y2 &= 0x3FF;

		uTileAddr = (((Uint32)y2 >> 3) << 7) | (uX >> 3);
		if (uTileAddr != uLastTileAddr)
		{
			uChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
			uLastTileAddr = uTileAddr;
		}

		uChrAddr = uChrBase + (uX & 7u) + (((Uint32)y2 & 7u) << 3);
		*pLine++ = pVram[uChrAddr * 2 + 1];
		nPixels--;
	}
}


static void _FetchMode7_Repeat(Uint8 *pLine, Int32 nPixels, Uint8 *pVram, Int32 x, Int32 y, Int32 dx, Int32 dy)
{
	Int32 x2, y2;
	Uint32 uTileAddr;
	Uint32 uLastTileAddr = 0xFFFFFFFFu;
	Uint32 uChrBase = 0;
	Uint32 uChrAddr;
	Uint8 uChrData;

	if (dy == 0)
	{
		_FetchMode7_RepeatDY0(pLine, nPixels, pVram, x, y, dx);
		return;
	}
	if (dx == 0 || dx == 256 || dx == -256)
	{
		_FetchMode7_RepeatUnitX(
			pLine, nPixels, pVram, x, y, dx, dy);
		return;
	}

	while (nPixels > 0)
	{
		x2 = x >> 8;
		y2 = y >> 8;

		x += dx;
		y += dy;

		x2 &= 0x3FF;
		y2 &= 0x3FF;

		uTileAddr = ((y2 >> 3) << 7) | (x2 >> 3);
		if (uTileAddr != uLastTileAddr)
		{
			uChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
			uLastTileAddr = uTileAddr;
		}

		uChrAddr = uChrBase + (x2 & 7) + ((y2 & 7) << 3);
		uChrData = pVram[uChrAddr * 2 + 1];

		*pLine++ = uChrData;
		nPixels--;
	}
}

/* AURORA_MODE7_CLAMP_DY0_V2_20261003
 * Exact repeat-mode-3 specialization for DY=0.
 *
 * Y is invariant for the complete scanline, so hoist the signed source Y,
 * character-row offset and tile-map row out of the per-pixel loop.  The
 * out-of-map rule remains identical to _FetchMode7_Clamp(): outside the
 * 1024x1024 tile map, character 0 is sampled with the original X/Y low bits.
 *
 * DX=0 is also exact: every source coordinate is unchanged for the complete
 * line, so one original-equivalent fetch can fill the destination line.
 */
static _INLINE void _FetchMode7_ClampDY0(
	Uint8 *pLine, Int32 nPixels, Uint8 *pVram,
	Int32 x, Int32 y, Int32 dx)
{
	const Int32 y2 = y >> 8;
	const Uint32 uChrRow = ((Uint32)y2 & 7u) << 3;
	Uint32 uLastTileX = 0xFFFFFFFFu;
	Uint32 uCachedChrBase = 0;

	if (nPixels <= 0)
		return;

	/* With both deltas zero, the old loop reads the exact same byte for
	 * every output pixel.  VRAM is ordinary host memory during RenderLine. */
	if (dx == 0)
	{
		const Int32 x2 = x >> 8;
		Uint32 uChrBase = 0;

		if (!((x2 | y2) >> 10))
		{
			const Uint32 uTileAddr =
				(((Uint32)y2 >> 3) << 7) | ((Uint32)x2 >> 3);
			uChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
		}

		const Uint32 uChrAddr =
			uChrBase + ((Uint32)x2 & 7u) + uChrRow;
		memset(pLine, pVram[uChrAddr * 2 + 1], (size_t)nPixels);
		return;
	}

	/* If Y alone is outside the 1024-line map, Clamp's map test is true
	 * for every pixel regardless of X.  Character 0 is therefore invariant. */
	if (y2 >> 10)
	{
		while (nPixels > 0)
		{
			const Int32 x2 = x >> 8;
			const Uint32 uChrAddr =
				((Uint32)x2 & 7u) + uChrRow;

			x += dx;
			*pLine++ = pVram[uChrAddr * 2 + 1];
			nPixels--;
		}
		return;
	}

	/* Y is in range here.  Only X can make a pixel out of map, and the
	 * tile-map row is constant.  Cache the tile column instead of rebuilding
	 * the complete tile address on every in-range pixel. */
	const Uint32 uTileRow = ((Uint32)y2 >> 3) << 7;

	while (nPixels > 0)
	{
		const Int32 x2 = x >> 8;
		Uint32 uChrBase = 0;

		x += dx;

		/* Since y2 is known to be 0..1023, this is exactly equivalent to
		 * the original ((x2 | y2) >> 10) test. */
		if (!(x2 >> 10))
		{
			const Uint32 uTileX = (Uint32)x2 >> 3;

			if (uTileX != uLastTileX)
			{
				uCachedChrBase =
					(Uint32)pVram[(uTileRow | uTileX) * 2] << 6;
				uLastTileX = uTileX;
			}
			uChrBase = uCachedChrBase;
		}

		const Uint32 uChrAddr =
			uChrBase + ((Uint32)x2 & 7u) + uChrRow;
		*pLine++ = pVram[uChrAddr * 2 + 1];
		nPixels--;
	}
}



/* AURORA_MODE7_CLAMP_ALLINSIDE_V3_23_20261006
 * Exact generic-CLAMP specialization for a complete affine span that never
 * leaves the 1024x1024 Mode7 tile map.  The existing DY==0 specialization is
 * dispatched first and is not changed by this path.
 *
 * x/y are fixed-point source coordinates here: x>>8/y>>8 must stay in
 * [0,1023].  For a linear sequence, both endpoints inside the closed
 * fixed-point interval [0, 0x3FFFF] prove every intermediate sample inside.
 * 64-bit temporaries are used only once per call for proof; pixel work remains
 * the original 32-bit C++ operations.
 */
static _INLINE Bool _SnesPPUMode7ClampAllInside(
	Int32 nPixels, Int32 x, Int32 y, Int32 dx, Int32 dy)
{
	if (nPixels <= 0)
		return FALSE;

	const long long nLast = (long long)nPixels - 1;
	const long long x0 = (long long)x;
	const long long y0 = (long long)y;
	const long long xN = x0 + (long long)dx * nLast;
	const long long yN = y0 + (long long)dy * nLast;
	const long long kMax = 0x3FFFFLL;

	return x0 >= 0 && x0 <= kMax &&
	       xN >= 0 && xN <= kMax &&
	       y0 >= 0 && y0 <= kMax &&
	       yN >= 0 && yN <= kMax;
}

static _INLINE void _FetchMode7_ClampAllInside(
	Uint8 *pLine, Int32 nPixels, Uint8 *pVram,
	Int32 x, Int32 y, Int32 dx, Int32 dy)
{
	Int32 x2, y2;
	Uint32 uTileAddr;
	Uint32 uLastTileAddr = 0xFFFFFFFFu;
	Uint32 uCachedChrBase = 0;
	Uint32 uChrAddr;
	Uint8 uChrData;

#if AURORA_FRONTEND_PROFILER
	const Uint32 uAuroraInsidePixels = (Uint32)nPixels;
	Uint32 uAuroraTileReuse = 0;
	Uint32 uAuroraTileLoads = 0;
#endif

	while (nPixels > 0)
	{
		x2 = x >> 8;
		y2 = y >> 8;

		x += dx;
		y += dy;

		uTileAddr = ((y2 >> 3) << 7) | (x2 >> 3);
		if (uTileAddr != uLastTileAddr)
		{
#if AURORA_FRONTEND_PROFILER
			++uAuroraTileLoads;
#endif
			uCachedChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
			uLastTileAddr = uTileAddr;
		}
#if AURORA_FRONTEND_PROFILER
		else
		{
			++uAuroraTileReuse;
		}
#endif

		uChrAddr = uCachedChrBase + (x2 & 7) + ((y2 & 7) << 3);
		uChrData = pVram[uChrAddr * 2 + 1];

		*pLine++ = uChrData;
		nPixels--;
	}

#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_INSIDE_PIXELS,
		0u, uAuroraInsidePixels);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_OUTSIDE_PIXELS,
		0u, 0u);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_TILE_REUSE,
		0u, uAuroraTileReuse);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_TILE_LOADS,
		0u, uAuroraTileLoads);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_BOUNDARY_TRANSITIONS,
		0u, 0u);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_ALL_INSIDE_CALLS,
		0u, 1u);
#endif
}

static void _FetchMode7_Clamp(Uint8 *pLine, Int32 nPixels, Uint8 *pVram, Int32 x, Int32 y, Int32 dx, Int32 dy)
{
#if AURORA_FRONTEND_PROFILER
	/* AURORA_SNES_TARGET_TRAITS_V3_21_1_20261006
	 * Call-shape counters live before the retained DY0 fast path so DY0 calls
	 * are visible even though that specialization returns before the generic loop. */
	if (dx == 0)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_DX0_CALLS, 0u, 1u);
	if (dy == 0)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_DY0_CALLS, 0u, 1u);
	if (dx == 256 || dx == -256)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_UNITX_CALLS, 0u, 1u);
	if (dy == 256 || dy == -256)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_UNITY_CALLS, 0u, 1u);
#endif

	if (dy == 0)
	{
		_FetchMode7_ClampDY0(pLine, nPixels, pVram, x, y, dx);
		return;
	}

	/* AURORA_MODE7_CLAMP_ALLINSIDE_V3_23_20261006
	 * Keep mixed/outside geometry on the canonical generic path. */
	if (_SnesPPUMode7ClampAllInside(nPixels, x, y, dx, dy))
	{
		_FetchMode7_ClampAllInside(pLine, nPixels, pVram, x, y, dx, dy);
		return;
	}

#if AURORA_FRONTEND_PROFILER
	Uint32 uAuroraGenericInside = 0;
	Uint32 uAuroraGenericOutside = 0;
	Uint32 uAuroraGenericTileReuse = 0;
	Uint32 uAuroraGenericTileLoads = 0;
	Uint32 uAuroraGenericBoundaryTransitions = 0;
	Bool bAuroraGenericHaveBoundsState = FALSE;
	Bool bAuroraGenericLastInside = FALSE;
#endif

	Int32 x2, y2;
	Uint32 uTileAddr;
	Uint32 uLastTileAddr = 0xFFFFFFFFu;
	Uint32 uCachedChrBase = 0;
	Uint32 uChrBase;
	Uint32 uChrAddr;
	Uint8 uChrData;

	while (nPixels > 0)
	{
		x2 = x >> 8;
		y2 = y >> 8;

		x += dx;
		y += dy;

#if AURORA_FRONTEND_PROFILER
		const Bool bAuroraGenericInside = (((x2 | y2) >> 10) == 0);
		if (bAuroraGenericInside)
			++uAuroraGenericInside;
		else
			++uAuroraGenericOutside;
		if (bAuroraGenericHaveBoundsState &&
		    bAuroraGenericInside != bAuroraGenericLastInside)
			++uAuroraGenericBoundaryTransitions;
		bAuroraGenericLastInside = bAuroraGenericInside;
		bAuroraGenericHaveBoundsState = TRUE;
#endif

		if ((x2 | y2) >> 10)
		{
			uChrBase = 0;
		}
		else
		{
			uTileAddr = ((y2 >> 3) << 7) | (x2 >> 3);
#if AURORA_FRONTEND_PROFILER
			if (uTileAddr == uLastTileAddr)
				++uAuroraGenericTileReuse;
			else
				++uAuroraGenericTileLoads;
#endif
			if (uTileAddr != uLastTileAddr)
			{
				uCachedChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
				uLastTileAddr = uTileAddr;
			}
			uChrBase = uCachedChrBase;
		}

		uChrAddr = uChrBase + (x2 & 7) + ((y2 & 7) << 3);
		uChrData = pVram[uChrAddr * 2 + 1];

		*pLine++ = uChrData;
		nPixels--;
	}

#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_INSIDE_PIXELS,
		0u, uAuroraGenericInside);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_OUTSIDE_PIXELS,
		0u, uAuroraGenericOutside);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_TILE_REUSE,
		0u, uAuroraGenericTileReuse);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_TILE_LOADS,
		0u, uAuroraGenericTileLoads);
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_BOUNDARY_TRANSITIONS,
		0u, uAuroraGenericBoundaryTransitions);
	if (uAuroraGenericInside && !uAuroraGenericOutside)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_ALL_INSIDE_CALLS,
			0u, 1u);
	else if (uAuroraGenericInside && uAuroraGenericOutside)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_MIXED_CALLS,
			0u, 1u);
	else if (uAuroraGenericOutside)
		AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_CLAMP_ALL_OUTSIDE_CALLS,
			0u, 1u);
#endif
}

static void _FetchMode7_Black(Uint8 *pLine, Int32 nPixels, Uint8 *pVram, Int32 x, Int32 y, Int32 dx, Int32 dy)
{
	Int32 x2, y2;
	Uint32 uTileAddr;
	Uint32 uLastTileAddr = 0xFFFFFFFFu;
	Uint32 uCachedChrBase = 0;
	Uint32 uChrAddr;
	Uint8 uChrData;

	while (nPixels > 0)
	{
		x2 = x >> 8;
		y2 = y >> 8;

		x += dx;
		y += dy;

		if ((x2 | y2) >> 10)
		{
			uChrData = 0;
		}
		else
		{
			uTileAddr = ((y2 >> 3) << 7) | (x2 >> 3);
			if (uTileAddr != uLastTileAddr)
			{
				uCachedChrBase = (Uint32)pVram[uTileAddr * 2] << 6;
				uLastTileAddr = uTileAddr;
			}

			uChrAddr = uCachedChrBase + (x2 & 7) + ((y2 & 7) << 3);
			uChrData = pVram[uChrAddr * 2 + 1];
		}

		*pLine++ = uChrData;
		nPixels--;
	}
}

#if 0
static void _FetchMode7Priority(Uint8 *pPriority, Uint8 *pLine, Int32 nPixels)
{
	Uint8 uPriority =0;

	while (nPixels > 0)
	{
		Uint8 uData;
		uData = pLine[0];

		uPriority >>= 1;
		uPriority  |= uData & 0x80;

		if (!(nPixels & 7))
		{
			*pPriority++ = uPriority;
			uPriority = 0;
		}

		pLine[0] = uData & 0x7F;

		pLine++;
		nPixels--;
	}
}
#else

static void _FetchMode7Priority(Uint8 *pPriority, Uint8 *pLine, Int32 nPixels)
{
	Uint64 uMask64;
	Uint64 *pLine64 = (Uint64 *)pLine;

	uMask64 = 0x8080808080808080;

	while (nPixels > 0)
	{
		Uint64 uData64;
		Uint64 uPriority = 0;
		Uint64 uPri64;

		// fetch 8 pixels
		uData64 = pLine64[0];

		// get priority bits
		uPri64 = uData64 & uMask64;

		uPriority|= (uPri64 >> ( 0x00 + 7)) << 0;
		uPriority|= (uPri64 >> ( 0x08 + 7)) << 1;
		uPriority|= (uPri64 >> ( 0x10 + 7)) << 2;
		uPriority|= (uPri64 >> ( 0x18 + 7)) << 3;
		uPriority|= (uPri64 >> ( 0x20 + 7)) << 4;
		uPriority|= (uPri64 >> ( 0x28 + 7)) << 5;
		uPriority|= (uPri64 >> ( 0x30 + 7)) << 6;
		uPriority|= (uPri64 >> ( 0x38 + 7)) << 7;

		// remove priority bits
		uData64 &= ~uMask64;

		// store priority
		pPriority[0] = (Uint8)uPriority;
		pPriority++;

		// store line data
		pLine64[0] = uData64;
		pLine64++;

		nPixels-=8;
	}
}

#endif

#if CODE_PLATFORM == CODE_PS2
static void _FetchMode7Opaque(Uint8 *pMask, Uint8 *pLine, Int32 nPixels)
{
	Uint64 uMask64;
	Uint64 *pLine64 = (Uint64 *)pLine;
	Uint64 uZero;
	Uint64 uOne;

	uMask64 = 0x8080808080808080;
	uZero	= 0x0000000000000000;
	uOne    = 0xFFFFFFFFFFFFFFFF;

	while (nPixels > 0)
	{
		Uint64 uData64;
		Uint64 uOpaque = 0;

		// fetch 8 pixels
		uData64 = pLine64[0];
		pLine64++;

		__asm__ (
			"pceqb      %0,%0,$0        \n"   // %0 = FF or 00
			: "+r" (uData64)
			);

		if (uData64==uZero)
		{
			pMask[0] = 0xFF;
			pMask++;
		} else
		if (uData64==uOne)
		{
			pMask[0] = 0x00;
			pMask++;
		} else
		{
			// get priority bits
			uData64 = uData64 & uMask64;

			uOpaque|= (uData64 >> ( 0x00 + 7)) << 0;
			uOpaque|= (uData64 >> ( 0x08 + 7)) << 1;
			uOpaque|= (uData64 >> ( 0x10 + 7)) << 2;
			uOpaque|= (uData64 >> ( 0x18 + 7)) << 3;
			uOpaque|= (uData64 >> ( 0x20 + 7)) << 4;
			uOpaque|= (uData64 >> ( 0x28 + 7)) << 5;
			uOpaque|= (uData64 >> ( 0x30 + 7)) << 6;
			uOpaque|= (uData64 >> ( 0x38 + 7)) << 7;

			// store priority
			pMask[0] = (Uint8)(uOpaque^0xFF);
			pMask++;
		}
		nPixels-=8;
	}
}
#else

static void _FetchMode7Opaque(Uint8 *pMask, Uint8 *pLine, Int32 nPixels)
{
	Uint64 *pLine64 = (Uint64 *)pLine;

	while (nPixels > 0)
	{
		Uint64 uData64;
		Uint64 uOpaque = 0;

		// fetch 8 pixels
		uData64 = pLine64[0];
		pLine64++;

		// on a mips processor this produces several movn instructions
		uOpaque|= ((uData64 >> 0x00) & 0xFF) ? 1 : 0;
		uOpaque|= ((uData64 >> 0x08) & 0xFF) ? 2 : 0;
		uOpaque|= ((uData64 >> 0x10) & 0xFF) ? 4 : 0;
		uOpaque|= ((uData64 >> 0x18) & 0xFF) ? 8 : 0;
		uOpaque|= ((uData64 >> 0x20) & 0xFF) ? 16 : 0;
		uOpaque|= ((uData64 >> 0x28) & 0xFF) ? 32 : 0;
		uOpaque|= ((uData64 >> 0x30) & 0xFF) ? 64 : 0;
		uOpaque|= ((uData64 >> 0x38) & 0xFF) ? 128 : 0;


		// store priority
		pMask[0] = (Uint8)uOpaque;
		pMask++;
		nPixels-=8;
	}
}
#endif
static void _FetchMode7(Uint8 *pLine, SnesPPU *pPPU, Int32 iLine, SNMaskT *pPriority, SNMaskT *pOpaque)
{
#if AURORA_SMK_PROFILER
	const Bool bSmkProfile = g_AuroraSmkProfilerActive;
	const Uint32 uSmkMode7Start = bSmkProfile ? ProfCtrGetCycle() : 0u;
#endif
	const SnesPPURegsT *pRegs = pPPU->GetRegs();
	Uint8 *pVram = (Uint8 *)pPPU->GetVramPtr(0);
	Int32 m7a = (Int16)pRegs->m7a.w;
	Int32 m7b = (Int16)pRegs->m7b.w;
	Int32 m7c = (Int16)pRegs->m7c.w;
	Int32 m7d = (Int16)pRegs->m7d.w;
	Int32 hofs, vofs, cx, cy;
	Int32 screenY, screenX;
	Int32 originX, originY;
	Int32 x, y, dx, dy;
#if AURORA_FRONTEND_PROFILER
	const Uint32 uAuroraM7SetupStart = ProfCtrGetCycle();
#endif

	/* AURORA_ACCURACY_MODE7_CORE_V1
	 * The Mode 7 adder clips the translated H/V differences to its internal
	 * signed range and discards six fractional bits after each product. This
	 * matters at wrap boundaries and with non-trivial centre coordinates. */
	#define AURORA_M7_SIGN13(_n) \
		((((Int32)(_n) & 0x1000) != 0) ? ((Int32)(_n) | ~0x1FFF) : ((Int32)(_n) & 0x1FFF))
	#define AURORA_M7_CLIP(_n) \
		((((Int32)(_n) & 0x2000) != 0) ? ((Int32)(_n) | ~1023) : ((Int32)(_n) & 1023))

	/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
	hofs = AURORA_M7_SIGN13(pPPU->GetMode7LineHofs());
	vofs = AURORA_M7_SIGN13(pPPU->GetMode7LineVofs());
	cx   = AURORA_M7_SIGN13(pRegs->m7x.w);
	cy   = AURORA_M7_SIGN13(pRegs->m7y.w);

	/* AURORA_SAFE_CODE_PERF_V1_MODE7_MATRIX: reuse scanline invariants. */
	const Uint8 m7sel = pRegs->m7sel;
	const Int32 clippedH = AURORA_M7_CLIP(hofs - cx);
	const Int32 clippedV = AURORA_M7_CLIP(vofs - cy);

	/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
	const Int32 iMode7SourceLine = pPPU->GetMode7MosaicSourceLine(iLine);
	screenY = (m7sel & 0x02) ? (255 - iMode7SourceLine) : iMode7SourceLine;

	/* AURORA_UPSTREAM_20260827_MODE7_SIGNED_CENTER_V1
	 * Keep Aurora's already-correct per-product truncation/flip/repeat path,
	 * but do not left-shift a negative signed centre coordinate: that is
	 * undefined behavior in C++.  The hardware-equivalent *256 is bounded
	 * by the signed 13-bit centre range and is well inside Int32. */
	/* AURORA_TOPGEAR_MODE7_ZEROCOEFF_V4_20260917
	 * Multiplication by an exact zero matrix coefficient contributes
	 * exact zero. Nonzero cases retain the original expression/order. */
	if (m7b == 0)
		originX = ((m7a * clippedH) & ~63) + cx * 256;
	else
		originX = ((m7a * clippedH) & ~63) +
		          ((m7b * clippedV) & ~63) +
		          ((m7b * screenY) & ~63) + cx * 256;

	if (m7c == 0)
		originY = ((m7d * clippedV) & ~63) +
		          ((m7d * screenY) & ~63) + cy * 256;
	else
		originY = ((m7c * clippedH) & ~63) +
		          ((m7d * clippedV) & ~63) +
		          ((m7d * screenY) & ~63) + cy * 256;

	screenX = (m7sel & 0x01) ? 255 : 0;
	x = originX + m7a * screenX;
	y = (m7c == 0) ? originY : (originY + m7c * screenX);
	dx = (m7sel & 0x01) ? -m7a : m7a;
	dy = (m7c == 0) ? 0 : ((m7sel & 0x01) ? -m7c : m7c);

	/* AURORA_MODE7_FULL_RETIRE_V1_20261014:
	 * The old Half path was retired. Always use the unmodified Full
	 * coordinates and 256-pixel fetch. Keep the constant for profiling. */
	const Int32 nMode7Pixels = 256;

	/* M7SEL repeat values 0 and 1 both wrap. Value 2 is transparent/backdrop
	 * outside the 1024x1024 map, value 3 repeats character 0. */
#if AURORA_SMK_PROFILER
	const Uint32 uSmkMode7FetchStart = bSmkProfile ? ProfCtrGetCycle() : 0u;
#endif
#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_SETUP,
		ProfCtrGetCycle() - uAuroraM7SetupStart, 1u);
#endif
	switch ((m7sel >> 6) & 3)
	{
	case 0:
	case 1:
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraM7FetchStart = ProfCtrGetCycle();
			const Uint32 uAuroraM7FetchBucket =
				(dy == 0) ? AURORA_SNES_DEEP_M7_FETCH_REPEAT_DY0 :
				((dx == 0 || dx == 256 || dx == -256) ?
				 AURORA_SNES_DEEP_M7_FETCH_REPEAT_UNITX :
				 AURORA_SNES_DEEP_M7_FETCH_REPEAT_GENERIC);
#endif
			_FetchMode7_Repeat(pLine, nMode7Pixels, pVram, x, y, dx, dy);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(uAuroraM7FetchBucket,
				ProfCtrGetCycle() - uAuroraM7FetchStart, (Uint32)nMode7Pixels);
#endif
		}
		break;
	case 3:
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraM7FetchStart = ProfCtrGetCycle();
#endif
			_FetchMode7_Clamp(pLine, nMode7Pixels, pVram, x, y, dx, dy);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_FETCH_CLAMP,
				ProfCtrGetCycle() - uAuroraM7FetchStart, (Uint32)nMode7Pixels);
#endif
		}
		break;
	case 2:
	default:
		{
#if AURORA_FRONTEND_PROFILER
			const Uint32 uAuroraM7FetchStart = ProfCtrGetCycle();
#endif
			_FetchMode7_Black(pLine, nMode7Pixels, pVram, x, y, dx, dy);
#if AURORA_FRONTEND_PROFILER
			AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_FETCH_BLACK,
				ProfCtrGetCycle() - uAuroraM7FetchStart, (Uint32)nMode7Pixels);
#endif
		}
		break;
	}
#if AURORA_SMK_PROFILER
	const Uint32 uSmkMode7FetchEnd = bSmkProfile ? ProfCtrGetCycle() : 0u;
#endif

	/* AURORA_MODE7_EXTBG_COMPOSITOR_V1_20260903
	 * BG1 keeps the full 8-bit Mode 7 sample; EXTBG derives BG2 separately. */
	if (pPriority)
		SNMaskSet(pPriority);

#if AURORA_FRONTEND_PROFILER
	const Uint32 uAuroraM7OpaqueStart = ProfCtrGetCycle();
#endif
	_FetchMode7Opaque(pOpaque->uMask8, pLine, 256);
#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_OPAQUE,
		ProfCtrGetCycle() - uAuroraM7OpaqueStart, 256u);
#endif
#if AURORA_SMK_PROFILER
	if (bSmkProfile)
	{
		const Uint32 uSmkMode7End = ProfCtrGetCycle();
		AuroraSmkProfilerMode7Line(
			m7sel, pRegs->mosaic, (Uint8)SNPPURenderGetSoftwareHackFlags(),
			x, y, dx, dy, m7b, m7c, nMode7Pixels,
			(Uint32)(uSmkMode7FetchStart - uSmkMode7Start),
			(Uint32)(uSmkMode7FetchEnd - uSmkMode7FetchStart),
			(Uint32)(uSmkMode7End - uSmkMode7FetchEnd),
			(Uint32)(uSmkMode7End - uSmkMode7Start));
	}
#endif

	#undef AURORA_M7_CLIP
	#undef AURORA_M7_SIGN13
}

static void _BuildMode7ExtBG(Uint8 *pLineBG2, SNMaskT *pPriority,
	SNMaskT *pOpaque, const Uint8 *pLineBG1)
{
	/* AURORA_MODE7_EXTBG_COMPOSITOR_V1_20260903 */
#if AURORA_FRONTEND_PROFILER
	Uint32 uAuroraStart = ProfCtrGetCycle();
#endif
	memcpy(pLineBG2, pLineBG1, 256);
#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_EXTBG_COPY,
		ProfCtrGetCycle() - uAuroraStart, 256u);
	uAuroraStart = ProfCtrGetCycle();
#endif
	_FetchMode7Priority(pPriority->uMask8, pLineBG2, 256);
#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_EXTBG_PRIORITY,
		ProfCtrGetCycle() - uAuroraStart, 256u);
	uAuroraStart = ProfCtrGetCycle();
#endif
	_FetchMode7Opaque(pOpaque->uMask8, pLineBG2, 256);
#if AURORA_FRONTEND_PROFILER
	AURORA_SNES_DEEP_RECORD(AURORA_SNES_DEEP_M7_EXTBG_OPAQUE,
		ProfCtrGetCycle() - uAuroraStart, 256u);
#endif
}
