
#include <string.h>
#include <stdio.h>
#include "types.h"
#include "prof.h"
#include "snspcdsp.h"
#include "snspcmix.h"
#include "console.h"
#include "mixbuffer.h"
#include "sntiming.h"
#include "snspcdefs.h"
#include "sndbglog.h"
#include "platform/ps2/system/aurora_runtime_trace.h"
/* AURORA_SNES_BINARY_TRACE_V6D_SPARSE_HIGHSIGNAL_20260918 */
extern "C" {
#include "snspcbrr.h"
};
#if CODE_PLATFORM == CODE_PS2
#include "ps2mem.h"
#endif

/* AURORA_CPU_SPC_DSP_HOST_WORK_REDUCTION_V3_20260920 */
#define SNSPCDSP_INFOSCRATCHPAD ((CODE_PLATFORM == CODE_PS2) && TRUE)
#define SNSPCDSP_MIXSILENCE (FALSE)


#define SNSPCDSP_MIXASM ((CODE_PLATFORM == CODE_PS2) && 1)

Uint32 _ChMask=0xFF;


typedef Int16 SNSpcEchoSampleT;
typedef Int32 SNSpcMixSampleT;


/*
SPC Timing:

CPU cycles/sec: 1022727.272727

sample rate: 32000hz (31.25 microseconds)
32 CPU cycles / sample (32 * 32000 = 1024000)
Envelope updates at 32000hz

Attack:
Linear 0->1 Increments by 1/64 
Requires 64 steps to reach 1.0
envticks = AttackTimeMS * 32000hz / 64

Decay:
Exponential 1->0    Decs by X * 1/256  		595 updates to 1/10

Sustain:
Exponential -> 0    Decs by X * 1/256  		595 updates to 1/10

Increase Bent Line:
0->0.75 Increase by 1/64
0.75->1 Increase by 1/256
(48+64) = 112 total steps
envticks = TimeMS * 32000hz / 112

*/


// channel[i].mix = channel[i].envx * channel[i].outx 
// main_mix = channel[i].mix *  * channel_vol
// echo_mix = channel[i].echo_enabled ? (channel[i].mix * channel_vol) : 0
// echo_out = filter(echo_buffer);
// echo_buffer = echo_out * echo_feedback + echo_mix
// output = main_mix * main_vol + echo_mix * echo_vol




static Uint32 _SNSpcDsp_AttackTimeMS[16]=
{
	4100, 2600, 1500, 1000, 640, 380, 260, 160, 96, 64, 40, 24, 16, 10, 6, 0
};

static Uint32 _SNSpcDsp_DecayTimeMS[8]=
{
	1200, 740, 440, 290, 180, 110, 74, 37
};

static Uint32 _SNSpcDsp_SustainTimeMS[32]=
{
	0xFFFFFFF, 38000, 28000, 24000, 19000, 14000, 12000, 9400, 7100, 5900, 4700, 3500, 2900, 2400, 1800, 1500,
		1200, 880, 740, 590, 440, 370, 290, 220, 180, 150, 110, 92, 74, 55, 37, 28
};

static Uint32 _SNSpcDsp_LinearMS[32]=
{
	0xFFFFFFF,
		4100, 	3100,	2600,	2000,	1500,	1300,	1000,	770,	640,	510,	380,	320,
		260,	190,	160,	130,	96, 	80,	64,	48,	40, 	32,	24,	20,	16,	12,	10,	8,	6,	4,	2,
};

static Uint32 _SNSpcDsp_BentLineMS[32]=
{
	0xFFFFFFF,
		7200, 5400, 4600, 3500, 2600, 2300, 1800,
		1300, 1100, 900, 670, 580, 450, 340, 280,
		220, 170, 140, 110, 84, 70, 56, 42, 
		35, 28, 21, 18, 14, 11, 7, 3
};

/* AURORA_SPC700_MEGA_ACCURACY_V1_20260916 */
static const Uint16 _SNSpcDspCounterRate[32]={0,2048,1536,1280,1024,768,640,512,384,320,256,192,160,128,96,80,64,48,40,32,24,20,16,12,10,8,6,5,4,3,2,1};
static const Uint16 _SNSpcDspCounterOffset[32]={0,0,1040,536,0,1040,536,0,1040,536,0,1040,536,0,1040,536,0,1040,536,0,1040,536,0,1040,536,0,1040,536,0,1040,0,0};



/* AURORA_AUDIO_NATIVE_PHASE_V3
 * Exact host-side fast path. No DSP clock, pitch or interpolation rule changes. */
static _INLINE Int32 _SNSpcDspPhaseInc(Uint32 uPitch, Int32 nSampleRate)
{
	if (nSampleRate == SNSPCDSP_SAMPLERATE)
		return (Int32)(uPitch << 4);

	return (Int32)((uPitch * SNSPCDSP_SAMPLERATE / nSampleRate) << 4);
}



//
//
//

void SNSpcDspMix::BuildLookupTables(Uint32 nSampleRate)
{
	int i;
	Uint32 uFactor;
	Uint32 uScale64;
	Uint32 uScale595;
	Uint32 uScale112;

	uFactor = nSampleRate * 0x10000 / SNSPCDSP_SAMPLERATE;

	/* AURORA_SNES_SAFE_PERF_V1_20260919
	 * These expressions depend only on uFactor. Evaluating them once is
	 * bit-for-bit identical to recalculating them in every table iteration. */
	uScale64  = 32 * uFactor / 64;
	uScale595 = 32 * uFactor / 595;
	uScale112 = 32 * uFactor / 112;

	for (i=0; i < 16; i++)
	{
		m_AttackTicks[i] = _SNSpcDsp_AttackTimeMS[i] * uScale64;
	}

	for (i=0; i < 8; i++)
	{
		m_DecayTicks[i] = _SNSpcDsp_DecayTimeMS[i] * uScale595;
	}

	for (i=0; i < 32; i++)
	{
		if (_SNSpcDsp_SustainTimeMS[i] != 0xFFFFFFF) 
		{
			m_SustainTicks[i] = _SNSpcDsp_SustainTimeMS[i] * uScale595;
		} else
		{
			m_SustainTicks[i] = 0xFFFFFFF;
		}
	}


	for (i=0; i < 32; i++)
	{
		if (_SNSpcDsp_LinearMS[i] != 0xFFFFFFF) 
		{
			m_LinearTicks[i] = _SNSpcDsp_LinearMS[i] * uScale64;
		} else
		{
			m_LinearTicks[i] = 0xFFFFFFF;
		}
	}

	for (i=0; i < 32; i++)
	{
		if (_SNSpcDsp_BentLineMS[i] != 0xFFFFFFF) 
		{
			m_BentLineTicks[i] = _SNSpcDsp_BentLineMS[i] * uScale112;
		} else
		{
			m_BentLineTicks[i] = 0xFFFFFFF;
		}
	}
}


void SNSpcDspMix::Reset()
{
	memset(m_Channels, 0, sizeof(m_Channels));
	/* AURORA_DKC_SPC_HOST_SAFETY_V1_RESET_20260917
	 * Do not depend on SnesSystem/mixer storage having been zero-filled.
	 * The next Mix() must build the envelope tables for the real rate. */
	m_nSampleRate = 0;
}

void SNSpcDspMix::SoftReset()
{
	Int32 i;
	for (i=0;i<SNSPCDSP_CHANNEL_NUM;i++)
	{
		SNSpcChannelT *c=&m_Channels[i];
		c->eEnvState=SNSPCDSP_ENVSTATE_RELEASE;
		c->iEnvelope=0;
		c->nEnvCount=0;
		c->envx=0;
		c->outx=0;
		c->endx=FALSE;
	}
}

void SNSpcDspMix::KeyOn(Int32 iChannel)
{
	SNSpcChannelT *pChannel = &m_Channels[iChannel];
	const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);

	pChannel->eEnvState  = SNSPCDSP_ENVSTATE_ATTACK;
	pChannel->iEnvelope  = 0;
	pChannel->nEnvCount  = 0;
	pChannel->envx       = 0;
	pChannel->outx       = 0;
	pChannel->uBlockAddr = m_pDsp->GetSampleDir(pRegs->srcn, 0);

	/* AURORA_DKC_SPC_HOST_SAFETY_V1_KEYON_20260917
	 * A new KON starts a new BRR/interpolation stream.  Reset the simplified
	 * interpolation history and place phase at the existing decode threshold:
	 * the first OutputSample iteration FetchBlock()s the new BRR block, then
	 * starts from the two zero history samples.  This also prevents a stale
	 * end-of-batch phase from becoming an out-of-row BlockData subscript. */
	memset(pChannel->BlockData, 0, sizeof(pChannel->BlockData));
	pChannel->iPhase = 14 << 16;
	pChannel->uOldBlockAddr = pChannel->uBlockAddr;

    // clear endx
	pChannel->endx		 = FALSE;
}


void SNSpcDspMix::KeyOff(Int32 iChannel)
{
	SNSpcChannelT *pChannel = &m_Channels[iChannel];

	if ((pChannel->eEnvState!= SNSPCDSP_ENVSTATE_RELEASE) && (pChannel->eEnvState!= SNSPCDSP_ENVSTATE_SILENCE))
	{
		pChannel->eEnvState = SNSPCDSP_ENVSTATE_RELEASE;
		pChannel->nEnvCount = 0;
	}
}

/* AURORA_HW_ACCURACY_SDSP_SILENT_OUTX_V1_20260916
 * Once a BRR voice has ended, OUTX must expose zero rather than stale ENVX. */
Bool SNSpcDspMix::GetChannelState(Int32 iChannel, Uint8 *pEnvX, Uint8 *pOutX)
{
	SNSpcChannelT *pChannel = &m_Channels[iChannel];
	Bool endx = FALSE;

	*pEnvX = pChannel->envx;
	*pOutX = pChannel->outx;
	endx   = pChannel->endx;

	pChannel->endx = FALSE;
	return endx;
}


Int32 SNSpcDspMix::OutputEnvelope(Int32 iChannel, Uint8 *pOut, Int32 nSamples)
{
	SNSpcChannelT *pChannel = GetChannel(iChannel);
	const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);
	Int32 iEnvelope;
	Int32 nEnvCount;
	Int32 nEnvRate = 0;

	// envx=0 by default
	pChannel->envx = 0;					
	pChannel->outx = 0;

	#if !SNSPCDSP_MIXSILENCE
	// sample has ended
	if (pChannel->uBlockAddr== 0)
	{
		return 0;
	}
	#endif

	/* AURORA_SAFE_CODE_PERF_V1_MIX
	 * The synchronous envelope batch does not call back into the DSP. Keep
	 * the register bytes used by setup local, but only for an active voice. */
	const Uint8 uAdsr1 = pRegs->adsr1;
	const Uint8 uGain  = pRegs->gain;

	//
	// initialze envelope state based on register settings
	//
	if (!(uAdsr1 & 0x80))
	{
		SNSpcEnvStateE eNewState = pChannel->eEnvState;
		
		#if !SNSPCDSP_MIXSILENCE
		if (!uGain)
		{
			return 0;
		}
		#endif

		if (pChannel->eEnvState!=SNSPCDSP_ENVSTATE_RELEASE && pChannel->eEnvState!=SNSPCDSP_ENVSTATE_SILENCE)
		{
			// enforce gain modes
			switch (uGain >> 5)
			{
			case 0x6:
				eNewState = SNSPCDSP_ENVSTATE_INCREASELINEAR;
				break;
			case 0x7:
				eNewState = SNSPCDSP_ENVSTATE_INCREASEBENTLINE;
				break;
			case 0x4:
				eNewState = SNSPCDSP_ENVSTATE_DECREASELINEAR;
				break;
			case 0x5:
				eNewState = SNSPCDSP_ENVSTATE_DECREASEEXP;
				break;
			default:
				eNewState = SNSPCDSP_ENVSTATE_DIRECT;
			}

			// the equivilent of a gain "key-on"
			if (eNewState!= pChannel->eEnvState)
			{
				pChannel->eEnvState = eNewState;
				pChannel->nEnvCount = 0;
			}
		}
	} 

#if !SNSPCDSP_MIXSILENCE
	if (pChannel->eEnvState==SNSPCDSP_ENVSTATE_SILENCE)
	{
		return 0;
	}
#endif
	

	const Uint8 uAdsr2 = pRegs->adsr2;
	SNSpcEnvStateE eEnvState = pChannel->eEnvState;

	/* AURORA_SNES_SAFE_PERF_V7_20260919: envelope register-decode invariants.
	 * No DSP callback/write occurs inside this synchronous envelope batch. */
	const Uint32 uAttackIndex = (Uint32)(uAdsr1 & 0x0F);
	const Uint32 uDecayIndex = (Uint32)((uAdsr1 >> 4) & 0x07);
	const Uint32 uSustainIndex = (Uint32)(uAdsr2 & 0x1F);
	const Uint32 uGainRateIndex = (Uint32)(uGain & 0x1F);
	const Int32 iSustainTarget =
		((Int32)(uAdsr2 >> 5) + 1) << (SNSPCDSP_ENVELOPE_BITS - 3);
	const Int32 nAttackRate = (Int32)m_AttackTicks[uAttackIndex];
	const Int32 nDecayRate = (Int32)m_DecayTicks[uDecayIndex];
	const Int32 nSustainRate = (Int32)m_SustainTicks[uSustainIndex];
	const Int32 nGainLinearRate = (Int32)m_LinearTicks[uGainRateIndex];
	const Int32 nGainSustainRate = (Int32)m_SustainTicks[uGainRateIndex];
	const Int32 nGainBentRate = (Int32)m_BentLineTicks[uGainRateIndex];

	PROF_ENTER("SNSpcDspOutputEnvelope");

	//
	// process envelope
	//
	iEnvelope = pChannel->iEnvelope;
	nEnvCount = pChannel->nEnvCount;
	while (nSamples > 0)
	{
		if (nEnvCount <= 0)
		{
			// update envelope
			switch (eEnvState)
			{
			case SNSPCDSP_ENVSTATE_ATTACK:
				// get attack rate
				nEnvRate = nAttackRate;
				// update envelope
				iEnvelope += SNSPCDSP_ENVELOPE_MAX >> 6;		// increment by 1/64

				// has attack completed?
				if (iEnvelope >= SNSPCDSP_ENVELOPE_MAX)
				{
					iEnvelope = SNSPCDSP_ENVELOPE_MAX;

					// begin decay
					eEnvState = SNSPCDSP_ENVSTATE_DECAY;
				}
				break;

			case SNSPCDSP_ENVSTATE_DECAY:
				// get decay rate
				nEnvRate = nDecayRate;
				// get sustain level

				// update envelope
				iEnvelope -= iEnvelope >> 8;           // decrement by x / 256

				// has decay completed?
				if (iEnvelope <= iSustainTarget)
				{
					iEnvelope = iSustainTarget;
					// begin sustain
					eEnvState = SNSPCDSP_ENVSTATE_SUSTAIN;
				}
				break;

			case SNSPCDSP_ENVSTATE_SUSTAIN:
				// get sustain rate
				nEnvRate = nSustainRate;
				// update envelope
				iEnvelope -= iEnvelope >> 8;					// decrement by x / 256

				// has sustain completed?
				if (iEnvelope <= 0)
				{
					iEnvelope = 0;
				}
				break;

			case SNSPCDSP_ENVSTATE_RELEASE:
				// set rate, should decrease to 0 in 256 steps ( 256 / 32000 = 0.008 sec)
				nEnvRate = 1 << 16;

				iEnvelope -= SNSPCDSP_ENVELOPE_MAX >> 8;		// decrement by 1/256
				if (iEnvelope <= 0)
				{
					iEnvelope = 0;
					eEnvState = SNSPCDSP_ENVSTATE_SILENCE;
				}
				break;

			case SNSPCDSP_ENVSTATE_DECREASELINEAR:
				// get rate
				nEnvRate = nGainLinearRate;

				iEnvelope -= SNSPCDSP_ENVELOPE_MAX >> 6;		// decrement by 1/64
				if (iEnvelope <= 0)
				{
					iEnvelope = 0;
				}
				break;

			case SNSPCDSP_ENVSTATE_DECREASEEXP:
				// get sustain rate
				nEnvRate = nGainSustainRate;
				// update envelope
				iEnvelope -= iEnvelope >> 8;					// decrement by x / 256

				// has decrease completed?
				if (iEnvelope <= 0)
				{
					iEnvelope = 0;
				}
				break;

			case SNSPCDSP_ENVSTATE_INCREASELINEAR:
				// get rate
				nEnvRate = nGainLinearRate;
				// update envelope
				iEnvelope += SNSPCDSP_ENVELOPE_MAX >> 6;		// increment by 1/64

				// has increase completed?
				if (iEnvelope >= SNSPCDSP_ENVELOPE_MAX)
				{
					iEnvelope = SNSPCDSP_ENVELOPE_MAX;
				}
				break;

			case SNSPCDSP_ENVSTATE_INCREASEBENTLINE:
				// get rate
				nEnvRate = nGainBentRate;

				if (iEnvelope >= (SNSPCDSP_ENVELOPE_MAX * 3 / 4 ))
				{
					// update envelope
					iEnvelope += SNSPCDSP_ENVELOPE_MAX >> 8;		// increment by 1/256
				} else
				{
					// update envelope
					iEnvelope += SNSPCDSP_ENVELOPE_MAX >> 6;		// increment by 1/64
				}

				// has increase completed?
				if (iEnvelope >= SNSPCDSP_ENVELOPE_MAX)
				{
					iEnvelope = SNSPCDSP_ENVELOPE_MAX;
				}
				break;

			case SNSPCDSP_ENVSTATE_DIRECT:
				iEnvelope = uGain << (SNSPCDSP_ENVELOPE_BITS - 7);
				nEnvCount = 0x10000000;
				nEnvRate  = 0;
				break;

			default:
			case SNSPCDSP_ENVSTATE_SILENCE:
				iEnvelope = 0;
				nEnvCount = 0x10000000;
				nEnvRate  = 0;
				break;
			}

			// increment envcount (number of ticks until next update)
			nEnvCount += nEnvRate;
		}


		/* AURORA_AUDIO_ENVELOPE_RUNS_V3
		 * nEnvCount is 16.16 samples-until-update. The envelope cannot
		 * change while it remains positive, so emit that constant run
		 * together. ceil(count / 65536) reproduces the old <= 0 test
		 * exactly; a non-positive post-update count still emits the one
		 * current sample before the next update, just like the old loop. */
		Int32 nRun = (nEnvCount > 0)
			? ((nEnvCount + 0xFFFF) >> 16) : 1;
		if (nRun > nSamples)
			nRun = nSamples;

		Uint8 uEnv = (Uint8)(iEnvelope >>
			(SNSPCDSP_ENVELOPE_BITS - 7));
		if (nRun >= 8)
		{
			memset(pOut, uEnv, (size_t)nRun);
		}
		else
		{
			Int32 i;
			for (i = 0; i < nRun; ++i)
				pOut[i] = uEnv;
		}

		pOut += nRun;
		nEnvCount -= nRun << 16;
		nSamples -= nRun;
	}

	// cleanup
	pChannel->eEnvState = eEnvState;
	pChannel->iEnvelope = iEnvelope;
	pChannel->nEnvCount = nEnvCount;

	// update envx
	pChannel->envx = iEnvelope >> (SNSPCDSP_ENVELOPE_BITS - 7);

	/* Envelope reaching zero does not assert ENDX; BRR end flags do. */

	PROF_LEAVE("SNSpcDspOutputEnvelope");
	return 1;
}

//
// full (real) mixer
//

void SNSpcDspMixFull::Reset()
{
	SNSpcDspMix::Reset();

	memset(&m_Echo, 0, sizeof(m_Echo));
	memset(m_EchoBuffer, 0, sizeof(m_EchoBuffer));
	m_iNoisePhase = 0;
	m_uNoiseGen   = 0x4000;
}

Int32 SNSpcDspMixFull::OutputNoise(Int16 *pOut, Uint16 *pFrac, Int32 nSamples, Uint32 uRate)
{
	Uint32 rate=uRate&31u;
	Uint32 counter=(Uint32)m_iNoisePhase;
	Uint32 noise=m_uNoiseGen&0x7FFFu;
	Uint32 period=_SNSpcDspCounterRate[rate];
	Uint32 offset=_SNSpcDspCounterOffset[rate];
	Uint32 eventCountdown=0;

	if(!noise) noise=0x4000;

	/* AURORA_TOPGEAR_ACCURACY_PERF_RECOVERY_V2_DSP2_20260917
	 * The S-DSP noise counter is free-running even with NON/NOV == 0.
	 * When nobody consumes the generated samples, advance the exact counter
	 * phase and exact number of LFSR events without writing the transient
	 * sample/fraction buffers.  All legal periods divide the 30720-step
	 * counter, so the event cadence remains identical across wrap. */
	if(!pOut || !pFrac)
	{
		Uint32 n=(nSamples > 0) ? (Uint32)nSamples : 0u;

		/* V1 never leaves 30720 after a positive sample, but accept that
		 * boundary representation from old/debug states: on the next sample
		 * it behaves exactly like stored zero. Preserve it when n==0. */
		if(n && counter==30720u) counter=0;

		if(rate && period && n)
		{
			Uint32 firstCounter=counter ? (counter-1u) : 30719u;
			Uint32 firstEvent=((firstCounter+offset)%period)+1u;
			Uint32 events=(n < firstEvent) ? 0u :
				(1u + (n-firstEvent)/period);
			while(events--)
				noise=(((noise<<13)^(noise<<14))&0x4000u)|(noise>>1);
		}

		if(n)
		{
			Uint32 step=n%30720u;
			if(step)
			{
				Uint32 start=counter ? counter : 30720u;
				if(step < start) counter=start-step;
				else if(step == start) counter=0;
				else counter=30720u-(step-start);
			}
			/* A complete 30720-sample revolution returns the stored counter
			 * to exactly the same representation, including zero. */
		}

		m_iNoisePhase=(Int32)counter;
		m_uNoiseGen=noise;
		return 1;
	}

	/* V1 generated-output path: one exact phase calculation per chunk.
	 * AURORA_SNES_SAFE_PERF_V7_20260919: noise rate is invariant for this call. Select once outside the
	 * output loop instead of retesting the same FLG rate every sample. */
	if(rate)
	{
		/* Every legal nonzero FLG rate has a nonzero period in the table. */
		Uint32 firstCounter=counter ? (counter-1u) : 30719u;
		eventCountdown=((firstCounter+offset)%period)+1u;

		while(nSamples-- > 0)
		{
			if(!counter) counter=30720;
			counter--;

			eventCountdown--;
			if(!eventCountdown)
			{
				noise=(((noise<<13)^(noise<<14))&0x4000u)|(noise>>1);
				eventCountdown=period;
			}

			Int16 s=(Int16)(noise<<1);
			pOut[0]=s; pOut[1]=s; pFrac[0]=0;
			pOut+=2; pFrac++;
		}
	}
	else
	{
		/* Rate 0 never clocks the LFSR; only the free-running master counter
		 * and the exact repeated current-noise sample advance. */
		while(nSamples-- > 0)
		{
			if(!counter) counter=30720;
			counter--;

			Int16 s=(Int16)(noise<<1);
			pOut[0]=s; pOut[1]=s; pFrac[0]=0;
			pOut+=2; pFrac++;
		}
	}
	m_iNoisePhase=(Int32)counter;
	m_uNoiseGen=noise;
	return 1;
}

void SNSpcDspMixFull::FetchBlock(Int32 iChannel)
{
	SNSpcChannelT *pChannel = GetChannel(iChannel);
	Uint8 uFlags = 0;

	// copy previous samples
	pChannel->BlockData[0][14] = pChannel->BlockData[1][14];
	pChannel->BlockData[0][15] = pChannel->BlockData[1][15];

	// decode next block
	if (pChannel->uBlockAddr!=0)
	{
		PROF_ENTER("SNSpcBRRDecode");
		Uint16 uBrrAddr = (Uint16)pChannel->uBlockAddr;
		/* AURORA_SNES_BINARY_TRACE_V6_20260918_BRR_PRE
		 * AURORA_TRACE_OFF_PERF_V6_20260919: BRR trace host gate; decode/state stay outside. */
#if AURORA_RUNTIME_TRACE
		if (g_AuroraTraceEnabled)
		{
			AuroraTraceBrrPre(
			    iChannel, uBrrAddr,
			    (Int32)pChannel->BlockData[0][14],
			    (Int32)pChannel->BlockData[0][15]);
		}
#endif
		Uint8 *pBrrBlock = m_pDsp->GetRAMSpan(uBrrAddr, 9);

		/* AURORA_TOPGEAR_ACCURACY_PERF_RECOVERY_V1_20260917: almost every BRR block is contiguous physical APURAM.
		 * Keep byte-wise ReadRAM only for the real exceptional boundaries. */
		if (pBrrBlock)
		{
			uFlags = SNSpcBRRDecode(
				pBrrBlock, pChannel->BlockData[1],
				pChannel->BlockData[0][15], pChannel->BlockData[0][14]);
		}
		else
		{
			Uint8 BrrBlock[9];
			Int32 iBrrByte;
			for (iBrrByte=0; iBrrByte<9; ++iBrrByte)
			{
				BrrBlock[iBrrByte] = m_pDsp->ReadRAM(uBrrAddr);
				uBrrAddr = (Uint16)(uBrrAddr + 1);
			}
			uFlags = SNSpcBRRDecode(
				BrrBlock, pChannel->BlockData[1],
				pChannel->BlockData[0][15], pChannel->BlockData[0][14]);
		}
		PROF_LEAVE("SNSpcBRRDecode");
		/* AURORA_SNES_BINARY_TRACE_V6_20260918_BRR_POST
		 * AURORA_TRACE_OFF_PERF_V6_20260919: BRR trace host gate. */
#if AURORA_RUNTIME_TRACE
		if (g_AuroraTraceEnabled)
		{
			AuroraTraceBrrPost(
			    iChannel, uBrrAddr, uFlags,
			    (Int32)pChannel->BlockData[1][0],
			    (Int32)pChannel->BlockData[1][1],
			    (Int32)pChannel->BlockData[1][14],
			    (Int32)pChannel->BlockData[1][15]);
		}
#endif
		pChannel->uBlockAddr += 9;
	}
	else
	{
		SNSpcBRRClear(pChannel->BlockData[1], 0);
	}

	if (uFlags&1)
	{
		const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);
		pChannel->endx = TRUE;
		if (uFlags & 2)
			pChannel->uBlockAddr = m_pDsp->GetSampleDir(pRegs->srcn, 2);
		else
			pChannel->uBlockAddr = 0;
	}
}


/* AURORA_DKC_SPC_HOST_SAFETY_V1_HISTORY_20260917
 * BlockData is one 32-sample object arranged as two rows of 16.  Aurora keeps
 * two previous samples in row 0 and addresses them as phase -2/-1 while row 1
 * holds the current block.  Keep that physical layout, but never index before
 * or beyond a C++ row subobject.
 *
 * A legacy/restored state may also carry the end-of-batch phase that used to
 * reach 16..17 (or higher with PMON) before a block-address change.  Clamp the
 * history source to the last fully decoded pair rather than reading unrelated
 * channel/object memory. */
static _INLINE void _SNSpcDspCaptureInterpolationHistory(
	SNSpcChannelT *pChannel)
{
	Int32 iSample = pChannel->iPhase >> 16;
	Int16 *pAll = &pChannel->BlockData[0][0];
	Int16 s0, s1;

	if (iSample < -2) iSample = -2;
	if (iSample > 14) iSample = 14;

	/* row 1 starts at flat index 16; -2/-1 intentionally select row 0's
	 * final two samples, while 0..14 remain wholly inside row 1. */
	const Int32 iFlat = 16 + iSample;
	s0 = pAll[iFlat + 0];
	s1 = pAll[iFlat + 1];
	pChannel->BlockData[1][14] = s0;
	pChannel->BlockData[1][15] = s1;
}


Int32 SNSpcDspMixFull::OutputSample(Int32 iChannel, Int16 *pOut, Uint16 *pFrac, Int32 nSamples, Int32 nSampleRate)
{
	SNSpcChannelT *pChannel = GetChannel(iChannel);
	const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);
	Int32 iPhase;
	Int32 iPhaseInc;
	Uint32 uPitch;
	/* AURORA_SNES_SAFE_PERF_V7_20260919: pBlockBase address is invariant; FetchBlock changes contents only. */
	Int16 *pBlockBase = &pChannel->BlockData[0][0];

#if !SNSPCDSP_MIXSILENCE
	if (pChannel->uBlockAddr == 0)
	{
		pChannel->uOldBlockAddr = pChannel->uBlockAddr;
		// silent sample data
		return 0;
	}
#endif

	PROF_ENTER("SNSpcDspOutputSample");


	// did something external change the block address?
	// fix to keep interpolation correct
	if (pChannel->uBlockAddr!= pChannel->uOldBlockAddr)
	{
		// this is done to ensure interpolation is correct from old sample to new sample
		_SNSpcDspCaptureInterpolationHistory(pChannel);

		// trigger decode, retain fractional component 
		pChannel->iPhase &= 0xFFFF;
		pChannel->iPhase |= 14 << 16;		
	}



	// get pitch
	uPitch = pRegs->pitch_lo | (pRegs->pitch_hi<<8);
	uPitch&= 0x3FFF;

	iPhase = pChannel->iPhase;
	
	// output at correct pitch based on sample rate
	iPhaseInc = _SNSpcDspPhaseInc(uPitch, nSampleRate);


	if (!pOut || !pFrac)
	{
		while (nSamples > 0)
		{
			if (iPhase >= (14 << 16))
			{
				FetchBlock(iChannel);
				iPhase -= (16 << 16);
			}
			iPhase += iPhaseInc;
			nSamples--;
		}
	}
	else
	{
		while (nSamples > 0)
		{
			Int16 *pSample;
			Int32 iSample0, iSample1;

			if (iPhase >= (14 << 16))
			{
				FetchBlock(iChannel);
				iPhase -= (16 << 16);
			}

			Int32 iSampleIndex = 16 + (iPhase >> 16);
			pSample = pBlockBase + iSampleIndex;
			iSample0 = pSample[0];
			iSample1 = pSample[1];
			pFrac[0]= (Uint16)iPhase;
			pFrac++;
			pOut[0] = iSample0;
			pOut[1] = iSample1;
			pOut+=2;
			iPhase+= iPhaseInc;
			nSamples--;
		}
	}


	// voice ended?
	/* AURORA_SNES_SAFE_PERF_V7_20260919: uBlockAddr is not mutated between these two observations. */
	const Bool bVoiceEnded = (pChannel->uBlockAddr == 0);
	if (bVoiceEnded)
	{
		pChannel->eEnvState = SNSPCDSP_ENVSTATE_SILENCE;
		pChannel->endx = TRUE;
	}

	// set outx to be envx for now
	pChannel->outx = bVoiceEnded ? 0 : pChannel->envx;

	pChannel->uOldBlockAddr = pChannel->uBlockAddr;
	pChannel->iPhase = iPhase;
	PROF_LEAVE("SNSpcDspOutputSample");
	return 1;
}


/* AURORA_SNES_PMON_NOISE_V3_20260822
 *
 * PMON used to call the same OutputSample() path as a normal voice, making
 * pitch modulation a no-op. Keep ordinary voices untouched and use this path
 * only when the PMON bit is set for the current voice.
 */
Int32 SNSpcDspMixFull::OutputSampleModulated(
        Int32 iChannel, Int16 *pOut, Uint16 *pFrac,
        const Int16 *pPitchMod, Int32 nSamples, Int32 nSampleRate)
{
        SNSpcChannelT *pChannel = GetChannel(iChannel);
        const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);
        Int32 iPhase;
        Uint32 uPitch;
        /* AURORA_SNES_SAFE_PERF_V7_20260919: same invariant physical BlockData base as normal voices. */
        Int16 *pBlockBase = &pChannel->BlockData[0][0];

#if !SNSPCDSP_MIXSILENCE
        if (pChannel->uBlockAddr == 0)
        {
                pChannel->uOldBlockAddr = pChannel->uBlockAddr;
                return 0;
        }
#endif

        PROF_ENTER("SNSpcDspOutputSamplePMON");

        if (pChannel->uBlockAddr != pChannel->uOldBlockAddr)
        {
                _SNSpcDspCaptureInterpolationHistory(pChannel);
                pChannel->iPhase &= 0xFFFF;
                pChannel->iPhase |= 14 << 16;
        }

        uPitch = pRegs->pitch_lo | (pRegs->pitch_hi << 8);
        uPitch &= 0x3FFF;
        iPhase = pChannel->iPhase;

        /* AURORA_SNES_SAFE_PERF_V7_20260919: PMON native-rate specialization.
         * nSampleRate is invariant for this whole batch. The two loop
         * expansions below are identical except for the exact old helper's
         * two phase-increment expressions. */
#define AURORA_PMON_ADVANCE_LOOP(_PHASE_INC_EXPR) do { \
        while (nSamples > 0) \
        { \
                Int32 iModPitch; \
                if (iPhase >= (14 << 16)) \
                { \
                        FetchBlock(iChannel); \
                        iPhase -= (16 << 16); \
                } \
                iModPitch = (Int32)uPitch; \
                iModPitch += \
                        (((Int32)(*pPitchMod) >> 5) * \
                         (Int32)uPitch) >> 10; \
                pPitchMod++; \
                iPhase += (_PHASE_INC_EXPR); \
                nSamples--; \
        } \
} while (0)

#define AURORA_PMON_SAMPLE_LOOP(_PHASE_INC_EXPR) do { \
        while (nSamples > 0) \
        { \
                Int16 *pSample; \
                Int32 iSample0, iSample1; \
                Int32 iModPitch; \
                if (iPhase >= (14 << 16)) \
                { \
                        FetchBlock(iChannel); \
                        iPhase -= (16 << 16); \
                } \
                Int32 iSampleIndex = 16 + (iPhase >> 16); \
                pSample = pBlockBase + iSampleIndex; \
                iSample0 = pSample[0]; \
                iSample1 = pSample[1]; \
                pFrac[0] = (Uint16)iPhase; \
                pFrac++; \
                pOut[0] = iSample0; \
                pOut[1] = iSample1; \
                pOut += 2; \
                iModPitch = (Int32)uPitch; \
                iModPitch += \
                        (((Int32)(*pPitchMod) >> 5) * \
                         (Int32)uPitch) >> 10; \
                pPitchMod++; \
                iPhase += (_PHASE_INC_EXPR); \
                nSamples--; \
        } \
} while (0)

        if (!pOut || !pFrac)
        {
                if (nSampleRate == SNSPCDSP_SAMPLERATE)
                        AURORA_PMON_ADVANCE_LOOP((Int32)((Uint32)iModPitch << 4));
                else
                        AURORA_PMON_ADVANCE_LOOP(
                                (Int32)(((Uint32)iModPitch *
                                        SNSPCDSP_SAMPLERATE /
                                        nSampleRate) << 4));
        }
        else if (nSampleRate == SNSPCDSP_SAMPLERATE)
        {
                AURORA_PMON_SAMPLE_LOOP((Int32)((Uint32)iModPitch << 4));
        }
        else
        {
                AURORA_PMON_SAMPLE_LOOP(
                        (Int32)(((Uint32)iModPitch *
                                SNSPCDSP_SAMPLERATE /
                                nSampleRate) << 4));
        }
#undef AURORA_PMON_SAMPLE_LOOP
#undef AURORA_PMON_ADVANCE_LOOP

        /* AURORA_SNES_SAFE_PERF_V7_20260919: same no-mutation ended-test reuse as normal voices. */
        const Bool bVoiceEnded = (pChannel->uBlockAddr == 0);
        if (bVoiceEnded)
        {
                pChannel->eEnvState = SNSPCDSP_ENVSTATE_SILENCE;
                pChannel->endx = TRUE;
        }

        /* Preserve Aurora's pre-v2 externally visible OUTX approximation.
         * PMON uses its own transient per-sample data below. */
        pChannel->outx = bVoiceEnded ? 0 : pChannel->envx;
        pChannel->uOldBlockAddr = pChannel->uBlockAddr;
        pChannel->iPhase = iPhase;
        PROF_LEAVE("SNSpcDspOutputSamplePMON");
        return 1;
}







#if SNSPCDSP_MIXASM

__attribute__((noinline))
void _MixChannel(Int32 *pOutLeft, Int32 *pOutRight, Int16 *pIn, Uint8 *pEnvelope, Uint16 *pFrac, Int32 nSamples, Int32 iVolLeft, Int32 iVolRight)
{
	__asm__ (
		"pcpyh       %0,%0           \n"    
		"pcpyld      %0,%0,%0           \n"    
		"pcpyh       %1,%1           \n"    
		"pcpyld      %1,%1,%1           \n"    

		: "+r" (iVolLeft), "+r" (iVolRight)
		);    


	__asm__ __volatile__ (
		".set noreorder \n"
		".align 3           \n"
		"_MixChannelPS2_Loop:         \n"
		"lq         $10,0x00(%1)     \n"    // $10 = 8x frac bits (0.0.16)
		"lq          $8,0x00(%0)     \n"    // $8  = 4x sample pairs (1.15.0)
		"pnor       $11,$10,$0       \n"    // $11 = inv frac bits
		"lq          $9,0x10(%0)     \n"    // $9  = 4x sample pairs (1.15.0)
		"pextlh     $12,$10,$11      \n"    // $12 = invfrac, frac x 4  0.0.16
		"pextuh     $13,$10,$11      \n"    // $13 = invfrac, frac x 4  0.0.16

		"psrlh      $12,$12,1        \n"    // $12 = invfrace frac x 4  1.0.15
		"psrlh      $13,$13,1        \n"    // $13 = invfrace frac x 4  1.0.15

		"ld         $10,0x00(%4)     \n"    // $10 = 8x8 envelope 0.1.7   

		"phmadh     $8,$8,$12        \n"    // $8  = 4 interpolated samplse 2.15.15
		"pextlb     $10,$0,$10       \n"    // $10 =  8x8 envelope  8.1.7
		"phmadh     $9,$9,$13        \n"    // $9  = 4 interpolated samplse 2.15.15

		"pextuh     $11,$0,$10       \n"    // $11 = 4x16 envelope 24.1.7
		"pextlh     $10,$0,$10       \n"    // $10 = 4x16 envelope 24.1.7

		"psraw      $8,$8,15         \n"    // $8 = 32-bit interpolated samples 17.15.0
		"pmulth     $8,$8,$10        \n"    // $8 = 32-bit sample * envelope  10.15.7

		"psraw      $9,$9,15         \n"    // $9 = 32-bit interpolated samples 17.15.0
		"pmulth     $9,$9,$11        \n"    // $9 = 32-bit sample * envelope  10.15.7

		"addiu      %0,%0,0x20       \n"    // pInn+=16
		"addiu      %1,%1,0x10       \n"    // pFrac+=8
		"addiu      %4,%4,0x08       \n"    // pEnvelope+=8

		"psraw      $8,$8,7          \n"    // $8 = 32-bit sample * envelope 17.15.0
		"pmulth     $10,$8,%6        \n"    // $10= right  32-bit sample * envelope * volr  .15.14
		"psraw      $9,$9,7          \n"    // $9 = 32-bit sample * envelope 17.15.0
		"pmulth     $11,$9,%6        \n"    // $11= right  32-bit sample * envelope * volr  .15.14

		"pmulth     $8,$8,%5        \n"     // $8 = left   32-bit sample * envelope * voll  .15.14
		"lq         $12,0x00(%2)     \n"    // $12 = outl0
		"pmulth     $9,$9,%5        \n"     // $9 = left   32-bit sample * envelope * voll  .15.14
		"lq         $13,0x10(%2)     \n"    // $13 = outl1
		"lq         $14,0x00(%3)     \n"    // $14 = outr0
		"lq         $15,0x10(%3)     \n"    // $15 = outr1

		"paddsw		$12,$12,$8       \n"
		"paddsw		$13,$13,$9       \n"
		"paddsw		$14,$14,$10      \n"
		"paddsw		$15,$15,$11      \n"

		"sq         $12,0x00(%2)     \n"    // $12 = outl0
		"sq         $13,0x10(%2)     \n"    // $13 = outl1
		"sq         $14,0x00(%3)     \n"    // $12 = outr0
		"sq         $15,0x10(%3)     \n"    // $12 = outr1

		"addiu      %7,%7,-8         \n"
		"addiu      %2,%2,0x20       \n"

		"bgtz       %7,_MixChannelPS2_Loop \n"
		"addiu      %3,%3,0x20       \n"

		".set reorder \n"

		: 
	: "r" (pIn), "r" (pFrac), "r" (pOutLeft), "r" (pOutRight), "r" (pEnvelope), "r" (iVolLeft), "r" (iVolRight), "r" (nSamples)
		: "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15"
		);    
}


__attribute__((noinline))
void _MixChannelEcho(Int32 *pOutLeft, Int32 *pOutRight, Int16 *pEchoLeft, Int16 *pEchoRight, Int16 *pIn, Uint8 *pEnvelope, Uint16 *pFrac, Int32 nSamples, Int32 iVolLeft, Int32 iVolRight)
{
	__asm__ (
		"pcpyh       %0,%0           \n"    
		"pcpyld      %0,%0,%0           \n"    
		"pcpyh       %1,%1           \n"    
		"pcpyld      %1,%1,%1           \n"    

		: "+r" (iVolLeft), "+r" (iVolRight)
		);    


	__asm__ __volatile__ (
		".set noreorder \n"
		".align 3           \n"
		"_MixChannelEchoPS2_Loop:         \n"
		"lq         $10,0x00(%1)     \n"    // $10 = 8x frac bits (0.0.16)
		"lq          $8,0x00(%0)     \n"    // $8  = 4x sample pairs (1.15.0)
		"pnor       $11,$10,$0       \n"    // $11 = inv frac bits
		"lq          $9,0x10(%0)     \n"    // $9  = 4x sample pairs (1.15.0)
		"pextlh     $12,$10,$11      \n"    // $12 = invfrac, frac x 4  0.0.16
		"pextuh     $13,$10,$11      \n"    // $13 = invfrac, frac x 4  0.0.16

		"psrlh      $12,$12,1        \n"    // $12 = invfrace frac x 4  1.0.15
		"psrlh      $13,$13,1        \n"    // $13 = invfrace frac x 4  1.0.15

		"ld         $10,0x00(%4)     \n"    // $10 = 8x8 envelope 0.1.7   

		"phmadh     $8,$8,$12        \n"    // $8  = 4 interpolated samplse 2.15.15
		"pextlb     $10,$0,$10       \n"    // $10 =  8x8 envelope  8.1.7
		"phmadh     $9,$9,$13        \n"    // $9  = 4 interpolated samplse 2.15.15

		"pextuh     $11,$0,$10       \n"    // $11 = 4x16 envelope 24.1.7
		"pextlh     $10,$0,$10       \n"    // $10 = 4x16 envelope 24.1.7

		"psraw      $8,$8,15         \n"    // $8 = 32-bit interpolated samples 17.15.0
		"pmulth     $8,$8,$10        \n"    // $8 = 32-bit sample * envelope  10.15.7

		"psraw      $9,$9,15         \n"    // $9 = 32-bit interpolated samples 17.15.0
		"pmulth     $9,$9,$11        \n"    // $9 = 32-bit sample * envelope  10.15.7

		"addiu      %0,%0,0x20       \n"    // pInn+=16
		"addiu      %1,%1,0x10       \n"    // pFrac+=8
		"addiu      %4,%4,0x08       \n"    // pEnvelope+=8

		"psraw      $8,$8,7          \n"    // $8 = 32-bit sample * envelope 17.15.0
		"pmulth     $10,$8,%6        \n"    // $10= right  32-bit sample * envelope * volr  .15.14
		"psraw      $9,$9,7          \n"    // $9 = 32-bit sample * envelope 17.15.0
		"pmulth     $11,$9,%6        \n"    // $11= right  32-bit sample * envelope * volr  .15.14

		"pmulth     $8,$8,%5        \n"     // $8 = left   32-bit sample * envelope * voll  .15.14
		"lq         $12,0x00(%2)     \n"    // $12 = outl0
		"pmulth     $9,$9,%5        \n"     // $9 = left   32-bit sample * envelope * voll  .15.14
		"lq         $13,0x10(%2)     \n"    // $13 = outl1
		"lq         $14,0x00(%3)     \n"    // $14 = outr0
		"lq         $15,0x10(%3)     \n"    // $15 = outr1

		"paddsw		$12,$12,$8       \n"
		"paddsw		$13,$13,$9       \n"
		"paddsw		$14,$14,$10      \n"
		"paddsw		$15,$15,$11      \n"

		"sq         $12,0x00(%2)     \n"    // $12 = outl0
		"sq         $13,0x10(%2)     \n"    // $13 = outl1
		"sq         $14,0x00(%3)     \n"    // $12 = outr0
		"sq         $15,0x10(%3)     \n"    // $12 = outr1

		"psraw      $10,$10,7        \n"    // $10 = 32-bit sample * envelope * volr  17.15.0
		"psraw      $11,$11,7        \n"    // $11 = 32-bit sample * envelope * volr  17.15.0
		"psraw      $8,$8,7          \n"    // $8  = 32-bit sample * envelope * voll  17.15.0
		"psraw      $9,$9,7          \n"    // $9  = 32-bit sample * envelope * voll  17.15.0

		"lq         $12,0x00(%8)     \n"    // $12 = outl
		"lq         $13,0x00(%9)     \n"    // $13 = outr
		"ppach      $8,$9,$8         \n"    // $8 = left   1.15.0
		"ppach      $9,$11,$10       \n"    // $9 = right  1.15.0
		"paddsh     $12,$12,$8       \n"    // $12 = outl + samples
		"paddsh     $13,$13,$9       \n"    // $13 = outr + samples
		"sq         $12,0x00(%8)     \n"    // store outl
		"sq         $13,0x00(%9)     \n"    // store outr
		"addiu      %8,%8,0x10       \n"
		"addiu      %9,%9,0x10       \n"

		"addiu      %7,%7,-8         \n"
		"addiu      %2,%2,0x20       \n"

		"bgtz       %7,_MixChannelEchoPS2_Loop \n"
		"addiu      %3,%3,0x20       \n"

		".set reorder \n"

		: 
	: "r" (pIn), "r" (pFrac), "r" (pOutLeft), "r" (pOutRight), "r" (pEnvelope), "r" (iVolLeft), "r" (iVolRight), "r" (nSamples), "r" (pEchoLeft), "r" (pEchoRight)
		: "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15"
		);    
}




#else

void _MixChannel(Int32 *pOutLeft, Int32 *pOutRight, Int16 *pIn, Uint8 *pEnvelope, Uint16 *pFrac, Int32 nSamples, Int32 iVolLeft, Int32 iVolRight)
{
	while (nSamples > 0)
	{
		Int32 iSample1;
		Int32 iFrac0, iFrac1;
		Int32 iSample, iSampleLeft, iSampleRight;
		Int32 iEnvelope;

		iSample  = pIn[0];
		iSample1 = pIn[1];
		
		iFrac0   = pFrac[0] >> 1;
		iFrac1   = iFrac0 ^ 0x7FFF;
		
		iSample  =  iSample * iFrac1 + iSample1 * iFrac0;
		iSample >>= 15;

		iEnvelope = *pEnvelope;
		iSample *= iEnvelope;
		iSample >>= 7;

		iSampleLeft  = iSample * iVolLeft;
		iSampleRight = iSample * iVolRight;

		*pOutLeft  += iSampleLeft;
		*pOutRight += iSampleRight;

		pEnvelope++;
		pOutLeft++;
		pOutRight++;
		pIn+=2;
		pFrac++;
		nSamples--;
	}
}


void _MixChannelEcho(Int32 *pOutLeft, Int32 *pOutRight, SNSpcEchoSampleT *pEchoLeft, SNSpcEchoSampleT *pEchoRight, Int16 *pIn, Uint8 *pEnvelope, Uint16 *pFrac, Int32 nSamples, Int32 iVolLeft, Int32 iVolRight)
{
	while (nSamples > 0)
	{
		Int32 iSample1;
		Int32 iFrac0, iFrac1;
		Int32 iSample, iSampleLeft, iSampleRight;
		Int32 iEnvelope;

		iSample  = pIn[0];
		iSample1 = pIn[1];
		
		iFrac0   = pFrac[0] >> 1;
		iFrac1   = iFrac0 ^ 0x7FFF;
		
		iSample  =  iSample * iFrac1 + iSample1 * iFrac0;
		iSample >>= 15;

		iEnvelope = *pEnvelope;
		iSample *= iEnvelope;
		iSample >>= 7;

		iSampleLeft  = iSample * iVolLeft;
		iSampleRight = iSample * iVolRight;

		*pOutLeft  += iSampleLeft;
		*pOutRight += iSampleRight;
		iSampleLeft >>= 7;
		iSampleRight >>= 7;


		iSampleLeft += *pEchoLeft;
		iSampleRight += *pEchoRight;

#if 0
		if (iSampleLeft > 0x7FFF)
		{
			ConDebug("%d\n", iSampleLeft);
			iSampleLeft = 0x7FFF;
		}
		if (iSampleLeft < -0x8000)
		{
			ConDebug("%d\n", iSampleLeft);
			iSampleLeft = -0x8000;
		}
		if (iSampleRight > 0x7FFF)
		{
			ConDebug("%d\n", iSampleRight);
			iSampleRight = 0x7FFF;
		}
		if (iSampleRight < -0x8000)
		{
			ConDebug("%d\n", iSampleRight);
			iSampleRight = -0x8000;
		}
#endif


		*pEchoLeft  = iSampleLeft;
		*pEchoRight = iSampleRight;

		pEchoLeft++;
		pEchoRight++;

		pEnvelope++;
		pOutLeft++;
		pOutRight++;
		pIn+=2;
		pFrac++;
		nSamples--;
	}
}

#endif




static void _SNSpcDspMemset64(Uint64 *pDest, Int32 nDwords)
{
	while (nDwords>=4)
	{
		pDest[0] = 0;
		pDest[1] = 0;
		pDest[2] = 0;
		pDest[3] = 0;
		pDest+=4;
		nDwords-=4;
	}

	while (nDwords > 0)
	{
		pDest[0] = 0;
		pDest+=1;
		nDwords-=1;
	}
}


#if SNSPCDSP_MIXASM

#if 1
__attribute__((noinline))
void _MixEcho(Int16 *pOut, Int32 *pMain, Int16 *pEcho, Int32 nSamples, Int32 iMainVol, Int32 iEchoVol)
{

	__asm__ __volatile__ (
		"pcpyh       %4,%4           \n"    
		"pcpyld      %4,%4,%4           \n"    
		"pcpyh       %5,%5           \n"    
		"pcpyld      %5,%5,%5           \n"    

		"pnor		 $14, $0,$0			\n"
		"psrlw		 $14,$14,17         \n" // 7FFF
		"pnor		 $15, $0,$0			\n"
		"psllw		 $15,$15,15         \n" // 8000

		".set noreorder \n"
		".align 3           \n"
		"_MixEchoPS2_Loop:         \n"
		"lq          $8,0x00(%1)     \n"    // $8 = 4x main samples
		"lq          $9,0x10(%1)     \n"    // $9 = 4x main samples
		"lq         $10,0x00(%2)     \n"    // $10  = 8x echo samples
		"psraw		 $8,$8,7		 \n"
		"psraw		 $9,$9,7		 \n"
		"pminw       $8,$8,$14       \n"
		"pminw       $9,$9,$14       \n"
		"pmaxw       $8,$8,$15       \n"
		"pmaxw       $9,$9,$15       \n"
		"ppach		 $8,$9,$8         \n" 
		"pmulth		 $0,$8,%4         \n"     // lo = 5 4 1 0
		"pmaddh		 $0,$10,%5        \n"     // hi = 7 6 3 2

		"pmflo		 $8				\n"  // 8 = 5 4 1 0
		"pmfhi		 $9				\n"  // 9 = 7 6 3 2 
		"psraw		 $8,$8,6		 \n"
		"psraw		 $9,$9,6		 \n"
		"pminw       $8,$8,$14       \n"
		"pminw       $9,$9,$14       \n"
		"pmaxw       $8,$8,$15       \n"
		"pmaxw       $9,$9,$15       \n"
		"pcpyld		$10,$9,$8       \n"    // 3 2 1 0
 		"pcpyud		$11,$8,$9       \n"    // 7 6 5 4 
		"ppach		$10,$11,$10        \n" // 76543210
		"sq			$10,0x00(%0)     \n" 
		"addiu      %0,%0,0x10         \n"

		"addiu      %3,%3,-8         \n"
		"addiu      %1,%1,0x20       \n"
		"bgtz       %3,_MixEchoPS2_Loop \n"
		"addiu      %2,%2,0x10       \n"

		".set reorder \n"

		: 
		: "r" (pOut), "r" (pMain), "r" (pEcho), "r" (nSamples), "r" (iMainVol), "r" (iEchoVol)
		: "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15"
		);    
}
#endif

#else

/*

void _MixEcho(Int16 *pOut, Int32 *pMain, Int16 *pEcho, Int32 nSamples, Int32 iMainVol, Int32 iEchoVol)
{
	Int32 iMin = -0x8000;
	Int32 iMax = 0x7FFF;

	while (nSamples > 0)
	{
		Int32 iSample;

		// mix main + echo
		iSample  = pMain[0];
		iSample >>= 7;
		iSample *= iMainVol;           // (1.15.14)

		iSample += pEcho[0] * iEchoVol;        // (1.15.14)
		iSample >>= 6;
		if (iSample >  iMax) iSample = iMax;
		if (iSample <  iMin) iSample = iMin;
		pOut[0] = iSample;           // (1.15.0)

		pOut++;
		pMain++;
		pEcho++;
		nSamples--;
	}
}
*/

void _MixEcho(Int16 *pOut, Int32 *pMain, SNSpcEchoSampleT *pEcho, Int32 nSamples, Int32 iMainVol, Int32 iEchoVol)
{
	Int32 iMin = -0x8000;
	Int32 iMax = 0x7FFF;

	iEchoVol <<= 7;
	while (nSamples > 0)
	{
		Int32 iSample0, iSample1;

		// mix main + echo
		iSample0  = pMain[0] * iMainVol;           // (1.15.14)
		iSample0 += pEcho[0] * iEchoVol;        // (1.15.14)
		iSample0 >>= 14 - 1;

		iSample1  = pMain[1] * iMainVol;           // (1.15.14)
		iSample1 += pEcho[1] * iEchoVol;        // (1.15.14)
		iSample1 >>= 14 - 1;

		if (iSample0 >  iMax) iSample0 = iMax;
		if (iSample0 <  iMin) iSample0 = iMin;

		if (iSample1 >  iMax) iSample1 = iMax;
		if (iSample1 <  iMin) iSample1 = iMin;

		pOut[0] = iSample0;           // (1.15.0)
		pOut[1] = iSample1;           // (1.15.0)

		pOut+=2;
		pMain+=2;
		pEcho+=2;
		nSamples-=2;
	}
}
#endif


/*
 
            FLG(ECEN) ESA EDL    C0-C7           |
                 \/  \/  \/      \/     EVOL(L) |
                 *--------*   *------*    \/    |   *--------*
>-------------X->|External|-->|FIR   |-----X--->+-->|Parallel|---> Left D/O
 L-CH ECHO   /\  |Memory  |   |Filter| \/           |Serial  |
              |  *--------*   *------*  |           *--------*
              |                         |
              --------------X------------
                           /\
                           EFB

 */

static _INLINE Int32 _SNSpcClamp16(Int32 v)
{
	if(v>32767)return 32767;
	if(v<-32768)return -32768;
	return v;
}
static _INLINE Int16 _SNSpcEchoRead16(SNSpcDsp *dsp,Uint32 a)
{
	Uint16 addr=(Uint16)a;
	Uint16 lo=dsp->ReadRAM(addr);
	Uint16 hi=dsp->ReadRAM((Uint16)(addr+1));
	return (Int16)(lo|(hi<<8));
}
static _INLINE void _SNSpcEchoWrite16(SNSpcDsp *dsp,Uint32 a,Int16 v)
{
	Uint16 addr=(Uint16)a,u=(Uint16)v;
	dsp->WriteRAM(addr,(Uint8)u);
	dsp->WriteRAM((Uint16)(addr+1),(Uint8)(u>>8));
}
static _INLINE Int32 _SNSpcEchoFIR(const Int16 *l,const Int16 *c)
{
	/* AURORA_SNES_SAFE_PERF_V2_20260919
	 * Fixed-size FIR: preserve the exact original left-to-right Int32
	 * accumulation order, including the Int16 truncation before tap 7. */
	Int32 s=0;
	s+=((Int32)l[0]*c[0])>>6;
	s+=((Int32)l[1]*c[1])>>6;
	s+=((Int32)l[2]*c[2])>>6;
	s+=((Int32)l[3]*c[3])>>6;
	s+=((Int32)l[4]*c[4])>>6;
	s+=((Int32)l[5]*c[5])>>6;
	s+=((Int32)l[6]*c[6])>>6;
	s=(Int16)s;
	s+=(Int16)(((Int32)l[7]*c[7])>>6);
	return _SNSpcClamp16(s)&~1;
}

static Uint32 _FilterEchoStereoARAM(SNSpcEchoSampleT *L,SNSpcEchoSampleT *R,Int32 n,Int32 fb,SNSpcDsp *dsp,Uint32 base,Uint32 pos,Uint32 size,const Int16 *coef,SNSpcFIRFilterT *f,Bool wr,Bool inputZero)
{
	Int32 fp=f[0].iPos;
	Uint8 *pLinearRam=dsp->GetLinearPhysicalRAM();
	/* AURORA_SNES_SAFE_PERF_V2_20260919: FilterEcho() already canonicalizes zero size to 4. */
	if(pos>=size)pos%=size;
	/* AURORA_SNES_SAFE_PERF_V9_20260919
	 * AURORA_SNES_SAFE_PERF_V9_ECHO_20260919
	 * pLinearRam is already a once-per-call snapshot and wr is a by-value
	 * call invariant. Select those decisions outside the 32-kHz sample loop.
	 *
	 * The common direct path keeps the exact FIR/history/feedback order.
	 * When writes are disabled, inL/inR and feedback wl/wrv were dead: their
	 * only consumer was the disabled RAM write, so that arithmetic is omitted. */
	if(pLinearRam)
	{
		if(wr)
		{
			/* direct linear APURAM, writes enabled */
			while(n-- > 0)
			{
				Int32 inL=inputZero?0:*L,inR=inputZero?0:*R,rdL,rdR,fl,fr,wl,wrv;
				Int16 *ll,*rr;
				/* AURORA_SNES_SAFE_PERF_V9_20260919: Uint16 conversion is exactly modulo 65536. */
				Uint16 a=(Uint16)(base+pos);
				Uint16 a1=(Uint16)(a+1);
				Uint16 a2=(Uint16)(a+2);
				Uint16 a3=(Uint16)(a+3);

				rdL=(Int16)((Uint16)pLinearRam[a]|((Uint16)pLinearRam[a1]<<8));
				rdR=(Int16)((Uint16)pLinearRam[a2]|((Uint16)pLinearRam[a3]<<8));

				ll=&f[0].Line[fp&7]; rr=&f[1].Line[fp&7]; fp--;
				ll[0]=ll[8]=(Int16)(rdL>>1);
				rr[0]=rr[8]=(Int16)(rdR>>1);
				fl=_SNSpcEchoFIR(ll,coef); fr=_SNSpcEchoFIR(rr,coef);
				*L=(Int16)fl; *R=(Int16)fr;
				wl=_SNSpcClamp16(inL+(Int16)((fl*fb)>>7))&~1;
				wrv=_SNSpcClamp16(inR+(Int16)((fr*fb)>>7))&~1;

				Uint16 uL=(Uint16)(Int16)wl;
				Uint16 uR=(Uint16)(Int16)wrv;
				pLinearRam[a]=(Uint8)uL;
				pLinearRam[a1]=(Uint8)(uL>>8);
				pLinearRam[a2]=(Uint8)uR;
				pLinearRam[a3]=(Uint8)(uR>>8);

				pos+=4; if(pos>=size)pos=0; L++; R++;
			}
		}
		else
		{
			/* direct linear APURAM, writes disabled:
			 * echo input and feedback are unobservable because FLG.5 blocks
			 * the only state write that consumes them. */
			while(n-- > 0)
			{
				Int32 rdL,rdR,fl,fr;
				Int16 *ll,*rr;
				Uint16 a=(Uint16)(base+pos);
				Uint16 a1=(Uint16)(a+1);
				Uint16 a2=(Uint16)(a+2);
				Uint16 a3=(Uint16)(a+3);

				rdL=(Int16)((Uint16)pLinearRam[a]|((Uint16)pLinearRam[a1]<<8));
				rdR=(Int16)((Uint16)pLinearRam[a2]|((Uint16)pLinearRam[a3]<<8));

				ll=&f[0].Line[fp&7]; rr=&f[1].Line[fp&7]; fp--;
				ll[0]=ll[8]=(Int16)(rdL>>1);
				rr[0]=rr[8]=(Int16)(rdR>>1);
				fl=_SNSpcEchoFIR(ll,coef); fr=_SNSpcEchoFIR(rr,coef);
				*L=(Int16)fl; *R=(Int16)fr;

				pos+=4; if(pos>=size)pos=0; L++; R++;
			}
		}
	}
	else
	{
		/* wrapped/overlay path: preserve the exact ReadRAM/WriteRAM helpers.
		 * The obsolete per-sample pLinearRam test is gone; the write gate stays
		 * here because this is the rare IPL-overlay fallback. */
		while(n-- > 0)
		{
			Int32 inL=inputZero?0:*L,inR=inputZero?0:*R,rdL,rdR,fl,fr;
			Int16 *ll,*rr;
			Uint16 a=(Uint16)(base+pos);

			rdL=_SNSpcEchoRead16(dsp,a);
			rdR=_SNSpcEchoRead16(dsp,(Uint16)(a+2));

			ll=&f[0].Line[fp&7]; rr=&f[1].Line[fp&7]; fp--;
			ll[0]=ll[8]=(Int16)(rdL>>1);
			rr[0]=rr[8]=(Int16)(rdR>>1);
			fl=_SNSpcEchoFIR(ll,coef); fr=_SNSpcEchoFIR(rr,coef);
			*L=(Int16)fl; *R=(Int16)fr;

			if(wr)
			{
				Int32 wl=_SNSpcClamp16(inL+(Int16)((fl*fb)>>7))&~1;
				Int32 wrv=_SNSpcClamp16(inR+(Int16)((fr*fb)>>7))&~1;
				_SNSpcEchoWrite16(dsp,a,(Int16)wl);
				_SNSpcEchoWrite16(dsp,(Uint16)(a+2),(Int16)wrv);
			}

			pos+=4; if(pos>=size)pos=0; L++; R++;
		}
	}
	f[0].iPos=fp; f[1].iPos=fp;
	return pos;
}
void SNSpcDspMixFull::FilterEcho(Int16 *L,Int16 *R,Int32 n,Int32 rate,Bool wr,Bool inputZero)
{
	Uint32 pos=m_Echo.uEchoAddr;
	Uint32 size=(m_pDsp->GetReg(SNSPCDSP_REG_EDL)&15)<<11;
	Uint32 base=((Uint32)m_pDsp->GetReg(SNSPCDSP_REG_ESA))<<8;
	Int16 c[8];
	if(rate!=SNSPCDSP_SAMPLERATE && size)size=size*rate/SNSPCDSP_SAMPLERATE;
	if(!size)size=4;
	c[0]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR0);c[1]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR1);c[2]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR2);c[3]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR3);c[4]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR4);c[5]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR5);c[6]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR6);c[7]=(Int8)m_pDsp->GetReg(SNSPCDSP_REG_ECHOFIR7);
	m_Echo.uEchoAddr=(Uint16)_FilterEchoStereoARAM(L,R,n,(Int8)m_pDsp->GetReg(SNSPCDSP_REG_EFB),m_pDsp,base,pos,size,c,m_Echo.Filter,wr,inputZero);
}


struct SNSpcDspDataT
{
	Int16 iSampleData[SNSPCDSP_BUFFERSIZE*2] _ALIGN(16);
	Uint16 FracData[SNSPCDSP_BUFFERSIZE] _ALIGN(16);
	Uint8 EnvData[SNSPCDSP_BUFFERSIZE] _ALIGN(16);

	Int16 PitchModA[SNSPCDSP_BUFFERSIZE] _ALIGN(16);
	Int16 PitchModB[SNSPCDSP_BUFFERSIZE] _ALIGN(16);

	SNSpcMixSampleT  Main[2][SNSPCDSP_BUFFERSIZE] _ALIGN(16);
    SNSpcEchoSampleT Echo[2][SNSPCDSP_BUFFERSIZE] _ALIGN(16);
};

#if CODE_PLATFORM == CODE_PS2
/* AURORA_SNES_BG_LOOKUP_SCRATCHPAD_V2_20260920: the SPC transient workspace begins at scratchpad+0 and must
 * remain below the PlaneLookup[0] reservation at 14 KiB. */
typedef char SNSpcScratchLookupLayoutCheck[
	(sizeof(SNSpcDspDataT) <= PS2MEM_SNES_LOOKUP_OFFSET) ? 1 : -1];
#endif


static void _SNSpcBuildPitchModOutput(
        Int16 *pOut, const Int16 *pIn, const Uint8 *pEnvelope,
        const Uint16 *pFrac, Int32 nSamples)
{
        while (nSamples > 0)
        {
                Int32 iFrac0 = pFrac[0] >> 1;
                Int32 iFrac1 = iFrac0 ^ 0x7FFF;
                Int32 iSample;

                iSample = pIn[0] * iFrac1 + pIn[1] * iFrac0;
                iSample >>= 15;
                iSample *= *pEnvelope;
                iSample >>= 7;

                /* AURORA_SNES_SAFE_PERF_V6_20260919: PMON feeder bounds.
                 * iFrac0+iFrac1 == 0x7fff, both pIn values are Int16, so the
                 * interpolated value is already [-32767,32766].
                 * OutputEnvelope emits 0..128 (23-bit max >> 16); the
                 * following *env >> 7 cannot enlarge that range.
                 * The old Int16 clamps therefore never changed a value. */
                iSample &= ~1;
                *pOut++ = (Int16)iSample;

                pIn += 2;
                pEnvelope++;
                pFrac++;
                nSamples--;
        }
}


void SNSpcDspMixFull::Mix(CMixBuffer *pMixBuf)
{
	static Int16 OutLeftData[SNSPCDSP_BUFFERSIZE] _ALIGN(16);
	static Int16 OutRightData[SNSPCDSP_BUFFERSIZE] _ALIGN(16);
#if !SNSPCDSP_INFOSCRATCHPAD
	SNSpcDspDataT Data;
#endif
	SNSpcDspDataT *pData;
	Int32 nTotalSamples, nSamples;
	Uint32 nSampleRate, nSampleChannels, nSampleBits;
	Uint32 uCycle=0;
	Uint32 uCyclesPerSample;
	Int32 nSamplesPerUpdate;

#if SNSPCDSP_INFOSCRATCHPAD
	pData = (SNSpcDspDataT *)PS2MEM_SCRATCHPAD;
#else
	pData = (SNSpcDspDataT *)&Data;
#endif

#if CODE_PLATFORM == CODE_PS2
	if (sizeof(SNSpcDspDataT) > 16384)
	{
		printf("%d\n",sizeof(SNSpcDspDataT));
		return;
	}
#endif

	if (!pMixBuf)
	{
		return;
	}

	pMixBuf->GetFormat(&nSampleRate, &nSampleBits, &nSampleChannels);
	if (nSampleBits!=16) return;

	// get number of samples needed to mix
	nTotalSamples = pMixBuf->GetOutputSamples();
	/* AURORA_SNES_BINARY_TRACE_V6D_SPARSE_HIGHSIGNAL_20260918_MIX: routine MIX event suppressed. */
#if SNDBG_LOG
	g_DbgAudioSamples += (Uint32)nTotalSamples;
#endif

	// build envelope lookup tables based on sample rate
	if (nSampleRate!=m_nSampleRate)
	{
		BuildLookupTables(nSampleRate);
		m_nSampleRate = nSampleRate;
	}

	// calculate number of cycles per sample
	uCyclesPerSample  = 32 * SNSPC_CYCLE;
	nSamplesPerUpdate = SNSPCDSP_MAXSAMPLES;

	// mix in chunks of <SNSPCDSP_MAXSAMPLES size for cache coherency
	while (nTotalSamples > 0)
	{
		Int32 iChannel;
		Uint8 uEchoEnable;
		Uint8 uPitchMod;
		Uint8 uNoiseEnable; /* AURORA_TOPGEAR_ACCURACY_PERF_RECOVERY_V2_DSP2_20260917 */
		Uint8 uFlags; /* AURORA_SNES_SAFE_PERF_V2_20260919: stable for this post-Sync mixer chunk */
		Int16 *pPitchModPrev = pData->PitchModA;
		Int16 *pPitchModNext = pData->PitchModB;

		// dequeue write queue up to current cycle time
		m_pDsp->Sync(uCycle);

		// EON remains active even while FLG protects echo writes.
		uEchoEnable = m_pDsp->GetReg(SNSPCDSP_REG_EON);

		/* Voice 0 cannot be pitch-modulated on real hardware. */
		uPitchMod = m_pDsp->GetReg(SNSPCDSP_REG_PMON) & 0xFE;
		uNoiseEnable = m_pDsp->GetReg(SNSPCDSP_REG_NOV);
		/* AURORA_SNES_SAFE_PERF_V2_20260919: no DSP Sync/write occurs again until the next chunk. */
		uFlags = m_pDsp->GetReg(SNSPCDSP_REG_FLG);
		const Int32 iMainVolL = (Int8)m_pDsp->GetReg(SNSPCDSP_REG_MVOLL);
		const Int32 iMainVolR = (Int8)m_pDsp->GetReg(SNSPCDSP_REG_MVOLR);
		const Int32 iEchoVolL = (Int8)m_pDsp->GetReg(SNSPCDSP_REG_EVOLL);
		const Int32 iEchoVolR = (Int8)m_pDsp->GetReg(SNSPCDSP_REG_EVOLR);

		// dont update more than samples-per-update at a time
		nSamples = nTotalSamples;
		if (nSamples > nSamplesPerUpdate) nSamples = nSamplesPerUpdate;

		// clear main and echo buffers
		/* AURORA_SNES_SAFE_PERF_V7_20260919: identical L/R byte spans; calculate each 64-bit count once. */
		const Int32 nMainClear64 =
			(Int32)((sizeof(Int32) * nSamples + 7) / 8);
		const Int32 nEchoClear64 =
			(Int32)((sizeof(SNSpcEchoSampleT) * nSamples + 7) / 8);
		_SNSpcDspMemset64((Uint64 *)pData->Main[0], nMainClear64);
		_SNSpcDspMemset64((Uint64 *)pData->Main[1], nMainClear64);
		if (uEchoEnable)
		{
			_SNSpcDspMemset64((Uint64 *)pData->Echo[0], nEchoClear64);
			_SNSpcDspMemset64((Uint64 *)pData->Echo[1], nEchoClear64);
		}

		// Noise/rate counter free-runs independent of NON selection.
		{
			PROF_ENTER("SNSpcDspOutputNoise");
			/* AURORA_TOPGEAR_ACCURACY_PERF_RECOVERY_V2_DSP2_20260917: only materialize the transient noise buffers if a voice
			 * actually consumes them; otherwise advance exact internal state. */
			if(uNoiseEnable)
				OutputNoise(m_iNoiseSample, m_iNoiseFrac, nSamples, (Uint32)(uFlags & 31u));
			else
				OutputNoise(NULL, NULL, nSamples, (Uint32)(uFlags & 31u));
			PROF_LEAVE("SNSpcDspOutputNoise");
		}

		/* MUTE gates DAC output, not internal DSP state. */
		if (TRUE)
		{
			for (iChannel=0; iChannel < SNSPCDSP_CHANNEL_NUM; iChannel++)
			{
				/* AURORA_SNES_SAFE_PERF_V1_20260919
				 * PMON bit N means voice N consumes voice N-1. */
				/* AURORA_SNES_SAFE_PERF_V2_20260919: one variable shift per channel, reused by all DSP masks. */
				const Uint32 uChannelMask = 1u << iChannel;
				const Uint32 uNextChannelMask = uChannelMask << 1;
				const Bool bFeedsPitchMod =
					(uPitchMod & uNextChannelMask) != 0;
				Bool bPitchModWritten = FALSE;

				#if CODE_DEBUG
				if (_ChMask & uChannelMask)
				#endif
				
				// calculate envelope values for channel
				if (OutputEnvelope(iChannel, pData->EnvData, nSamples))
				{
					Bool bMix;
					const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);
					const Bool bNoiseVoice = (uNoiseEnable & uChannelMask) != 0;
					const Bool bZeroVolume = (pRegs->vol_l == 0 && pRegs->vol_r == 0);
					const Bool bNeedPcm =
						(!bNoiseVoice && (bFeedsPitchMod || !bZeroVolume));
					Int16 *pSampleData = bNeedPcm ? pData->iSampleData : NULL;
					Uint16 *pFracData = bNeedPcm ? pData->FracData : NULL;

					/* AURORA_SNES_SAFE_PERF_V1_20260919: bit 0 was cleared by PMON & 0xFE. */
					if (uPitchMod & uChannelMask)
					{
						bMix = OutputSampleModulated(
							iChannel, pSampleData, pFracData,
							pPitchModPrev, nSamples, nSampleRate);
						#if CODE_DEBUG
						//ConDebug("PitchModulation %d\n", iChannel);
						#endif
					} else
					{
						bMix = OutputSample(
							iChannel, pSampleData, pFracData,
							nSamples, nSampleRate);
					}

					if (bMix)
					{
						// is noise enabled for this channel?
						if (bNoiseVoice)
						{
							// use pre-generated noise channel data instead of pcm data
							pSampleData = m_iNoiseSample;
							pFracData   = m_iNoiseFrac;
						}

						if (bFeedsPitchMod)
						{
							_SNSpcBuildPitchModOutput(
								pPitchModNext, pSampleData,
								pData->EnvData, pFracData, nSamples);
							/* AURORA_SNES_SAFE_PERF_V1_20260919: helper overwrote all nSamples entries. */
							bPitchModWritten = TRUE;
						}

						//
						// mix channel into main and echo buffers
						//

						PROF_ENTER("SNSpcDspMixStereo");
						if (!bZeroVolume && (uEchoEnable & uChannelMask))
						{
							// Echo is enabled, so mix channel into both the main and the echo buffers
							// mix using channel volume
							_MixChannelEcho(
								pData->Main[0], pData->Main[1], 
								pData->Echo[0], pData->Echo[1], 
								pSampleData, pData->EnvData, pFracData, nSamples, 
								pRegs->vol_l, pRegs->vol_r
								);
						} else if (!bZeroVolume)
						{
							// Echo is not enabled, so mix channel into the main channel only
							// mix using channel volume
							_MixChannel(
								pData->Main[0], pData->Main[1], 
								pSampleData, pData->EnvData, pFracData, nSamples, 
								pRegs->vol_l, pRegs->vol_r
								);
						}

						PROF_LEAVE("SNSpcDspMixStereo");
					}
				}

				if (bFeedsPitchMod)
				{
					/* AURORA_SNES_SAFE_PERF_V1_20260919: preserve the old zero-input case only if needed. */
					if (!bPitchModWritten)
						memset(pPitchModNext, 0,
						       (size_t)nSamples * sizeof(*pPitchModNext));

					Int16 *pSwap = pPitchModPrev;
					pPitchModPrev = pPitchModNext;
					pPitchModNext = pSwap;
				}
			}

			/* FLG.5 protects echo writes only; read/FIR/address continue. */
			/* AURORA_SNES_SAFE_PERF_V2_20260919: use the same post-Sync FLG snapshot for this chunk. */
			FilterEcho(pData->Echo[0], pData->Echo[1], nSamples, nSampleRate, (uFlags&0x20)==0, uEchoEnable==0);
		}

		// mix main + echo to output buffer
		PROF_ENTER("SNSpcDspMixEcho");
		if (uFlags&0x40) /* AURORA_SNES_SAFE_PERF_V6_20260919: DAC mute: skip work whose result was overwritten */
		{
			/* Keep the exact old final buffers. FIR/read/write/address state
			 * has already advanced in FilterEcho() above. */
			PROF_LEAVE("SNSpcDspMixEcho");
			memset(OutLeftData,0,(size_t)nSamples*sizeof(*OutLeftData));
			memset(OutRightData,0,(size_t)nSamples*sizeof(*OutRightData));
		}
		else
		{
			_MixEcho(OutLeftData, pData->Main[0], pData->Echo[0], nSamples, 
				iMainVolL, iEchoVolL);
			_MixEcho(OutRightData, pData->Main[1], pData->Echo[1], nSamples, 
				iMainVolR, iEchoVolR);
			PROF_LEAVE("SNSpcDspMixEcho");
		}

		// output buffer to sound hardware
		if (nSampleChannels == 2)
			pMixBuf->OutputSamplesStereo(OutLeftData, OutRightData, nSamples);
		else
			pMixBuf->OutputSamplesMono(OutLeftData, nSamples);

		// decrement total sample count
		nTotalSamples -= nSamples;

		// increment cycle count
		uCycle += nSamples * uCyclesPerSample;
	}

	// flush sample data to output
	/* V6D: no routine MIX completion record. */
	pMixBuf->Flush();
}




//
// silent (deterministic) mixer
//


void SNSpcDspMixSilent::FetchBlock(Int32 iChannel)
{
	SNSpcChannelT *pChannel = GetChannel(iChannel);

	// decode next block
	if (pChannel->uBlockAddr!=0) 
	{
		Uint8 uFlags = 0;

		uFlags = m_pDsp->ReadRAM(pChannel->uBlockAddr);
		pChannel->uBlockAddr += 9;

		// end of sample reached?
		if (uFlags&1)
		{
			const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);

			// set ENDX
			pChannel->endx = TRUE;
			if (uFlags & 2 )
			{
				// do looping here?
				pChannel->uBlockAddr = m_pDsp->GetSampleDir(pRegs->srcn, 2);
			} else
			{
				pChannel->uBlockAddr = 0;
			}
		}

	}  
}





Int32 SNSpcDspMixSilent::OutputSample(Int32 iChannel, Int32 nSamples, Int32 nSampleRate)
{
	SNSpcChannelT *pChannel = GetChannel(iChannel);
	const SNSpcVoiceRegsT *pRegs = m_pDsp->GetVoiceRegs(iChannel);
	Int32 iPhase;
	Int32 iPhaseInc;
	Uint32 uPitch;

	if (pChannel->uBlockAddr == 0)
	{
		pChannel->uOldBlockAddr = pChannel->uBlockAddr;
		// silent sample data
		return 0;
	}

	PROF_ENTER("SNSpcDspOutputSampleSilent");

	// get pitch
	uPitch = pRegs->pitch_lo | (pRegs->pitch_hi<<8);
	uPitch&= 0x3FFF;

	iPhase = pChannel->iPhase;
	
	// output at correct pitch based on sample rate
	iPhaseInc = _SNSpcDspPhaseInc(uPitch, nSampleRate);

	while (nSamples > 0)
	{
		if (iPhase >= (14 << 16))
		{
			// fetch next block
			FetchBlock(iChannel);
			iPhase -= (16<<16);
		}

		// next sample
		iPhase+= iPhaseInc;

		nSamples--;
	}

	// voice ended?
	if (pChannel->uBlockAddr == 0)
	{
		pChannel->eEnvState = SNSPCDSP_ENVSTATE_SILENCE;
		pChannel->endx = TRUE;      // this is what we came here for
	}

	// set outx to be envx for now
	pChannel->outx = (pChannel->uBlockAddr == 0) ? 0 : pChannel->envx;

	pChannel->uOldBlockAddr = pChannel->uBlockAddr;
	pChannel->iPhase = iPhase;
	PROF_LEAVE("SNSpcDspOutputSampleSilent");
	return 1;
}




void SNSpcDspMixSilent::Mix(CMixBuffer *pMixBuf)
{
	Int32 nTotalSamples;
	Uint32 nSampleRate, nSampleChannels, nSampleBits;
	(void)nSampleChannels;
	(void)nSampleBits;
	Uint8 Envelope[SNSPCDSP_SAMPLERATE / 60];
	Int32 iChannel;

	nSampleRate = SNSPCDSP_SAMPLERATE;
	nSampleBits = 16;
	nSampleChannels = 0;
	nTotalSamples = SNSPCDSP_SAMPLERATE / 60;

	// build envelope lookup tables based on sample rate
	if (nSampleRate!=m_nSampleRate)
	{
		BuildLookupTables(nSampleRate);
		m_nSampleRate = nSampleRate;
	}

	/* Deterministic state advances while DAC is muted. */
	if (TRUE)
	{
		for (iChannel=0; iChannel < SNSPCDSP_CHANNEL_NUM; iChannel++)
		{
			// calculate envelope values for channel
			if (OutputEnvelope(iChannel, Envelope, nTotalSamples))
			{
					// output sample data
				OutputSample(iChannel, nTotalSamples, nSampleRate);
			}
		}
	}
}
