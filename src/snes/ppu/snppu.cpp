
#include <string.h>
#include <stdio.h>
#include "types.h"
#include "console.h"
#include "snppu.h"
#include "prof.h"
#include "sntiming.h"
#include "sndebug.h"
#include "sndbglog.h"

#define SNPPU_VERSION_5C77 (0x01)
#define SNPPU_VERSION_5C78 (0x01)

void SnesPPU::WriteCGDATA(Uint8 uData)
{
#if SNDBG_LOG
	g_DbgCGRAMWrites++;
#endif
	Uint32 uCGAddr;

	uCGAddr = (m_Regs.cgadd.w >> 1) & (SNESPPU_CGRAM_NUM - 1);

	/* AURORA_ACCURACY_CGRAM_LATCH_V1
	 * $2122 is a latched 16-bit write port. The first byte is internal only;
	 * the CGRAM word changes when the second byte arrives. CGRAM is 15-bit,
	 * so high-byte bit 7 is discarded by the PPU. */
	if (!(m_Regs.cgadd.w & 1))
	{
		m_CGRAMLatch = uData;
	}
	else
	{
		Uint16 uColor = (Uint16)m_CGRAMLatch |
		                ((Uint16)(uData & 0x7F) << 8);
		Bool bChanged = FALSE;

		if (IsCGRAMAccessAllowed())
		{
			bChanged = m_CGRAM[uCGAddr] != uColor;
			m_CGRAM[uCGAddr] = uColor;
		}

		/* AURORA_REDUNDANT_CGRAM_ELIDE_V2
		 * The write and CGADD advance remain identical. Only host-side
		 * palette conversion/upload is skipped when the color is unchanged. */
		if (bChanged)
			m_pRender->UpdateCGRAM(uCGAddr, uColor);
	}

	/* Aurora stores CGADD as a byte-phase address. Advancing once per port
	 * access therefore reproduces the hardware's flip-flop and word increment. */
	m_Regs.cgadd.w++;
}

Uint8 SnesPPU::ReadCGDATA()
{
	Uint32 uCGAddr;
	Uint8 uData;

	uCGAddr = m_Regs.cgadd.w >> 1;
	uCGAddr&= SNESPPU_CGRAM_NUM-1;
	if (!IsCGRAMAccessAllowed())
	{
		/* Hardware exposes the DAC's dot-selected CGRAM address here. The
		 * scanline renderer cannot know that address yet, so retain PPU2 MDR
		 * rather than incorrectly reading the software-selected CGADD entry. */
		uData = m_PPU2MDR;
	}
	else if (!(m_Regs.cgadd.w&1))
	{
		// lower byte
		uData =  m_CGRAM[uCGAddr] & 0xFF;
	} else
	{
		// upper byte
		uData =  (m_CGRAM[uCGAddr] >> 8);
	}

	// increment color address
	m_Regs.cgadd.w++;

	return uData;
}


static Uint32 _SwizzleVramAddr(Uint32 uVramAddr, Uint32 uFullGraphic)
{
	switch (uFullGraphic)
	{
	case 0:
	default:
		//	0: 0aaaaaaaaaaaaaaa -> 0aaaaaaaaaaaaaaa
		return uVramAddr & 0x7FFF;

	case 1:
		// 32:  0aaaaaaabbbccccc -> 0aaaaaaacccccbbb
		return (uVramAddr & 0x7F00) | ((uVramAddr &0x1F)<<3) | ((uVramAddr>>5) & 0x07);

	case 2:
		//	64:  0aaaaaabbbcccccc -> 0aaaaaaccccccbbb
		return (uVramAddr & 0x7E00) | ((uVramAddr &0x3F)<<3) | ((uVramAddr>>6) & 0x07);

	case 3:
		//	128: 0aaaaabbbccccccc -> 0aaaaacccccccbbb
		return (uVramAddr & 0x7C00) | ((uVramAddr &0x7F)<<3) | ((uVramAddr>>7) & 0x07);
	}
}


/* AURORA_PPU_MEMORY_V3_PPU_20260915
 * Memory-port arbitration follows the high-confidence hardware windows used
 * by ares/bsnes.  Exact dot-selected OAM/CGRAM contention addresses are a
 * separate dot-raster concern; V3 keeps requested-address corruption out of
 * the emulated memories when that address cannot be known. */
Uint8 SnesPPU::BuildMemoryAccessFlags(Uint32 uLine, Uint32 uHClock) const
{
	Uint8 uFlags = 0;
	if (uLine <= m_uFrameVisibleLines)
		uFlags |= SNESPPU_MEMBUS_VRAM_OAM_BUSY;
	if (uLine > 0 && uLine <= m_uFrameVisibleLines &&
	    uHClock >= SNESPPU_CGRAM_ACTIVE_H_BEGIN &&
	    uHClock < SNESPPU_CGRAM_ACTIVE_H_END)
		uFlags |= SNESPPU_MEMBUS_CGRAM_BUSY;
	return uFlags;
}

Bool SnesPPU::IsVRAMAccessAllowed() const
{
	return IsForceBlank() || !(m_uMemoryAccessFlags & SNESPPU_MEMBUS_VRAM_OAM_BUSY);
}

Bool SnesPPU::IsOAMAccessAllowed() const
{
	return IsForceBlank() || !(m_uMemoryAccessFlags & SNESPPU_MEMBUS_VRAM_OAM_BUSY);
}

Bool SnesPPU::IsCGRAMAccessAllowed() const
{
	return IsForceBlank() || !(m_uMemoryAccessFlags & SNESPPU_MEMBUS_CGRAM_BUSY);
}

/* AURORA_ACCURACY_VRAM_READ_BUFFER_V1
 * The S-PPU exposes a prefetched 16-bit VRAM data latch. $2116/$2117 fill
 * this latch; $2139/$213a return the old latch, then (on the selected port)
 * fetch the current VMADD word and advance VMADD. This is data, not an address. */
void SnesPPU::UpdateVRAMReadBuffer()
{
	/* During active display VRAM is owned by the PPU. The read buffer is
	 * filled with zero; the externally visible read still returns the OLD
	 * buffer first, exactly like the normal prefetch pipeline. */
	if (!IsVRAMAccessAllowed())
	{
		m_Regs.vmreadlatch.w = 0;
		return;
	}
	Uint32 uVramAddr = _SwizzleVramAddr(
		m_Regs.vmaddr.w, (m_Regs.vmain >> 2) & 3);
	m_Regs.vmreadlatch.w = m_VRAM[uVramAddr];
}


void SnesPPU::WriteVMDATAL(Uint8 uData)
{
#if SNDBG_LOG
	g_DbgVRAMWrites++;
#endif
	SnesReg16T *pVram = (SnesReg16T *)m_VRAM;
	Uint32 uVramAddr;
	Bool bChanged;

	uVramAddr = _SwizzleVramAddr(m_Regs.vmaddr.w, (m_Regs.vmain >> 2) & 3);

	bChanged = FALSE;
	if (IsVRAMAccessAllowed())
	{
		bChanged = pVram[uVramAddr].b.l != uData;
		pVram[uVramAddr].b.l = uData;
	}

	/* Address increment is outside the VRAM write itself on hardware. */
	m_Regs.vmaddr.w += m_Regs.vminc[0];

	/* AURORA_REDUNDANT_VRAM_ELIDE_V2 */
	if (bChanged)
		m_pRender->UpdateVRAM(uVramAddr);
}

void SnesPPU::WriteVMDATAH(Uint8 uData)
{
#if SNDBG_LOG
	g_DbgVRAMWrites++;
#endif
	SnesReg16T *pVram = (SnesReg16T *)m_VRAM;
	Uint32 uVramAddr;
	Bool bChanged;

	uVramAddr = _SwizzleVramAddr(m_Regs.vmaddr.w, (m_Regs.vmain >> 2) & 3);

	bChanged = FALSE;
	if (IsVRAMAccessAllowed())
	{
		bChanged = pVram[uVramAddr].b.h != uData;
		pVram[uVramAddr].b.h = uData;
	}

	m_Regs.vmaddr.w += m_Regs.vminc[1];

	if (bChanged)
		m_pRender->UpdateVRAM(uVramAddr);
}

void SnesPPU::WriteVMDATALH(Uint8 uDataL, Uint8 uDataH)
{
	/* Preserve both port increments on an invalid active-display transfer.
	 * The slow fallback is cold by definition and avoids duplicating VMAIN
	 * increment-mode corner cases. */
	if (!IsVRAMAccessAllowed())
	{
		WriteVMDATAL(uDataL);
		WriteVMDATAH(uDataH);
		return;
	}
#if SNDBG_LOG
	g_DbgVRAMWrites += 2;
#endif
	SnesReg16T *pVram = (SnesReg16T *)m_VRAM;
	Uint32 uVramAddr, uFirstVramAddr;
	Bool bFirstChanged, bSecondChanged;

	uVramAddr = _SwizzleVramAddr(m_Regs.vmaddr.w, (m_Regs.vmain >> 2) & 3);
	uFirstVramAddr = uVramAddr;

	bFirstChanged = pVram[uVramAddr].b.l != uDataL;
	pVram[uVramAddr].b.l = uDataL;

	if (m_Regs.vminc[0])
	{
		m_Regs.vmaddr.w += m_Regs.vminc[0];
		uVramAddr = _SwizzleVramAddr(m_Regs.vmaddr.w, (m_Regs.vmain >> 2) & 3);
	}

	bSecondChanged = pVram[uVramAddr].b.h != uDataH;
	pVram[uVramAddr].b.h = uDataH;

	m_Regs.vmaddr.w += m_Regs.vminc[1];

	if (uVramAddr == uFirstVramAddr)
	{
		if (bFirstChanged || bSecondChanged)
			m_pRender->UpdateVRAM(uFirstVramAddr);
	}
	else
	{
		if (bFirstChanged)
			m_pRender->UpdateVRAM(uFirstVramAddr);
		if (bSecondChanged)
			m_pRender->UpdateVRAM(uVramAddr);
	}
}

void SnesPPU::WriteVMDATABlock(const Uint8 *pData, Int32 nBytes)
{
	if (!IsVRAMAccessAllowed())
	{
		/* Invalid active-display MDMA still clocks the two data ports and
		 * therefore advances VMADD according to VMAIN. */
		while (nBytes >= 2)
		{
			WriteVMDATAL(pData[0]);
			WriteVMDATAH(pData[1]);
			pData += 2;
			nBytes -= 2;
		}
		if (nBytes) WriteVMDATAL(*pData);
		return;
	}

	/* DMA mode 1 to $2118/$2119 is by far the most common path for tile
	   uploads. With normal address mapping and increment-after-high, each
	   byte pair is one consecutive VRAM word. Copy those words here and
	   invalidate the renderer once for the whole burst instead of making
	   thousands of calls through WriteVMDATALH(). */
	if (nBytes >= 2 &&
	    ((m_Regs.vmain >> 2) & 3) == 0 &&
	    m_Regs.vminc[0] == 0 && m_Regs.vminc[1] == 1)
	{
		Int32 nWords = nBytes >> 1;
		Int32 nWordsLeft = nWords;
		Uint32 uFirstPhysical = m_Regs.vmaddr.w & 0x7FFF;

		while (nWordsLeft > 0)
		{
			Uint32 uPhysical = m_Regs.vmaddr.w & 0x7FFF;
			Int32 nChunk = 0x8000 - (Int32)uPhysical;
			if (nChunk > nWordsLeft)
				nChunk = nWordsLeft;

			/* AURORA_MEGA_V4_VRAM_DMA_PORTABLE
			 * The PS2 EE is little-endian, matching the guarded mode-1
			 * $2118/$2119 L,H stream exactly.  Keep the old explicit assembly
			 * for any non-PS2 build so this optimisation cannot silently make
			 * the shared source endian-dependent. */
#if CODE_PLATFORM == CODE_PS2
			memcpy(&m_VRAM[uPhysical], pData, nChunk * sizeof(Uint16));
			pData += nChunk * 2;
#else
			{
				Int32 iWord;
				for (iWord = 0; iWord < nChunk; iWord++)
				{
					m_VRAM[uPhysical + iWord] =
						(Uint16)pData[0] | ((Uint16)pData[1] << 8);
					pData += 2;
				}
			}
#endif

			m_Regs.vmaddr.w = (Uint16)(m_Regs.vmaddr.w + nChunk);
			nWordsLeft -= nChunk;
		}

#if SNDBG_LOG
		g_DbgVRAMWrites += nWords * 2;
#endif
		m_pRender->UpdateVRAMRange(uFirstPhysical, (Uint32)nWords);

		nBytes -= nWords * 2;
	}
	else
	{
		while (nBytes >= 2)
		{
			WriteVMDATALH(pData[0], pData[1]);
			pData += 2;
			nBytes -= 2;
		}
	}

	/* An odd DMA length ends on $2118, so preserve the low-port increment
	   behaviour for its final byte. */
	if (nBytes > 0)
		WriteVMDATAL(*pData);
}



Uint8 SnesPPU::ReadVMDATAL()
{
	Uint8 uData = m_Regs.vmreadlatch.b.l;

	/* Return the already-prefetched byte first. If VMAIN selects the low
	 * port for increment, the next buffer is fetched from the current VMADD
	 * before VMADD advances. */
	if (m_Regs.vminc[0])
	{
		UpdateVRAMReadBuffer();
		m_Regs.vmaddr.w += m_Regs.vminc[0];
	}

	return uData;
}

Uint8 SnesPPU::ReadVMDATAH()
{
	Uint8 uData = m_Regs.vmreadlatch.b.h;

	if (m_Regs.vminc[1])
	{
		UpdateVRAMReadBuffer();
		m_Regs.vmaddr.w += m_Regs.vminc[1];
	}

	return uData;
}




static Uint32 _MapOAMAddress(Uint32 uAddress)
{
	uAddress &= 0x3FF;
	return (uAddress < 0x200) ? uAddress : 0x200 | (uAddress & 0x1F);
}

#if SNDBG_DEEP
static void _TraceOAMAddressWrite(Uint32 uPort, Uint8 uData,
	Uint16 uOldAddress, Uint16 uOldBase, const SnesPPURegsT *pRegs)
{
	static Uint32 s_uFrame = (Uint32)-1;
	static Uint32 s_uWrites = 0;

	if (!g_DbgCaptureActive)
		return;

	if (s_uFrame != g_DbgCaptureFrameNo)
	{
		s_uFrame = g_DbgCaptureFrameNo;
		s_uWrites = 0;
	}

	/* Jogos normalmente escrevem o par uma vez por frame. O limite evita
	   transformar um caso patologico em milhares de linhas de diagnostico. */
	if (s_uWrites < 12)
	{
		DLog("[snes-oam-reg] f=%u port=%04X data=%02X addr=%04X>%04X base=%04X>%04X first=%u",
			(unsigned)g_DbgCaptureFrameNo, (unsigned)uPort,
			(unsigned)uData, (unsigned)uOldAddress,
			(unsigned)pRegs->oamaddr.w, (unsigned)uOldBase,
			(unsigned)pRegs->oamaddrlatch.w,
			(unsigned)pRegs->oampri.w);
	}
	s_uWrites++;
}
#endif

void SnesPPU::UpdateOAMPriority()
{
	Uint16 uOldPriority = m_Regs.oampri.w;
	Bool bPriorityRotation;

	m_Regs.oampri.w = (m_Regs.oamaddr.w & 0x8000)
		? ((m_Regs.oamaddr.w & 0x1FF) >> 2) : 0;
	bPriorityRotation = (m_Regs.oamaddr.w & 0x8000) != 0;

	/* AURORA_ACCURACY_OAM_FIRSTSPRITE_Y_V1_DIRTY_20260825
	 * With priority rotation enabled the low two bits of Aurora's byte-phase
	 * OAM address are semantically relevant too: phase 3 selects the SNES
	 * FirstSprite+scanline evaluation mode. That phase can change without
	 * changing oampri itself, so keep the derived OBJ visibility cache dirty
	 * whenever priority rotation is active. WriteOAMBlock still publishes
	 * a whole DMA burst only once, so the common OAM-DMA path stays cheap. */
	if ((m_Regs.oampri.w != uOldPriority || bPriorityRotation) && m_pRender)
		m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_OBJ);
}

void SnesPPU::WriteOAMDATA(Uint8 uData)
{
#if SNDBG_LOG
	g_DbgOAMWrites++;
#endif
	Uint8	*pOamData = (Uint8 *)&m_OAM;
	Uint32 uAddress = m_Regs.oamaddr.w & 0x3FF;
	Bool bChanged = FALSE;

	/* Low OAM is a 16-bit write port: the even byte is latched and the pair
	   is committed only by the odd byte. High OAM writes immediately and is
	   mirrored every 32 bytes throughout the logical $200-$3ff range. */
	if (!(uAddress & 1))
		m_OAMLatch = uData;

	if (!IsOAMAccessAllowed())
	{
		/* Accurate hardware redirects to the current OBJ-fetch latch. Until
		 * V6 has dot timing, use the same $0218 contention approximation as
		 * the mature ares performance PPU. Low-OAM even writes remain latch-only. */
		if ((uAddress & 0x200) || (uAddress & 1))
		{
			Uint32 uPhysical = 0x218;
			bChanged = pOamData[uPhysical] != uData;
			pOamData[uPhysical] = uData;
		}
	}
	else if (uAddress & 0x200)
	{
		Uint32 uPhysical = _MapOAMAddress(uAddress);
		bChanged = pOamData[uPhysical] != uData;
		pOamData[uPhysical] = uData;
	}
	else if (uAddress & 1)
	{
		Uint32 uEven = uAddress & ~1;
		bChanged = pOamData[uEven] != m_OAMLatch ||
		           pOamData[uAddress] != uData;
		pOamData[uEven] = m_OAMLatch;
		pOamData[uAddress] = uData;
	}

	m_Regs.oamaddr.w = (m_Regs.oamaddr.w & 0x8000) |
	                     ((uAddress + 1) & 0x3FF);
	UpdateOAMPriority();

	if (bChanged)
		m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_OBJ);
}

void SnesPPU::WriteOAMBlock(const Uint8 *pData, Int32 nBytes)
{
	if (!IsOAMAccessAllowed())
	{
		/* Cold invalid-transfer path; preserve every latch/address side effect. */
		while (nBytes-- > 0) WriteOAMDATA(*pData++);
		return;
	}
	Uint8 *pOamData = (Uint8 *)&m_OAM;
	Uint32 uAddress = m_Regs.oamaddr.w & 0x3FF;
	Bool bChanged = FALSE;

#if SNDBG_LOG
	g_DbgOAMWrites += nBytes;
#endif

	/* OAM is normally refreshed by one 544-byte DMA every frame. Preserve
	   the low-table latch and high-table mirroring exactly, but publish the
	   final address/priority and renderer invalidation only once. */
	while (nBytes-- > 0)
	{
		Uint8 uData = *pData++;

		if (!(uAddress & 1))
			m_OAMLatch = uData;

		if (uAddress & 0x200)
		{
			Uint32 uPhysical = _MapOAMAddress(uAddress);
			if (pOamData[uPhysical] != uData)
				bChanged = TRUE;
			pOamData[uPhysical] = uData;
		}
		else if (uAddress & 1)
		{
			Uint32 uEven = uAddress & ~1;
			if (pOamData[uEven] != m_OAMLatch ||
			    pOamData[uAddress] != uData)
				bChanged = TRUE;
			pOamData[uEven] = m_OAMLatch;
			pOamData[uAddress] = uData;
		}

		uAddress = (uAddress + 1) & 0x3FF;
	}

	m_Regs.oamaddr.w = (m_Regs.oamaddr.w & 0x8000) | uAddress;
	UpdateOAMPriority();
	if (bChanged)
		m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_OBJ);
}

Uint8 SnesPPU::ReadOAMDATA()
{
	Uint8	*pOamData = (Uint8 *)&m_OAM;
	Uint32 uAddress = m_Regs.oamaddr.w & 0x3FF;
	Uint32 uPhysical = IsOAMAccessAllowed()
		? _MapOAMAddress(uAddress) : 0x218;
	Uint8	uData = pOamData[uPhysical];

	m_Regs.oamaddr.w = (m_Regs.oamaddr.w & 0x8000) |
	                     ((uAddress + 1) & 0x3FF);
	UpdateOAMPriority();

	return uData;
}


void SnesPPU::UpdateMatMul()
{
	Int32 iMulA, iMulB;
	Int32 iProduct;

	iMulA = (Int16)m_Regs.m7a.w;
	iMulB = (Int16)m_Regs.m7b.w;

	iProduct = iMulA * (iMulB >> 8);

	m_Regs.mpyl = (Uint8)(iProduct  >> 0);
	m_Regs.mpym = (Uint8)(iProduct  >> 8);
	m_Regs.mpyh = (Uint8)(iProduct  >> 16);
}






void SnesPPU::Write8(Uint32 uAddr, Uint8 uData)
{
	//if (uAddr!= 0x2118 && uAddr!= 0x2119 && uAddr!= 0x2122  && uAddr!= 0x2104)
	//ConDebug("write8[%06X]:ppu.%s=%02X\n", uAddr, GetRegName(uAddr), uData);

	switch (uAddr)
	{
	case 0x2100:	// inidisp (screen display)
		m_Regs.inidisp = uData;
		break;

	case 0x2101:	// obsel (oam size)
		if (m_Regs.obsel != uData)
		{
			m_Regs.obsel = uData;
			m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_OBJ);
#if SNDBG_LOG
			// DLog("[snes-obj] OBSEL=%02X base=%d nameSel=%d baseSize=%d",
			// 	(int)uData, (int)(uData & 7), (int)((uData >> 3) & 3), (int)((uData >> 5) & 7));
#endif
		}
		break;

	case 0x2102:	// oamaddl (oam address low)
	{
	#if SNDBG_DEEP
		Uint16 uOldAddress = m_Regs.oamaddr.w;
		Uint16 uOldBase = m_Regs.oamaddrlatch.w;
#endif

		/* $2102/$2103 formam um endereco BASE separado do endereco interno
		   que $2104/$2138 incrementam. Escrever qualquer metade recarrega o
		   endereco interno a partir das duas ultimas metades gravadas. Usar
		   o endereco ja incrementado aqui desloca a OAM quando um jogo muda
		   apenas uma metade; esse e' o caso historico do Final Fight 2. */
		m_Regs.oamaddrlatch.w =
			(m_Regs.oamaddrlatch.w & 0x8200) | ((Uint16)uData << 1);
		m_Regs.oamaddr.w = m_Regs.oamaddrlatch.w;
		m_OAMLatch = 0;
		UpdateOAMPriority();
	#if SNDBG_DEEP
		_TraceOAMAddressWrite(uAddr, uData, uOldAddress, uOldBase, &m_Regs);
#endif
		break;
	}
	case 0x2103:	// oamaddh (oam address high)
	{
	#if SNDBG_DEEP
		Uint16 uOldAddress = m_Regs.oamaddr.w;
		Uint16 uOldBase = m_Regs.oamaddrlatch.w;
#endif

		m_Regs.oamaddrlatch.w =
			(m_Regs.oamaddrlatch.w & 0x01FE) |
			((uData & 0x01) << 9) | ((uData & 0x80) << 8);
		m_Regs.oamaddr.w = m_Regs.oamaddrlatch.w;
		m_OAMLatch = 0;
		UpdateOAMPriority();
	#if SNDBG_DEEP
		_TraceOAMAddressWrite(uAddr, uData, uOldAddress, uOldBase, &m_Regs);
#endif
		break;
	}

	case 0x2104:	// oamdata (oam data)
		WriteOAMDATA(uData);
		break;

	case 0x2105:	// bgmode (screen mode)
        if (m_Regs.bgmode!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGSCR | SNESPPURENDER_UPDATE_BGCHR);
		m_Regs.bgmode = uData;
		break;

	case 0x2106:	// mosaic (screen pixelation)
		/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
		if (m_Regs.mosaic != uData)
		{
			m_Regs.mosaic = uData;
			m_uMosaicStartLine =
				(m_uLine > 0 && m_uLine <= m_uFrameVisibleLines) ? m_uLine : 1;
		}
		break;

	case 0x2107:	// bg1sc (BG1 vram location)
        if (m_Regs.bg1sc!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGSCR);
		m_Regs.bg1sc = uData;
		break;
	case 0x2108:	// bg2sc (BG2 vram location)
        if (m_Regs.bg2sc!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGSCR);
		m_Regs.bg2sc = uData;
		break;
	case 0x2109:	// bg3sc (BG3 vram location)
        if (m_Regs.bg3sc!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGSCR);
		m_Regs.bg3sc = uData;
		break;
	case 0x210A:	// bg4sc (BG4 vram location)
        if (m_Regs.bg4sc!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGSCR);
		m_Regs.bg4sc = uData;
		break;
	case 0x210B:	// bg12nba (BG1 & BG2 vram location)
        if (m_Regs.bg12nba !=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGCHR);
		m_Regs.bg12nba = uData;
		break;
	case 0x210C:	// bg34nba (BG3 & BG4 vram location)
        if (m_Regs.bg34nba !=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_BGCHR);
		m_Regs.bg34nba = uData;
		break;

	case 0x210D:	// m7hofs + bg1hofs
		/* Mode 7 has its own 13-bit scroll and shares one byte latch with
		   every Mode 7 matrix/centre register. */
		m_Regs.m7hofs.w = ((uData << 8) | m_Regs.m7latch) & 0x1FFF;
		m_Regs.m7latch = uData;
		/* Horizontal BG scroll combines bits 3-7 from the common H/V latch
		   with bits 0-2 from the previous horizontal write. */
		m_Regs.bg1hofs.w = ((uData << 8) |
			(m_Regs.bgofslo & 0xF8) | (m_Regs.bghofslo & 0x07)) & 0x03FF;
		m_Regs.bgofslo = uData;
		m_Regs.bghofslo = uData;
		break;
	case 0x210E:	// m7vofs + bg1vofs
		m_Regs.m7vofs.w = ((uData << 8) | m_Regs.m7latch) & 0x1FFF;
		m_Regs.m7latch = uData;
		m_Regs.bg1vofs.w = ((uData << 8) | m_Regs.bgofslo) & 0x03FF;
		m_Regs.bgofslo = uData;
		break;
	case 0x210F:	// bg2hofs 
		m_Regs.bg2hofs.w = ((uData << 8) |
			(m_Regs.bgofslo & 0xF8) | (m_Regs.bghofslo & 0x07)) & 0x03FF;
		m_Regs.bgofslo = uData;
		m_Regs.bghofslo = uData;
		break;
	case 0x2110:	// bg2vofs 
		m_Regs.bg2vofs.w = ((uData << 8) | m_Regs.bgofslo) & 0x03FF;
		m_Regs.bgofslo = uData;
		break;
	case 0x2111:	// bg3hofs 
		m_Regs.bg3hofs.w = ((uData << 8) |
			(m_Regs.bgofslo & 0xF8) | (m_Regs.bghofslo & 0x07)) & 0x03FF;
		m_Regs.bgofslo = uData;
		m_Regs.bghofslo = uData;
		break;
	case 0x2112:	// bg3vofs 
		m_Regs.bg3vofs.w = ((uData << 8) | m_Regs.bgofslo) & 0x03FF;
		m_Regs.bgofslo = uData;
		break;
	case 0x2113:	// bg4hofs 
		m_Regs.bg4hofs.w = ((uData << 8) |
			(m_Regs.bgofslo & 0xF8) | (m_Regs.bghofslo & 0x07)) & 0x03FF;
		m_Regs.bgofslo = uData;
		m_Regs.bghofslo = uData;
		break;
	case 0x2114:	// bg4vofs 
		m_Regs.bg4vofs.w = ((uData << 8) | m_Regs.bgofslo) & 0x03FF;
		m_Regs.bgofslo = uData;
		break;

	case 0x2115:	// vmain (video port control)
		{
			static Uint8 _SNPPU_VramInc[4]={1,32,128,128};
			//ConDebug("write8[%06X]:ppu.%s=%02X\n", uAddr, GetRegName(uAddr), uData);
			m_Regs.vmain = uData;
			if (uData & 0x80)
			{	// auto-inc on 2119
				m_Regs.vminc[0] = 0;
				m_Regs.vminc[1] = _SNPPU_VramInc[uData & 3];
			} else
			{
				m_Regs.vminc[0] = _SNPPU_VramInc[uData & 3];
				m_Regs.vminc[1] = 0;
			}
		}
		break;

	case 0x2116:	// vmaddl (video port address low)
		m_Regs.vmaddr.b.l = uData;
		UpdateVRAMReadBuffer();
		break;
	case 0x2117:	// vmaddh (video port address hi)
		m_Regs.vmaddr.b.h = uData;
		UpdateVRAMReadBuffer();
		break;

	case 0x2118:	// vmdatal (video port data low)
		WriteVMDATAL(uData);
		break;
	case 0x2119:	// vmdatah (video port data hi)
		WriteVMDATAH(uData);
		break;

	case 0x211A:	// m7sel (mode 7 setting)
		m_Regs.m7sel = uData;
		break;

	case 0x211B:	// m7a
		m_Regs.m7a.w = (uData << 8) | m_Regs.m7latch;
		m_Regs.m7latch = uData;
		UpdateMatMul();
		break;
	case 0x211C:	// m7b
		m_Regs.m7b.w = (uData << 8) | m_Regs.m7latch;
		m_Regs.m7latch = uData;
		UpdateMatMul();
		break;
	case 0x211D:	// m7c
		m_Regs.m7c.w = (uData << 8) | m_Regs.m7latch;
		m_Regs.m7latch = uData;
		break;
	case 0x211E:	// m7d
		m_Regs.m7d.w = (uData << 8) | m_Regs.m7latch;
		m_Regs.m7latch = uData;
		break;
	case 0x211F:	// m7x
		m_Regs.m7x.w = (uData << 8) | m_Regs.m7latch;
		m_Regs.m7latch = uData;
		break;
	case 0x2120:	// m7y
		m_Regs.m7y.w = (uData << 8) | m_Regs.m7latch;
		m_Regs.m7latch = uData;
		break;

	case 0x2121:	// cgadd (color address)
		m_Regs.cgadd.w = uData << 1;
		break;
	case 0x2122:	// cgdata (color data)
		WriteCGDATA(uData);
		break;

	case 0x2123:	// window registers
        if (m_Regs.w12sel!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.w12sel = uData;
		break;
	case 0x2124:	
        if (m_Regs.w34sel!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.w34sel = uData;
		break;
	case 0x2125:
        if (m_Regs.wobjsel!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wobjsel = uData;
		break;
	case 0x2126:	
        if (m_Regs.wh0!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wh0 = uData;
		break;
	case 0x2127:	
        if (m_Regs.wh1!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wh1 = uData;
		break;
	case 0x2128:	
        if (m_Regs.wh2!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wh2 = uData;
		break;
	case 0x2129:	
        if (m_Regs.wh3!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wh3 = uData;
		break;
	case 0x212A:	
        if (m_Regs.wbglog!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wbglog = uData;
		break;
	case 0x212B:	
        if (m_Regs.wobjlog!=uData) 
		    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_WINDOW);
		m_Regs.wobjlog = uData;
		break;

	case 0x212C:	// TM (main screen designation)
		m_Regs.tm = uData;
		break;
	case 0x212D:	// TS (sub screen designation)
		m_Regs.ts = uData;
		break;
	case 0x212E:	// TMW (window mask main screen designation)
		m_Regs.tmw = uData;
		break;
	case 0x212F:	// TSW (window mask sub screen designation)
		m_Regs.tsw = uData;
		break;

	case 0x2130:	// CGWSEL 
		m_Regs.cgwsel = uData;
		break;
	case 0x2131:	// CGADSUB 
		m_Regs.cgadsub = uData;
		break;
	case 0x2132:	// COLDATA 
		//m_Regs.coldata = 0;
		if (uData & 0x20)
		{
			// red
			m_Regs.coldata &= ~(0x1F << 0);
			m_Regs.coldata |= (uData & 0x1F) << 0;
		}

		if (uData & 0x40)
		{
			// green
			m_Regs.coldata &= ~(0x1F << 5);
			m_Regs.coldata |= (uData & 0x1F) << 5;
		}

		if (uData & 0x80)
		{
			// blue
			m_Regs.coldata &= ~(0x1F << 10);
			m_Regs.coldata |= (uData & 0x1F) << 10;
		}
		break;

	case 0x2133:	// SETINI (screen mode)
		/* Screen interlace/overscan are frame-latched by BeginFrame().
		 * Pseudo-hires and EXTBG remain live register state. OBJ interlace is
		 * consumed by the V2 evaluator/fetcher, so a bit-1 change invalidates
		 * every derived OBJ scanline before the next rendered line. */
		if (((Uint8)m_Regs.setini ^ uData) & SNESPPU_SETINI_OBJ_INTERLACE)
			m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_OBJ);
		m_Regs.setini = uData;
		break;

	case 0x2134:
	    break;

	case 0x2135: //mpym
		break;

	case 0x2139:
		break;
	case 0x213A:
		break;

	default:
		#if SNES_DEBUG
        if (Snes_bDebugUnhandledIO)
            SnesDebugRead(uAddr);
		#endif
		break;

	}
}



/*
Uint8 SnesPPU::Read8(Uint32 uAddr)
{
	switch (uAddr)
	{
	case 0x2137: // slhv
		ConDebug("readppu_slhv\n");
		return 0;
	case 0x213c: // ophct
		return m_Regs.ophct.Read8();

	case 0x213d: // opvct
		return m_Regs.opvct.Read8();

	case 0x213e: // stat77
		return m_Regs.stat77;

	case 0x213f: // stat78
		m_Regs.ophct.Reset();
		m_Regs.opvct.Reset();
		return m_Regs.stat78;
	case 0x2134: //mpyl
		return m_Regs.mpyl;
	case 0x2135: //mpym
		return m_Regs.mpym;
	case 0x2136: //mpyh
		return m_Regs.mpyh;
	default:
		ConDebug("readppu[%06X]\n", uAddr);
	}
	return 0;
}
*/


/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
Int32 SnesPPU::GetMode7MosaicSourceLine(Int32 iLine) const
{
	Int32 nSize;
	Int32 nStart;

	if (!(m_Regs.mosaic & 0x01))
		return iLine;
	nSize = (Int32)(((m_Regs.mosaic >> 4) & 0x0F) + 1);
	if (nSize <= 1)
		return iLine;
	nStart = (Int32)m_uMosaicStartLine;
	if (nStart < 1) nStart = 1;
	if (iLine < nStart) return iLine;
	return nStart + ((iLine - nStart) / nSize) * nSize;
}


void SnesPPU::SetRegionPAL(Bool bPAL)
{
    if (bPAL)
        m_Regs.stat78 |= 0x10;
    else
        m_Regs.stat78 &= ~0x10;
}

void SnesPPU::BeginFrame()
{
	m_uLine   = 0;
    m_bVBlank = FALSE;
    m_bRasterLineRendered = FALSE;

	/* AURORA_SETINI_DISPLAY_V1_PPU_20260915
	 * Hardware samples screen interlace/overscan for the frame at V=0.
	 * Do not let a mid-frame SETINI write move this frame's VBlank edge. */
	const Uint32 uPreviousVisibleLines = m_uFrameVisibleLines;
	m_uFrameVisibleLines = (m_Regs.setini & SNESPPU_SETINI_OVERSCAN)
		? SNESPPU_VISIBLE_LINES_OVERSCAN
		: SNESPPU_VISIBLE_LINES_NORMAL;
	m_bFrameInterlace = (m_Regs.setini & SNESPPU_SETINI_INTERLACE) != 0;
	m_bTimingInterlace = m_bFrameInterlace;

	/* AURORA_OBJ_STAT77_V2_PPU_20260915
	 * Range/Time Over are sticky for the current picture and restart at V=0.
	 * Preserve STAT77 version/open-bus/master bits while clearing only 6/7. */
	m_Regs.stat77 &= (Uint8)~(SNESPPU_STAT77_RANGE_OVER |
	                            SNESPPU_STAT77_TIME_OVER);

	/* When 239-line output falls back to 224, retire pixels 225..239 from
	 * the persistent GS output texture. Safe Frameskip can give us a NULL
	 * target, so keep the request pending until every tail line was really
	 * cleared on a presented frame. Overscan itself overwrites the tail. */
	if (m_uFrameVisibleLines > SNESPPU_VISIBLE_LINES_NORMAL)
	{
		m_bInactiveTailClearPending = FALSE;
	}
	else
	{
		if (uPreviousVisibleLines > m_uFrameVisibleLines)
			m_bInactiveTailClearPending = TRUE;
		if (m_bInactiveTailClearPending && m_pRender)
		{
			Bool bCleared = TRUE;
			for (Uint32 uLine = SNESPPU_VISIBLE_LINES_NORMAL + 1u;
			     uLine <= SNESPPU_VISIBLE_LINES_OVERSCAN; ++uLine)
			{
				if (!m_pRender->ClearLine((Int32)uLine))
					bCleared = FALSE;
			}
			if (bCleared)
				m_bInactiveTailClearPending = FALSE;
		}
	}

	/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
	m_uMosaicStartLine = 1;
	m_Mode7LineHofs = m_Regs.m7hofs.w;
	m_Mode7LineVofs = m_Regs.m7vofs.w;
}

void SnesPPU::EndFrame()
{
    /* AURORA_SAFE_RASTER_V4_PPU_20260915
     * This is the VBlank edge, not the end of the physical field.
     * OAM address reset belongs here; STAT78.field does not. */
    m_bVBlank = TRUE;

    // forced blanking?
    if (!(m_Regs.inidisp & 0x80))
    {
        // reset oam addr to latched value
        // RTYPE-3 needs this
        m_Regs.oamaddr.w = m_Regs.oamaddrlatch.w;
		m_OAMLatch = 0;
		UpdateOAMPriority();
    }
}

void SnesPPU::AdvanceField()
{
    /* Field toggles on V-counter wrap, after the final scanline. */
    m_Regs.stat78 ^= 0x80;
}



void SnesPPU::ApplyQueuedWritesBefore(Uint32 uRasterTime)
{
    SNQueueElementT *pElement;
    while ((pElement = m_Queue.Dequeue(uRasterTime)) != NULL)
    {
        m_uMemoryAccessFlags = pElement->uPad;
        Write8(pElement->uAddr, pElement->uData);
#if SNDBG_LOG
        g_DbgPPUAppliedWrites++;
#endif
    }
    m_uMemoryAccessFlags = 0;
}

void SnesPPU::Sync(Uint32 uLine, Uint32 uHClock)
{
    /* AURORA_DOT_RASTER_V6_PPU_20260915
     * The old queue tagged only V and therefore every write made on line N
     * became visible on N+1. V6 retains the scanline renderer but timestamps
     * writes in master clocks. A line is sampled once at H=512: writes before
     * that point are visible on the current line; writes at/after H=512 are
     * committed after rendering and therefore affect the following line.
     * Reads can catch the PPU up to the live H without rendering early. */
    if (uHClock > SNESPPU_RASTER_H_MASK)
        uHClock = SNESPPU_RASTER_H_MASK;

    if (m_bVBlank)
    {
        SNQueueElementT *pElement;
        while ((pElement = m_Queue.Dequeue()) != NULL)
        {
            m_uMemoryAccessFlags = pElement->uPad;
            Write8(pElement->uAddr, pElement->uData);
#if SNDBG_LOG
            g_DbgPPUAppliedWrites++;
#endif
        }
        m_uMemoryAccessFlags = 0;
        return;
    }

    while (m_uLine <= uLine)
    {
        Uint32 uTargetH = (m_uLine < uLine)
            ? SNESPPU_RASTER_H_FALLBACK : uHClock;

        if (!m_bRasterLineRendered)
        {
            if (uTargetH < SNESPPU_RASTER_SNAPSHOT_H)
            {
                ApplyQueuedWritesBefore(SnesPPURasterAfter(m_uLine, uTargetH));
                break;
            }

            /* Strictly-before H=512 matches synchronize-before-MMIO ordering:
             * a write timestamped exactly at the snapshot is a post-snapshot
             * change and therefore must not repaint the already sampled line. */
            ApplyQueuedWritesBefore(
                SnesPPUPackRasterTime(m_uLine, SNESPPU_RASTER_SNAPSHOT_H));

            /* Mode 7 line state must be captured AFTER pre-snapshot writes. */
            m_Mode7LineHofs = m_Regs.m7hofs.w;
            m_Mode7LineVofs = m_Regs.m7vofs.w;

            if (m_uLine > 0 && m_uLine <= m_uFrameVisibleLines)
            {
                PROF_ENTER("PPURender");
#if SNDBG_LOG
                Uint32 _tPPU = ProfCtrGetCycle();
                g_DbgPPURenderLines++;
#endif
                m_pRender->RenderLine(m_uLine);
#if SNDBG_LOG
                g_TmgCycPPU += ProfCtrGetCycle() - _tPPU;
#endif
                PROF_LEAVE("PPURender");
            }
            m_bRasterLineRendered = TRUE;
        }

        /* Commit post-snapshot state through the caller's live H. It cannot
         * change pixels already emitted for this line, but reads and the next
         * scanline observe the correct register/latch ordering. */
        ApplyQueuedWritesBefore(SnesPPURasterAfter(m_uLine, uTargetH));

        if (m_uLine == uLine)
            break;

        ++m_uLine;
        m_bRasterLineRendered = FALSE;
    }
}

void SnesPPU::Reset()
{
	m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_ALL);
	m_Queue.Reset();
	m_uLine = 0;
	m_bRasterLineRendered = FALSE;
	m_uFrameVisibleLines = SNESPPU_VISIBLE_LINES_NORMAL;
	m_bFrameInterlace = FALSE;
	m_bTimingInterlace = FALSE;
	m_bInactiveTailClearPending = FALSE;

	memset(&m_Regs, 0, sizeof(m_Regs));
	memset(&m_CGRAM, 0, sizeof(m_CGRAM));
	memset(&m_VRAM, 0, sizeof(m_VRAM));
	memset(&m_OAM, 0, sizeof(m_OAM));
	m_pRender->UpdateVRAMRange(0, SNESPPU_VRAM_NUMWORDS);
	m_OAMLatch = 0;
	m_CGRAMLatch = 0;
	/* AURORA_MEGA_V2_PPU_MDR_RESET */
	m_PPU1MDR = 0;
	m_PPU2MDR = 0;
	m_uMemoryAccessFlags = 0;
	/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
	m_Mode7LineHofs = 0;
	m_Mode7LineVofs = 0;
	m_uMosaicStartLine = 1;

	// confirmed:
	m_Regs.stat77 =  SNPPU_VERSION_5C77;
	m_Regs.stat78 =  SNPPU_VERSION_5C78; 
}

void SnesPPU::SoftReset()
{
    m_pRender->SetUpdateFlags(SNESPPURENDER_UPDATE_ALL);
    m_Queue.Reset();
    m_uLine = 0;
    m_bVBlank = FALSE;
    m_bRasterLineRendered = FALSE;
    m_uFrameVisibleLines = SNESPPU_VISIBLE_LINES_NORMAL;
    m_bFrameInterlace = FALSE;
	m_bTimingInterlace = FALSE;
    m_bInactiveTailClearPending = FALSE;

    /*
     * Reset only PPU registers/internal state.
     * VRAM, CGRAM and OAM must survive a soft reset.
     */
    memset(&m_Regs, 0, sizeof(m_Regs));
    m_OAMLatch = 0;
    m_CGRAMLatch = 0;
	/* AURORA_MEGA_V2_PPU_MDR_RESET */
	m_PPU1MDR = 0;
	m_PPU2MDR = 0;
	m_uMemoryAccessFlags = 0;
	/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
	m_Mode7LineHofs = 0;
	m_Mode7LineVofs = 0;
	m_uMosaicStartLine = 1;

    m_Regs.stat77 = SNPPU_VERSION_5C77;
    m_Regs.stat78 = SNPPU_VERSION_5C78;
}

SnesPPU::SnesPPU()
{
	m_pRender = NULL;
	m_bRasterLineRendered = FALSE;
	m_uFrameVisibleLines = SNESPPU_VISIBLE_LINES_NORMAL;
	m_bFrameInterlace = FALSE;
	m_bTimingInterlace = FALSE;
	m_bInactiveTailClearPending = FALSE;
	m_OAMLatch = 0;
	m_CGRAMLatch = 0;
	m_PPU1MDR = 0;
	m_PPU2MDR = 0;
	m_uMemoryAccessFlags = 0;
	/* AURORA_V9_MODE7_MOSAIC_LATCH_20260915 */
	m_Mode7LineHofs = 0;
	m_Mode7LineVofs = 0;
	m_uMosaicStartLine = 1;
}

#ifdef SNES_DEBUG

char *SnesPPU::GetRegName(Uint32 uAddr)
{
    switch (uAddr)
    {
	    case 0x2100: return (char *)"inidisp";
	    case 0x2101: return (char *)"obsel";
	    case 0x2102: return (char *)"oamaddl";
	    case 0x2103: return (char *)"oamaddh";
	    case 0x2104: return (char *)"oamdata";
	    case 0x2105: return (char *)"bgmode";
	    case 0x2106: return (char *)"mosaic";
	    case 0x2107: return (char *)"bg1sc";
	    case 0x2108: return (char *)"bg2sc";
	    case 0x2109: return (char *)"bg3sc";
	    case 0x210A: return (char *)"bg4sc";
	    case 0x210B: return (char *)"bg12nba";
	    case 0x210C: return (char *)"bg34nba";
	    case 0x210D: return (char *)"bg1hofs";
	    case 0x210E: return (char *)"bg1vofs";
	    case 0x210F: return (char *)"bg2hofs";
	    case 0x2110: return (char *)"bg2vofs";
	    case 0x2111: return (char *)"bg3hofs";
	    case 0x2112: return (char *)"bg3vofs";
	    case 0x2113: return (char *)"bg4hofs";
	    case 0x2114: return (char *)"bg4vofs";
	    case 0x2115: return (char *)"vmain";
	    case 0x2116: return (char *)"vmaddl";
	    case 0x2117: return (char *)"vmaddh";
	    case 0x2118: return (char *)"vmdatal";
	    case 0x2119: return (char *)"vmdatah";
	    case 0x211A: return (char *)"m7sel";
	    case 0x211B: return (char *)"m7a";
	    case 0x211C: return (char *)"m7b";
	    case 0x211D: return (char *)"m7c";
	    case 0x211E: return (char *)"m7d";
	    case 0x211F: return (char *)"m7x";
	    case 0x2120: return (char *)"m7y";
	    case 0x2121: return (char *)"cgadd";
	    case 0x2122: return (char *)"cgdata";
	    case 0x2123: return (char *)"w12sel";
	    case 0x2124: return (char *)"w34sel";
	    case 0x2125: return (char *)"wobjsel";
	    case 0x2126: return (char *)"wh0";
	    case 0x2127: return (char *)"wh1";
	    case 0x2128: return (char *)"wh2";
	    case 0x2129: return (char *)"wh3";
	    case 0x212A: return (char *)"wbglog";
	    case 0x212B: return (char *)"wobjlog";
	    case 0x212C: return (char *)"tm";
	    case 0x212D: return (char *)"ts";
	    case 0x212E: return (char *)"tmw";
	    case 0x212F: return (char *)"tsw";
	    case 0x2130: return (char *)"cgwsel";
	    case 0x2131: return (char *)"cgadsub";
	    case 0x2132: return (char *)"coldata";
	    case 0x2133: return (char *)"setini";
		default:
			return NULL;
    }
}
#endif
