

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "console.h"
#include "snppu.h"
#include "snppurender.h"
#include "rendersurface.h"
#include "snmask.h"
#include "snmaskop.h"
#include "prof.h"
#include "sndbglog.h"



static Uint32 _SnesPPU_ScrSizeOffset[4][4]=
{
	{0x0000, 0x0000, 0x0000, 0x0000},
	{0x0000, 0x0400, 0x0000, 0x0400},
	{0x0000, 0x0000, 0x0400, 0x0400},
	{0x0000, 0x0400, 0x0800, 0x0C00}
};

static Uint8 _SNPPUBg_Tile16Pos[4][4] = 
{
	{ 0,  1}, // normal
	{ 1,  0}, // flipx
	{16, 17}, // flipy
	{17, 16}, // flipxy
};

// BG Fetch 
/* AURORA_SNES_SAFE_PERF_V4_20260919: invariant-hoisting only; no BG addressing/timing rule changes. */

/*
 * Move one tile row down in the renderer's abstract SNES tilemap address.
 *
 * Layout used by this file:
 *
 *     ss yyyyy xxxxx
 *
 * Bit 10 selects the horizontal 32x32 screen and bit 11 selects the
 * vertical 32x32 screen.  Plain `uAddr + 32` is therefore incorrect
 * when yyyyy == 31: its carry enters the horizontal screen bit.
 *
 * Keeping this as an address-space operation lets _GetScreenPtrs()
 * continue to alias nonexistent screens correctly for 32x32/64x32
 * maps, while 32x64/64x64 maps can really cross into their lower half.
 */
/* AURORA_SAFE_CODE_PERF_V1_BG
 * Keep the renderer's exact abstract tilemap layout, but operate on its
 * bitfields directly instead of unpacking and immediately repacking them. */
static Uint16 _SNPPUOffsetNextRow(Uint16 uAddr)
{
	if ((uAddr & 0x03E0) != 0x03E0)
		return (Uint16)(uAddr + 0x20);

	/* wrap tile row 31 -> 0 and toggle the vertical screen */
	uAddr &= (Uint16)~(0x1F << 5);
	uAddr ^= 0x0800;
	return uAddr;
}


static Uint32 _FetchOffset(Uint16 uAddr, Uint16 *pOffset, Int32 nTiles, SnesPPUScreenT **ppScreen)
{
	Uint16 *pScrData;
	Uint32 uOffsetOR = 0;

	PROF_ENTER("_FetchOffset");

	// get pointer to screen data
	pScrData  =  (Uint16 *)ppScreen[(uAddr >> 10) & 3];

	/* Leftmost visible tile never receives offset-per-tile.
	 *
	 * This helper is called independently for the horizontal stream
	 * (pOffset) and the vertical stream (pOffset + 1).  Clear only
	 * the current stream: clearing pOffset[1] here would clobber the
	 * following horizontal entry during the vertical pass.
	 */
	pOffset[0] = 0;
	pOffset += 2;
	nTiles--;

	while (nTiles > 0)
	{
		Uint16 uScrData;

		// fetch screen data
		uScrData = pScrData[uAddr & 0x03FF];
		uAddr++;

		// have we wrapped horizontally?
		if (!(uAddr&0x1F))
		{
			// wrap address
			uAddr-=(1<< 5);	// move back up
			uAddr^=(1<<10); // switch screens

			// get pointer to screen data
			pScrData  =  (Uint16 *)ppScreen[(uAddr >> 10) & 3];
		}

		uOffsetOR |= uScrData;

		// store offset
		pOffset[0] = uScrData;
		pOffset+=2;
		nTiles--;
	}

	PROF_LEAVE("_FetchOffset");

	return uOffsetOR;
}


static void _FetchBG8x8(Uint32 uAddr, SnesRenderTileT *pTile, Int32 nTiles, SnesPPUScreenT **ppScreen)
{
	//  each screen is 32x32
	// vram address is: ssyyyyyxxxxx
	//   ss = screen # 00,01,10,11
	//   yyyyy is tile vert row
	//   xxxxx is tile horiz column

	Uint16 *pScrData;

    PROF_ENTER("_FetchBG8x8");

	// get pointer to screen data
	pScrData  =  (Uint16 *)ppScreen[(uAddr >> 10) & 3];

	while (nTiles > 0)
	{
		Uint16 uScrData;

		// fetch screen data
		uScrData = pScrData[uAddr & 0x03FF];
		uAddr++;

		// have we wrapped horizontally?
		if (!(uAddr&0x1F))
		{
			// wrap address
			uAddr-=(1<< 5);	// move back up
			uAddr^=(1<<10); // switch screens

			// get pointer to screen data
			pScrData  =  (Uint16 *)ppScreen[(uAddr >> 10) & 3];
		}

		// screen data is of format: YX?cccNNNNNNNNNN
		pTile->uFlip = (uScrData >> 14) & 3;
		pTile->uPal  = (uScrData >> 10) & 0xF;
		pTile->uTile = (uScrData >>  0) & 0x3FF;
		pTile->uOffsetY = 0;
		pTile++;

		nTiles--;
	}

    PROF_LEAVE("_FetchBG8x8");
}


static void _FetchBG16x16(Uint32 uAddr, SnesRenderTileT *pTile, Int32 nTiles, SnesPPUScreenT **ppScreen, Uint32 uFlipXOR)
{
	//  each screen is 32x32
	// vram address is: ssyyyyyxxxxx
	//   ss = screen # 00,01,10,11
	//   yyyyy is tile vert row
	//   xxxxx is tile horiz column

	Uint16 *pScrData;

	PROF_ENTER("_FetchBG16x16");

	// get pointer to screen data
	pScrData  =  (Uint16 *)ppScreen[(uAddr >> 10) & 3];

	while (nTiles > 0)
	{
		Uint16 uScrData;
		Uint32 uTile16;
		Uint32 uFlip;
		Uint32 uPal;
		Uint8 *pOffset;

		// fetch screen data
		uScrData = pScrData[uAddr & 0x03FF];
		uAddr++;

		// have we wrapped horizontally?
		if (!(uAddr&0x1F))
		{
			// wrap address
			uAddr-=(1<< 5);	// move back up
			uAddr^=(1<<10); // switch screens

			// get pointer to screen data
			pScrData  =  (Uint16 *)ppScreen[(uAddr >> 10) & 3];
		}

		// extract tile 
		// screen data is of format: YX?cccNNNNNNNNNN
		uTile16 = ((uScrData >>  0) & 0x3FF);
		uFlip	= (uScrData >> 14) & 3;
		uPal	= (uScrData >> 10) & 0xF;

		pOffset = _SNPPUBg_Tile16Pos[uFlip ^ uFlipXOR];

		// write first 8x8 tile
		pTile->uTile = (Uint16)((uTile16 + pOffset[0]) & 0x03FF); /* AURORA_V3_MODE1_16X16_WRAP_20260915 */
		pTile->uFlip = uFlip;
		pTile->uPal  = uPal;
		pTile->uOffsetY = 0;
		pTile++;
		nTiles--;

		if (!(uFlipXOR&1))
		{
			// write second 8x8 tile
			pTile->uTile = (Uint16)((uTile16 + pOffset[1]) & 0x03FF);
			pTile->uFlip = uFlip;
			pTile->uPal  = uPal;
			pTile->uOffsetY = 0;
			pTile++;
			nTiles--;
		}

		// remove x-flip
		uFlipXOR&=~1;
	}

	PROF_LEAVE("_FetchBG16x16");
}



static void _FetchBG8x8Offset(Uint32 uScrollX, Uint32 uScrollY, Int32 iLine, SnesRenderTileT *pTile, Int32 nTiles, SnesPPUScreenT **ppScreen, Uint16 *pOffset, Uint32 uOffsetMask)
{
	//  each screen is 32x32
	// vram address is: ssyyyyyxxxxx
	//   ss = screen # 00,01,10,11
	//   yyyyy is tile vert row
	//   xxxxx is tile horiz column

	Uint32 uTileX, uTileY;
	Uint32 uX = 0;
	/* AURORA_SNES_SAFE_PERF_V4_20260919: fine X scroll is invariant for this helper call. */
	const Uint32 uScrollXFine = uScrollX & 7;

	PROF_ENTER("_FetchBG8x8Offset");

	while (nTiles > 0)
	{
		Uint16 uScrData;
		Uint32 uTileScrollX, uTileScrollY;
		const Uint16 uOffsetX = pOffset[0];
		const Uint16 uOffsetY = pOffset[1];

		if (uOffsetX & uOffsetMask)
		{
			uTileScrollX =
				uScrollXFine | (uOffsetX & 0x3F8);
		} else
		{
			uTileScrollX = uScrollX;
		}

		if (uOffsetY & uOffsetMask)
		{
			uTileScrollY = uOffsetY & 0x3FF;
		} else
		{
			uTileScrollY = uScrollY;
		}

		uTileScrollX += uX;
		uTileScrollY += iLine;

		// calculate tile x/y
		uTileX = (uTileScrollX >> 3) & 63;
		uTileY = (uTileScrollY >> 3) & 63;

		// Direct decomposition of the same ss-yyyyy-xxxxx address.
		const Uint32 uScreen = (uTileX >> 5) | ((uTileY >> 5) << 1);
		const Uint32 uMapIndex = (uTileX & 0x1F) | ((uTileY & 0x1F) << 5);
		uScrData = ((Uint16 *)ppScreen[uScreen])[uMapIndex];

		// screen data is of format: YX?cccNNNNNNNNNN
		pTile->uFlip = (uScrData >> 14) & 3;
		pTile->uPal  = (uScrData >> 10) & 0xF;
		pTile->uTile = (uScrData >>  0) & 0x3FF;
		pTile->uOffsetY = uTileScrollY & 7;
		pTile++;
		nTiles--;

		uX+=8;
		pOffset+=2;
	}

	PROF_LEAVE("_FetchBG8x8Offset");
}

static void _FetchBG8x8Offset2(Uint32 uScrollX, Uint32 uScrollY, Int32 iLine, SnesRenderTileT *pTile, Int32 nTiles, SnesPPUScreenT **ppScreen, Uint16 *pOffset, Uint32 uOffsetMask)
{
	//  each screen is 32x32
	// vram address is: ssyyyyyxxxxx
	//   ss = screen # 00,01,10,11
	//   yyyyy is tile vert row
	//   xxxxx is tile horiz column

	Uint32 uTileX, uTileY;
	Uint32 uX = 0;
	/* AURORA_SNES_SAFE_PERF_V4_20260919: fine X scroll is invariant for this helper call. */
	const Uint32 uScrollXFine = uScrollX & 7;

	PROF_ENTER("_FetchBG8x8Offset2");

	while (nTiles > 0)
	{
		Uint16 uScrData;
		Uint32 uTileScrollX, uTileScrollY;
		const Uint16 uOffset = pOffset[0];

		uTileScrollX = uScrollX;
		uTileScrollY = uScrollY;

		if (uOffset & uOffsetMask)
		{
			if (uOffset & 0x8000)
			{
				uTileScrollY = uOffset & 0x3FF;
			} else
			{
				uTileScrollX =
				uScrollXFine | (uOffset & 0x3F8);
			}
		} 

		uTileScrollX += uX;
		uTileScrollY += iLine;

		// calculate tile x/y
		uTileX = (uTileScrollX >> 3) & 63;
		uTileY = (uTileScrollY >> 3) & 63;

		// Direct decomposition of the same ss-yyyyy-xxxxx address.
		const Uint32 uScreen = (uTileX >> 5) | ((uTileY >> 5) << 1);
		const Uint32 uMapIndex = (uTileX & 0x1F) | ((uTileY & 0x1F) << 5);
		uScrData = ((Uint16 *)ppScreen[uScreen])[uMapIndex];

		// screen data is of format: YX?cccNNNNNNNNNN
		pTile->uFlip = (uScrData >> 14) & 3;
		pTile->uPal  = (uScrData >> 10) & 0xF;
		pTile->uTile = (uScrData >>  0) & 0x3FF;
		pTile->uOffsetY = uTileScrollY & 7;
		pTile++;
		nTiles--;

		uX+=8;
		pOffset+=2;
	}

	PROF_LEAVE("_FetchBG8x8Offset2");
}



static void _GetScreenPtrs(SnesPPUScreenT **ppScreen, SnesPPU *pPPU, Uint32 uScrAddr, Uint32 uScrSize)
{
	/* AURORA_TOPGEAR_BG_SCREENPTR_ALIAS_V5_20260917
	 * DecodeBGInfo constrains uScrSize to 0..3. Preserve GetVramPtr() on
	 * every UNIQUE address so the 0x7fff VRAM wrap remains per-address. */
	ppScreen[0] = (SnesPPUScreenT *)pPPU->GetVramPtr(uScrAddr);

	switch (uScrSize)
	{
	case 0: /* 32x32: all four abstract screens alias screen 0 */
		ppScreen[1] = ppScreen[0];
		ppScreen[2] = ppScreen[0];
		ppScreen[3] = ppScreen[0];
		break;

	case 1: /* 64x32: 0/2 and 1/3 alias */
		ppScreen[1] =
			(SnesPPUScreenT *)pPPU->GetVramPtr(uScrAddr + 0x0400);
		ppScreen[2] = ppScreen[0];
		ppScreen[3] = ppScreen[1];
		break;

	case 2: /* 32x64: 0/1 and 2/3 alias */
		ppScreen[1] = ppScreen[0];
		ppScreen[2] =
			(SnesPPUScreenT *)pPPU->GetVramPtr(uScrAddr + 0x0400);
		ppScreen[3] = ppScreen[2];
		break;

	case 3: /* 64x64: four distinct screens */
		ppScreen[1] =
			(SnesPPUScreenT *)pPPU->GetVramPtr(uScrAddr + 0x0400);
		ppScreen[2] =
			(SnesPPUScreenT *)pPPU->GetVramPtr(uScrAddr + 0x0800);
		ppScreen[3] =
			(SnesPPUScreenT *)pPPU->GetVramPtr(uScrAddr + 0x0C00);
		break;

	default:
		/* Private helper callers are expected to obey DecodeBGInfo's &3.
		 * Keep an explicit conservative fallback instead of aliasing a bad
		 * value silently if that invariant is ever broken. */
		ppScreen[0] = (SnesPPUScreenT *)pPPU->GetVramPtr(
			uScrAddr + _SnesPPU_ScrSizeOffset[uScrSize & 3][0]);
		ppScreen[1] = (SnesPPUScreenT *)pPPU->GetVramPtr(
			uScrAddr + _SnesPPU_ScrSizeOffset[uScrSize & 3][1]);
		ppScreen[2] = (SnesPPUScreenT *)pPPU->GetVramPtr(
			uScrAddr + _SnesPPU_ScrSizeOffset[uScrSize & 3][2]);
		ppScreen[3] = (SnesPPUScreenT *)pPPU->GetVramPtr(
			uScrAddr + _SnesPPU_ScrSizeOffset[uScrSize & 3][3]);
		break;
	}
}



Uint32 SnesPPURender::FetchBG(SnesBGInfoT *pBGInfo, struct SnesRenderTileT *pTiles, Int32 nTiles, Int32 iLine, Uint32 &uOldVramAddr)
{
	Uint32 uScrollX, uScrollY;
	Uint32 uTileX, uTileY;
	Uint32 uVramAddr = 0;
	Uint32 uFlipXOR;
	SnesPPUScreenT *pScreen[4];
	Uint32 uResult = 0;

	if (!pBGInfo->uBitDepth) 
	{
		// no fetching
		return uResult;
	}

	/* AURORA_SNES_SAFE_PERF_V4_20260919: all are invariant for this synchronous BG fetch. */
	const SnesPPURegsT *pRegs = m_pPPU->GetRegs();
	const Uint8 uBGMode = (Uint8)(pRegs->bgmode & 7);
	const Uint32 uMosaic = pBGInfo->uMosaic;
	const Uint32 uMosaicSize = uMosaic + 1;

	if (uMosaic > 0)
	{
		iLine /= uMosaicSize;
		iLine *= uMosaicSize;
	}

	uScrollX = pBGInfo->uScrollX;
	uScrollY = pBGInfo->uScrollY + iLine;
	const Uint32 uFineX = uScrollX & 7;
	const Uint32 uFineY = uScrollY & 7;

	// determine if fine scrollX has changed
	if (((uOldVramAddr>>16)&7)!=uFineX)
	{
		// force palette fetch
		uResult |= SNPPU_BGFLAGS_FETCHPAL;
	}

	// determine if fine scrollY has changed
	if (((uOldVramAddr>>24)&7)!=uFineY)
	{
		// force chr fetch
		uResult |= SNPPU_BGFLAGS_FETCHCHR;
	}

	/* AURORA_ACCURACY_MODE5_HIRES_DECIMATE_V1_1_20260906
	 *
	 * Mode 5 is 512-wide on the real PPU, but Aurora's PS2 pipeline is
	 * deliberately kept 256-wide here. One tilemap entry still occupies
	 * eight low-resolution coordinates horizontally: its sixteen physical
	 * hires samples come from the adjacent character pair N/N+1.
	 *
	 * When BGMODE selects the large BG size, hires does NOT make the
	 * tilemap cell 16 low-resolution pixels wide. Only the vertical axis
	 * becomes 16 pixels tall. Fetch one map entry per 8 logical pixels and
	 * select character +16 for the lower vertical half (swapped by V-flip).
	 *
	 * The paired-character horizontal expansion itself is handled later by
	 * the Mode 5 CHR decimator in snppurender8.cpp.
	 */
	if (((uBGMode == 5) || (uBGMode == 6)) && pBGInfo->uChrSize)
	{
		Uint32 uVHalf;
		Int32 iTile;

		uTileX = (uScrollX >> 3) & 63;
		uTileY = (uScrollY >> 4) & 63;
		uVHalf = (uScrollY >> 3) & 1;

		// abstract map address: ss yyyyy xxxxx
		uVramAddr = (uTileX & 0x1F) << 0;
		uVramAddr|= (uTileY & 0x1F) << 5;
		uVramAddr|= (uTileX >> 5) << 10;
		uVramAddr|= (uTileY >> 5) << 11;

		// Bit 13 is outside the 12-bit abstract map address and exists only
		// in the cache key so crossing the upper/lower 8-line half refetches.
		uVramAddr|= uVHalf << 13;

		if (uVramAddr != (uOldVramAddr & 0xFFFF))
		{
#if SNDBG_LOG
			g_DbgBGMapReloads++;
#endif
			_GetScreenPtrs(
				pScreen,
				m_pPPU,
				pBGInfo->uScrAddr,
				pBGInfo->uScrSize
			);

			_FetchBG8x8(
				uVramAddr & 0x0FFF,
				pTiles,
				nTiles,
				pScreen
			);

			for (iTile = 0; iTile < nTiles; iTile++)
			{
				// 16px vertical character layout: lower half is +16.
				// V-flip swaps the two halves. Keep the 10-bit tile wrap.
				if (uVHalf ^ ((pTiles[iTile].uFlip >> 1) & 1))
					pTiles[iTile].uTile =
						(Uint16)((pTiles[iTile].uTile + 16) & 0x03FF);
			}

			uResult |= SNPPU_BGFLAGS_FETCHCHR | SNPPU_BGFLAGS_FETCHPAL;
		}

		uVramAddr|= uFineX << 16;
		uVramAddr|= uFineY << 24;
		uOldVramAddr = uVramAddr;
		return uResult;
	}

	// perform BG line caching
	switch(pBGInfo->uChrSize)
	{
	case 0:
		// fetch tiles (8x8)
		// calculate tile x/y
		uTileX = (uScrollX >> 3) & 63;
		uTileY = (uScrollY >> 3) & 63;

		// calculate vram tile address (ssyyyyyxxxxx)
		uVramAddr = (uTileX & 0x1F) << 0 ;
		uVramAddr|= (uTileY & 0x1F) << 5 ;
		uVramAddr|= (uTileX >>   5) << 10;
		uVramAddr|= (uTileY >>   5) << 11;

		if (uVramAddr != (uOldVramAddr&0xFFFF))
		{
#if SNDBG_LOG
			g_DbgBGMapReloads++;
#endif
			// get pointers to screens
			_GetScreenPtrs(pScreen, m_pPPU, pBGInfo->uScrAddr, pBGInfo->uScrSize);

			_FetchBG8x8(uVramAddr, pTiles, nTiles, pScreen);

			// force chr+pal fetch
			uResult |= SNPPU_BGFLAGS_FETCHCHR | SNPPU_BGFLAGS_FETCHPAL;
		}
		break;
	case 1:
		// fetch tiles (16x16)
		// calculate tile x/y
		uTileX = (uScrollX >> 4) & 63;
		uTileY = (uScrollY >> 4) & 63;

		// calculate tile flip
		uFlipXOR = 0;
		if (uScrollX & 8) uFlipXOR|=1;
		if (uScrollY & 8) uFlipXOR|=2;

		// calculate vram tile address (ssyyyyyxxxxx)
		uVramAddr = (uTileX & 0x1F) << 0 ;
		uVramAddr|= (uTileY & 0x1F) << 5 ;
		uVramAddr|= (uTileX >>   5) << 10;
		uVramAddr|= (uTileY >>   5) << 11;
		uVramAddr|= uFlipXOR << 12;

		if (uVramAddr != (uOldVramAddr&0xFFFF))
		{
#if SNDBG_LOG
			g_DbgBGMapReloads++;
#endif
			// get pointers to screens
			_GetScreenPtrs(pScreen, m_pPPU, pBGInfo->uScrAddr, pBGInfo->uScrSize);

			_FetchBG16x16(uVramAddr & 0xFFF, pTiles, nTiles, pScreen, uFlipXOR);

			// force chr+pal fetch
			uResult |= SNPPU_BGFLAGS_FETCHCHR | SNPPU_BGFLAGS_FETCHPAL;
		}
		break;

	default:
		assert(0);
		break;

	}

	uVramAddr|= uFineX<<16;
	uVramAddr|= uFineY<<24;

	// set vram addr
	uOldVramAddr = uVramAddr;

    return uResult;
}




/* SMB2_OPT_16X16_FIX
 *
 * Offset-per-tile operates on 8-pixel screen columns even when the
 * target BG itself uses 16x16 map tiles. Resolve the enclosing 16x16
 * tilemap entry and then select the appropriate 8x8 quadrant.
 */
static void _FetchBG16x16Offset(
    Uint32 uScrollX,
    Uint32 uScrollY,
    Int32 iLine,
    SnesRenderTileT *pTile,
    Int32 nTiles,
    SnesPPUScreenT **ppScreen,
    Uint16 *pOffset,
    Uint32 uOffsetMask,
    Bool bMode4)
{
    Uint32 uX = 0;
    /* AURORA_SNES_SAFE_PERF_V4_20260919: fine X scroll is invariant for this helper call. */
    const Uint32 uScrollXFine = uScrollX & 7;

    PROF_ENTER("_FetchBG16x16Offset");

    while (nTiles > 0)
    {
        Uint32 uTileScrollX = uScrollX;
        Uint32 uTileScrollY = uScrollY;

        Uint32 uTileX;
        Uint32 uTileY;
        Uint32 uSubX;
        Uint32 uSubY;
        Uint32 uAddr;

        Uint16 uScrData;
        Uint16 *pScrData;

        Uint32 uTile16;
        Uint32 uFlip;
        Uint32 uQuadrant;
        Uint8 *pSubTile;
        Uint16 *pOpt;

        /* AURORA_V5_MODE02_OPT_16X16_8PX_20260915
         * OPT is consumed per visible 8x8 fetch column even when the
         * target BG uses 16x16 tilemap entries. FetchOffset() already
         * clears pair zero, so only the first 8-pixel column is exempt;
         * pair one (BG3 entry 0) applies to the second visible column. */
        pOpt = pOffset;

        if (pOpt)
        {
        if (bMode4)
        {
            /*
             * Mode 4 stores either H or V in the same offset word.
             * Bit 15 selects vertical offset.
             */
            if (pOpt[0] & uOffsetMask)
            {
                if (pOpt[0] & 0x8000)
                {
                    uTileScrollY = pOpt[0] & 0x3FF;
                }
                else
                {
                    uTileScrollX =
                        uScrollXFine |
                        (pOpt[0] & 0x3F8);
                }
            }
        }
        else
        {
            /* Mode 2 has independent H and V offset words. */

            if (pOpt[0] & uOffsetMask)
            {
                uTileScrollX =
                    uScrollXFine |
                    (pOpt[0] & 0x3F8);
            }

            if (pOpt[1] & uOffsetMask)
            {
                uTileScrollY =
                    pOpt[1] & 0x3FF;
            }
        }
        }

        uTileScrollX += uX;
        uTileScrollY += iLine;

        /*
         * Tilemap itself is 16x16.
         */
        uTileX = (uTileScrollX >> 4) & 63;
        uTileY = (uTileScrollY >> 4) & 63;

        /*
         * Renderer works in 8-pixel columns, so determine which
         * quadrant of the 16x16 tile is needed.
         */
        uSubX = (uTileScrollX >> 3) & 1;
        uSubY = (uTileScrollY >> 3) & 1;

        /*
         * Abstract screen address:
         *
         *   ss yyyyy xxxxx
         */
        uAddr  = (uTileX & 0x1F);
        uAddr |= (uTileY & 0x1F) << 5;
        uAddr |= (uTileX >> 5) << 10;
        uAddr |= (uTileY >> 5) << 11;

        pScrData =
            (Uint16 *)ppScreen[(uAddr >> 10) & 3];

        uScrData =
            pScrData[uAddr & 0x03FF];

        uTile16 =
            uScrData & 0x03FF;

        uFlip =
            (uScrData >> 14) & 3;

        /*
         * The existing table already describes the four 8x8
         * quadrants for normal/H/V/HV orientations.
         */
        uQuadrant =
            uSubX | (uSubY << 1);

        pSubTile =
            _SNPPUBg_Tile16Pos[uFlip ^ uQuadrant];

        pTile->uTile =
            (Uint16)((uTile16 + pSubTile[0]) & 0x03FF);

        pTile->uFlip =
            (Uint8)uFlip;

        pTile->uPal =
            (Uint8)((uScrData >> 10) & 0x0F);

        pTile->uOffsetY =
            (Uint8)(uTileScrollY & 7);

        pTile++;

        pOffset += 2;
        uX += 8;

        nTiles--;
    }

    PROF_LEAVE("_FetchBG16x16Offset");
}


/* AURORA_V7_MODE56_HIRES_20260915
 * Mode 6 + large BG1 is asymmetric in Aurora's 256-wide carrier:
 * one map entry is 8 logical pixels wide (16 physical hires dots) but
 * 16 pixels tall. Resolve the map at X/8, Y/16, then select vertical
 * +16 subtile for the lower half. Horizontal N/N+1 expansion happens
 * later in the hires CHR decoder. */
static void _FetchBGMode6LargeOffset(
	Uint32 uScrollX,
	Uint32 uScrollY,
	Int32 iLine,
	SnesRenderTileT *pTile,
	Int32 nTiles,
	SnesPPUScreenT **ppScreen,
	Uint16 *pOffset,
	Uint32 uOffsetMask)
{
	Uint32 uX = 0;
	/* AURORA_SNES_SAFE_PERF_V4_20260919: fine X scroll is invariant for this helper call. */
	const Uint32 uScrollXFine = uScrollX & 7;

	while (nTiles > 0)
	{
		Uint32 uTileScrollX = uScrollX;
		Uint32 uTileScrollY = uScrollY;
		Uint32 uTileX, uTileY, uAddr;
		Uint16 uScrData;
		const Uint16 uOffsetX = pOffset[0];
		const Uint16 uOffsetY = pOffset[1];

		if (uOffsetX & uOffsetMask)
			uTileScrollX = uScrollXFine | (uOffsetX & 0x3F8);
		if (uOffsetY & uOffsetMask)
			uTileScrollY = uOffsetY & 0x3FF;

		uTileScrollX += uX;
		uTileScrollY += iLine;

		/* 16 hires dots = 8 logical carrier pixels horizontally. */
		uTileX = (uTileScrollX >> 3) & 63;
		uTileY = (uTileScrollY >> 4) & 63;

		uAddr  = (uTileX & 0x1F);
		uAddr |= (uTileY & 0x1F) << 5;
		uAddr |= (uTileX >> 5) << 10;
		uAddr |= (uTileY >> 5) << 11;
		uScrData = ((Uint16 *)ppScreen[(uAddr >> 10) & 3])[uAddr & 0x03FF];

		pTile->uFlip = (Uint8)((uScrData >> 14) & 3);
		pTile->uPal  = (Uint8)((uScrData >> 10) & 0x0F);
		pTile->uTile = (Uint16)(uScrData & 0x03FF);

		/* Large Mode 6 doubles only the vertical map-cell size. */
		if (((uTileScrollY >> 3) & 1) ^ ((pTile->uFlip >> 1) & 1))
			pTile->uTile = (Uint16)((pTile->uTile + 16) & 0x03FF);

		pTile->uOffsetY = (Uint8)(uTileScrollY & 7);
		pTile++;
		pOffset += 2;
		uX += 8;
		nTiles--;
	}
}


Uint32 SnesPPURender::FetchBGOffset(SnesBGInfoT *pBGInfo, struct SnesRenderTileT *pTiles, Int32 nTiles, Int32 iLine, Uint16 *pOffset, Uint32 uOffsetMask, Bool bVOffset)
{
	SnesPPUScreenT *pScreen[4];

	if (!pBGInfo->uBitDepth) 
	{
		// no fetching
		return 0;
	}

#if SNDBG_LOG
	g_DbgBGMapReloads++;
#endif

	// perform BG line caching
	switch(pBGInfo->uChrSize)
	{
	case 0:
		// get pointers to screens
		_GetScreenPtrs(pScreen, m_pPPU, pBGInfo->uScrAddr, pBGInfo->uScrSize);

		if (bVOffset)
		{
			_FetchBG8x8Offset2(pBGInfo->uScrollX, pBGInfo->uScrollY, iLine, pTiles, nTiles, pScreen, pOffset, uOffsetMask);
		} else
		{
			_FetchBG8x8Offset(pBGInfo->uScrollX, pBGInfo->uScrollY, iLine, pTiles, nTiles, pScreen, pOffset, uOffsetMask);		
		}
		break;
	case 1:
		// target BG uses a large map cell
		_GetScreenPtrs(
			pScreen,
			m_pPPU,
			pBGInfo->uScrAddr,
			pBGInfo->uScrSize
		);

		if ((m_pPPU->GetRegs()->bgmode & 7) == 6)
		{
			_FetchBGMode6LargeOffset(
				pBGInfo->uScrollX,
				pBGInfo->uScrollY,
				iLine,
				pTiles,
				nTiles,
				pScreen,
				pOffset,
				uOffsetMask
			);
		}
		else
		{
			_FetchBG16x16Offset(
				pBGInfo->uScrollX,
				pBGInfo->uScrollY,
				iLine,
				pTiles,
				nTiles,
				pScreen,
				pOffset,
				uOffsetMask,
				bVOffset
			);
		}
		break;

	default:
		assert(0);
		break;

	}

	return SNPPU_BGFLAGS_FETCHCHR | SNPPU_BGFLAGS_FETCHPAL | SNPPU_BGFLAGS_OFFSET;
}



static Uint16 _SNPPUReadOffset16Cell(
    Uint32 uPixelX,
    Uint32 uPixelY,
    SnesPPUScreenT **ppScreen)
{
    Uint32 uTileX;
    Uint32 uTileY;
    Uint32 uAddr;

    uTileX = (uPixelX >> 4) & 63;
    uTileY = (uPixelY >> 4) & 63;

    uAddr  = (uTileX & 0x1F);
    uAddr |= (uTileY & 0x1F) << 5;
    uAddr |= (uTileX >> 5) << 10;
    uAddr |= (uTileY >> 5) << 11;

    return
        ((Uint16 *)ppScreen[(uAddr >> 10) & 3])
        [uAddr & 0x03FF];
}


static Uint32 _FetchOffset16x16Map(
    Uint32 uScrollX,
    Uint32 uScrollY,
    Uint16 *pOffset,
    SnesPPUScreenT **ppScreen,
    Bool bVOffset)
{
    Uint32 uOffsetOR = 0;
    Int32 i;

    /*
     * Leftmost target tile does not receive OPT.
     */
    pOffset[0] = 0;
    pOffset[1] = 0;

    /*
     * 33 renderer columns:
     *
     *   pair 0      = unmodified left edge
     *   pairs 1..32 = offset entries
     */
    for (i = 1; i < 33; i++)
    {
        Uint32 uX;
        Uint16 h;

        /*
         * Offset lookup advances by 8 screen pixels even though
         * BG3's own map entry is 16 pixels wide.
         */
        uX =
            (uScrollX & ~7U) +
            ((Uint32)(i - 1) << 3);

        h = _SNPPUReadOffset16Cell(
            uX,
            uScrollY,
            ppScreen
        );

        pOffset[i * 2] = h;
        uOffsetOR |= h;

        if (bVOffset)
        {
            Uint16 v;

            /*
             * Vertical offset table is selected eight pixels below
             * the horizontal offset row.
             */
            v = _SNPPUReadOffset16Cell(
                uX,
                uScrollY + 8,
                ppScreen
            );

            pOffset[i * 2 + 1] = v;
            uOffsetOR |= v;
        }
        else
        {
            pOffset[i * 2 + 1] = 0;
        }
    }

    return uOffsetOR;
}


/* AURORA_V7_MODE56_HIRES_20260915 */
static Uint16 _SNPPUReadOffsetMode6LargeCell(
	Uint32 uPixelX,
	Uint32 uPixelY,
	SnesPPUScreenT **ppScreen)
{
	Uint32 uTileX = (uPixelX >> 3) & 63;
	Uint32 uTileY = (uPixelY >> 4) & 63;
	Uint32 uAddr;

	uAddr  = (uTileX & 0x1F);
	uAddr |= (uTileY & 0x1F) << 5;
	uAddr |= (uTileX >> 5) << 10;
	uAddr |= (uTileY >> 5) << 11;

	return ((Uint16 *)ppScreen[(uAddr >> 10) & 3])[uAddr & 0x03FF];
}


static Uint32 _FetchOffsetMode6LargeMap(
	Uint32 uScrollX,
	Uint32 uScrollY,
	Uint16 *pOffset,
	SnesPPUScreenT **ppScreen)
{
	Uint32 uOffsetOR = 0;
	Int32 i;

	pOffset[0] = 0;
	pOffset[1] = 0;

	for (i = 1; i < 33; i++)
	{
		Uint32 uX = (uScrollX & ~7U) + ((Uint32)(i - 1) << 3);
		Uint16 h = _SNPPUReadOffsetMode6LargeCell(uX, uScrollY, ppScreen);
		Uint16 v = _SNPPUReadOffsetMode6LargeCell(uX, uScrollY + 8, ppScreen);

		pOffset[i * 2]     = h;
		pOffset[i * 2 + 1] = v;
		uOffsetOR |= h | v;
	}

	return uOffsetOR;
}


Uint32 SnesPPURender::FetchOffset(
    SnesBGInfoT *pBGInfo,
    Uint16 *pOffset,
    Int32 iLine,
    Uint32 &uOldVramAddr,
    Bool bVOffset)
{
    Uint32 uScrollX;
    Uint32 uScrollY;
    Uint32 uTileX;
    Uint32 uTileY;
    Uint32 uVramAddr;
    Uint32 uCacheKey;
    Uint32 uOffsetOR = 0;

    SnesPPUScreenT *pScreen[4];

    uScrollX = pBGInfo->uScrollX;
    uScrollY = pBGInfo->uScrollY;

    /*
     * The current screen scanline does NOT select another BG3
     * offset row. BG3VOFS selects the horizontal row; the vertical
     * row in Mode 2 is the corresponding row eight pixels below.
     */
    (void)iLine;

    /*
     * Cache key includes the 8-pixel phases as well as BG3 map
     * configuration. This is important with 16x16 BG3 tiles:
     * moving by eight pixels can retain the same 16x16 cell while
     * changing which renderer columns reuse that entry.
     */
    uCacheKey =
          ((uScrollX >> 3) & 0x7F)
        | (((uScrollY >> 3) & 0x7F) << 7)
        | (((pBGInfo->uScrAddr >> 10) & 0x1F) << 14)
        | ((pBGInfo->uScrSize & 3) << 19)
        | ((pBGInfo->uChrSize & 1) << 21)
        | ((bVOffset ? 1U : 0U) << 22)
        /* Mode 6 changes the horizontal interpretation of large BG3
         * offset cells, so mode is part of the derived-cache identity. */
        | (((m_pPPU->GetRegs()->bgmode & 7) & 7U) << 23);

    if (uCacheKey != uOldVramAddr)
    {
        _GetScreenPtrs(
            pScreen,
            m_pPPU,
            pBGInfo->uScrAddr,
            pBGInfo->uScrSize
        );

        if (pBGInfo->uChrSize == 0)
        {
            /*
             * Standard 8x8 BG3 offset map.
             */
            uTileX =
                (uScrollX >> 3) & 63;

            uTileY =
                (uScrollY >> 3) & 63;

            uVramAddr  =
                (uTileX & 0x1F);

            uVramAddr |=
                (uTileY & 0x1F) << 5;

            uVramAddr |=
                (uTileX >> 5) << 10;

            uVramAddr |=
                (uTileY >> 5) << 11;

            /*
             * 33 visible/fetch columns.
             *
             * _FetchOffset reserves pair zero for the unaffected
             * left edge and fills the remaining 32 entries.
             */
            uOffsetOR =
                _FetchOffset(
                    (Uint16)uVramAddr,
                    pOffset,
                    33,
                    pScreen
                );

            if (bVOffset)
            {
                uOffsetOR |=
                    _FetchOffset(
                        _SNPPUOffsetNextRow(
                            (Uint16)uVramAddr
                        ),
                        pOffset + 1,
                        33,
                        pScreen
                    );
            }
            else
            {
                Int32 i;

                for (i = 0; i < 33; i++)
                {
                    pOffset[i * 2 + 1] = 0;
                }
            }
        }
        else
        {
            /*
             * Large BG3 is 16x16 normally. In forced-hires Mode 6 it is
             * still 16 physical dots wide, i.e. 8 logical carrier pixels,
             * while remaining 16 pixels tall.
             */
            if ((m_pPPU->GetRegs()->bgmode & 7) == 6)
            {
                uOffsetOR =
                    _FetchOffsetMode6LargeMap(
                        uScrollX,
                        uScrollY,
                        pOffset,
                        pScreen
                    );
            }
            else
            {
                uOffsetOR =
                    _FetchOffset16x16Map(
                        uScrollX,
                        uScrollY,
                        pOffset,
                        pScreen,
                        bVOffset
                    );
            }
        }

        uOldVramAddr = uCacheKey;

        /*
         * BGOffset[68]:
         *
         *   0..65 = 33 H/V pairs
         *   66    = cached OR of all offset words
         *   67    = spare
         *
         * The old slot 64 overlapped the final horizontal offset.
         */
        pOffset[66] =
            (Uint16)uOffsetOR;
    }
    else
    {
        uOffsetOR =
            pOffset[66];
    }

    return uOffsetOR;
}




void _SetBGMask(SNMaskT *pMask, SNMaskT *pWindow, Uint8 uWSel, Uint8 uWLog)
{
	SNMaskT *pWindow1=NULL;
	SNMaskT *pWindow2 = NULL;

	// get pointers to windows
	if (uWSel & 2)
		pWindow1 = (uWSel & 1) ? &pWindow[2] : &pWindow[0];
	if (uWSel & 8)
		pWindow2 = (uWSel & 4) ? &pWindow[3] : &pWindow[1];

	if (pWindow1)
	{
		if (pWindow2)
		{
			// logically combine windows
			switch (uWLog & 3)
			{
			case 0: // OR
				SNMaskOR(pMask, pWindow1, pWindow2);
				break;
			case 1: // AND
				SNMaskAND(pMask, pWindow1, pWindow2);
				break;
			case 2: // XOR
				SNMaskXOR(pMask, pWindow1, pWindow2);
				break;
			case 3: // XNOR
				SNMaskXNOR(pMask, pWindow1, pWindow2);
				break;
			}
		} else
		{
			// use window 1
			SNMaskCopy(pMask, pWindow1);
		}

	} else
	{
		if (pWindow2)
		{
			// use window 1
			SNMaskCopy(pMask, pWindow2);
		} else
		{
			// use no windows
			SNMaskClear(pMask);
		}
	}

//	SNMaskClear(pMask);

}

void SnesPPURender::DecodeBGInfo(SnesBGInfoT *pBGInfo)
{
	const SnesPPURegsT *pRegs = m_pPPU->GetRegs();

	/* Several modes do not assign every palette/priority/depth field below.
	   Leaving this scanline-local structure uninitialized can enable phantom
	   BG layers and add a large amount of useless tile work. */
	memset(pBGInfo, 0, sizeof(*pBGInfo) * 4);

	pBGInfo[0].uScrollX = pRegs->bg1hofs.w & 0x3FF;
	pBGInfo[0].uScrollY = pRegs->bg1vofs.w & 0x3FF;
	pBGInfo[0].uScrAddr = (pRegs->bg1sc >> 2) << 10;
	pBGInfo[0].uScrSize =  pRegs->bg1sc & 3;
	/* AURORA_ACCURACY_BGNBA_4BIT_V1
	 * BGNBA exposes a full four-bit tiledata base for each BG.
	 * The old &7 mask silently discarded base bit 3. */
	pBGInfo[0].uChrAddr =  ((pRegs->bg12nba >> 0) & 0xF) << 12;
	pBGInfo[0].uChrSize =  (pRegs->bgmode >> 4) & 1;
	pBGInfo[0].uMosaic  = (pRegs->mosaic&1) ? (pRegs->mosaic>>4) : 0;

	pBGInfo[1].uScrollX = pRegs->bg2hofs.w & 0x3FF;
	pBGInfo[1].uScrollY = pRegs->bg2vofs.w & 0x3FF;
	pBGInfo[1].uScrAddr = (pRegs->bg2sc >> 2) << 10;
	pBGInfo[1].uScrSize =  pRegs->bg2sc & 3;
	pBGInfo[1].uChrAddr =  ((pRegs->bg12nba >> 4) & 0xF) << 12;
	pBGInfo[1].uChrSize =  (pRegs->bgmode >> 5) & 1;
	pBGInfo[1].uMosaic  = (pRegs->mosaic&2) ? (pRegs->mosaic>>4) : 0;

	pBGInfo[2].uScrollX = pRegs->bg3hofs.w & 0x3FF;
	pBGInfo[2].uScrollY = pRegs->bg3vofs.w & 0x3FF;
	pBGInfo[2].uScrAddr = (pRegs->bg3sc >> 2) << 10;
	pBGInfo[2].uScrSize =  pRegs->bg3sc & 3;
	pBGInfo[2].uChrAddr =  ((pRegs->bg34nba >> 0) & 0xF) << 12;
	pBGInfo[2].uChrSize =  (pRegs->bgmode >> 6) & 1;
	pBGInfo[2].uMosaic  = (pRegs->mosaic&4) ? (pRegs->mosaic>>4) : 0;

	pBGInfo[3].uScrollX = pRegs->bg4hofs.w & 0x3FF;
	pBGInfo[3].uScrollY = pRegs->bg4vofs.w & 0x3FF;
	pBGInfo[3].uScrAddr = (pRegs->bg4sc >> 2) << 10;
	pBGInfo[3].uScrSize =  pRegs->bg4sc & 3;
	pBGInfo[3].uChrAddr =  ((pRegs->bg34nba >> 4) & 0xF) << 12;
	pBGInfo[3].uChrSize =  (pRegs->bgmode >> 7) & 1;
	pBGInfo[3].uMosaic  = (pRegs->mosaic&8) ? (pRegs->mosaic>>4) : 0;

	switch (pRegs->bgmode & 7)
	{
	case 0:
		pBGInfo[0].uBitDepth= 2;
		pBGInfo[1].uBitDepth= 2;
		pBGInfo[2].uBitDepth= 2;
		pBGInfo[3].uBitDepth= 2;
		pBGInfo[0].uPalBase = 0x0;
		pBGInfo[1].uPalBase = 0x1;
		pBGInfo[2].uPalBase = 0x2;
		pBGInfo[3].uPalBase = 0x3;
		pBGInfo[0].Priority  =  3;
		pBGInfo[1].Priority  =  2;
		pBGInfo[2].Priority  =  1;
		pBGInfo[3].Priority  =  0;
		break;

	case 1:
		pBGInfo[0].uBitDepth= 4;
		pBGInfo[1].uBitDepth= 4;
		pBGInfo[2].uBitDepth= 2;
		pBGInfo[3].uBitDepth= 0;
		pBGInfo[0].uPalBase = 0x00;
		pBGInfo[1].uPalBase = 0x00;
		pBGInfo[2].uPalBase = 0x00;
		pBGInfo[3].uPalBase = 0x00;
		pBGInfo[0].Priority  =  3;
		pBGInfo[1].Priority  =  2;
		pBGInfo[2].Priority  =  1;
		pBGInfo[3].Priority  =  0;
		break;

	case 3:
		pBGInfo[0].uBitDepth= 8;
		pBGInfo[1].uBitDepth= 4;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
        pBGInfo[0].Priority  =  5;
		pBGInfo[1].Priority  =  4;
		break;

	// these have offset per tile!
	case 2:
        pBGInfo[0].uBitDepth= 4;
		pBGInfo[1].uBitDepth= 4;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
		pBGInfo[0].Priority  =  5;
		pBGInfo[1].Priority  =  4;
		break;
		// these have offset per tile!
	case 4:
		/* AURORA_V6_MODE4_AUDIT_20260915
		 * Mode 4: BG1=8bpp, BG2=2bpp, BG3 supplies one-row OPT.
		 * BG2 palettes occupy CGRAM 0x00-0x1f. Direct Color, when
		 * CGWSEL.0 is set, applies only to BG1. The OPT reader handles
		 * bit15 H/V selection; V5 keeps 16x16 targets 8px-granular. */
		pBGInfo[0].uBitDepth= 8;
		pBGInfo[1].uBitDepth= 2;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
		pBGInfo[0].uPalBase = 0x00;
		pBGInfo[1].uPalBase = 0x00;
		pBGInfo[0].Priority  =  5;
		pBGInfo[1].Priority  =  4;
		break;

	case 5: // hi-res
		/* AURORA_V7_MODE56_HIRES_20260915
		 * Mode 5: BG1=4bpp, BG2=2bpp. Both are forced hires.
		 * The tilemap size bit selects 16x8 vs 16x16 physical cells. */
		pBGInfo[0].uBitDepth= 4;
		pBGInfo[1].uBitDepth= 2;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
		pBGInfo[0].uPalBase = 0x00;
		pBGInfo[1].uPalBase = 0x00;
		pBGInfo[0].Priority  =  5;
		pBGInfo[1].Priority  =  4;
		break;

	case 6: // hi-res + OPT
		/* AURORA_ACCURACY_MODE6_4BPP_V1
		 * AURORA_V7_MODE56_HIRES_20260915
		 * Mode 6: BG1=4bpp forced hires; invisible BG3 supplies
		 * independent horizontal/vertical OPT exactly like Mode 2. */
		pBGInfo[0].uBitDepth= 4;
		pBGInfo[1].uBitDepth= 0;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
		pBGInfo[0].uPalBase = 0x00;
		pBGInfo[0].Priority  =  5;
		pBGInfo[1].Priority  =  4;
		break;

	case 7:
		pBGInfo[0].uBitDepth= 8;
		pBGInfo[1].uBitDepth= 0;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
		pBGInfo[0].uScrollX = 0;
		pBGInfo[0].uScrollY = 0;
		
		pBGInfo[0].Priority  =  
		pBGInfo[1].Priority  =  
		pBGInfo[2].Priority  =  
		pBGInfo[3].Priority  =  7;
		
		break;

	default:
		pBGInfo[0].uBitDepth= 0;
		pBGInfo[1].uBitDepth= 0;
		pBGInfo[2].uBitDepth= 0;
		pBGInfo[3].uBitDepth= 0;
		break;
	}
}



void SnesPPURender::DecodeWindows(SNMaskT *pWindow, SNMaskT *pBGWindow)
{
    const SnesPPURegsT *pRegs = m_pPPU->GetRegs();

	PROF_ENTER("DecodeWindows");

	// create windows & inverted windows
	// confirmed:
	// window range is INCLUSIVE 
	//    40->41 is two pixels wide
	//    40->40 is one pixel wide
	//    40->39 is 0 pixels wide
	SNMaskRange(&pWindow[0], pRegs->wh0,  pRegs->wh1, false);
	SNMaskRange(&pWindow[1], pRegs->wh2,  pRegs->wh3, false);
	SNMaskNOT(&pWindow[2], &pWindow[0]);
	SNMaskNOT(&pWindow[3], &pWindow[1]);

	// create windows for each BG Layer & color window
	_SetBGMask(&pBGWindow[SNPPU_BGWINDOW_BG1],   pWindow, (pRegs->w12sel >> 0) & 0xF, (pRegs->wbglog >> 0) & 3);
	_SetBGMask(&pBGWindow[SNPPU_BGWINDOW_BG2],   pWindow, (pRegs->w12sel >> 4) & 0xF, (pRegs->wbglog >> 2) & 3);
	_SetBGMask(&pBGWindow[SNPPU_BGWINDOW_BG3],   pWindow, (pRegs->w34sel >> 0) & 0xF, (pRegs->wbglog >> 4) & 3);
	_SetBGMask(&pBGWindow[SNPPU_BGWINDOW_BG4],   pWindow, (pRegs->w34sel >> 4) & 0xF, (pRegs->wbglog >> 6) & 3);
	_SetBGMask(&pBGWindow[SNPPU_BGWINDOW_OBJ],   pWindow, (pRegs->wobjsel>> 0) & 0xF, (pRegs->wobjlog>> 0) & 3);
	_SetBGMask(&pBGWindow[SNPPU_BGWINDOW_COLOR], pWindow, (pRegs->wobjsel>> 4) & 0xF, (pRegs->wobjlog>> 2) & 3);

	PROF_LEAVE("DecodeWindows");
}
