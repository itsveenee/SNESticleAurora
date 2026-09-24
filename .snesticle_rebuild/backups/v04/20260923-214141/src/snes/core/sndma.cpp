
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "types.h"
#include "console.h"
#include "prof.h"
extern "C" {
#include "sncpu.h"
};
#include "sndma.h"
#include "snppu.h"
#include "snsdd1.h"
#include "sndbglog.h"
#include "sntiming.h"
#include "platform/ps2/system/aurora_runtime_trace.h"

#define SNESDMA_DEBUG 0
/* AURORA_CPU_SPC_DSP_PPU_HOST_WORK_REDUCTION_V4_20260920_HDMA */

static Uint8	_SNDma_MDMATransfer[8][4]=
{
	// 000 1-address
	{0,0,0,0},
	// 001 2-address (l,h)
	{0,1,0,1},
	// 010 1-address
	{0,0,0,0},
	// 011 2-address (l,l,h,h)
	{0,0,1,1},
	// 100 4-address (l,h,l,h)
	{0,1,2,3},
	// 101 4-address (l,h,l,h)
	{0,1,0,1},
	// 110 4-address (l,h,l,h)
	{0,0,0,0},
	// 111 4-address (l,h,l,h)
	{0,0,1,1},
};

static Uint8 _SNDma_HDMABytes[8] =
{
	1, 2, 2, 4, 4, 4, 2, 4
};


static Int8 _SNDma_MDMAInc[4] = 
{
	1, 0, -1, 0
};

/* AURORA_SPEEDY_MDR_HDMA_V1
   Every HDMA read passes through the S-CPU MDR/open-bus latch.
   Scope is intentionally HDMA-only; the 65816 ASM hot path is untouched. */
/* AURORA_MEGA_V2_DMA_BUS
 * The S-CPU DMA unit has separate A and B buses. A-bus DMA cannot use the
 * CPU/PPU I/O aliases, and B-bus $2180 cannot be used to copy WRAM back to
 * WRAM. Invalid reads return zero and still drive the CPU MDR; invalid writes
 * are discarded. These predicates follow the same address masks as bsnes.
 *
 * The raw helpers inline SNCPURead8/SNCPUWrite8's exact bank/trap decision.
 * That preserves PPU queue side effects and emulated cycles while avoiding a
 * C function call for each ordinary ROM/WRAM HDMA byte on the PS2 EE.
 */
static _INLINE Bool SnesDMAValidAAddress(Uint32 uAddr)
{
	uAddr &= 0xFFFFFF;
	if ((uAddr & 0x40FF00) == 0x002100) return FALSE;
	if ((uAddr & 0x40FE00) == 0x004000) return FALSE;
	if ((uAddr & 0x40FFE0) == 0x004200) return FALSE;
	if ((uAddr & 0x40FF80) == 0x004300) return FALSE;
	return TRUE;
}

static _INLINE Bool SnesDMAValidBAddress(Uint32 uAddrA, Uint8 uPortB)
{
	if (uPortB != 0x80) return TRUE;
	uAddrA &= 0xFFFFFF;
	if ((uAddrA & 0xFE0000) == 0x7E0000) return FALSE;
	if ((uAddrA & 0x40E000) == 0x000000) return FALSE;
	return TRUE;
}

static _INLINE Uint8 SnesDMARawRead8(SNCpuT *pCPU, Uint32 uAddr)
{
	SNCpuBankT *pBank;
	uAddr &= 0xFFFFFF;
	pBank = &pCPU->Bank[uAddr >> SNCPU_BANK_SHIFT];
	if (pBank->pMem)
		return pBank->pMem[uAddr];
	return pBank->pReadTrapFunc(pCPU, uAddr);
}

static _INLINE void SnesDMARawWrite8(SNCpuT *pCPU, Uint32 uAddr, Uint8 uData)
{
	SNCpuBankT *pBank;
	uAddr &= 0xFFFFFF;
	pBank = &pCPU->Bank[uAddr >> SNCPU_BANK_SHIFT];
	if (pBank->bRAM)
		pBank->pMem[uAddr] = uData;
	else
		pBank->pWriteTrapFunc(pCPU, uAddr, uData);
}

static _INLINE Uint8 SnesDMAReadA(SNCpuT *pCPU, Uint32 uAddr)
{
	Uint8 uData = SnesDMAValidAAddress(uAddr) ? SnesDMARawRead8(pCPU, uAddr) : 0;
	pCPU->uMDR = uData;
	return uData;
}

static _INLINE void SnesDMAWriteA(SNCpuT *pCPU, Uint32 uAddr, Uint8 uData)
{
	if (SnesDMAValidAAddress(uAddr))
		SnesDMARawWrite8(pCPU, uAddr, uData);
}

static _INLINE Uint8 SnesDMAReadB(SNCpuT *pCPU, Uint32 uAddrA, Uint8 uPortB)
{
	Uint8 uData = SnesDMAValidBAddress(uAddrA, uPortB)
		? SnesDMARawRead8(pCPU, 0x2100 | uPortB) : 0;
	pCPU->uMDR = uData;
	return uData;
}

static _INLINE void SnesDMAWriteB(SNCpuT *pCPU, Uint32 uAddrA, Uint8 uPortB, Uint8 uData)
{
	if (SnesDMAValidBAddress(uAddrA, uPortB))
		SnesDMARawWrite8(pCPU, 0x2100 | uPortB, uData);
}

static _INLINE Uint8 SnesHDMARead8(SNCpuT *pCPU, Uint32 uAddr)
{
	return SnesDMAReadA(pCPU, uAddr);
}

/* AURORA_HDMA_PPU_FASTQUEUE_V2
 *
 * Universal, semantics-preserving HDMA fast path for S-PPU writes.
 * B-bus $2100-$213F normally reaches SnesSystem::Write2000(), whose PPU
 * path enqueues the same (scanline, address, data) tuple. Queue it here
 * directly to avoid generic CPU bank/trap dispatch on every HDMA byte.
 *
 * No transfer is skipped, reordered or made cheaper in emulated cycles.
 * Queue-full returns FALSE so the caller takes the original path, which
 * performs SyncPPU() as needed and retries the write.
 */
/* AURORA_DOT_RASTER_V6_DMA_20260915 */
static _INLINE Bool SnesHDMATryQueuePPUWrite(
    SnesPPU *pPPU, Uint32 uLine, Uint32 uHClock,
    Uint8 uPortB, Uint8 uData)
{
    if (uPortB < 0x40)
    {
        /* AURORA_CUMULATIVE_V5_DMA_20260915 */
        Uint8 uMemoryAccessFlags = 0;
        if (uPortB == 0x04 || uPortB == 0x18 ||
            uPortB == 0x19 || uPortB == 0x22)
        {
            /* HDMA is fixed at H=1104. CGRAM active-dot contention
             * ended at H=1096, so only vertical VRAM/OAM ownership remains. */
            uMemoryAccessFlags =
                (uLine <= pPPU->GetFrameVisibleLineCount())
                    ? SNESPPU_MEMBUS_VRAM_OAM_BUSY : 0;
        }
        return pPPU->EnqueueWrite(
            uLine, 0x2100u | (Uint32)uPortB, uData, FALSE,
            uMemoryAccessFlags, uHClock);
    }

    return FALSE;
}

static _INLINE Bool SnesDMAPathTouches16(Uint16 uStart, Uint32 nBytes,
                                         Int32 iDelta, Uint16 uLo, Uint16 uHi)
{
	Uint32 uDistance;
	if (!nBytes) return FALSE;
	if (uStart >= uLo && uStart <= uHi) return TRUE;
	if (iDelta == 0) return FALSE;
	if (nBytes >= 0x10000u) return TRUE;
	if (iDelta > 0)
		uDistance = ((Uint32)uLo - uStart) & 0xFFFFu;
	else
		uDistance = ((Uint32)uStart - uHi) & 0xFFFFu;
	return uDistance < nBytes;
}

/* AURORA_PPU_MEMORY_V3_DMA_20260915 */
static Bool SnesDMAChannelTouchesBPort(const SnesDMAChT *pChan, Uint8 uTarget)
{
	Uint32 i;
	Uint8 uMode = (Uint8)(pChan->dmapx & 7);
	for (i = 0; i < 4; i++)
		if ((Uint8)(pChan->bbadx + _SNDma_MDMATransfer[uMode][i]) == uTarget)
			return TRUE;
	return FALSE;
}

static Bool SnesDMAChannelNeedsAccurateBus(const SnesDMAChT *pChan)
{
	Uint32 nBytes = pChan->dasx ? (Uint32)pChan->dasx : 0x10000u;
	Int32 iDelta = _SNDma_MDMAInc[(pChan->dmapx >> 3) & 3];
	Bool bRestrictedBank = (pChan->a1bx & 0x40) == 0;  // $00-$3f/$80-$bf
	Bool bUsesWramPort = FALSE;
	Uint32 i;

	if (bRestrictedBank)
	{
		if (SnesDMAPathTouches16(pChan->a1tx, nBytes, iDelta, 0x2100, 0x21FF) ||
		    SnesDMAPathTouches16(pChan->a1tx, nBytes, iDelta, 0x4000, 0x41FF) ||
		    SnesDMAPathTouches16(pChan->a1tx, nBytes, iDelta, 0x4200, 0x421F) ||
		    SnesDMAPathTouches16(pChan->a1tx, nBytes, iDelta, 0x4300, 0x437F))
			return TRUE;
	}

	for (i = 0; i < 4; i++)
	{
		if ((Uint8)(pChan->bbadx + _SNDma_MDMATransfer[pChan->dmapx & 7][i]) == 0x80)
		{
			bUsesWramPort = TRUE;
			break;
		}
	}
	if (!bUsesWramPort) return FALSE;
	if ((pChan->a1bx & 0xFE) == 0x7E) return TRUE;
	if (bRestrictedBank &&
	    SnesDMAPathTouches16(pChan->a1tx, nBytes, iDelta, 0x0000, 0x1FFF))
		return TRUE;
	return FALSE;
}

void SnesDMAWritePPUPort(SnesPPU *pPPU, Uint32 uPort, Uint8 uData)
{
	assert(pPPU != NULL);
	assert((uPort & 0xFF) < 0x40);

	/* The caller has already synchronized the scanline write queue at MDMA
	   start.  Do not re-enter SNCPUWrite8 here: it would enqueue address and
	   control registers while the optimized data ports below take effect
	   immediately, reversing the byte order observed by the PPU. */
	pPPU->Write8(0x2100 + (uPort & 0xFF), uData);
}

#if SNDBG_DEEP
struct SNDmaOAMCaptureT
{
	Bool Active;
	Uint32 Frame;
	Uint32 SourceHash;
	Uint32 Bytes;
	Uint16 StartAddress;
	Uint8 Channel;
};

static SNDmaOAMCaptureT _SNDmaOAMCapture;

static Uint32 _SNDmaHashBytes(const Uint8 *pData, Uint32 nBytes)
{
	Uint32 h = 2166136261u;
	while (nBytes-- > 0)
	{
		h ^= *pData++;
		h *= 16777619u;
	}
	return h;
}
#endif



Uint8 SnesDMAC::Read8(Uint32 uChan, Uint32 uAddr)
{
	SnesDMAChT *pChan;
	pChan = &m_Channels[uChan];

	switch(uAddr & 0xF)
	{
	case 0x0: 
		return pChan->dmapx;

	case 0x1: 
		return pChan->bbadx;

	case 0x2: 
		return pChan->a1tx & 0xFF;

	case 0x3: 
		return pChan->a1tx >> 8;

	case 0x4: 
		return pChan->a1bx;

	case 0x5: 
		return	pChan->dasx & 0xFF;

	case 0x6: 
		return	pChan->dasx >> 8;

	case 0x7: 
		return pChan->dasbx;

	case 0x8: 
		return pChan->a2ax & 0xFF;

	case 0x9: 
		return pChan->a2ax >> 8;

	case 0xA: 
		return pChan->ntlrx;

	case 0xB:
	case 0xF:
		return pChan->unknown;

	default:
		//ConDebug("read_dma8[%06X]\n", uAddr);
		return 0x00;
	}
}






void SnesDMAC::Write8(Uint32 uChan, Uint32 uAddr, Uint8 uData)
{
	SnesDMAChT *pChan;

	pChan = &m_Channels[uChan];
//	if (uChan==7)
//	ConDebug("dma%d[%02X]=%02X\n", uChan, uAddr & 0xF, uData);

	switch(uAddr & 0xF)
	{
	case 0x0: 
		pChan->dmapx = uData;
		break;

	case 0x1: 
		pChan->bbadx = uData;
		break;

	case 0x2: 
		pChan->a1tx &= 0xFF00;
		pChan->a1tx |= uData << 0;
		break;

	case 0x3: 
		pChan->a1tx &= 0x00FF;
		pChan->a1tx |= uData << 8;
		break;

	case 0x4: 
		pChan->a1bx = uData;
		break;

	case 0x5: 
		pChan->dasx &= 0xFF00;
		pChan->dasx |= uData << 0;
		break;

	case 0x6: 
		pChan->dasx &= 0x00FF;
		pChan->dasx |= uData << 8;
		break;

	case 0x7: 
		pChan->dasbx = uData;
		break;

	case 0x8: 
		pChan->a2ax &= 0xFF00;
		pChan->a2ax |= uData << 0;
		break;

	case 0x9: 
		pChan->a2ax &= 0x00FF;
		pChan->a2ax |= uData << 8;
		break;

	case 0xA: 
		pChan->ntlrx = uData;
		break;

	case 0xB:
	case 0xF:
		pChan->unknown = uData;
		break;

	default:
//		ConDebug("write_dma8[%06X]=%02X\n", uAddr, uData);
		break;
	}
}

void SnesDMAC::SetMDMAEnable(Uint8 uData)
{
	/* AURORA_SNES_BINARY_TRACE_V7_VISUAL_BREADCRUMBS_20260918_MDMA */
	AuroraTraceBreadcrumb(ATR_SNAP_MDMA, (Uint16)uData);
#if SNDBG_LOG
	Uint32 uChan;

	for (uChan = 0; uChan < SNESDMAC_CHANNEL_NUM; uChan++)
	{
		SnesDMAChT *pChan;
		Uint32 uBytes;
		Uint32 uMode;
		Int32 iDelta;

		if (!(uData & (1 << uChan)))
			continue;

		pChan = &m_Channels[uChan];
		uBytes = pChan->dasx ? (Uint32)pChan->dasx : 0x10000u;
		uMode = pChan->dmapx & 7;
		iDelta = _SNDma_MDMAInc[(pChan->dmapx >> 3) & 3];

		g_DbgDMAStarts++;
		g_DbgDMAModes[uMode]++;
		if (uBytes > g_DbgDMAMaxBytes)
			g_DbgDMAMaxBytes = uBytes;

		#if SNDBG_DEEP
		if (g_DbgCaptureActive)
		{
			const SnesPPURegsT *pRegs = m_pPPU->GetRegs();
			DLog("[snes-dma-start] f=%u ch=%u dmap=%02X mode=%u dir=%s src=%02X:%04X len=%u bbad=%02X oam/vm/cg=%04X/%04X/%04X vmain=%02X",
				(unsigned)g_DbgCaptureFrameNo, (unsigned)uChan,
				(unsigned)pChan->dmapx, (unsigned)uMode,
				(pChan->dmapx & 0x80) ? "B>A" : "A>B",
				(unsigned)pChan->a1bx, (unsigned)pChan->a1tx,
				(unsigned)uBytes, (unsigned)pChan->bbadx,
				(unsigned)pRegs->oamaddr.w, (unsigned)pRegs->vmaddr.w,
				(unsigned)pRegs->cgadd.w, (unsigned)(Uint8)pRegs->vmain);

			/* Snapshot the complete WRAM OAM mirror before its DMA. At channel
			   completion we hash physical OAM too, proving whether corruption
			   happened before or inside the $2104 transfer path. */
			if (!(pChan->dmapx & 0x80) && uMode == 0 &&
			    pChan->bbadx == 0x04 && iDelta == 1 &&
			    uBytes == sizeof(SnesOAMT))
			{
				Uint32 uAddr = ((Uint32)pChan->a1bx << 16) | pChan->a1tx;
				Uint32 uHash = 2166136261u;
				Uint32 i;
				for (i = 0; i < uBytes; i++)
				{
					uHash ^= SNCPUPeek8(m_pCPU,
						((Uint32)pChan->a1bx << 16) |
						((pChan->a1tx + i) & 0xFFFF));
					uHash *= 16777619u;
				}
				_SNDmaOAMCapture.Active = TRUE;
				_SNDmaOAMCapture.Frame = g_DbgCaptureFrameNo;
				_SNDmaOAMCapture.SourceHash = uHash;
				_SNDmaOAMCapture.Bytes = uBytes;
				_SNDmaOAMCapture.StartAddress = pRegs->oamaddr.w;
				_SNDmaOAMCapture.Channel = (Uint8)uChan;
				DLog("[snes-oam-dma] f=%u ch=%u src=%06X bytes=%u start=%04X source-hash=%08X",
					(unsigned)g_DbgCaptureFrameNo, (unsigned)uChan,
					(unsigned)uAddr, (unsigned)uBytes,
					(unsigned)pRegs->oamaddr.w, (unsigned)uHash);
			}
		}
		#endif

		if ((iDelta > 0 && uBytes > 0x10000u - pChan->a1tx) ||
		    (iDelta < 0 && uBytes > (Uint32)pChan->a1tx + 1u))
			g_DbgDMAWraps++;

		if (pChan->dmapx & 0x80)
		{
			g_DbgDMAReadBytes += uBytes;
		}
		else
		{
			/* Count the exact B-bus ports selected by the four-byte mode
			   pattern, rather than assuming that BBAD alone names the port. */
			Uint32 uPhase;
			for (uPhase = 0; uPhase < 4 && uPhase < uBytes; uPhase++)
			{
				Uint32 uCount = 1u + (uBytes - 1u - uPhase) / 4u;
				Uint32 uPort = (pChan->bbadx +
					_SNDma_MDMATransfer[uMode][uPhase]) & 0xFF;
				if (uPort == 0x04)
					g_DbgDMAOAMBytes += uCount;
				else if (uPort == 0x18 || uPort == 0x19)
					g_DbgDMAVRAMBytes += uCount;
				else if (uPort == 0x22)
					g_DbgDMACGRAMBytes += uCount;
				else
					g_DbgDMAOtherBytes += uCount;
			}
		}
	}
#endif
	/* AURORA_V7_MDMA_START_OVERHEAD
	 * AURORA_V82_MDMA_START_DEFERRED
	 * $420B arms MDMA but does not steal bus clocks in the middle of the
	 * instruction that wrote it. ExecuteCPU() observes SNCPU_SIGNAL_DMA
	 * after that instruction/abort boundary; ProcessMDMA() then performs
	 * divider synchronization, global overhead and per-channel overhead. */
	if (uData)
	{
		Uint32 uStartChan;
		for (uStartChan = 0; uStartChan < SNESDMAC_CHANNEL_NUM; uStartChan++)
		{
			if (uData & (1 << uStartChan))
				m_MDMAPhase[uStartChan] = 0;
		}
		m_MDMAStartupPending = 1;
		m_MDMAChannelStartup = uData;
	}
	else
	{
		m_MDMAStartupPending = 0;
		m_MDMAChannelStartup = 0;
	}
	m_MDMAEnable = uData;
}

void SnesDMAC::SetHDMAEnable(Uint8 uData)
{
	/* AURORA_SNES_BINARY_TRACE_V7_VISUAL_BREADCRUMBS_20260918_HDMA */
	AuroraTraceBreadcrumb(ATR_SNAP_HDMA, (Uint16)uData);
	// confirm:
	// ghouls and ghosts enabled hdma mid-frame
	/* $420C keeps the programmed enable bits.  A channel that already read
	   its zero terminator stays stopped until the next frame, even if $420C
	   is written again (Mesen/reference emulator keep a separate ended-channel mask). */
	m_HDMAEnable = uData;
}


void SnesDMAC::ProcessMDMAChRead(Uint32 uChan)
{
	SnesDMAChT *pChan;
	Int32 uDstDelta;
	Uint8 *pTransfer;
	Uint8 uPhase;

	assert(uChan < SNESDMAC_CHANNEL_NUM);
	pChan = &m_Channels[uChan];
	if (m_pCPU->Cycles <= 0) return;

	uDstDelta = _SNDma_MDMAInc[(pChan->dmapx >> 3) & 3];
	pTransfer = _SNDma_MDMATransfer[pChan->dmapx & 7];
	uPhase = m_MDMAPhase[uChan] & 3;

	/* AURORA_V82_MDMA_PHASE_READ */
	do
	{
		Uint32 uAddrA = ((Uint32)pChan->a1bx << 16) | pChan->a1tx;
		Uint8 uPortB = (Uint8)(pChan->bbadx + pTransfer[uPhase]);
		Uint8 uData;
		uPhase = (Uint8)((uPhase + 1) & 3);

		uData = SnesDMAReadB(m_pCPU, uAddrA, uPortB);
		SnesDMAWriteA(m_pCPU, uAddrA, uData);
		pChan->a1tx = (Uint16)(pChan->a1tx + uDstDelta);
		pChan->dasx--;
		SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
	}
	while (pChan->dasx != 0 && m_pCPU->Cycles > 0);

	m_MDMAPhase[uChan] = uPhase;
	if (pChan->dasx == 0)
	{
		m_MDMAEnable &= ~(1 << uChan);
		m_MDMAPhase[uChan] = 0;
	}
}

#if 0
Uint32 SnesDMAC::ProcessMDMACh(Uint32 uChan)
{
	SnesDMAChT *pChan;
	Int32 uSrcDelta;
	Uint8 *pTransfer;
	Int32 iTransfer=0;

	assert(uChan < SNESDMAC_CHANNEL_NUM);

	pChan = &m_Channels[uChan];
	
	#if SNESDMA_DEBUG
	ConDebug("dma%d: %02X %02X%04X -> %02X %04X (%04X)\n", uChan, 
		pChan->dmapx,
		pChan->a1bx, 
		pChan->a1tx, 
		pChan->bbadx, 
		pChan->dasx,
		m_pPPU->m_Regs.vmaddr.w
		);
	#endif


	if (pChan->dmapx & 0x80)
	{
		// ppu -> mem
		return ProcessMDMAChRead(uChan);
	}

	// determine a-bus increment
	uSrcDelta = _SNDma_MDMAInc[(pChan->dmapx>>3) & 3];

	// get transfer order
	pTransfer = _SNDma_MDMATransfer[pChan->dmapx & 7];
	iTransfer = 0;
	
	do
	{
		Uint8 uData;
		Uint32 uAddr;

		// read byte
		uData = SNCPURead8(m_pCPU, pChan->a1tx | (pChan->a1bx << 16));

		// increment src address (does overflow go into next bank?)
		pChan->a1tx += uSrcDelta;

		// get address to write to (b-bus)
		uAddr = 0x2100 + pChan->bbadx + pTransfer[iTransfer & 3];
		iTransfer++;

		// write byte
		SNCPUWrite8(m_pCPU, uAddr, uData);

		// decrement cpu clock cycles
		SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW * 1);

		// decrement byte count
		pChan->dasx--;
	}
	while (pChan->dasx!=0);

	return 1;
}
#endif

void SnesDMAC::TransferData(SnesDMAChT *pChan, Uint8 *pData, Int32 nBytes)
{
    Uint32 uChan = (Uint32)(pChan - m_Channels);
    Uint8 uPhase = m_MDMAPhase[uChan] & 3;
    Int32 nOriginalBytes = nBytes;

    /* A->B MDMA leaves the last A-bus source byte on MDR. */
    if (nBytes > 0)
        m_pCPU->uMDR = pData[nBytes - 1];

    if (!m_pPPU->IsForceBlank() &&
        !(m_pSDD1 && m_pSDD1->DmaActive()) &&
        m_uRasterLine > 0 &&
        m_uRasterLine <= m_pPPU->GetFrameVisibleLineCount() &&
        SnesDMAChannelTouchesBPort(pChan, 0x22))
    {
        Uint8 *pTransfer = _SNDma_MDMATransfer[pChan->dmapx & 7];
        while (nBytes > 0)
        {
            Uint8 uData = *pData++;
            Uint8 uPortB = (Uint8)(pChan->bbadx + pTransfer[uPhase]);
            uPhase = (Uint8)((uPhase + 1) & 3);

            SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
            m_pPPU->SetMemoryAccessFlags(
                m_pPPU->BuildMemoryAccessFlags(
                    m_uRasterLine,
                    (Uint32)SNCPUGetCounter(m_pCPU, SNCPU_COUNTER_LINE)));

            switch (uPortB)
            {
            case 0x04: m_pPPU->WriteOAMDATA(uData); break;
            case 0x18: m_pPPU->WriteVMDATAL(uData); break;
            case 0x19: m_pPPU->WriteVMDATAH(uData); break;
            case 0x22: m_pPPU->WriteCGDATA(uData); break;
            default:
                if (uPortB < 0x40)
                    SnesDMAWritePPUPort(m_pPPU, uPortB, uData);
                else
                    SNCPUWrite8(m_pCPU, 0x2100 + uPortB, uData);
                break;
            }
            nBytes--;
        }
        m_MDMAPhase[uChan] = uPhase;
        m_pPPU->SetMemoryAccessFlags(0);
        return;
    }

    SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW * nBytes);

    m_pPPU->SetMemoryAccessFlags(
        m_pPPU->BuildMemoryAccessFlags(
            m_uRasterLine,
            (Uint32)SNCPUGetCounter(m_pCPU, SNCPU_COUNTER_LINE)));

    /* AURORA_V82_MDMA_BYTE_SLICING
     * V8.1's fast path rounded every slice UP to a four-byte group because
     * TransferData() restarted the mode phase at zero. That can run MDMA
     * up to three bytes (24 master clocks) beyond HBlank/HDMA/IRQ/refresh.
     * Keep the phase per channel instead: chunks may now end after any byte. */
    if ((pChan->dmapx & 7) == 0)
    {
        switch (pChan->bbadx)
        {
        case 0x04:
            m_pPPU->WriteOAMBlock(pData, nBytes);
            break;
        case 0x18:
            while (nBytes > 0) { m_pPPU->WriteVMDATAL(*pData++); nBytes--; }
            break;
        case 0x19:
            while (nBytes > 0) { m_pPPU->WriteVMDATAH(*pData++); nBytes--; }
            break;
        case 0x22:
            while (nBytes > 0) { m_pPPU->WriteCGDATA(*pData++); nBytes--; }
            break;
        default:
            while (nBytes > 0)
            {
                SNCPUWrite8(m_pCPU, 0x2100 + pChan->bbadx, *pData++);
                nBytes--;
            }
            break;
        }
        uPhase = (Uint8)((uPhase + nOriginalBytes) & 3);
    }
    else
    {
        Uint8 *pTransfer = _SNDma_MDMATransfer[pChan->dmapx & 7];

        /* Preserve the hot mode-1 VRAM block path across an event split. */
        if ((pChan->dmapx & 7) == 1 && pChan->bbadx == 0x18)
        {
            if ((uPhase & 1) && nBytes > 0)
            {
                m_pPPU->WriteVMDATAH(*pData++);
                uPhase = (Uint8)((uPhase + 1) & 3);
                nBytes--;
            }
            if (nBytes >= 2)
            {
                Int32 nBulk = nBytes & ~1;
                m_pPPU->WriteVMDATABlock(pData, nBulk);
                pData += nBulk;
                nBytes -= nBulk;
                uPhase = (Uint8)((uPhase + nBulk) & 3);
            }
        }

        while (nBytes > 0)
        {
            Uint8 uData = *pData++;
            Uint8 uPortB = (Uint8)(pChan->bbadx + pTransfer[uPhase]);
            uPhase = (Uint8)((uPhase + 1) & 3);

            switch (uPortB)
            {
            case 0x04: m_pPPU->WriteOAMDATA(uData); break;
            case 0x18: m_pPPU->WriteVMDATAL(uData); break;
            case 0x19: m_pPPU->WriteVMDATAH(uData); break;
            case 0x22: m_pPPU->WriteCGDATA(uData); break;
            default:
                if (uPortB < 0x40)
                    SnesDMAWritePPUPort(m_pPPU, uPortB, uData);
                else
                    SNCPUWrite8(m_pCPU, 0x2100 + uPortB, uData);
                break;
            }
            nBytes--;
        }
    }

    m_MDMAPhase[uChan] = uPhase;
    m_pPPU->SetMemoryAccessFlags(0);
}


void SnesDMAC::ProcessMDMAChAccurate(Uint32 uChan)
{
	SnesDMAChT *pChan;
	Int32 iSrcDelta;
	Uint8 *pTransfer;
	Uint8 uPhase;

	assert(uChan < SNESDMAC_CHANNEL_NUM);
	pChan = &m_Channels[uChan];
	if (m_pCPU->Cycles <= 0) return;

	iSrcDelta = _SNDma_MDMAInc[(pChan->dmapx >> 3) & 3];
	pTransfer = _SNDma_MDMATransfer[pChan->dmapx & 7];
	uPhase = m_MDMAPhase[uChan] & 3;

	/* AURORA_V82_MDMA_PHASE_ACCURATE */
	do
	{
		Uint32 uAddrA = ((Uint32)pChan->a1bx << 16) | pChan->a1tx;
		Uint8 uPortB = (Uint8)(pChan->bbadx + pTransfer[uPhase]);
		Uint8 uData;
		uPhase = (Uint8)((uPhase + 1) & 3);

		uData = SnesDMAReadA(m_pCPU, uAddrA);
		SnesDMAWriteB(m_pCPU, uAddrA, uPortB, uData);
		pChan->a1tx = (Uint16)(pChan->a1tx + iSrcDelta);
		pChan->dasx--;
		SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
	}
	while (pChan->dasx != 0 && m_pCPU->Cycles > 0);

	m_MDMAPhase[uChan] = uPhase;
	if (pChan->dasx == 0)
	{
		m_MDMAEnable &= ~(1 << uChan);
		m_MDMAPhase[uChan] = 0;
	}
}


void SnesDMAC::ProcessMDMAChFast(Uint32 uChan)
{
	SnesDMAChT *pChan;

	assert(uChan < SNESDMAC_CHANNEL_NUM);

    pChan = &m_Channels[uChan];

#if SNESDMA_DEBUG
	ConDebug("dma%d: %02X %02X%04X -> %02X %04X vram=%04X nCycles=%d\n", uChan, 
		pChan->dmapx,
		pChan->a1bx, 
		pChan->a1tx, 
		pChan->bbadx, 
		pChan->dasx,
		m_pPPU->m_Regs.vmaddr.w,
        m_pCPU->Cycles
		);
#endif

    // any cycles available?
    if (m_pCPU->Cycles <= 0) {
        return;
    }
	if (pChan->dmapx & 0x80)
	{
		// ppu -> mem
		return ProcessMDMAChRead(uChan);
	}

	/* AURORA_MEGA_V3_MDMA_DECREMENT_BUS
	 * Keep decrementing A-bus transfers on mega-v2's byte-accurate path.
	 * The old fast branch was already per-byte for decrement mode, so this
	 * costs essentially no useful bulk optimisation while ensuring every
	 * byte gets the same A/B validity and MDR semantics.  Fixed-address
	 * mode is intentionally NOT forced here: S-DD1 legitimately uses that
	 * mode and has its own decompression fast path below. */
	if ((((pChan->dmapx >> 3) & 3) == 2) ||
	    SnesDMAChannelNeedsAccurateBus(pChan))
	{
		ProcessMDMAChAccurate(uChan);
		return;
	}

	// S-DD1: descomprime quando o DMA tem endereco-A fixo (dmapx bit 0x08) e
	// $4801 != 0 (mesma condicao do reference emulator). Antes eu so' checava o bit do
	// canal em $4801, o que podia disparar num DMA normal por engano.
	if (m_pSDD1 && (pChan->dmapx & 0x08) && m_pSDD1->DmaActive())
	{
		static Uint8 s_DecodeBuf[0x10000];
		Int32  count   = pChan->dasx ? pChan->dasx : 0x10000;
		Uint32 srcAddr = ((Uint32)pChan->a1bx << 16) | pChan->a1tx;
		Uint8 *pIn     = m_pCPU->Bank[srcAddr >> SNCPU_BANK_SHIFT].pMem;

		if (pIn)
		{
			// pMem ja' inclui (-base do banco), entao soma-se o endereco
			// completo (mesma convencao de SNCPUPeek8: pMem[Addr]).
			pIn += srcAddr;
			m_pSDD1->Decompress(s_DecodeBuf, pIn, (Int32)pChan->dasx);
			TransferData(pChan, s_DecodeBuf, count);
#if SNDBG_LOG
			{
				static int n = 0;
				if (n < 120) {
					DLog("[sdd1] dma ch=%d src=%06X cnt=%d bbad=%02X mode=%d hdr=%02X out=%02X%02X%02X%02X",
						(int)uChan, (unsigned)srcAddr, (int)count,
						(int)pChan->bbadx, (int)(pChan->dmapx & 7),
						(int)pIn[0], (int)s_DecodeBuf[0], (int)s_DecodeBuf[1],
						(int)s_DecodeBuf[2], (int)s_DecodeBuf[3]);
					n++;
				}
			}
#endif
		}
#if SNDBG_LOG
		else
		{
			static int nn = 0;
			if (nn < 40) {
				DLog("[sdd1] dma ch=%d src=%06X SEM PONTEIRO (banco nao mapeado!)",
					(int)uChan, (unsigned)srcAddr);
				nn++;
			}
		}
#endif

		m_pSDD1->ClearDmaEnable();
		pChan->dasx = 0;
		m_MDMAEnable &= ~(1 << uChan);
		return;
	}

    do
	{
		Uint8 DmaBuffer[256];
		Int32 nBytes;

		// calculate number of bytes remaining to transfer
		nBytes = pChan->dasx ? pChan->dasx : 0x10000;

        // clamp number of bytes to the size of our temporary buffer
		if (nBytes > (Int32)sizeof(DmaBuffer)) 
            nBytes = sizeof(DmaBuffer);

        // clamp number of bytes to cycle time remaining
        /* AURORA_V82_MDMA_FAST_BYTE_BOUNDARY
         * Round only to the next complete DMA byte. TransferData() now
         * preserves mode phase, so four-byte rounding is unnecessary. */
        Int32 nMaxBytes = ((m_pCPU->Cycles + 7) >> 3);
        if (nBytes > nMaxBytes)
            nBytes = nMaxBytes;

        // any bytes to transfer?
        if (nBytes > 0)
        {
		    PROF_ENTER("DMAREADMEM");
		    switch ((pChan->dmapx>>3) & 3)
		    {
		    case 0: //+1 increment
                {
					Int32 nFirst = nBytes;
					Int32 nToBankEnd = 0x10000 - (Int32)pChan->a1tx;

					if (nFirst > nToBankEnd)
						nFirst = nToBankEnd;

			        // A1T wraps inside A1B. Keep both pieces in one buffer so
			        // TransferData also keeps its B-bus mode phase continuous.
			        SNCPUReadMem(m_pCPU,
			                     pChan->a1tx | (pChan->a1bx << 16),
			                     DmaBuffer, nFirst);
					if (nFirst < nBytes)
					{
						SNCPUReadMem(m_pCPU, pChan->a1bx << 16,
						             DmaBuffer + nFirst, nBytes - nFirst);
					}

			        // increment src address 
			        pChan->a1tx = (Uint16)(pChan->a1tx + nBytes);
                }
			    break;
		    case 2: //-1 decrement
			    {
                    // read data into dma buffer (decrement)
				    Int32 iByte;
				    for (iByte=0; iByte < nBytes; iByte++)
				    {
					    DmaBuffer[iByte] = SNCPURead8(m_pCPU, pChan->a1tx | (pChan->a1bx << 16));
					    pChan->a1tx--;
				    }
			    }
			    break;
		    case 1:
		    case 3: // 0
			    // read data into dma buffer (no increment)
			    memset(DmaBuffer, SNCPURead8(m_pCPU, pChan->a1tx | (pChan->a1bx << 16)), nBytes);
			    break;
		    }
		    PROF_LEAVE("DMAREADMEM");

            // transfer cached data to B-bus
		    TransferData(pChan, DmaBuffer, nBytes);

            // decrement byte count
            pChan->dasx -= nBytes;
        }

	}	while ( (pChan->dasx!=0) && (m_pCPU->Cycles > 0) );

    // are we done?
    if (pChan->dasx == 0)
    {
#if SNDBG_DEEP
		if (_SNDmaOAMCapture.Active &&
		    _SNDmaOAMCapture.Frame == g_DbgCaptureFrameNo &&
		    _SNDmaOAMCapture.Channel == uChan)
		{
			Uint32 uDestHash = _SNDmaHashBytes(
				(const Uint8 *)m_pPPU->GetOAM(), sizeof(SnesOAMT));
			Bool bComparable =
				(_SNDmaOAMCapture.StartAddress & 0x3FF) == 0 &&
				_SNDmaOAMCapture.Bytes == sizeof(SnesOAMT);
			DLog("[snes-oam-dma] f=%u ch=%u dest-hash=%08X comparable=%u match=%u end=%04X",
				(unsigned)g_DbgCaptureFrameNo, (unsigned)uChan,
				(unsigned)uDestHash, (unsigned)bComparable,
				(unsigned)(bComparable &&
					uDestHash == _SNDmaOAMCapture.SourceHash),
				(unsigned)m_pPPU->GetRegs()->oamaddr.w);
			_SNDmaOAMCapture.Active = FALSE;
		}
#endif
        // clear channel enable bit
        m_MDMAEnable &= ~(1 << uChan);
        /* AURORA_V82_MDMA_PHASE_COMPLETE */
        m_MDMAPhase[uChan] = 0;
    }
}








void SnesDMAC::BeginHDMA()
{
	Uint8 uEnabled = m_HDMAEnable;
	m_HDMAEnded = 0;
	m_HDMADoTransfer = uEnabled ? 0xFF : 0;

	if (!uEnabled)
		return;

	/* Mesen: HDMA initialization has one 8-clock global overhead, then
	   initializes every enabled channel before the first scanline transfer. */
	SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);

	for (Uint32 uChan = 0; uChan < SNESDMAC_CHANNEL_NUM; uChan++)
	{
		Uint8 uMask = (Uint8)(1 << uChan);
		SnesDMAChT *pChan;
		Bool bStopped;
		Uint8 uLow;

		if (!(uEnabled & uMask))
			continue;

		pChan = &m_Channels[uChan];
		/* HDMA setup cancels MDMA already armed on the same channel. */
		m_MDMAEnable &= (Uint8)~uMask;
		m_MDMAChannelStartup &= (Uint8)~uMask;
		m_MDMAPhase[uChan] = 0;
		pChan->a2ax = pChan->a1tx;
		pChan->ntlrx = SnesHDMARead8(m_pCPU,
			(Uint16)pChan->a2ax | (pChan->a1bx << 16));
		SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
		pChan->a2ax++;

		bStopped = pChan->ntlrx == 0;
		if (bStopped)
			m_HDMAEnded |= uMask;

		if (pChan->dmapx & 0x40)
		{
			uLow = SnesHDMARead8(m_pCPU,
				(Uint16)pChan->a2ax | (pChan->a1bx << 16));
			SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
			pChan->a2ax++;

			if (bStopped)
			{
				/* Hardware's terminal-channel oddity treats the one byte as
				   the high half of the otherwise-unused indirect address. */
				pChan->dasx = (Uint16)uLow << 8;
			}
			else
			{
				pChan->dasx = uLow | (SnesHDMARead8(m_pCPU,
					(Uint16)pChan->a2ax | (pChan->a1bx << 16)) << 8);
				SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
				pChan->a2ax++;
			}
		}
	}
}

void SnesDMAC::ProcessHDMACh(Uint32 uChan, Uint32 uLine)
{
	SnesDMAChT *pChan;
	Uint8 *pTransfer;
	Uint8 uDmap;
	Uint32 uMode;
	Uint32 nBytes;
	Uint32 uHClock;
	Uint32 uTableBank;
	Uint32 uIndirectBank;
	Bool bIndirect;
	Bool bReverse;

	assert(uChan < SNESDMAC_CHANNEL_NUM);
	pChan = &m_Channels[uChan];
	uDmap = pChan->dmapx;
	bIndirect = (uDmap & 0x40u) ? TRUE : FALSE;
	bReverse = (uDmap & 0x80u) ? TRUE : FALSE;
	uTableBank = (Uint32)pChan->a1bx << 16;
	uIndirectBank = (Uint32)pChan->dasbx << 16;
	uHClock = (Uint32)SNCPUGetCounter(m_pCPU, SNCPU_COUNTER_LINE);
	/* MDMA cancellation/phase reset are performed by ProcessHDMA() before
	 * this private helper is entered. */
	/* AURORA_V82_LOST_VIKINGS_HDMA_FIXED_DECREMENT
	 * The Lost Vikings is a canonical compatibility case here: DMAP bits
	 * 3 (fixed) and 4 (decrement) affect MDMA only. HDMA direct A2A and
	 * indirect DAS addresses ALWAYS advance after each transferred byte.
	 * Deliberately do not call _SNDma_MDMAInc from this function. */
	uMode = uDmap & 7u;
	nBytes = _SNDma_HDMABytes[uMode];
	pTransfer = _SNDma_MDMATransfer[uMode];

	for (Uint32 i = 0; i < nBytes; i++)
	{
		Uint32 uAddrA;
		Uint8 uPortB = (Uint8)(pChan->bbadx + pTransfer[i]);
		Uint8 uData;

		if (bIndirect)
			uAddrA = uIndirectBank | pChan->dasx;
		else
			uAddrA = uTableBank | pChan->a2ax;

		if (bReverse)
		{
			uData = SnesDMAReadB(m_pCPU, uAddrA, uPortB);
			SnesDMAWriteA(m_pCPU, uAddrA, uData);
		}
		else
		{
			uData = SnesDMAReadA(m_pCPU, uAddrA);

			/* AURORA_HDMA_PPU_FASTQUEUE_V2
			 * Preserve the exact old fallback for every non-PPU port and
			 * whenever the PPU queue is full. Emulated HDMA cycle charging
			 * remains below, unchanged. */
			if (!SnesHDMATryQueuePPUWrite(
			        m_pPPU, uLine,
			        uHClock,
			        uPortB, uData))
			{
				SnesDMAWriteB(m_pCPU, uAddrA, uPortB, uData);
			}
#if SNDBG_LOG
			{
				Uint32 uPort = uPortB;
				if (uPort >= 0x0D && uPort <= 0x14)
					g_DbgHDMAScrollBytes++;
				else if (uPort == 0x22)
					g_DbgHDMACGRAMBytes++;
				else if (uPort >= 0x23 && uPort <= 0x32)
					g_DbgHDMAWindowColorBytes++;
				else
					g_DbgHDMAOtherBytes++;
			}
#endif
		}

		if (bIndirect)
			pChan->dasx++;
		else
			pChan->a2ax++;
		SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
		/* Counter[LINE]-Cycles advances by exactly the cycles just consumed. */
		uHClock += SNCPU_CYCLE_SLOW;
	}
}






void SnesDMAC::ProcessMDMA()
{
    Uint32 uChan = 0;

    /* AURORA_V82_MDMA_DEFERRED_ENGINE
     * Finish the $420B-writing instruction first, synchronize to the DMA
     * divider, pay 8 global clocks, then 8 clocks when each enabled channel
     * actually starts. Pending HDMA can take priority at the boundaries
     * between those stages and between individual data bytes. */
    if (m_MDMAEnable && m_MDMAStartupPending)
    {
        Int32 nPhase = SNCPUGetCounter(m_pCPU, SNCPU_COUNTER_TOTAL) & 7;
        Int32 nAlign = 8 - nPhase;
        SNCPUConsumeCycles(m_pCPU, nAlign + SNCPU_CYCLE_SLOW);
        m_MDMAStartupPending = 0;
        if (m_pCPU->Cycles <= 0)
            return;
    }

    while (m_MDMAEnable && (m_pCPU->Cycles > 0))
    {
        if (m_MDMAEnable & (1 << uChan))
        {
            Uint8 uMask = (Uint8)(1 << uChan);
            if (m_MDMAChannelStartup & uMask)
            {
                SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
                m_MDMAChannelStartup &= (Uint8)~uMask;
                if (m_pCPU->Cycles <= 0)
                    return;
            }
            ProcessMDMAChFast(uChan);
        }
        else
        {
            uChan++;
        }
    }

    if (!m_MDMAEnable)
    {
        m_MDMAStartupPending = 0;
        m_MDMAChannelStartup = 0;
    }
}

void SnesDMAC::ProcessHDMA(Uint32 uLine)
{
	Uint8 uActive = m_HDMAEnable & ~m_HDMAEnded;

	if (!uActive)
		return;

#if SNDBG_LOG
	Uint32 _tHDMAData = ProfCtrGetCycle();
	g_DbgHDMALines++;
#endif

	/* Mesen performs every channel's data phase first, followed by every
	   channel's counter/table phase.  Interleaving those phases changes both
	   B-bus side effects and the point at which IRQ/NMI can be observed. */
	SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);

	/* Every active HDMA channel cancels pending MDMA before testing its
	 * transfer latch. Clearing the same active mask in bulk is identical. */
	m_MDMAEnable &= (Uint8)~uActive;
	m_MDMAChannelStartup &= (Uint8)~uActive;

	for (Uint32 uChan = 0; uChan < SNESDMAC_CHANNEL_NUM; uChan++)
	{
		Uint8 uMask = (Uint8)(1 << uChan);
		if (!(uActive & uMask))
			continue;

		/* AURORA_V7_HDMA_CANCELS_MDMA_ON_SKIP
		 * Bulk MDMA/startup cancellation was performed above for the same
		 * active mask. Keep the per-channel phase reset unchanged. */
		m_MDMAPhase[uChan] = 0;

		if (m_HDMADoTransfer & uMask)
		{
#if SNDBG_LOG
			g_DbgHDMATransferChannels++;
#endif
			ProcessHDMACh(uChan, uLine);
		}
	}

#if SNDBG_LOG
	g_TmgCycHDMAData += ProfCtrGetCycle() - _tHDMAData;
	Uint32 _tHDMATable = ProfCtrGetCycle();
#endif

	for (Uint32 uChan = 0; uChan < SNESDMAC_CHANNEL_NUM; uChan++)
	{
		Uint8 uMask = (Uint8)(1 << uChan);
		SnesDMAChT *pChan;
		Uint8 uNewCounter;

		if (!(uActive & uMask))
			continue;

#if SNDBG_LOG
		g_DbgHDMAActiveChannels++;
#endif

		pChan = &m_Channels[uChan];
		pChan->ntlrx--;
		if (pChan->ntlrx & 0x80)
			m_HDMADoTransfer |= uMask;
		else
			m_HDMADoTransfer &= ~uMask;

		/* The S-CPU performs this table read on every active scanline.  Its
		   value is discarded until the seven-bit line counter reaches zero. */
		uNewCounter = SnesHDMARead8(m_pCPU,
			(Uint16)pChan->a2ax | (pChan->a1bx << 16));
		SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);

		if ((pChan->ntlrx & 0x7F) == 0)
		{
			pChan->ntlrx = uNewCounter;
			pChan->a2ax++;

			if (pChan->dmapx & 0x40)
			{
				Uint8 uHigherMask = (Uint8)~((1u << (uChan + 1)) - 1u);
				/* Higher channels have not yet run their table phase, so the
				 * start-of-line active snapshot is exactly the same set here. */
				Bool bLastActive = !(uActive & uHigherMask);
				Uint8 uLow;

				uLow = SnesHDMARead8(m_pCPU,
					(Uint16)pChan->a2ax | (pChan->a1bx << 16));
				SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
				pChan->a2ax++;

				if (uNewCounter == 0 && bLastActive)
				{
					/* The last terminating indirect channel fetches only one
					   address byte and places it in the high half. */
					pChan->dasx = (Uint16)uLow << 8;
				}
				else
				{
					pChan->dasx = uLow | (SnesHDMARead8(m_pCPU,
						(Uint16)pChan->a2ax |
						(pChan->a1bx << 16)) << 8);
					SNCPUConsumeCycles(m_pCPU, SNCPU_CYCLE_SLOW);
					pChan->a2ax++;
				}
			}

			if (uNewCounter == 0)
				m_HDMAEnded |= uMask;
			m_HDMADoTransfer |= uMask;
		}
	}
#if SNDBG_LOG
	g_TmgCycHDMATable += ProfCtrGetCycle() - _tHDMATable;
#endif
}

void SnesDMAC::Reset()
{
	memset(m_Channels, 0xFF, sizeof(m_Channels));
	m_MDMAEnable = 0;
	/* AURORA_V82_MDMA_PHASE_RESET */
	memset(m_MDMAPhase, 0, sizeof(m_MDMAPhase));
	m_MDMAChannelStartup = 0;
	m_MDMAStartupPending = 0;
	m_HDMAEnable = 0;
	m_HDMAEnded = 0;
	m_HDMADoTransfer = 0;
	m_uRasterLine = 0;
}
