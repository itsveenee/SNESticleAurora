

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "console.h"
#include "snppu.h"
#include "snppurender.h"
#include "rendersurface.h"
#include "snmask.h"
#include "prof.h"
#include "sndbglog.h"



// OBSEL.5-7 escolhe dois tamanhos. Os modos 6/7 sao retangulares e nao
// podem ser representados por um unico shift, como fazia o renderer antigo.
static const Uint8 _SnesPPU_OAMWidth[8][2]=
{
	{ 8, 16}, { 8, 32}, { 8, 64}, {16, 32},
	{16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static const Uint8 _SnesPPU_OAMHeight[8][2]=
{
	{ 8, 16}, { 8, 32}, { 8, 64}, {16, 32},
	{16, 64}, {32, 64}, {32, 64}, {32, 32}
};

Bool _SnesPPUOBJVisibleX(Uint16 uPosX, Uint8 uWidth)
{
	uPosX &= 0x1FF;

	/* X=256 is the hardware's special counted-but-hidden position. Other
	   negative positions count only while at least one pixel reaches x=0. */
	if (uPosX == 0x100)
		return TRUE;

	if (uPosX & 0x100)
		return ((Int32)uPosX - 512) > -(Int32)uWidth;

	return TRUE;
}


/* AURORA_OBJ_STAT77_V2_OBJ_20260915
 * SETINI.1 halves the number of display scanlines occupied by an OBJ while
 * selecting alternating source rows by field. The documented 16x32 quirk is
 * harsher: it behaves as the top 16x16 only, squeezed into 8 display lines.
 * 32x64 follows the ordinary interlace rule. */
static _INLINE Uint32 _SnesPPUOBJDisplayHeight(
	const SnesRenderObjT *pObj, Bool bObjInterlace)
{
	if (!bObjInterlace)
		return pObj->uHeight;
	if (pObj->uWidth == 16 && pObj->uHeight == 32)
		return 8;
	return (Uint32)pObj->uHeight >> 1;
}

static _INLINE Uint16 _SnesPPUOBJCountPhysicalSlivers(
	const SnesRenderObjT *pObj)
{
	Int32 iObjectX;
	Int32 iFirstTile;
	Int32 nTiles;
	/* AURORA_SNES_SAFE_PERF_V4_20260919: SNES OBJ X is nine bits; reuse the same masked value. */
	const Uint16 uPosX = (Uint16)(pObj->uPosX & 0x1FF);

	iObjectX = (uPosX & 0x100)
		? ((Int32)uPosX - 512)
		: (Int32)uPosX;
	_SnesPPUOBJCountedTileRange(
		uPosX, iObjectX, pObj->uWidth, &iFirstTile, &nTiles);
	(void)iFirstTile;
	return (Uint16)nTiles;
}


#if SNDBG_DEEP
static Uint32 _ObjCountBits8(Uint32 v)
{
	v &= 0xFF;
	v = v - ((v >> 1) & 0x55);
	v = (v & 0x33) + ((v >> 2) & 0x33);
	return (v + (v >> 4)) & 0x0F;
}
#endif


void _SnesPPURenderOBJ8(Uint8 *pLine8, SNMaskT *pLine,
	const SnesRenderObj8T *pObjLine, Int32 nObjLine,
	const SNMaskT *pWindow, const SNMaskT *pMask,
	SNMaskT *pAddSubMask, Bool bAddSubMask,
	Uint8 *pDirectAttrib, Uint8 uDirectShift)
{
	SNMaskT ObjMask;
	SNMaskT PriorityMask[4];
	Int32 iWord;

	if (nObjLine <= 0)
		return;

	/* AURORA_SNES_SAFE_PERF_V3_20260919
	 * uDirectShift is invariant for this compositor call. Keep the exact
	 * original nibble mask, but do not rebuild it per tile/pixel. */
	const Uint8 uDirectKeep = uDirectShift ? 0x0F : 0xF0;

	PROF_ENTER("_RenderOBJPlanar");

	/* Os buffers de destino possuem exatamente oito palavras. O renderer
	   antigo criava guardas somente para ObjMask, mas ainda formava ponteiros
	   antes de pLine8/pLine e escrevia pAddSubMask[-1] ou [8] quando um tile
	   cruzava x=0/255. Final Fight 2 usa OBJ em x negativo durante o gameplay.

	   Tiles inteiramente visiveis continuam no caminho de mascara por palavra
	   (o caso quente). Somente os dois recortes de borda usam o caminho por
	   pixel, evitando tanto o acesso fora do buffer quanto uma regressao de
	   desempenho para todos os OBJ da scanline. */
	/* AURORA_REVIVE_A06DD_OBJ_HOTPATH_20260829
	 * Revive a06dd0719853: usa as operações SNMask/MMI já inline no EE e
	 * calcula as combinações de prioridade uma única vez por scanline. */
	if (pWindow)
		SNMaskCopy(&ObjMask, pWindow);
	else
		SNMaskClear(&ObjMask);
	if (pMask)
		SNMaskOR(&ObjMask, &ObjMask, pMask);

	SNMaskOR(&PriorityMask[0], &pLine[SNPPU_BGPLANE_LAYER0],
		&pLine[SNPPU_BGPLANE_LAYER1]);
	SNMaskCopy(&PriorityMask[1], &pLine[SNPPU_BGPLANE_LAYER1]);
	SNMaskAND(&PriorityMask[2], &pLine[SNPPU_BGPLANE_LAYER0],
		&pLine[SNPPU_BGPLANE_LAYER1]);
	SNMaskClear(&PriorityMask[3]);

	while (--nObjLine >= 0)
	{
		const SnesRenderObj8T *pObj = pObjLine + nObjLine;
		/* AURORA_SAFE_CODE_PERF_V1_OBJ
		 * Immutable per-tile metadata kept local in the hot renderer. */
		const Int32 iPosX = pObj->iPosX;
		const Uint32 uPri = pObj->uPri;
		const Uint32 uPal = pObj->uPal;
		const Uint8 *pObjData = pObj->uData;
		Uint32 uOpaque = pObjData[SNPPU_BGPLANE_OPAQUE];

		if (!uOpaque || iPosX <= -8 || iPosX >= 256)
			continue;

		const SNMaskT *pPriorityMask = &PriorityMask[uPri];

#if SNDBG_DEEP
		g_DbgObjCandidatePixels += _ObjCountBits8(uOpaque);
#endif

		if (iPosX >= 0 && iPosX <= 248)
		{
			Uint32 uShift = iPosX & 31;
			Uint32 uMask0 = uOpaque << uShift;
			Uint32 uVisible;
			Uint8 *pDest8 = pLine8 + iPosX;

			iWord = iPosX >> 5;

			/* AURORA_TOPGEAR_OBJ_ONEWORD_V6_20260917
			 * An 8-pixel row crosses a 32-bit mask boundary only for shifts
			 * 25..31. For shifts 0..24 the second word is mathematically
			 * absent; keep the old two-word algorithm intact otherwise. */
			if (uShift <= 24)
			{
				Uint32 uBlocked0 =
					ObjMask.uMask32[iWord] |
					pPriorityMask->uMask32[iWord];

				ObjMask.uMask32[iWord] |= uMask0;
				uMask0 &= ~uBlocked0;

				if (pAddSubMask)
				{
					if ((bAddSubMask & 1) &&
					    ((uPal | bAddSubMask) & 0x4))
						pAddSubMask->uMask32[iWord] |= uMask0;
					else
						pAddSubMask->uMask32[iWord] &= ~uMask0;
				}

				/* AURORA_SNES_SAFE_PERF_V3_20260919: uOpaque is 8-bit; the mask can only clear bits. */
				uVisible = uMask0 >> uShift;
			}
			else
			{
				Uint32 uInvShift = 32 - uShift;
				Uint32 uMask1 = uOpaque >> uInvShift;
				Uint32 uBlocked0 = ObjMask.uMask32[iWord];
				Uint32 uBlocked1 =
					uMask1 ? ObjMask.uMask32[iWord + 1] : 0;

				ObjMask.uMask32[iWord] |= uMask0;
				if (uMask1)
					ObjMask.uMask32[iWord + 1] |= uMask1;

				uBlocked0 |= pPriorityMask->uMask32[iWord];
				if (uMask1)
					uBlocked1 |= pPriorityMask->uMask32[iWord + 1];

				uMask0 &= ~uBlocked0;
				uMask1 &= ~uBlocked1;

				if (pAddSubMask)
				{
					if ((bAddSubMask & 1) &&
					    ((uPal | bAddSubMask) & 0x4))
					{
						pAddSubMask->uMask32[iWord] |= uMask0;
						if (uMask1)
							pAddSubMask->uMask32[iWord + 1] |= uMask1;
					}
					else
					{
						pAddSubMask->uMask32[iWord] &= ~uMask0;
						if (uMask1)
							pAddSubMask->uMask32[iWord + 1] &= ~uMask1;
					}
				}

				uVisible = uMask0 >> uShift;
				uVisible |= uMask1 << uInvShift;
				/* AURORA_SNES_SAFE_PERF_V3_20260919: recomposed value is still a subset of 8-bit uOpaque. */
			}

#if SNDBG_DEEP
			g_DbgObjDrawnPixels += _ObjCountBits8(uVisible);
#endif
			/* AURORA_V7_OBJ_FULLROW_MEMCPY
			 * Exact semantic fast path: after all priority/window masks have
			 * already been resolved, an entirely visible 8-pixel row is just
			 * the same eight byte stores.  memcpy is alignment-safe on EE and
			 * avoids eight branches in sprite-heavy scenes (Top Gear). */
			if (!uVisible)
				continue;

			/* AURORA_V4_MODE34_DIRECT_COLOR_20260915
			 * OBJ is composited after BG1. Clear Direct Color ownership only
			 * for OBJ pixels that survived window and priority masking. */
			if (pDirectAttrib)
			{
				Int32 iDirect;
				for (iDirect = 0; iDirect < 8; iDirect++)
					if (uVisible & (1u << iDirect))
						pDirectAttrib[iPosX + iDirect] &= uDirectKeep;
			}

			if (uVisible == 0xFF)
			{
				memcpy(pDest8, pObjData, 8);
			}
			else
			{
				if (uVisible & 0x01) pDest8[0] = pObjData[0];
				if (uVisible & 0x02) pDest8[1] = pObjData[1];
				if (uVisible & 0x04) pDest8[2] = pObjData[2];
				if (uVisible & 0x08) pDest8[3] = pObjData[3];
				if (uVisible & 0x10) pDest8[4] = pObjData[4];
				if (uVisible & 0x20) pDest8[5] = pObjData[5];
				if (uVisible & 0x40) pDest8[6] = pObjData[6];
				if (uVisible & 0x80) pDest8[7] = pObjData[7];
			}
		} else
		{
			Int32 iPixel;
			/* AURORA_SNES_SAFE_PERF_V3_20260919: invariant for all pixels of this clipped OBJ row. */
			const Bool bClippedAddSub =
				(bAddSubMask & 1) && ((uPal | bAddSubMask) & 0x4);
#if SNDBG_DEEP
			g_DbgObjClippedTiles++;
#endif
			for (iPixel = 0; iPixel < 8; iPixel++)
			{
				Int32 iX;
				Uint32 uBit;
				Uint32 uBlocked;

				if (!(uOpaque & (1u << iPixel)))
					continue;

				iX = iPosX + iPixel;
				if ((Uint32)iX >= 256u)
					continue;

				iWord = iX >> 5;
				uBit = 1u << (iX & 31);
				/* AURORA_SNES_SAFE_PERF_V3_20260919
				 * (A&bit)|(B&bit) == (A|B)&bit. Read both old masks before
				 * publishing this OBJ bit to ObjMask. */
				uBlocked =
					(ObjMask.uMask32[iWord] |
					 pPriorityMask->uMask32[iWord]) & uBit;
				ObjMask.uMask32[iWord] |= uBit;

				if (uBlocked)
					continue;

				if (pAddSubMask)
				{
					if (bClippedAddSub)
						pAddSubMask->uMask32[iWord] |= uBit;
					else
						pAddSubMask->uMask32[iWord] &= ~uBit;
				}

				pLine8[iX] = pObjData[iPixel];
				if (pDirectAttrib)
					pDirectAttrib[iX] &= uDirectKeep;
#if SNDBG_DEEP
				g_DbgObjDrawnPixels++;
#endif
			}
		}
	}

	PROF_LEAVE("_RenderOBJPlanar");
}


void _DecodeOBJEX(Uint8 *pObjEx, SnesRenderObjT *pObjs, Int32 nObjs, Uint32 uBaseSize)
{
	uBaseSize &= 7;
	/* AURORA_SNES_SAFE_PERF_V4_20260919: base-size row is invariant for the whole packed OAM decode. */
	const Uint8 *pWidthRow = _SnesPPU_OAMWidth[uBaseSize];
	const Uint8 *pHeightRow = _SnesPPU_OAMHeight[uBaseSize];
	while (nObjs > 0)
	{
		Uint8	uObjEx;
		Uint8  uLarge;

		// fetch obj byte
		uObjEx = *pObjEx++;

		//uObjEx|=0xAA;

		pObjs->uPosX	   = (uObjEx & 1) << 8;
		uObjEx>>=1;
		uLarge = uObjEx & 1;
		pObjs->uWidth  = pWidthRow[uLarge];
		pObjs->uHeight = pHeightRow[uLarge];
		uObjEx>>=1;
		pObjs++;

		pObjs->uPosX	   = (uObjEx & 1) << 8;
		uObjEx>>=1;
		uLarge = uObjEx & 1;
		pObjs->uWidth  = pWidthRow[uLarge];
		pObjs->uHeight = pHeightRow[uLarge];
		uObjEx>>=1;
		pObjs++;

		pObjs->uPosX	   = (uObjEx & 1) << 8;
		uObjEx>>=1;
		uLarge = uObjEx & 1;
		pObjs->uWidth  = pWidthRow[uLarge];
		pObjs->uHeight = pHeightRow[uLarge];
		uObjEx>>=1;
		pObjs++;

		pObjs->uPosX	   = (uObjEx & 1) << 8;
		uObjEx>>=1;
		uLarge = uObjEx & 1;
		pObjs->uWidth  = pWidthRow[uLarge];
		pObjs->uHeight = pHeightRow[uLarge];
		uObjEx>>=1;
		pObjs++;

		nObjs-=4;
	}

}


void _DecodeOBJ(SnesPPUOBJT *pPPUObj, SnesRenderObjT *pObjs, Int32 nObjs, Uint8 *pObjY, Uint8 *pObjSize)
{
	// xxxxxxxx
	// yyyyyyyy
	// CCCCCCCC
	// vhppcccC


	while (nObjs > 0)
	{
		Uint32 uTile;
		Uint8 uAttrib;

		uAttrib = pPPUObj->uAttrib;

		uTile =  pPPUObj->uTile;
		uTile|= ((uAttrib&1)<<8);

		pObjs->uPosX   |= pPPUObj->uX;
		pObjs->uPosY    = pPPUObj->uY + 1;
		pObjs->uPal   = (uAttrib >> 1) & 7;
		pObjs->uPri   = (uAttrib >> 4) & 3;
		pObjs->bHFlip = (uAttrib >> 6) & 1;
		if (uAttrib & 0x80)
		{
			// Nos modos H=2*W, o PPU vira duas metades W x W em vez de
			// espelhar o retangulo inteiro. Isso equivale a XOR com W-1.
			pObjs->uVXOR = pObjs->uWidth - 1;
		} else
		{
			pObjs->uVXOR = 0;
		}

		pObjs->uTile = uTile;

        *pObjY++    = pObjs->uPosY;
        *pObjSize++ = pObjs->uHeight;

		// next obj
		pPPUObj++;
		pObjs++;
		nObjs--;
	}
}



Int32 SnesPPURender::CheckOBJ(SnesRenderObjT *pObjs, Int32 iObj, Int32 nObjs, Uint8 *pObjList, Int32 MaxObjLine, Int32 iLine)
{
	Int32 nObjLine = 0;

	while (nObjs > 0)
	{
		SnesRenderObjT *pObj;
		Uint32 uObjY;

		// get pointer to object
		pObj   = &pObjs[iObj & 0x7F];

		uObjY = iLine - pObj->uPosY;
		uObjY&= 0xFF;

		if (uObjY < pObj->uHeight &&
		    _SnesPPUOBJVisibleX(pObj->uPosX, pObj->uWidth))
		{
			// we got an obj
			*pObjList =  iObj;
			pObjList++;

			nObjLine++;
			if (nObjLine >= MaxObjLine) break;
		}

		
		iObj++;
		nObjs--;
	}

	return nObjLine;
}







Int32 SnesPPURender::CheckOBJ(Uint8 *pObjY, Uint8 *pObjSize, Int32 iObj, Int32 nObjs, Uint8 *pObjList, Int32 MaxObjLine, Int32 iLine)
{
	Int32 nObjLine = 0;

	while (nObjs > 0)
	{
		Uint32 uObjY, uObjSize;

		iObj &= 0x7F;

		// get pointer to object
        uObjSize = pObjSize[iObj];
		uObjY    = pObjY[iObj];

		uObjY = iLine - uObjY;
		uObjY&= 0xFF;

		if (uObjY < uObjSize) 
		{
			// we got an obj
			*pObjList =  iObj;
			pObjList++;

			nObjLine++;
			if (nObjLine >= MaxObjLine) break;
		}

		iObj++;
		nObjs--;
	}

	return nObjLine;
}



Int32 SnesPPURender::CheckOBJ(Uint8 *pObjList, Int32 iLine)
{
    if (iLine >= 0 && iLine < SNPPU_MAXLINE)
    {
        memcpy(pObjList, m_ObjLine[iLine], SNPPU_MAXOBJ);
        return m_nObjLine[iLine];
    } else
    {
        return 0;
    }
}



/* AURORA_OBJ_LIMIT_V1_2_SCREEN
 * "Per Screen" is intentionally a performance hack rather than SNES
 * behavior. It chooses the first N genuinely visible OBJ in the same OAM
 * traversal order already used by the renderer, then keeps each chosen OBJ
 * for every scanline it occupies. This avoids vertically sliced sprites. */
static Bool _SnesPPUOBJScreenLimiterVisibleX(Uint16 uPosX, Uint8 uWidth)
{
	Int32 iX;
	uPosX &= 0x1FF;
	iX = (uPosX & 0x100) ? ((Int32)uPosX - 512) : (Int32)uPosX;
	return iX < 256 && iX > -(Int32)uWidth;
}

static Bool _SnesPPUOBJScreenLimiterVisibleY(
	Uint32 uObjY, Uint32 uObjSize, Uint32 uLineCount)
{
	while (uObjSize > 0)
	{
		if (uObjY < uLineCount)
			return TRUE;
		uObjY = (uObjY + 1) & 0xFF;
		uObjSize--;
	}
	return FALSE;
}

void SnesPPURender::UpdateOBJVisibility(Uint8 *pObjY, Uint8 *pObjSize, Int32 iObj, Int32 nObjs)
{
	const Int32 screenBudget = SNPPURenderGetObjScreenBudget();
	const Bool screenLimited = screenBudget < SNESPPU_OBJ_NUM;
	/* AURORA_SETINI_DISPLAY_V1_OBJ_20260915
	 * V2 supersedes V1's visibility implementation but retains its active
	 * 224/239-line contract and marker for cumulative-idempotency checks. */
	const Uint32 lineCount = m_pPPU->GetFrameVisibleLineCount() + 1u;
	const Bool bObjInterlace = m_pPPU->IsObjInterlace();
	const SnesPPURegsT *pRegs = m_pPPU->GetRegs();
	const Bool bFirstSpritePlusY =
		(pRegs->oamaddr.w & 0x8000) != 0 &&
		(pRegs->oamaddr.w & 3) == 3;

	(void)pObjSize;
	memset(m_nObjLine, 0, sizeof(m_nObjLine));
	memset(m_nObjTilePotential, 0, sizeof(m_nObjTilePotential));
	memset(m_ObjRangeOver, 0, sizeof(m_ObjRangeOver));
	memset(m_ObjTimeOver, 0, sizeof(m_ObjTimeOver));

	/* Phase 1: physical range evaluation. Keep at most the first 32 OBJ in
	 * the current priority traversal and remember the real 33rd candidate.
	 * Sliver pressure is accumulated only from those selected 32 OBJ. */
	if (bFirstSpritePlusY)
	{
		const Int32 baseFirst = iObj & 0x7F;
		for (Int32 line = 0; line < (Int32)lineCount; line++)
		{
			Int32 obj = (baseFirst + line) & 0x7F;
			Int32 left = nObjs;

			while (left-- > 0)
			{
				const SnesRenderObjT *pObj = &m_Objs[obj];
				const Uint32 displayHeight =
					_SnesPPUOBJDisplayHeight(pObj, bObjInterlace);
				const Uint32 relY =
					((Uint32)line - (Uint32)pObjY[obj]) & 0xFF;

				if (relY < displayHeight &&
				    _SnesPPUOBJVisibleX(pObj->uPosX, pObj->uWidth))
				{
					if (m_nObjLine[line] < SNPPU_MAXOBJ)
					{
						m_ObjLine[line][m_nObjLine[line]++] = (Uint8)obj;
						m_nObjTilePotential[line] +=
							_SnesPPUOBJCountPhysicalSlivers(pObj);
					}
					else
					{
						m_ObjRangeOver[line] = 1;
						break;
					}
				}
				obj = (obj + 1) & 0x7F;
			}
		}
	}
	else
	{
		Int32 obj = iObj;
		Int32 left = nObjs;
		while (left-- > 0)
		{
			SnesRenderObjT *pObj;
			Uint32 y;
			Uint32 h;
			Uint16 slivers;

			obj &= 0x7F;
			pObj = &m_Objs[obj];
			h = _SnesPPUOBJDisplayHeight(pObj, bObjInterlace);
			y = pObjY[obj];
			slivers = _SnesPPUOBJCountPhysicalSlivers(pObj);

			if (_SnesPPUOBJVisibleX(pObj->uPosX, pObj->uWidth))
			{
				while (h-- > 0)
				{
					if (y < lineCount)
					{
						if (m_nObjLine[y] < SNPPU_MAXOBJ)
						{
							m_ObjLine[y][m_nObjLine[y]++] = (Uint8)obj;
							m_nObjTilePotential[y] += slivers;
						}
						else
						{
							m_ObjRangeOver[y] = 1;
						}
					}
					y = (y + 1) & 0xFF;
				}
			}
			obj++;
		}
	}

	/* Phase 2: the 34-sliver fetch limit is applied only to the OBJ selected
	 * by phase 1. Equality is legal; the 35th counted sliver sets Time Over.
	 * Actual dropout order remains in _FetchOBJ: selected OBJ are traversed
	 * in reverse, while each OBJ advances left-to-right onscreen. */
	for (Uint32 line = 0; line < lineCount; line++)
	{
		if (m_nObjTilePotential[line] > SNPPU_MAXOBJCHR)
			m_ObjTimeOver[line] = 1;
	}

	if (!screenLimited)
		return;

	/* Aurora's optional per-screen limiter is non-hardware policy. Rebuild
	 * only the render list after the physical flags above are frozen. */
	{
		Uint8 candidates[SNESPPU_OBJ_NUM];
		Uint8 selected[SNESPPU_OBJ_NUM];
		Int32 nc = 0;
		Int32 obj = iObj;
		Int32 left = nObjs;

		memset(selected, 0, sizeof(selected));
		while (left-- > 0)
		{
			SnesRenderObjT *pObj;
			Uint32 h;
			Uint32 y;
			Bool keep;

			obj &= 0x7F;
			pObj = &m_Objs[obj];
			h = _SnesPPUOBJDisplayHeight(pObj, bObjInterlace);
			y = pObjY[obj];
			keep = _SnesPPUOBJVisibleX(pObj->uPosX, pObj->uWidth) &&
			       _SnesPPUOBJScreenLimiterVisibleX(pObj->uPosX, pObj->uWidth) &&
			       _SnesPPUOBJScreenLimiterVisibleY(y, h, lineCount);
			if (keep && nc < SNESPPU_OBJ_NUM)
				candidates[nc++] = (Uint8)obj;
			obj++;
		}

		if (nc)
		{
			const Int32 keep = nc < screenBudget ? nc : screenBudget;
			Int32 begin = 0;
			if (nc > screenBudget)
				begin = (Int32)(((g_SnesObjLimitFramePhase % (Uint32)nc) *
					(Uint32)screenBudget) % (Uint32)nc);
			for (Int32 i = 0; i < keep; i++)
				selected[candidates[(begin + i) % nc]] = 1;
		}

		memset(m_nObjLine, 0, sizeof(m_nObjLine));
		memset(m_nObjTilePotential, 0, sizeof(m_nObjTilePotential));

		obj = iObj;
		left = nObjs;
		while (left-- > 0)
		{
			SnesRenderObjT *pObj;
			Uint32 y;
			Uint32 h;
			Uint16 slivers;

			obj &= 0x7F;
			if (!selected[obj])
			{
				obj++;
				continue;
			}
			pObj = &m_Objs[obj];
			h = _SnesPPUOBJDisplayHeight(pObj, bObjInterlace);
			y = pObjY[obj];
			slivers = _SnesPPUOBJCountPhysicalSlivers(pObj);
			while (h-- > 0)
			{
				if (y < lineCount && m_nObjLine[y] < SNPPU_MAXOBJ)
				{
					m_ObjLine[y][m_nObjLine[y]++] = (Uint8)obj;
					m_nObjTilePotential[y] += slivers;
				}
				y = (y + 1) & 0xFF;
			}
			obj++;
		}
	}
}


void SnesPPURender::UpdateOBJ(Uint8 *pObjY, Uint8 *pObjSize)
{
	SnesOAMT *pOAM = m_pPPU->GetOAM();
	const SnesPPURegsT *pRegs  = m_pPPU->GetRegs();

	// decode objs
	_DecodeOBJEX(pOAM->ObjEx, m_Objs, SNESPPU_OBJ_NUM,
	             (pRegs->obsel >> 5) & 7);
	_DecodeOBJ(pOAM->Objs, m_Objs, SNESPPU_OBJ_NUM, pObjY, pObjSize);
}
