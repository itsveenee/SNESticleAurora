

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "types.h"
#include "prof.h"
#include "snmask.h"
#include "rendersurface.h"
#include "snppurender.h"
#include "snppublend_gs.h"
#include "snppucolor.h"
#include "sndbglog.h"

#include <tamtypes.h>
extern "C" {

#include <kernel.h>
#include "ps2dma.h"
#include "gpfifo.h"
#include "gpprim.h"
#include "gs.h"
#include "gslist.h"
#include "ps2mem.h"
#include "gskit_backend.h"
}

/* RenderInfo occupies the beginning of the 16 KiB EE scratchpad. Keep a
   second, DMA-owned copy of BlendInfo after it: the CPU may then compose the
   next scanline while GIF is still consuming the previous one. */
#define SNPPU_DMA_BLENDINFO_OFFSET (6 * 1024)
/* AURORA_SNES_RENDERER_PERF_V8_GS_PINGPONG
 * Prepare the next DMA-owned line in an alternate scratchpad slot. */
#define SNPPU_DMA_BLENDINFO_SLOTS 2u
#define SNPPU_DMA_BLENDINFO_STRIDE \
	((sizeof(SNPPUBlendInfoT) + 63u) & ~63u)
#define SNPPU_DMA_BLENDINFO_SLOT_ADDR(i) \
	(PS2MEM_SCRATCHPAD + SNPPU_DMA_BLENDINFO_OFFSET + \
	 (Uint32)(i) * SNPPU_DMA_BLENDINFO_STRIDE)
#define SNPPU_DMA_BLENDINFO_ADDR SNPPU_DMA_BLENDINFO_SLOT_ADDR(0)

/* Renderer + both staging slots stay below the 14 KiB lookup reservation. */
typedef char SNPPUScratchLayoutCheck[
	(sizeof(SnesRender8pInfoT) <= SNPPU_DMA_BLENDINFO_OFFSET &&
	 SNPPU_DMA_BLENDINFO_OFFSET + SNPPU_DMA_BLENDINFO_SLOTS *
		SNPPU_DMA_BLENDINFO_STRIDE <= PS2MEM_SNES_LOOKUP_OFFSET &&
	 PS2MEM_SNES_LOOKUP_OFFSET + PS2MEM_SNES_LOOKUP_SIZE <= 16 * 1024)
		? 1 : -1];

/* AURORA_TOPGEAR_GS_LINE_PAYLOAD_COPY_V4_20260917
 * Compile-time proof required before merging three 256-byte copies. */
typedef char SNPPUBlendLinePayloadLayoutCheck[
	(offsetof(SNPPUBlendInfoT, uSub8) ==
	 offsetof(SNPPUBlendInfoT, uMain8) +
	 sizeof(((SNPPUBlendInfoT *)0)->uMain8) &&
	 offsetof(SNPPUBlendInfoT, uAttrib8) ==
	 offsetof(SNPPUBlendInfoT, uSub8) +
	 sizeof(((SNPPUBlendInfoT *)0)->uSub8)) ? 1 : -1];

#if SNDBG_LOG
#define SNPPU_GS_DIAG_SAMPLES 8
struct SNPPUGSDiagT
{
	Uint32 Frames;
	Uint32 Lines;
	Uint32 SyncCalls;
	Uint32 SyncCycles;
	Uint32 CopyCycles;
	Uint32 KickCycles;
	Uint32 CopyBytes;
	Uint32 PaletteUploads;
	Uint32 IntensityLines;
	Uint32 DirectMainLines;
	Uint32 StageMismatch;
	Uint32 CopyMismatch;
	Uint32 SourceHash;
	Uint32 StageHash;
	Uint32 Expected[SNPPU_GS_DIAG_SAMPLES];
	Bool   HasExpected;
};

static SNPPUGSDiagT _SNPPUGSDiag;

#if SNDBG_DEEP
static Uint32 _SNPPUGSSample(const SNPPUBlendInfoT *pInfo,
	                          Uint32 *pSamples)
{
	const Uint32 *pWords = (const Uint32 *)pInfo;
	const Uint32 nWords = sizeof(*pInfo) / sizeof(Uint32);
	Uint32 h = 2166136261u;
	Uint32 i;

	for (i = 0; i < SNPPU_GS_DIAG_SAMPLES; i++)
	{
		Uint32 uIndex = (nWords - 1) * i / (SNPPU_GS_DIAG_SAMPLES - 1);
		Uint32 uValue = pWords[uIndex];
		if (pSamples)
			pSamples[i] = uValue;
		h ^= uValue;
		h *= 16777619u;
	}
	return h;
}

static void _SNPPUGSValidateStage(const SNPPUBlendInfoT *pInfo)
{
	Uint32 uSamples[SNPPU_GS_DIAG_SAMPLES];
	Uint32 i;

	if (!_SNPPUGSDiag.HasExpected)
		return;

	_SNPPUGSSample(pInfo, uSamples);
	for (i = 0; i < SNPPU_GS_DIAG_SAMPLES; i++)
	{
		if (uSamples[i] != _SNPPUGSDiag.Expected[i])
		{
			_SNPPUGSDiag.StageMismatch++;
			break;
		}
	}
}
#endif
#endif

#define SNPPUBLEND_PAL32 (TRUE)

extern SnesChrLookupT _SnesPPU_PlaneLookup[2];

/* AURORA_TOPGEAR_GS_COLORLUT_CACHE_V4_20260917
 * SNPPUColorGetPalette() returns a fixed static array. Calibration
 * mutates its contents in place, so this address remains valid. */
static const Uint32 *_SNPPUBlend_ColorLUT = NULL;

/* AURORA_PS2_EE_CLUT_DMA_BOUNDS_V1_20260917
 * GPPrimUploadTexture sends these as 16x16 PSMCT32 sources (1024 bytes).
 * Keep the eight logical HSM entries unchanged, but own the entire DMA
 * source range so the GIF/DMAC never reads beyond the C object. Unspecified
 * entries are zero-initialized by C/C++. */
static Uint32 _SNPPUBlend_AttribMainPal[256] _ALIGN(64) =
{                   // HSM
    0x00000000,     // 000
    0x80000000,     // 001
    0x00000000,     // 010
    0x80000000,     // 011
    0x00000000,     // 100
    0x40000000,     // 101
    0x00000000,     // 110
    0x40000000,     // 111
};


static Uint32 _SNPPUBlend_AttribSubPal[256] _ALIGN(64) =
{                   // HSM
    0x00000000,     // 000
    0x00000000,     // 001
    0x80000000,     // 010
    0x80000000,     // 011
    0x00000000,     // 100
    0x00000000,     // 101
    0x40000000,     // 110
    0x40000000,     // 111
};



/* AURORA_TOPGEAR_GS_PLANAR_PTR_V4_20260917
 * The old loop advanced SNMaskT pointers one byte through casts. Consume the
 * exact same 32 mask bytes through byte pointers directly. */
static void _PlanarTo3(Uint8 *pDest, SNMaskT *pSrc0, SNMaskT *pSrc1, SNMaskT *pSrc2)
{
	Uint32 nBytes = 256 / 8;
	/* AURORA_SNES_BG_LOOKUP_SCRATCHPAD_V2_20260920: PlanarTo3 uses PlaneLookup[1], which remains in ordinary RAM. */
	SnesChrLookup64T *pLookup64 =
		(SnesChrLookup64T *)&_SnesPPU_PlaneLookup[1];
	Uint64 *pDest64 = (Uint64 *)pDest;
	const Uint8 *pSrc8_0 = pSrc0->uMask8;
	const Uint8 *pSrc8_1 = pSrc1->uMask8;
	const Uint8 *pSrc8_2 = pSrc2->uMask8;

	/* AURORA_SNES_RENDERER_PERF_V8_PLANAR_HALF_ELIDE
	 * Half-color disabled => plane 2 is exactly zero.  Four 64-bit tests
	 * replace 32 zero-plane LUT reads, shifts and ORs. */
	const Uint64 uHalfAny = pSrc2->uMask64[0] | pSrc2->uMask64[1] |
		pSrc2->uMask64[2] | pSrc2->uMask64[3];
	if (!uHalfAny)
	{
		while (nBytes > 0)
		{
			Uint64 uData = (*pLookup64)[*pSrc8_0++] << 0;
			uData |= (*pLookup64)[*pSrc8_1++] << 1;
			*pDest64++ = uData;
			nBytes--;
		}
		return;
	}

	while (nBytes > 0)
	{
		Uint64 uData;
		uData  = (*pLookup64)[*pSrc8_0++] << 0;
		uData |= (*pLookup64)[*pSrc8_1++] << 1;
		uData |= (*pLookup64)[*pSrc8_2++] << 2;
		*pDest64++ = uData;
		nBytes--;
	}
}

void SNPPUBlendGS::MarkPaletteEntryDirty(Uint32 uAddr)
{
	const Uint32 uWord = (uAddr >> 5) & 7;
	const Uint32 uBit = 1u << (uAddr & 31);

	if (!(m_uPaletteDirty[uWord] & uBit))
	{
		m_uPaletteDirty[uWord] |= uBit;
		m_nPaletteDirty++;
	}
	for (Uint32 uSlot = 0; uSlot < SNPPU_DMA_BLENDINFO_SLOTS; ++uSlot)
	{
		if (!(m_uStagePaletteDirty[uSlot][uWord] & uBit))
		{
			m_uStagePaletteDirty[uSlot][uWord] |= uBit;
			m_nStagePaletteDirty[uSlot]++;
		}
	}
	m_bPaletteDirty = TRUE;
}

void SNPPUBlendGS::MarkPaletteAllDirty()
{
	for (Int32 iWord = 0; iWord < 8; iWord++)
	{
		m_uPaletteDirty[iWord] = 0xFFFFFFFFu;
		for (Uint32 uSlot = 0; uSlot < SNPPU_DMA_BLENDINFO_SLOTS; ++uSlot)
			m_uStagePaletteDirty[uSlot][iWord] = 0xFFFFFFFFu;
	}
	m_nPaletteDirty = 256;
	for (Uint32 uSlot = 0; uSlot < SNPPU_DMA_BLENDINFO_SLOTS; ++uSlot)
		m_nStagePaletteDirty[uSlot] = 256;
	m_bPaletteDirty = TRUE;
}

Uint32 SNPPUBlendGS::CopyDirtyPalette(PaletteT *pDest,
	                                  const PaletteT *pSource,
	                                  Uint32 uSlot)
{
	Uint32 uCopiedBytes = 0;
	if (!m_nStagePaletteDirty[uSlot])
		return 0;

	/* Keep each ping-pong slot a coherent 1 KiB CLUT source. Sparse HDMA
	 * changes accumulate independently until that slot is prepared again. */
	if (m_nStagePaletteDirty[uSlot] >= 64)
	{
		memcpy(pDest, pSource, sizeof(*pDest));
		uCopiedBytes = sizeof(*pDest);
	}
	else
	{
		for (Int32 iWord = 0; iWord < 8; iWord++)
		{
			Uint32 uBits = m_uStagePaletteDirty[uSlot][iWord];
			while (uBits)
			{
				const Uint32 uBit = (Uint32)__builtin_ctz(uBits);
				const Uint32 uAddr = (Uint32)iWord * 32 + uBit;
#if SNPPUBLEND_PAL32
				pDest->Color32[uAddr] = pSource->Color32[uAddr];
				uCopiedBytes += sizeof(pDest->Color32[0]);
#else
				pDest->Color16[uAddr] = pSource->Color16[uAddr];
				uCopiedBytes += sizeof(pDest->Color16[0]);
#endif
				uBits &= uBits - 1;
			}
		}
	}
	memset(m_uStagePaletteDirty[uSlot], 0, sizeof(m_uStagePaletteDirty[uSlot]));
	m_nStagePaletteDirty[uSlot] = 0;
	return uCopiedBytes;
}

#if SNPPUBLEND_PAL32

void SNPPUBlendGS::UpdatePaletteEntry(SNPPUBlendInfoT *pInfo, Uint32 uAddr, Uint32 uData, Uint32 uIntensity)
{
    PaletteT *pPal = pInfo->Pal;

	uData = _SNPPUBlend_ColorLUT[uData & 0x7FFF];

	if (uAddr > 0)
	{
		uData |= 0x80000000;
	} 

	// swap 8 and 0x10 of addr
	uAddr = (uAddr & ~0x18) | ((uAddr & 0x10) >> 1) | ((uAddr & 0x08) << 1);

	if (pPal->Color32[uAddr] != uData)
	{
		pPal->Color32[uAddr] = uData;
		MarkPaletteEntryDirty(uAddr);
	}
}

void SNPPUBlendGS::UpdatePalette(SNPPUBlendInfoT *pInfo, Uint16 *pCGRam, Uint32 uIntensity)
{
	Int32 iEntry;
	Int32 iBase;
	PaletteT *pPal = pInfo->Pal;
	const Uint32 *pColorLUT = _SNPPUBlend_ColorLUT;

	PROF_ENTER("SNPPUBlendUpdatePalette");

	/* AURORA_TOPGEAR_GS_PAL_SWIZZLE_V5_20260917
	 * CSM1's address permutation is only swap(bit3, bit4), repeated in every
	 * 32-entry block. Emit that exact permutation directly instead of
	 * recomputing it for all 255 nonzero logical entries. */
	pPal->Color32[0] = pColorLUT[pCGRam[0] & 0x7FFF];

	for (iEntry = 1; iEntry < 8; iEntry++)
		pPal->Color32[iEntry] =
			pColorLUT[pCGRam[iEntry] & 0x7FFF] | 0x80000000u;

	for (iEntry = 0; iEntry < 8; iEntry++)
	{
		pPal->Color32[8 + iEntry] =
			pColorLUT[pCGRam[16 + iEntry] & 0x7FFF] | 0x80000000u;
		pPal->Color32[16 + iEntry] =
			pColorLUT[pCGRam[8 + iEntry] & 0x7FFF] | 0x80000000u;
		pPal->Color32[24 + iEntry] =
			pColorLUT[pCGRam[24 + iEntry] & 0x7FFF] | 0x80000000u;
	}

	for (iBase = 32; iBase < 256; iBase += 32)
	{
		for (iEntry = 0; iEntry < 8; iEntry++)
		{
			pPal->Color32[iBase + iEntry] =
				pColorLUT[pCGRam[iBase + iEntry] & 0x7FFF] |
				0x80000000u;
			pPal->Color32[iBase + 8 + iEntry] =
				pColorLUT[pCGRam[iBase + 16 + iEntry] & 0x7FFF] |
				0x80000000u;
			pPal->Color32[iBase + 16 + iEntry] =
				pColorLUT[pCGRam[iBase + 8 + iEntry] & 0x7FFF] |
				0x80000000u;
			pPal->Color32[iBase + 24 + iEntry] =
				pColorLUT[pCGRam[iBase + 24 + iEntry] & 0x7FFF] |
				0x80000000u;
		}
	}
	MarkPaletteAllDirty();

	PROF_LEAVE("SNPPUBlendUpdatePalette");
}


#else

static Uint32 SNPPUColorConvert15to32(SnesColor16T uColor16)
{
	Uint32 uColor32;
	Uint32 uR, uG, uB;

	uR = ((uColor16 >>  0) & 0x1F);
	uG = ((uColor16 >>  5) & 0x1F);
	uB = ((uColor16 >>  10) & 0x1F);

	// convert snes16->generic32
	uColor32 =  uR <<  (0  + 3);
	uColor32|=  uG <<  (8  + 3);
	uColor32|=  uB <<  (16 + 3);
	return uColor32;
}


void SNPPUBlendGS::UpdatePaletteEntry(SNPPUBlendInfoT *pInfo, Uint32 uAddr, Uint32 uData, Uint32 uIntensity)
{
    PaletteT *pPal = pInfo->Pal;
	if (uAddr > 0)
	{
		uData |= 0x8000;
	} 
	if (pPal->Color16[uAddr] != uData)
	{
		pPal->Color16[uAddr] = uData;
		MarkPaletteEntryDirty(uAddr);
	}
}

void SNPPUBlendGS::UpdatePalette(SNPPUBlendInfoT *pInfo, Uint16 *pCGRam, Uint32 uIntensity)
{
	Int32 iEntry;
    PaletteT *pPal = pInfo->Pal;

	PROF_ENTER("SNPPUBlendUpdatePalette");


	pPal->Color16[0] = pCGRam[0];
	for (iEntry=1; iEntry < 256; iEntry++)
	{
		// set palette entry (with alpha set)
		pPal->Color16[iEntry] = pCGRam[iEntry] | 0x8000;
	}
	MarkPaletteAllDirty();

	PROF_LEAVE("SNPPUBlendUpdatePalette");
}



#endif


static void _GPFifoUploadTexture(int TBP, int TBW, int xofs, int yofs, int pxlfmt, void *tex, int wpxls, int hpxls)
{
    int numq;

    numq = wpxls * hpxls;
    switch (pxlfmt)
    {
    case 0x00: numq = (numq >> 2) + ((numq & 0x03) != 0 ? 1 : 0); break;
    case 0x02: numq = (numq >> 3) + ((numq & 0x07) != 0 ? 1 : 0); break;
    case 0x13: numq = (numq >> 4) + ((numq & 0x0f) != 0 ? 1 : 0); break;
    case 0x14: numq = (numq >> 5) + ((numq & 0x1f) != 0 ? 1 : 0); break;
    default:   numq = 0;
    }

    GSGifTagOpenAD();

    GSGifRegAD(GS_REG_BITBLTBUF,GS_SET_BITBLTBUF( 0, (TBW/64), pxlfmt,  (TBP/256), (TBW/64), pxlfmt));
    GSGifRegAD(GS_REG_TRXPOS,GS_SET_TRXPOS(0,0,xofs,yofs,0));
    GSGifRegAD(GS_REG_TRXREG,GS_SET_TRXREG(wpxls, hpxls));
    GSGifRegAD(GS_REG_TRXDIR,GS_SET_TRXDIR(0));
    
    GSGifTagCloseAD();
    
    // image gif tag
    GSGifTagImage(numq);

    // close last dma cnt
    GSDmaCntClose();

    // dma image data
    GSDmaRef((Uint128 *)tex, numq);


    // start new dma cnt
    GSDmaCntOpen();
}


static void _SNPPURenderTexLine(Int32 iDestLine, Int32 iSrcLine, Uint32 RGBA, int abe)
{
    int x1,x2,y1,y2;
    int u1,u2,v1,v2;

    x1  =   0 << 4;
    x2  = 256 << 4;
    y1  = (iDestLine + 0) << 4;
    y2  = (iDestLine + 1) << 4;

    u1  =   0 << 4;
    u2  = 256 << 4;
    v1  = (iSrcLine + 0) << 4;
    v2  = (iSrcLine + 1) << 4;

    x1+=0x8000;
    y1+=0x8000;
    x2+=0x8000;
    y2+=0x8000;
    
    GSGifTagOpen(GIF_SET_TAG(1, 1, 0, 0, 1, 6), 0xF535310);
    
	GSGifReg(GS_SET_PRIM(0x06, 0, 1, 0, abe, 0, 1, 0, 0));
	GSGifReg(RGBA);
	GSGifReg(GS_SET_UV(u1, v1));
	GSGifReg(GS_SET_XYZ(x1,y1,0));
	GSGifReg(GS_SET_UV(u2, v2));
	GSGifReg(GS_SET_XYZ(x2,y2,0));
    
    GSGifTagClose();

}


static void _SNPPURenderLine(Int32 iDestLine, int abe)
{
    int x1,x2,y1,y2;

    x1  =   0 << 4;
    x2  = 256 << 4;
    y1  = (iDestLine + 0) << 4;
    y2  = (iDestLine + 1) << 4;

    x1+=0x8000;
    x2+=0x8000;
    y1+=0x8000;
    y2+=0x8000;
  
    GSGifTagOpen(GIF_SET_TAG(1, 1, 0, 0, 1, 4), 0xF550);
    
	GSGifReg(GS_SET_PRIM(0x06, 0, 0, 0, abe, 0, 1, 0, 0));
	GSGifReg(GS_SET_XYZ(x1,y1,0));
	GSGifReg(GS_SET_XYZ(x2,y2,0));
	GSGifReg(0);
    
    GSGifTagClose();
}
  


void SNPPUBlendGS::Begin(CRenderSurface *pTarget)
{
#if SNDBG_LOG
	/* End() ja esperou a ultima chain. O mixer de audio tambem usa o
	   scratchpad entre quadros, portanto uma expectativa antiga nao deve ser
	   comparada com o primeiro scanline do quadro seguinte. */
	_SNPPUGSDiag.HasExpected = FALSE;
#endif
    m_pTarget = pTarget;
	if (!m_pTarget)
	{
		return;
	}

	/* The audio mixer may reuse scratchpad between frames.  Refresh the
	   staged CLUT on the first rendered line even when CGRAM did not change. */
	MarkPaletteAllDirty();

    /* These two attribute CLUTs never change and live in a VRAM range
       reserved exclusively for the SNES blender.  Upload them once for the
       lifetime of the renderer instead of spending two transfers per frame.
       The dynamic uploads inside _SNPPUBlendBuildList still use REF tags so
       they pick up the current staged scanline on every kick.

       The legacy _GPFifoUploadTexture took TBP in bytes (it divides
       by 256 internally to encode BITBLTBUF.DBP); GPPrimUploadTexture
       takes TBP in 256-byte units (it multiplies by 256 internally).
       m_DmaList.uAttribMainPal / uAttribSubPal are already stored in
       TBP units, so drop the * 0x100 that converted to bytes for the
       legacy call.

       AURORA_PS2_EE_CLUT_DMA_BOUNDS_V1_20260917: the source arrays are now full 16 x 16
       PSMCT32 slabs (1024 bytes each). TEXCLUT still uses the same first
       eight HSM entries, while the remaining zero padding merely makes the
       EE->GS DMA source range memory-safe. */
    if (!m_bAttribPalettesUploaded)
    {
        GPPrimUploadTexture(
             m_DmaList[0].uAttribMainPal,
             64, 0, 0,
             GS_PSMCT32,
             _SNPPUBlend_AttribMainPal,
             16,
             16);

        GPPrimUploadTexture(
             m_DmaList[0].uAttribSubPal,
             64, 0, 0,
             GS_PSMCT32,
             _SNPPUBlend_AttribSubPal,
             16,
             16);
        m_bAttribPalettesUploaded = TRUE;
    }


    GSGifTagOpenAD();

	GSGifRegAD(GS_REG_TEXCLUT,256/64);

	GSGifRegAD(GS_REG_TEXA,GS_SET_TEXA(0x00,0,0x80));

    // clamp_1
	GSGifRegAD(GS_REG_CLAMP_1,GS_SET_CLAMP(0, 0, 0, 0, 0, 0));

    // tex1_1
    GSGifRegAD(GS_REG_TEX1_1, 0x000);

    GSGifTagCloseAD();


    GPFifoPause();
}

void SNPPUBlendGS::End()
{
	if (!m_pTarget)
	{
		return;
	}

    // wait for previous dma to finish
#if SNDBG_LOG
	{
		Uint32 uStart = ProfCtrGetCycle();
		DmaSyncGIF();
		_SNPPUGSDiag.SyncCycles += ProfCtrGetCycle() - uStart;
		_SNPPUGSDiag.SyncCalls++;
		#if SNDBG_DEEP
		_SNPPUGSValidateStage(
			(const SNPPUBlendInfoT *)
				SNPPU_DMA_BLENDINFO_SLOT_ADDR(m_uLastDmaSlot));
		#endif
		_SNPPUGSDiag.HasExpected = FALSE;
	}
#else
    DmaSyncGIF();
#endif

    GPFifoResume();

    /* AURORA_BLEND_POST_RESTORE_ELIDE_V2
     *
     * The legacy path queued FRAME_1/XYOFFSET_1 restoration here, but this
     * newly-resumed raw list is not dispatched until the end-of-frame
     * GPFifoFlush(). Modern MainLoopRender calls GSK_ResetFrame() BEFORE its
     * first gsKit primitive, restoring FRAME_1, XYOFFSET_1, ALPHA_1 and
     * COLCLAMP in the actual draw queue that needs them.
     *
     * No host GS primitive is issued between this End() and that reset during
     * gameplay. Leave the raw list at its exact idle state (one open CNT tag)
     * so GPFifoFlush() can take its empty-list fast path instead of sending a
     * two-register restore chain that is already superseded. */

    /* The blender chain has just rendered into _OutTex via raw GIF
       DMA (FRAME_1 = uOutAddr). The next gsKit textured prim that
       samples _OutTex (PolyTexture(&_OutTex) + PolyRect in
       MainLoopRender) needs an explicit TEXFLUSH before sampling, or
       the GS hardware texture cache will keep serving the stale
       texels it cached on the previous frame. Without this, on
       hardware and on emulators, the visible output is whatever was
       in the texture cache before the blender ran - typically a
       mostly-black screen, with a brief correct frame whenever some
       other path (e.g. menu font upload via GPPrimUploadTexture, on
       L2+R2 or on menu redraw) happens to call
       GSK_InvalidateTextureCache for an unrelated reason. Hooking
       the invalidate here closes that race so every gsKit sample of
       _OutTex sees the fresh blender output. */
    GSK_InvalidateTextureCache();

#if SNDBG_LOG
	_SNPPUGSDiag.Frames++;
	if (_SNPPUGSDiag.Frames >= SNDBG_FRAME_PERIOD)
	{
		Uint32 uLines = _SNPPUGSDiag.Lines ? _SNPPUGSDiag.Lines : 1;
		Uint32 uSync = _SNPPUGSDiag.SyncCalls ? _SNPPUGSDiag.SyncCalls : 1;
		DLog("[snes-gs] frames/lines=%u/%u avgcyc sync/copy/kick=%u/%u/%u avg-copy-bytes=%u pal-uploads=%u intensity-lines=%u direct-main-lines=%u",
			(unsigned)_SNPPUGSDiag.Frames, (unsigned)_SNPPUGSDiag.Lines,
			(unsigned)(_SNPPUGSDiag.SyncCycles / uSync),
			(unsigned)(_SNPPUGSDiag.CopyCycles / uLines),
			(unsigned)(_SNPPUGSDiag.KickCycles / uLines),
			(unsigned)(_SNPPUGSDiag.CopyBytes / uLines),
			(unsigned)_SNPPUGSDiag.PaletteUploads,
			(unsigned)_SNPPUGSDiag.IntensityLines,
			(unsigned)_SNPPUGSDiag.DirectMainLines);
		#if SNDBG_DEEP
		DLog("[snes-gs-deep] mismatch stage/copy=%u/%u",
			(unsigned)_SNPPUGSDiag.StageMismatch,
			(unsigned)_SNPPUGSDiag.CopyMismatch);
		DLog("[snes-gs-deep] sampled cpu/stage hash=%08X/%08X blendbytes=%u renderbytes=%u stage=%08X",
			(unsigned)_SNPPUGSDiag.SourceHash,
			(unsigned)_SNPPUGSDiag.StageHash,
			(unsigned)sizeof(SNPPUBlendInfoT),
			(unsigned)sizeof(SnesRender8pInfoT),
			(unsigned)SNPPU_DMA_BLENDINFO_ADDR);
		#endif
		memset(&_SNPPUGSDiag, 0, sizeof(_SNPPUGSDiag));
	}
#endif

    m_pTarget = NULL;
}




static void _SNPPUBlendBuildList(SNPPUDmaListT *pList,
	SNPPUBlendInfoT *pInfo, Uint32 uOutAddr, Bool bUploadPalette,
	Bool bApplyIntensity, Bool bDirectMain)
{
    PaletteT *pPal = pInfo->Pal;

	pList->pFixedColor = NULL;
	pList->pAddSub = NULL;
	pList->pIntensity = NULL;
	pList->pXYOffset = NULL;

    // begin dma list
    GSListBegin(pList->Data, sizeof(pList->Data) / sizeof(Uint128), NULL);

    GSDmaCntOpen();

	if (bUploadPalette)
	{
	#if SNPPUBLEND_PAL32
	// upload as 16x16 psmct32 for use as csm1
    _GPFifoUploadTexture(
         pList->uPalAddr * 0x100, 
         1, 0, 0, 
         GS_PSMCT32, 
         (void *)(((Uint32)pPal) | 0x80000000), 
         16, 
         16);
	#else
	// upload as 256x1 psmct16 for use as csm2
    _GPFifoUploadTexture(
         pList->uPalAddr * 0x100, 
         256, 0, 0, 
         GS_PSMCT16, 
         (void *)(((Uint32)pPal) | 0x80000000), 
		 256,
		 1);
	#endif
	}


    _GPFifoUploadTexture(
         pList->uInputAddr * 0x100, 
         256, 0, 0, 
         GS_PSMT8, 
         (void *)(((Uint32)pInfo->uMain8) | 0x80000000), 
         256, 
         1);

	if (bDirectMain)
	{
		/* No CGADSUB target and no main-screen color clipping: the SNES
		   result is exactly the palette-expanded main screen.  Write it to
		   the output in one primitive instead of constructing temp main/sub
		   colors and two attribute masks that can no longer affect a pixel. */
		GSGifTagOpenAD();
		GSGifRegAD(GS_REG_TEXFLUSH, 0);
		GSGifRegAD(GS_REG_FRAME_1,
			GS_SET_FRAME((uOutAddr/0x20), 256/64, GS_PSMCT32, 0));
#if SNPPUBLEND_PAL32
		GSGifRegAD(GS_REG_TEX0_1,
			GS_SET_TEX0(pList->uInputAddr, 256/64, GS_PSMT8, 8, 3,
				1, 0, pList->uPalAddr, GS_PSMCT32, 0, 0, 1));
#else
		GSGifRegAD(GS_REG_TEX0_1,
			GS_SET_TEX0(pList->uInputAddr, 256/64, GS_PSMT8, 8, 3,
				1, 0, pList->uPalAddr, GS_PSMCT16, 1, 0, 1));
#endif
		pList->pXYOffset = (Uint64 *)GSListGetUncachedPtr();
		GSGifRegAD(GS_REG_XYOFFSET_1, 0);
		GSGifTagCloseAD();

		_SNPPURenderTexLine(0, 0, 0x80808080, 0);

		GSDmaCntClose();
		GSDmaEnd();
		GSListEnd();
		return;
	}

    _GPFifoUploadTexture(
         pList->uInputAddr * 0x100, 
         256, 0, 1, 
         GS_PSMT8, 
         (void *)(((Uint32)pInfo->uSub8) | 0x80000000), 
         256, 
         1);

    _GPFifoUploadTexture(
         pList->uInputAddr * 0x100, 
         256, 0, 2, 
         GS_PSMT8, 
         (void *)(((Uint32)pInfo->uAttrib8) | 0x80000000), 
         256, 
         1);



    GSGifTagOpenAD();

    // texflush
    GSGifRegAD(GS_REG_TEXFLUSH,0);

    // setup frame register to point to our temporary texture
	GSGifRegAD(GS_REG_FRAME_1, GS_SET_FRAME((pList->uTempAddr/0x20),256/64,GS_PSMCT32,0 ));

	GSGifRegAD(GS_REG_XYOFFSET_1, GS_SET_XYOFFSET(0x8000, 0x8000));

    GSGifTagCloseAD();


    // setup src texture

    GSGifTagOpenAD();
	#if SNPPUBLEND_PAL32
	// use clut psmct32 csm1
	GSGifRegAD(GS_REG_TEX0_1,GS_SET_TEX0(pList->uInputAddr, 256/64, GS_PSMT8, 8, 3,    1, 0, pList->uPalAddr, GS_PSMCT32, 0, 0, 1));
	#else
	// use clut psmct16 csm2
	GSGifRegAD(GS_REG_TEX0_1,GS_SET_TEX0(pList->uInputAddr, 256/64, GS_PSMT8, 8, 3,    1, 0, pList->uPalAddr, GS_PSMCT16, 1, 0, 1));
	#endif
    GSGifRegAD(GS_REG_ALPHA_1,GS_SET_ALPHA(0,1,0,1, 0x80));

    pList->pFixedColor = (Uint64 *)GSListGetUncachedPtr();
    GSGifRegAD(GS_REG_RGBAQ, 0);
    GSGifTagCloseAD();

    // render fixed color32 -> temp32[1]
    _SNPPURenderLine(1, 0);

    // render main8 -> temp32[0]
    _SNPPURenderTexLine(0, 0, 0x80808080, 0);

    // render sub8 -> temp32[1] (alpha=0 means use fixed color)
    _SNPPURenderTexLine(1, 1, 0x80808080, 1);


    //
    // render attribs
    //



    GSGifTagOpenAD();

	// tex0_1
	GSGifRegAD(GS_REG_TEX0_1,GS_SET_TEX0(pList->uInputAddr, 256/64, GS_PSMT8, 8, 3,    1, 0, pList->uAttribMainPal, GS_PSMCT32, 0, 0, 1));


    // alpha_1: A = Cs, B = Cd, C = As, D = Cd
    // (a - b) * c + d
    GSGifRegAD(GS_REG_ALPHA_1,GS_SET_ALPHA(1,2,0,2, 0x20));
    
    GSGifTagCloseAD();

    // render attrib main8 -> temp32 line 0
    _SNPPURenderTexLine(0, 2, 0x80808080, 1);

       

    GSGifTagOpenAD();

	// tex0_1
	GSGifRegAD(GS_REG_TEX0_1,GS_SET_TEX0(pList->uInputAddr, 256/64, GS_PSMT8, 8, 3,    1, 0, pList->uAttribSubPal, GS_PSMCT32, 0, 0, 1));


    // alpha_1: A = Cs, B = Cd, C = As, D = Cd
    // (a - b) * c + d
    GSGifRegAD(GS_REG_ALPHA_1,GS_SET_ALPHA(1,2,0,2, 0x80));
    
    GSGifTagCloseAD();


    // render attrib sub8 -> temp32 line 0
    _SNPPURenderTexLine(1, 2, 0x80808080, 1);


    // texflush
    GSGifTagOpenAD();
    GSGifRegAD(GS_REG_TEXFLUSH,0);

    // setup frame register to point to our output texture
	GSGifRegAD(GS_REG_FRAME_1, GS_SET_FRAME((uOutAddr/0x20),256/64,GS_PSMCT32,0 ));

	// tex0_1
	GSGifRegAD(GS_REG_TEX0_1,GS_SET_TEX0(pList->uTempAddr, 256/64, GS_PSMCT32, 8, 3,    1, 0, 0, 0, 0, 0, 0));

    /* AURORA_SNES_FINAL_COLCLAMP_V1_20260914
     *
     * SNES color math clamps every RGB component after add/subtract.
     * The PS2 GS wraps/masks overflow and negative results when COLCLAMP=0.
     * Own COLCLAMP here, immediately before the final main/sub combine,
     * instead of relying on host GS state inherited from GSK_ResetFrame().
     *
     * General renderer accuracy fix: no CRC/title/game-specific path.
     */
    GSGifRegAD(GS_REG_COLCLAMP, 1);

    // alpha_1: A = Cs, B = Cd, C = As, D = Cd
    // (a - b) * c + d
    pList->pAddSub = (Uint64 *)GSListGetUncachedPtr();
    GSGifRegAD(GS_REG_ALPHA_1,GS_SET_ALPHA(0,2,2,1, 0x80));

    pList->pXYOffset = (Uint64 *)GSListGetUncachedPtr();
	GSGifRegAD(GS_REG_XYOFFSET_1, 0);
    
    GSGifTagCloseAD();

    // render out32 = main32 * attrib
    _SNPPURenderTexLine(0, 0, 0x80808080, 0);

    // render out32 += sub32 * attrib
    _SNPPURenderTexLine(0, 1, 0x80808080, 1);

    /* Preserve the GS state left by the legacy chain even when brightness is
       full.  Only the mathematically redundant drawing primitive is omitted. */
    GSGifTagOpenAD();
    GSGifRegAD(GS_REG_ALPHA_1,GS_SET_ALPHA(1,2,0,2, 0x80 ));
    pList->pIntensity = (Uint64 *)GSListGetUncachedPtr();
    /* AURORA_TOPGEAR_GS_FULL_INTENSITY_CONST_V4_20260917
     * With no intensity primitive the caller's only possible normal
     * value is 15; the old formula equals 0x80000000 exactly. */
    GSGifRegAD(GS_REG_RGBAQ,
        bApplyIntensity ? 0 : 0x80000000u);
    GSGifTagCloseAD();

    if (bApplyIntensity)
    {
        // render out32 *= intensity
        _SNPPURenderLine(0, 1);
    }

    // close current dma cnt
    GSDmaCntClose();
    
    // add end tag
    GSDmaEnd();

    GSListEnd();
}






#if 1


static void _SNPPUBlendSetParm(SNPPUDmaListT *pList, Int32 iLine,
	Uint32 uFixedColor16, Bool bAddSub, Uint32 uIntensity,
	Bool bDirectMain)
{
	if (bDirectMain)
	{
		*pList->pXYOffset =
			GS_SET_XYOFFSET(0x8000, 0x8000 - (iLine << 4));
		__asm__ __volatile__ ("sync.l");
		return;
	}

    *pList->pFixedColor = _SNPPUBlend_ColorLUT[uFixedColor16 & 0x7FFF];
    *pList->pXYOffset   = GS_SET_XYOFFSET(0x8000, 0x8000 - (iLine<<4)  );
    if (uIntensity != 15)
        *pList->pIntensity = (uIntensity * 0x80 / 15) << 24;
    if (!bAddSub)
    {
        // add
        *pList->pAddSub     = GS_SET_ALPHA(1,2,2,0, 0x80);
    } else
    {
        // sub
        *pList->pAddSub     = GS_SET_ALPHA(1,0,2,2, 0x80);
    }
    __asm__ __volatile__ ("sync.l");
}



#include "gs.h"

SNPPUBlendGS::SNPPUBlendGS(Uint32 uVramAddr, Uint32 uOutAddr)
{
    _SNPPUBlend_ColorLUT = SNPPUColorGetPalette();
	memset(m_uPaletteDirty, 0, sizeof(m_uPaletteDirty));
	m_nPaletteDirty = 0;
	MarkPaletteAllDirty();
    m_bAttribPalettesUploaded = FALSE;
	m_uDmaSlot = 0;
	m_uLastDmaSlot = 0;

	/* AURORA_SNES_RENDERER_PERF_V8_GS_PINGPONG */
	for (Uint32 uSlot = 0; uSlot < SNPPU_DMA_BLENDINFO_SLOTS; ++uSlot)
	{
		SNPPUDmaListT *pList = &m_DmaList[uSlot];
		SNPPUDmaListT *pPaletteList = &m_DmaListWithPalette[uSlot];
		m_pDmaBlendInfo[uSlot] = NULL;
		m_bDmaListHasIntensity[uSlot] = FALSE;
		m_bDmaListDirectMain[uSlot] = FALSE;
		pList->uPalAddr = uVramAddr + 0x000;
		pList->uInputAddr = uVramAddr + 0x080;
		pList->uAttribMainPal = uVramAddr + 0x180;
		pList->uAttribSubPal = uVramAddr + 0x184;
		pList->uTempAddr = uVramAddr + 0x200;
		pList->uOutAddr = uOutAddr;
		pPaletteList->uPalAddr = pList->uPalAddr;
		pPaletteList->uInputAddr = pList->uInputAddr;
		pPaletteList->uAttribMainPal = pList->uAttribMainPal;
		pPaletteList->uAttribSubPal = pList->uAttribSubPal;
		pPaletteList->uTempAddr = pList->uTempAddr;
		pPaletteList->uOutAddr = pList->uOutAddr;
	}
#if SNDBG_LOG
	DLog("[snes-gs-layout] vram blend/out=%X/%X scratch render/stage=%08X/%08X bytes=%u/%u",
		(unsigned)uVramAddr, (unsigned)uOutAddr,
		(unsigned)PS2MEM_SCRATCHPAD, (unsigned)SNPPU_DMA_BLENDINFO_ADDR,
		(unsigned)sizeof(SnesRender8pInfoT), (unsigned)sizeof(SNPPUBlendInfoT));
#endif
}

void SNPPUBlendGS::Exec(SNPPUBlendInfoT *pInfo, Int32 iLine, Uint32 uFixedColor32, SNMaskT *pColorMask, Bool bAddSub, Uint32 uIntensity)
{
	/* AURORA_SNES_RENDERER_PERF_V8_GS_PINGPONG
	 * Release: prepare alternate slot -> mandatory DmaSyncGIF -> kick.
	 * GIF/GS ordering is unchanged; only host preparation moves before wait. */
	const Uint32 uSlot = m_uDmaSlot & 1u;
	SNPPUBlendInfoT *pDmaInfo =
		(SNPPUBlendInfoT *)SNPPU_DMA_BLENDINFO_SLOT_ADDR(uSlot);
	SNPPUDmaListT *pExecList;
	Bool bUploadPalette;
	Bool bApplyIntensity = uIntensity < 15;
	Bool bDirectMain = pColorMask == NULL && !bApplyIntensity;
	Uint32 uPaletteCopyBytes;

	if (!m_pTarget) return;
    if (pColorMask)
    {
        PROF_ENTER("SNPPUBlendPlanarTo3");
        _PlanarTo3(pInfo->uAttrib8, &pColorMask[0], &pColorMask[1], &pColorMask[2]);
        PROF_LEAVE("SNPPUBlendPlanarTo3");
    }

#if SNDBG_LOG
    PROF_ENTER("SNPPUGS");
	{
		Uint32 uStart = ProfCtrGetCycle();
		DmaSyncGIF();
		_SNPPUGSDiag.SyncCycles += ProfCtrGetCycle() - uStart;
		_SNPPUGSDiag.SyncCalls++;
		#if SNDBG_DEEP
		_SNPPUGSValidateStage((const SNPPUBlendInfoT *)
			SNPPU_DMA_BLENDINFO_SLOT_ADDR(m_uLastDmaSlot));
		#endif
	}
    PROF_LEAVE("SNPPUGS");
#endif

    if (m_pDmaBlendInfo[uSlot] != pInfo ||
        m_bDmaListHasIntensity[uSlot] != bApplyIntensity ||
		m_bDmaListDirectMain[uSlot] != bDirectMain)
    {
		_SNPPUBlendBuildList(&m_DmaList[uSlot], pDmaInfo,
			m_DmaList[uSlot].uOutAddr, FALSE, bApplyIntensity, bDirectMain);
		_SNPPUBlendBuildList(&m_DmaListWithPalette[uSlot], pDmaInfo,
			m_DmaListWithPalette[uSlot].uOutAddr, TRUE, bApplyIntensity, bDirectMain);
        SyncDCache(m_DmaList[uSlot].Data,
			(Uint8 *)m_DmaList[uSlot].Data + sizeof(m_DmaList[uSlot].Data) - 1);
        SyncDCache(m_DmaListWithPalette[uSlot].Data,
			(Uint8 *)m_DmaListWithPalette[uSlot].Data +
				sizeof(m_DmaListWithPalette[uSlot].Data) - 1);
        m_pDmaBlendInfo[uSlot] = pInfo;
        m_bDmaListHasIntensity[uSlot] = bApplyIntensity;
		m_bDmaListDirectMain[uSlot] = bDirectMain;
    }

	bUploadPalette = m_bPaletteDirty;
#if SNDBG_LOG
	{
		Uint32 uStart = ProfCtrGetCycle();
		#if SNDBG_DEEP
		Uint32 uSourceHash, uStageHash;
		#endif
		uPaletteCopyBytes = CopyDirtyPalette(pDmaInfo->Pal, pInfo->Pal, uSlot);
		memcpy(pDmaInfo->uMain8, pInfo->uMain8, sizeof(pDmaInfo->uMain8));
		if (!bDirectMain)
		{
			memcpy(pDmaInfo->uSub8, pInfo->uSub8, sizeof(pDmaInfo->uSub8));
			memcpy(pDmaInfo->uAttrib8, pInfo->uAttrib8, sizeof(pDmaInfo->uAttrib8));
		}
#if SNDBG_DEEP
		else
		{
			memcpy(pDmaInfo->uSub8, pInfo->uSub8, sizeof(pDmaInfo->uSub8));
			memcpy(pDmaInfo->uAttrib8, pInfo->uAttrib8, sizeof(pDmaInfo->uAttrib8));
		}
#endif
		_SNPPUGSDiag.CopyCycles += ProfCtrGetCycle() - uStart;
		_SNPPUGSDiag.CopyBytes += sizeof(pDmaInfo->uMain8);
		if (!bDirectMain) _SNPPUGSDiag.CopyBytes += sizeof(pDmaInfo->uSub8)+sizeof(pDmaInfo->uAttrib8);
#if SNDBG_DEEP
		else _SNPPUGSDiag.CopyBytes += sizeof(pDmaInfo->uSub8)+sizeof(pDmaInfo->uAttrib8);
#endif
		if (bUploadPalette) { _SNPPUGSDiag.CopyBytes += uPaletteCopyBytes; _SNPPUGSDiag.PaletteUploads++; }
		#if SNDBG_DEEP
		uSourceHash = _SNPPUGSSample(pInfo, NULL);
		uStageHash = _SNPPUGSSample(pDmaInfo, _SNPPUGSDiag.Expected);
		if (uSourceHash != uStageHash) _SNPPUGSDiag.CopyMismatch++;
		_SNPPUGSDiag.SourceHash = (_SNPPUGSDiag.SourceHash << 5) ^ uSourceHash ^ (Uint32)iLine;
		_SNPPUGSDiag.StageHash = (_SNPPUGSDiag.StageHash << 5) ^ uStageHash ^ (Uint32)iLine;
		_SNPPUGSDiag.HasExpected = TRUE;
		#endif
	}
#else
	uPaletteCopyBytes = CopyDirtyPalette(pDmaInfo->Pal, pInfo->Pal, uSlot);
	(void)uPaletteCopyBytes;
	if (!bDirectMain)
		memcpy(pDmaInfo->uMain8, pInfo->uMain8,
			sizeof(pDmaInfo->uMain8)+sizeof(pDmaInfo->uSub8)+sizeof(pDmaInfo->uAttrib8));
	else
		memcpy(pDmaInfo->uMain8, pInfo->uMain8, sizeof(pDmaInfo->uMain8));
#endif

	/* Per-slot staging dirtiness is distinct from GS-upload dirtiness. */
	if (bUploadPalette)
	{
		memset(m_uPaletteDirty, 0, sizeof(m_uPaletteDirty));
		m_nPaletteDirty = 0;
		m_bPaletteDirty = FALSE;
	}

	pExecList = bUploadPalette ? &m_DmaListWithPalette[uSlot] : &m_DmaList[uSlot];
    PROF_ENTER("SNPPUBlendExec");
    _SNPPUBlendSetParm(pExecList, iLine, uFixedColor32, bAddSub, uIntensity, bDirectMain);
    PROF_LEAVE("SNPPUBlendExec");

#if !SNDBG_LOG
    PROF_ENTER("SNPPUGS");
    DmaSyncGIF();
    PROF_LEAVE("SNPPUGS");
#endif

#if SNDBG_LOG
	{
		Uint32 uStart = ProfCtrGetCycle();
		DmaExecGIFChain(pExecList->Data);
		_SNPPUGSDiag.KickCycles += ProfCtrGetCycle() - uStart;
		_SNPPUGSDiag.Lines++;
		if (bApplyIntensity) _SNPPUGSDiag.IntensityLines++;
		if (bDirectMain) _SNPPUGSDiag.DirectMainLines++;
	}
#else
    DmaExecGIFChain(pExecList->Data);
#endif
	m_uLastDmaSlot = uSlot;
	m_uDmaSlot = uSlot ^ 1u;
}

void SNPPUBlendGS::Clear(SNPPUBlendInfoT *pInfo, Int32 iLine)
{
    // render clear line
    Exec(pInfo, iLine, 0, NULL, 0, 0);
}








#endif

/* AURORA_SNES_RENDERER_PERF_V8_FINAL_20260922
 * V2/V3/V4/V7 audit-only; V1/V5/V6 exact host-work reductions.
 * Safe Frameskip policy and emulated timing/event order are outside this patch. */
