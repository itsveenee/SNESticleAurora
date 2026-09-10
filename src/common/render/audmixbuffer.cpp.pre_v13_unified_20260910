
#include <stdio.h>
#include "types.h"
#include "prof.h"
#include "mixbuffer.h"
#include "audmixbuffer.h"
#include "audframeschedule.h"
#include <string.h>

extern "C" {
#include "audio.h"
};

/* Output gain for the emulated game audio (SNES/NES). The SPU2/audsrv
   volume is already at 100%, so to match players like reference emulator/RetroArch we
   raise the PCM amplitude here, with int16 saturation (loud games clip
   rather than wrap around).

   The internal Game Volume value is the actual PCM gain percentage:
   0 = mute, 100 = unity, 200 = Aurora's shipped/default gain, 400 = max.
   Video Config deliberately shows half of this value, so 200 internal is
   displayed as 100 and 400 internal as 200.  The single AudMixBuffer
   instance (_AudMix) is shared by SNES and NES. */
static int s_gameVolume = 200;   /* SNES/QuickNES: internal 0..400 */
static int s_segaVolume = 200;   /* PicoDrive: internal 0..400 */
static int s_pceVolume = 200;    /* Beetle PCE Fast: internal 0..400; UI 100 */
static int s_useSegaVolume = 0;  /* selected by frontend at gameplay entry */
static int s_usePceVolume = 0;   /* selected by frontend at gameplay entry */
/* PicoDrive-only speed mode. SNES/NES retain cubic resampling. */
static int s_fastResample = 0;

extern "C" void AudMixGameSetVolume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 400) vol = 400;
    s_gameVolume = vol;
}

extern "C" int AudMixGameGetVolume(void)
{
    return s_gameVolume;
}

extern "C" void AudMixSegaSetVolume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 400) vol = 400;
    s_segaVolume = vol;
}

extern "C" int AudMixSegaGetVolume(void)
{
    return s_segaVolume;
}

extern "C" void AudMixPceSetVolume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 400) vol = 400;
    s_pceVolume = vol;
}

extern "C" int AudMixPceGetVolume(void)
{
    return s_pceVolume;
}

extern "C" void AudMixSetSegaVolumeMode(int enabled)
{
    s_useSegaVolume = enabled ? 1 : 0;
}

extern "C" void AudMixSetPceVolumeMode(int enabled)
{
    s_usePceVolume = enabled ? 1 : 0;
}

extern "C" void AudMixSetFastResample(int enabled)
{
    s_fastResample = enabled ? 1 : 0;
}


AudMixBuffer::AudMixBuffer(Uint32 uSampleRate, Bool bAsync)
{
    m_uSampleRate = uSampleRate;
    m_bAsync      = bAsync;
    /* AURORA_MEGA_V2_AUDIO_CLOCK_INIT */
    m_uFrameRateNum = 60;
    m_uFrameRateDen = 1;
    Reset();
}

void AudMixBuffer::Reset()
{
    m_iPrevSample[0] = 0;
    m_iPrevSample[1] = 0;
    m_nOutSamples = 0;
    m_uLastOutput = 0;
    m_uFrameSamplePhase = 0;
    m_uLinearResamplePhase = 0;
    m_iLinearPrev[0] = m_iLinearPrev[1] = 0;
    m_bLinearHavePrev = FALSE;
    memset(m_OutData, 0, sizeof(m_OutData));
}


void AudMixBuffer::GetFormat(Uint32 *puSampleRate, Uint32 *pnSampleBits, Uint32 *pnChannels)
{
	*puSampleRate = m_uSampleRate;
	*pnSampleBits = 16;
	*pnChannels   = 2;
}


Int32 AudMixBuffer::GetOutputSamples()
{
    Int32 nSamples;

    if (!Aud_IsInitialized())
    {
        return 0;
    }

    /*
     * Audio acompanha TEMPO EMULADO, nao o espaco livre do ring do IOP.
     * Consultar Aud_Available() fazia um quadro lento encontrar o ring mais
     * vazio e misturar 1064..1069 amostras em vez de 532..536. Esse trabalho
     * extra tornava o quadro seguinte ainda mais lento (feedback positivo) e
     * tambem avancava o DSP por mais tempo do que um frame do SNES.
     *
     * O core atual executa 262 linhas a 60 quadros. Distribua exatamente uma
     * taxa de audio por esses quadros, em blocos multiplos de quatro exigidos
     * pelo conversor 2:3. Em 32 kHz a sequencia e' 532, 532, 536; ao fim de
     * 60 quadros a soma e' 32000. Se o EE realmente nao sustentar 60 fps, o
     * audsrv pode ter underrun, mas nunca tentamos "pagar a divida" dobrando
     * o custo do mixer no proximo quadro.
     */
    nSamples = AudFrameScheduleNextRational(&m_uFrameSamplePhase,
                                            m_uSampleRate,
                                            m_uFrameRateNum,
                                            m_uFrameRateDen, 4);

    m_uLastOutput  = nSamples;
    return nSamples;
}

/*
 * Cubic Lagrange 2:3 up-sampler (32 kHz SNES -> 48 kHz SPU2).
 *
 * For each input pair [s_i, s_{i+1}] we emit 3 output samples at
 * fractional times 0, 2/3, 4/3 (in units of one 32 kHz sample):
 *
 *   y[3k+0] = s_{2k}                                       (passthrough)
 *   y[3k+1] = cubic_lagrange(s_{2k-1}, s_{2k},   s_{2k+1}, s_{2k+2}) @ 2/3
 *   y[3k+2] = cubic_lagrange(s_{2k},   s_{2k+1}, s_{2k+2}, s_{2k+3}) @ 1/3
 *
 * Cubic Lagrange phase 2/3 coefficients (scaled by 81):
 *   c = [-4, +30, +60, -5] / 81           (sum = 81)
 * Cubic Lagrange phase 1/3 coefficients (scaled by 81):
 *   c = [-5, +60, +30, -4] / 81           (sum = 81)
 *
 * Replaces the linear 2-tap interpolator that this function used to
 * carry. Linear interpolation has a sinc^2 frequency response, which
 * leaves significant spectral images above the input Nyquist (16 kHz)
 * and is responsible for the "weird / harsh / metallic" artefacts
 * that show up on SPC700-rendered audio with high-frequency content
 * (cymbals, brass, FM-style leads). Cubic Lagrange's response is much
 * closer to an ideal low-pass at f_s_in/2 and suppresses those images
 * by ~20 dB at f_s_in, while still being cheap enough to run on the
 * EE (6 mults + 6 adds per 3 output samples).
 *
 * State carried across calls is exactly one input sample (s_{-1})
 * per channel, stored in *pPrevSample. The very last pair in a chunk
 * doesn't have its full 4-tap lookahead window available yet (we
 * haven't asked the SPC engine for those samples), so we degrade
 * that pair to plain linear interpolation. With ~800 input samples
 * per video frame this affects at most 3 output samples per frame
 * out of ~1200 (~0.25%), which is inaudible.
 */
Int32 AudMixBuffer::ConvertSamples2to3(Int16 *pOut, Int16 *pIn, Int32 nSamples, Int32 *pPrevSample)
{
    Int32 hist = *pPrevSample;
    Int16 *pOutStart = pOut;
    Int32 i;

    if (nSamples < 2) return 0;

    /* Main path: cubic Lagrange. Requires 2 samples of lookahead
       beyond the current pair (s_{2k+2}, s_{2k+3}). */
    for (i = 0; i + 3 < nSamples; i += 2)
    {
        Int32 s0 = pIn[i];
        Int32 s1 = pIn[i + 1];
        Int32 s2 = pIn[i + 2];
        Int32 s3 = pIn[i + 3];
        Int32 y;

        /* phase 0 - passthrough */
        pOut[0] = (Int16)s0;

        /* phase 2/3 between s0 and s1, using [hist, s0, s1, s2] */
        y = -4 * hist + 30 * s0 + 60 * s1 - 5 * s2;
        y = (y >= 0 ? y + 40 : y - 40) / 81;
        if (y > 32767)  y = 32767;
        if (y < -32768) y = -32768;
        pOut[1] = (Int16)y;

        /* phase 1/3 (= 4/3 from s0) between s1 and s2, using [s0, s1, s2, s3] */
        y = -5 * s0 + 60 * s1 + 30 * s2 - 4 * s3;
        y = (y >= 0 ? y + 40 : y - 40) / 81;
        if (y > 32767)  y = 32767;
        if (y < -32768) y = -32768;
        pOut[2] = (Int16)y;

        hist = s1;
        pOut += 3;
    }

    /* Tail path: last pair(s) without the 4-tap lookahead window.
       Fall back to plain linear interpolation. */
    for (; i + 1 < nSamples; i += 2)
    {
        Int32 s0 = pIn[i];
        Int32 s1 = pIn[i + 1];
        Int32 s2 = (i + 2 < nSamples) ? pIn[i + 2] : s1;

        pOut[0] = (Int16)s0;
        pOut[1] = (Int16)((s0 + 2 * s1) / 3);
        pOut[2] = (Int16)((2 * s1 + s2) / 3);

        hist = s1;
        pOut += 3;
    }

    *pPrevSample = hist;
    return (Int32)(pOut - pOutStart);
}



/* AURORA_PICODRIVE_FAST_RESAMPLE
 * Low-cost 32 -> 48 kHz converter for the experimental PicoDrive path.
 * SNES/NES still use the higher-quality cubic converter below. */
static Int32 AudMixConvertSamples2to3Fast(
    Int16 *pOut, Int16 *pIn, Int32 nSamples, Int32 *pPrevSample)
{
    Int16 *pOutStart = pOut;
    Int32 i;

    for (i = 0; i + 1 < nSamples; i += 2)
    {
        Int32 s0 = pIn[i];
        Int32 s1 = pIn[i + 1];
        Int32 s2 = (i + 2 < nSamples) ? pIn[i + 2] : s1;

        pOut[0] = (Int16)s0;
        pOut[1] = (Int16)((s0 + 2 * s1) / 3);
        pOut[2] = (Int16)((2 * s1 + s2) / 3);
        pOut += 3;
    }

    if (nSamples > 0)
        *pPrevSample = pIn[nSamples - 1];

    return (Int32)(pOut - pOutStart);
}


Int32 AudMixBuffer::ConvertSamplesStereo_32000(Int16 *pLeftSamples, Int16 *pRightSamples, Int16 *pOutLeft, Int16 *pOutRight, Int32 nInSamples)
{
    Int32 nOutSamples;

    if (nInSamples > AUDMIXBUFFER_MAXENQUEUE*2/3) nInSamples = AUDMIXBUFFER_MAXENQUEUE*2/3;

    PROF_ENTER("Aud_Convert");
    if (s_fastResample)
    {
        AudMixConvertSamples2to3Fast(
            pOutLeft, pLeftSamples, nInSamples, &m_iPrevSample[0]);
        nOutSamples = AudMixConvertSamples2to3Fast(
            pOutRight, pRightSamples, nInSamples, &m_iPrevSample[1]);
    }
    else
    {
        ConvertSamples2to3(
            pOutLeft, pLeftSamples, nInSamples, &m_iPrevSample[0]);
        nOutSamples = ConvertSamples2to3(
            pOutRight, pRightSamples, nInSamples, &m_iPrevSample[1]);
    }
    PROF_LEAVE("Aud_Convert");

    return nOutSamples;
}


/* AURORA_PD_POLISH_V3_20260820_LINEAR_RESAMPLER
 * PicoDrive may synthesize at the same Frequency selected for menu music.
 * SNES and QuickNES remain on their existing 32 kHz cubic path unchanged.
 * 16k and 24k get exact cheap integer-ratio paths; 22.05/38/44.1 use one
 * continuous rational phase so chunk boundaries cannot introduce pitch jitter. */
Int32 AudMixBuffer::ConvertSamplesStereo_Linear48(
    Int16 *pLeftSamples, Int16 *pRightSamples,
    Int16 *pOutLeft, Int16 *pOutRight,
    Int32 nInSamples, Int32 nMaxOut)
{
    Int32 out = 0;
    Int32 i = 0;

    if (!pLeftSamples || !pRightSamples || nInSamples <= 0 ||
        nMaxOut <= 0 || m_uSampleRate == 0)
        return 0;

    if (!m_bLinearHavePrev)
    {
        m_iLinearPrev[0] = pLeftSamples[0];
        m_iLinearPrev[1] = pRightSamples[0];
        m_uLinearResamplePhase = 0;
        m_bLinearHavePrev = TRUE;
        i = 1;
    }

    for (; i < nInSamples && out < nMaxOut; ++i)
    {
        Int32 l0 = m_iLinearPrev[0], r0 = m_iLinearPrev[1];
        Int32 l1 = pLeftSamples[i],   r1 = pRightSamples[i];

        if (m_uSampleRate == 16000)
        {
            if (out + 3 > nMaxOut) break;
            pOutLeft[out] = (Int16)l0;
            pOutRight[out++] = (Int16)r0;
            pOutLeft[out] = (Int16)((2 * l0 + l1) / 3);
            pOutRight[out++] = (Int16)((2 * r0 + r1) / 3);
            pOutLeft[out] = (Int16)((l0 + 2 * l1) / 3);
            pOutRight[out++] = (Int16)((r0 + 2 * r1) / 3);
        }
        else if (m_uSampleRate == 24000)
        {
            if (out + 2 > nMaxOut) break;
            pOutLeft[out] = (Int16)l0;
            pOutRight[out++] = (Int16)r0;
            pOutLeft[out] = (Int16)((l0 + l1) / 2);
            pOutRight[out++] = (Int16)((r0 + r1) / 2);
        }
        else
        {
            while (m_uLinearResamplePhase < 48000U && out < nMaxOut)
            {
                /* Q15 fraction. Worst-case delta*frac still fits Int32. */
                Int32 frac = (Int32)((m_uLinearResamplePhase * 32768U) / 48000U);
                pOutLeft[out] = (Int16)(l0 + (((l1 - l0) * frac) >> 15));
                pOutRight[out] = (Int16)(r0 + (((r1 - r0) * frac) >> 15));
                ++out;
                m_uLinearResamplePhase += m_uSampleRate;
            }
            if (m_uLinearResamplePhase >= 48000U)
                m_uLinearResamplePhase -= 48000U;
        }

        m_iLinearPrev[0] = l1;
        m_iLinearPrev[1] = r1;
    }

    return out;
}

/* AURORA_PS2_PERF_V4_20260824
 * Direct libretro LRLR input for Beetle PCE Fast and retired alternate SNES core.
 * This is equation/state equivalent to OutputSamplesStereo(), but fuses the
 * bridge's deinterleave pass with output/resampling and avoids two 1024-frame
 * channel planes.  PicoDrive retains its specialised +50% paths below. */
static inline Int16 _AudMixCubic81(Int32 value)
{
    value = (value >= 0 ? value + 40 : value - 40) / 81;
    if (value > 32767)  value = 32767;
    if (value < -32768) value = -32768;
    return (Int16)value;
}

Bool AudMixBuffer::OutputLibretroInterleaved(
    const Int16 *pStereo, Int32 nFrames)
{
    Int32 i;

    if (!pStereo || nFrames <= 0)
        return TRUE;

    if (m_uSampleRate == 48000)
    {
        if (m_nOutSamples + nFrames > AUDMIXBUFFER_MAXENQUEUE)
            return TRUE;
        for (i = 0; i < nFrames; ++i)
        {
            m_OutData[0][m_nOutSamples + i] = pStereo[i * 2 + 0];
            m_OutData[1][m_nOutSamples + i] = pStereo[i * 2 + 1];
        }
        m_nOutSamples += nFrames;
        return TRUE;
    }

    if (m_uSampleRate == 32000)
    {
        Int32 outNeeded;

        if (nFrames > AUDMIXBUFFER_MAXENQUEUE * 2 / 3)
            nFrames = AUDMIXBUFFER_MAXENQUEUE * 2 / 3;
        outNeeded = (nFrames / 2) * 3;
        if (m_nOutSamples + outNeeded > AUDMIXBUFFER_MAXENQUEUE)
            return TRUE;

        PROF_ENTER("Aud_Convert");
        if (s_fastResample)
        {
            for (i = 0; i + 1 < nFrames; i += 2)
            {
                Int32 l0 = pStereo[(i + 0) * 2 + 0];
                Int32 r0 = pStereo[(i + 0) * 2 + 1];
                Int32 l1 = pStereo[(i + 1) * 2 + 0];
                Int32 r1 = pStereo[(i + 1) * 2 + 1];
                Int32 l2 = (i + 2 < nFrames)
                    ? pStereo[(i + 2) * 2 + 0] : l1;
                Int32 r2 = (i + 2 < nFrames)
                    ? pStereo[(i + 2) * 2 + 1] : r1;

                m_OutData[0][m_nOutSamples] = (Int16)l0;
                m_OutData[1][m_nOutSamples++] = (Int16)r0;
                m_OutData[0][m_nOutSamples] = (Int16)((l0 + 2 * l1) / 3);
                m_OutData[1][m_nOutSamples++] = (Int16)((r0 + 2 * r1) / 3);
                m_OutData[0][m_nOutSamples] = (Int16)((2 * l1 + l2) / 3);
                m_OutData[1][m_nOutSamples++] = (Int16)((2 * r1 + r2) / 3);
            }
            if (nFrames > 0)
            {
                m_iPrevSample[0] = pStereo[(nFrames - 1) * 2 + 0];
                m_iPrevSample[1] = pStereo[(nFrames - 1) * 2 + 1];
            }
        }
        else if (nFrames >= 2)
        {
            Int32 histL = m_iPrevSample[0];
            Int32 histR = m_iPrevSample[1];

            for (i = 0; i + 3 < nFrames; i += 2)
            {
                Int32 l0 = pStereo[(i + 0) * 2 + 0];
                Int32 r0 = pStereo[(i + 0) * 2 + 1];
                Int32 l1 = pStereo[(i + 1) * 2 + 0];
                Int32 r1 = pStereo[(i + 1) * 2 + 1];
                Int32 l2 = pStereo[(i + 2) * 2 + 0];
                Int32 r2 = pStereo[(i + 2) * 2 + 1];
                Int32 l3 = pStereo[(i + 3) * 2 + 0];
                Int32 r3 = pStereo[(i + 3) * 2 + 1];

                m_OutData[0][m_nOutSamples] = (Int16)l0;
                m_OutData[1][m_nOutSamples++] = (Int16)r0;
                m_OutData[0][m_nOutSamples] = _AudMixCubic81(
                    -4 * histL + 30 * l0 + 60 * l1 - 5 * l2);
                m_OutData[1][m_nOutSamples++] = _AudMixCubic81(
                    -4 * histR + 30 * r0 + 60 * r1 - 5 * r2);
                m_OutData[0][m_nOutSamples] = _AudMixCubic81(
                    -5 * l0 + 60 * l1 + 30 * l2 - 4 * l3);
                m_OutData[1][m_nOutSamples++] = _AudMixCubic81(
                    -5 * r0 + 60 * r1 + 30 * r2 - 4 * r3);
                histL = l1;
                histR = r1;
            }

            for (; i + 1 < nFrames; i += 2)
            {
                Int32 l0 = pStereo[(i + 0) * 2 + 0];
                Int32 r0 = pStereo[(i + 0) * 2 + 1];
                Int32 l1 = pStereo[(i + 1) * 2 + 0];
                Int32 r1 = pStereo[(i + 1) * 2 + 1];
                Int32 l2 = (i + 2 < nFrames)
                    ? pStereo[(i + 2) * 2 + 0] : l1;
                Int32 r2 = (i + 2 < nFrames)
                    ? pStereo[(i + 2) * 2 + 1] : r1;

                m_OutData[0][m_nOutSamples] = (Int16)l0;
                m_OutData[1][m_nOutSamples++] = (Int16)r0;
                m_OutData[0][m_nOutSamples] = (Int16)((l0 + 2 * l1) / 3);
                m_OutData[1][m_nOutSamples++] = (Int16)((r0 + 2 * r1) / 3);
                m_OutData[0][m_nOutSamples] = (Int16)((2 * l1 + l2) / 3);
                m_OutData[1][m_nOutSamples++] = (Int16)((2 * r1 + r2) / 3);
                histL = l1;
                histR = r1;
            }
            m_iPrevSample[0] = histL;
            m_iPrevSample[1] = histR;
        }
        PROF_LEAVE("Aud_Convert");
        return TRUE;
    }

    /* Same state transitions and output order as
       ConvertSamplesStereo_Linear48(), read directly from LRLR input. */
    if (m_uSampleRate == 0 ||
        m_nOutSamples >= AUDMIXBUFFER_MAXENQUEUE)
        return TRUE;

    i = 0;
    if (!m_bLinearHavePrev)
    {
        m_iLinearPrev[0] = pStereo[0];
        m_iLinearPrev[1] = pStereo[1];
        m_uLinearResamplePhase = 0;
        m_bLinearHavePrev = TRUE;
        i = 1;
    }

    if (m_uSampleRate == 16000)
    {
        for (; i < nFrames; ++i)
        {
            Int32 l0, r0, l1, r1;
            if (m_nOutSamples + 3 > AUDMIXBUFFER_MAXENQUEUE)
                break;
            l0 = m_iLinearPrev[0]; r0 = m_iLinearPrev[1];
            l1 = pStereo[i * 2 + 0]; r1 = pStereo[i * 2 + 1];
            m_OutData[0][m_nOutSamples] = (Int16)l0;
            m_OutData[1][m_nOutSamples++] = (Int16)r0;
            m_OutData[0][m_nOutSamples] = (Int16)((2 * l0 + l1) / 3);
            m_OutData[1][m_nOutSamples++] = (Int16)((2 * r0 + r1) / 3);
            m_OutData[0][m_nOutSamples] = (Int16)((l0 + 2 * l1) / 3);
            m_OutData[1][m_nOutSamples++] = (Int16)((r0 + 2 * r1) / 3);
            m_iLinearPrev[0] = l1; m_iLinearPrev[1] = r1;
        }
    }
    else if (m_uSampleRate == 24000)
    {
        for (; i < nFrames; ++i)
        {
            Int32 l0, r0, l1, r1;
            if (m_nOutSamples + 2 > AUDMIXBUFFER_MAXENQUEUE)
                break;
            l0 = m_iLinearPrev[0]; r0 = m_iLinearPrev[1];
            l1 = pStereo[i * 2 + 0]; r1 = pStereo[i * 2 + 1];
            m_OutData[0][m_nOutSamples] = (Int16)l0;
            m_OutData[1][m_nOutSamples++] = (Int16)r0;
            m_OutData[0][m_nOutSamples] = (Int16)((l0 + l1) / 2);
            m_OutData[1][m_nOutSamples++] = (Int16)((r0 + r1) / 2);
            m_iLinearPrev[0] = l1; m_iLinearPrev[1] = r1;
        }
    }
    else
    {
        for (; i < nFrames &&
               m_nOutSamples < AUDMIXBUFFER_MAXENQUEUE; ++i)
        {
            Int32 l0 = m_iLinearPrev[0], r0 = m_iLinearPrev[1];
            Int32 l1 = pStereo[i * 2 + 0], r1 = pStereo[i * 2 + 1];
            while (m_uLinearResamplePhase < 48000U &&
                   m_nOutSamples < AUDMIXBUFFER_MAXENQUEUE)
            {
                Int32 frac = (Int32)(
                    (m_uLinearResamplePhase * 32768U) / 48000U);
                m_OutData[0][m_nOutSamples] =
                    (Int16)(l0 + (((l1 - l0) * frac) >> 15));
                m_OutData[1][m_nOutSamples] =
                    (Int16)(r0 + (((r1 - r0) * frac) >> 15));
                ++m_nOutSamples;
                m_uLinearResamplePhase += m_uSampleRate;
            }
            if (m_uLinearResamplePhase >= 48000U)
                m_uLinearResamplePhase -= 48000U;
            m_iLinearPrev[0] = l1;
            m_iLinearPrev[1] = r1;
        }
    }
    return TRUE;
}

/* AURORA_PD_AUDIO_INTERLEAVED_FAST_V2_CPP_20260821
 * AURORA_PD_AUDIO_INTERLEAVED_ALL_RATES_V3_20260821
 * PicoDrive supplies interleaved stereo. Consume it directly for every
 * selectable rate except 32 kHz, fusing the existing +50% pre-gain with
 * deinterleave/resampling so the input is read once and the 2x1024 temporary
 * channel planes are not touched.
 *
 * 16/24 kHz preserve the integer-ratio Linear48 path sample-for-sample.
 * 22.05/38/44.1 kHz preserve its continuous Q15 rational phase exactly.
 * 48 kHz preserves the old pre-gain + memcpy path sample-for-sample.
 * 32 kHz deliberately stays on the old multiple-of-four fast 2:3 path; its
 * chunk-tail semantics are different and are not changed by this patch. */
static inline Int16 _AudMixPicoGain150(Int16 sample)
{
    Int32 v = ((Int32)sample * 3) / 2;
    if (v > 32767)  v = 32767;
    if (v < -32768) v = -32768;
    return (Int16)v;
}

Bool AudMixBuffer::OutputPicoDriveInterleaved150(
    const Int16 *pStereo, Int32 nFrames)
{
    Int32 i;

    switch (m_uSampleRate)
    {
        case 16000:
        case 22050:
        case 24000:
        case 38000:
        case 44100:
        case 48000:
            break;
        default:
            return FALSE;
    }

    if (!pStereo || nFrames <= 0)
        return TRUE;

    if (m_uSampleRate == 48000)
    {
        if (m_nOutSamples + nFrames > AUDMIXBUFFER_MAXENQUEUE)
            return TRUE;

        for (i = 0; i < nFrames; ++i)
        {
            m_OutData[0][m_nOutSamples + i] =
                _AudMixPicoGain150(pStereo[i * 2 + 0]);
            m_OutData[1][m_nOutSamples + i] =
                _AudMixPicoGain150(pStereo[i * 2 + 1]);
        }
        m_nOutSamples += nFrames;
        return TRUE;
    }

    i = 0;
    if (!m_bLinearHavePrev)
    {
        m_iLinearPrev[0] = _AudMixPicoGain150(pStereo[0]);
        m_iLinearPrev[1] = _AudMixPicoGain150(pStereo[1]);
        m_uLinearResamplePhase = 0;
        m_bLinearHavePrev = TRUE;
        i = 1;
    }

    if (m_uSampleRate == 16000)
    {
        for (; i < nFrames; ++i)
        {
            Int32 l0, r0, l1, r1;
            Int16 *pOutLeft, *pOutRight;

            if (m_nOutSamples + 3 > AUDMIXBUFFER_MAXENQUEUE)
                break;

            l0 = m_iLinearPrev[0];
            r0 = m_iLinearPrev[1];
            l1 = _AudMixPicoGain150(pStereo[i * 2 + 0]);
            r1 = _AudMixPicoGain150(pStereo[i * 2 + 1]);

            pOutLeft = m_OutData[0] + m_nOutSamples;
            pOutRight = m_OutData[1] + m_nOutSamples;

            pOutLeft[0] = (Int16)l0;
            pOutRight[0] = (Int16)r0;
            pOutLeft[1] = (Int16)((2 * l0 + l1) / 3);
            pOutRight[1] = (Int16)((2 * r0 + r1) / 3);
            pOutLeft[2] = (Int16)((l0 + 2 * l1) / 3);
            pOutRight[2] = (Int16)((r0 + 2 * r1) / 3);

            m_iLinearPrev[0] = l1;
            m_iLinearPrev[1] = r1;
            m_nOutSamples += 3;
        }
    }
    else if (m_uSampleRate == 24000)
    {
        for (; i < nFrames; ++i)
        {
            Int32 l0, r0, l1, r1;
            Int16 *pOutLeft, *pOutRight;

            if (m_nOutSamples + 2 > AUDMIXBUFFER_MAXENQUEUE)
                break;

            l0 = m_iLinearPrev[0];
            r0 = m_iLinearPrev[1];
            l1 = _AudMixPicoGain150(pStereo[i * 2 + 0]);
            r1 = _AudMixPicoGain150(pStereo[i * 2 + 1]);

            pOutLeft = m_OutData[0] + m_nOutSamples;
            pOutRight = m_OutData[1] + m_nOutSamples;

            pOutLeft[0] = (Int16)l0;
            pOutRight[0] = (Int16)r0;
            pOutLeft[1] = (Int16)((l0 + l1) / 2);
            pOutRight[1] = (Int16)((r0 + r1) / 2);

            m_iLinearPrev[0] = l1;
            m_iLinearPrev[1] = r1;
            m_nOutSamples += 2;
        }
    }
    else
    {
        /* Same Q15 phase/order as ConvertSamplesStereo_Linear48(), but read
         * PicoDrive's interleaved input directly after the existing +50%. */
        for (; i < nFrames && m_nOutSamples < AUDMIXBUFFER_MAXENQUEUE; ++i)
        {
            Int32 l0 = m_iLinearPrev[0];
            Int32 r0 = m_iLinearPrev[1];
            Int32 l1 = _AudMixPicoGain150(pStereo[i * 2 + 0]);
            Int32 r1 = _AudMixPicoGain150(pStereo[i * 2 + 1]);

            while (m_uLinearResamplePhase < 48000U &&
                   m_nOutSamples < AUDMIXBUFFER_MAXENQUEUE)
            {
                Int32 frac =
                    (Int32)((m_uLinearResamplePhase * 32768U) / 48000U);
                m_OutData[0][m_nOutSamples] =
                    (Int16)(l0 + (((l1 - l0) * frac) >> 15));
                m_OutData[1][m_nOutSamples] =
                    (Int16)(r0 + (((r1 - r0) * frac) >> 15));
                ++m_nOutSamples;
                m_uLinearResamplePhase += m_uSampleRate;
            }

            if (m_uLinearResamplePhase >= 48000U)
                m_uLinearResamplePhase -= 48000U;

            m_iLinearPrev[0] = l1;
            m_iLinearPrev[1] = r1;
        }
    }

    return TRUE;
}

/* AURORA_PD_AUDIO_32K_DIRECT_V5_CPP_20260821
 * Exact fast 32 -> 48 kHz PicoDrive path without the 2x1024 bridge planes.
 * Prefix contains the bridge's 0..3 already-gained pending frames. Current
 * callback frames are gained +50% only as each sample is consumed. */
Bool AudMixBuffer::OutputPicoDriveInterleaved32000(
    const Int16 *pPrefixLeft, const Int16 *pPrefixRight,
    Int32 nPrefixFrames, const Int16 *pStereo, Int32 nStereoFrames)
{
    Int32 total = nPrefixFrames + nStereoFrames;
    Int32 i;
    Int32 outNeeded;

    if (m_uSampleRate != 32000)
        return FALSE;
    if (total <= 0)
        return TRUE;
    if (nPrefixFrames < 0 || nPrefixFrames > 3 || nStereoFrames < 0 ||
        (nPrefixFrames > 0 && (!pPrefixLeft || !pPrefixRight)) ||
        (nStereoFrames > 0 && !pStereo))
        return FALSE;
    if ((total & 3) != 0)
        return FALSE;

    outNeeded = (total / 2) * 3;
    if (m_nOutSamples + outNeeded > AUDMIXBUFFER_MAXENQUEUE)
        return TRUE;

#define AURORA_PD32_V5_FETCH(CH, IDX) \
    ((IDX) < nPrefixFrames ? \
        ((CH) == 0 ? pPrefixLeft[(IDX)] : pPrefixRight[(IDX)]) : \
        _AudMixPicoGain150(pStereo[((IDX) - nPrefixFrames) * 2 + (CH)]))

    /* AURORA_ALLCORES_PERF_V5_20260824
     * The old loop gained sample i+2 as lookahead and gained it again as the
     * next pair's i+0. Carry that exact Int16 value forward instead. */
    {
        Int32 l0 = AURORA_PD32_V5_FETCH(0, 0);
        Int32 r0 = AURORA_PD32_V5_FETCH(1, 0);
        Int32 lastL = l0;
        Int32 lastR = r0;

        for (i = 0; i + 1 < total; i += 2)
        {
            Int32 l1 = AURORA_PD32_V5_FETCH(0, i + 1);
            Int32 r1 = AURORA_PD32_V5_FETCH(1, i + 1);
            Int32 l2 = (i + 2 < total)
                ? AURORA_PD32_V5_FETCH(0, i + 2) : l1;
            Int32 r2 = (i + 2 < total)
                ? AURORA_PD32_V5_FETCH(1, i + 2) : r1;

            m_OutData[0][m_nOutSamples] = (Int16)l0;
            m_OutData[1][m_nOutSamples++] = (Int16)r0;
            m_OutData[0][m_nOutSamples] = (Int16)((l0 + 2 * l1) / 3);
            m_OutData[1][m_nOutSamples++] = (Int16)((r0 + 2 * r1) / 3);
            m_OutData[0][m_nOutSamples] = (Int16)((2 * l1 + l2) / 3);
            m_OutData[1][m_nOutSamples++] = (Int16)((2 * r1 + r2) / 3);

            lastL = l1;
            lastR = r1;
            l0 = l2;
            r0 = r2;
        }

        m_iPrevSample[0] = lastL;
        m_iPrevSample[1] = lastR;
    }
#undef AURORA_PD32_V5_FETCH
    return TRUE;
}

void AudMixBuffer::OutputSamplesStereo(Int16 *pLeftSamples, Int16 *pRightSamples, Int32 nSamples)
{
    /* AURORA_PD_POLISH_V3_20260820_RATE_DISPATCH */
    if (m_uSampleRate != 32000 && m_uSampleRate != 48000)
    {
        Int32 room = AUDMIXBUFFER_MAXENQUEUE - m_nOutSamples;
        if (room > 0)
        {
            Int16 *pOutLeft = m_OutData[0] + m_nOutSamples;
            Int16 *pOutRight = m_OutData[1] + m_nOutSamples;
            m_nOutSamples += ConvertSamplesStereo_Linear48(
                pLeftSamples, pRightSamples, pOutLeft, pOutRight,
                nSamples, room);
        }
        return;
    }

    Int16 *pOutLeft, *pOutRight;
    Int32 nOutSamples;

    // determine output space required (estimate)
    switch (m_uSampleRate)
    {
        case 24000:
            nOutSamples = nSamples * 2;
            break;
        case 32000:
            nOutSamples = nSamples * 6 / 4;
            break;
        default:
        case 48000:
            nOutSamples = nSamples;
            break;
    }

    // check for buffer overflow 
    if ((m_nOutSamples + nOutSamples) > AUDMIXBUFFER_MAXENQUEUE)
    {
        return;
    }

    // buffer samples locally
    pOutLeft    = m_OutData[0] + m_nOutSamples;
    pOutRight   = m_OutData[1] + m_nOutSamples;

    switch(m_uSampleRate)
    {
        case 32000:
            m_nOutSamples += ConvertSamplesStereo_32000(pLeftSamples, pRightSamples, pOutLeft, pOutRight, nSamples);
            break;

        default:
        case 24000:
        case 48000:
            // leave data as is
            memcpy(pOutLeft, pLeftSamples, nSamples * sizeof(Int16));
            memcpy(pOutRight, pRightSamples, nSamples * sizeof(Int16));
            m_nOutSamples += nSamples;
            break;
    }
}

void AudMixBuffer::Flush()
{
    Int32 nOutSamples;

    nOutSamples = m_nOutSamples;

    if (nOutSamples > 0)
    {
        /* AURORA_AUDIO_SPLIT_VOLUMES_V36_20260823
         * Shared mixer, separate final gain by active core family. */
        const Int32 gainPct =
            s_usePceVolume ? s_pceVolume :
            (s_useSegaVolume ? s_segaVolume : s_gameVolume);

        if ((nOutSamples & 1) && m_uSampleRate == 32000)
        {
            // uh oh
            #if CODE_DEBUG
            printf("Sample count not even! %d\n", nOutSamples);
            #endif
            nOutSamples &= ~1;
        }

        if (nOutSamples > AUDMIXBUFFER_MAXENQUEUE)
        {
            // uh oh
            #if CODE_DEBUG
            printf("Sample buffer overflow! %d\n", nOutSamples);
            #endif
            nOutSamples = AUDMIXBUFFER_MAXENQUEUE;
        }

        /* AURORA_AUDIO_ASYNC_GAIN_COPY_V4_FLUSH_20260821
         * Async gameplay fuses this exact gain with the FIFO copy below.
         * Keep the historical in-place transform only for synchronous
         * output, preserving that path byte-for-byte. */
        if (!m_bAsync)
        {
            /* AURORA_MEGA_V4_AUDIO_GAIN_FASTPATH
             * Internal Game Volume 200 (shown as 100) is the shipped 200% gain. For every int16
             * input, (sample * 200) / 100 is exactly sample * 2; retain the
             * identical saturation but avoid a division per channel/sample.
             * Muting is likewise exactly an all-zero byte plane. Every other
             * user volume keeps the original formula and rounding. */
            if (gainPct == 0)
            {
                memset(m_OutData[0], 0, nOutSamples * sizeof(Int16));
                memset(m_OutData[1], 0, nOutSamples * sizeof(Int16));
            }
            else if (gainPct == 200)
            {
                Int32 ch, i;
                for (ch = 0; ch < 2; ch++)
                {
                    Int16 *p = m_OutData[ch];
                    for (i = 0; i < nOutSamples; i++)
                    {
                        Int32 v = (Int32)p[i] * 2;
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        p[i] = (Int16)v;
                    }
                }
            }
            else if (gainPct != 100)
            {
                Int32 ch, i;
                for (ch = 0; ch < 2; ch++)
                {
                    Int16 *p = m_OutData[ch];
                    for (i = 0; i < nOutSamples; i++)
                    {
                        Int32 v = ((Int32)p[i] * gainPct) / 100;
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        p[i] = (Int16)v;
                    }
                }
            }
        }

        if (m_bAsync)
        {
            Aud_EnqueueAsyncGain(
                m_OutData[0], m_OutData[1], nOutSamples, gainPct);
        } else
        {
            Aud_Enqueue(m_OutData[0], m_OutData[1], nOutSamples,1);
        }
    }

    m_nOutSamples = 0;
}




/* AURORA_FCEUMM_FDS_V14_INT32_MONO_FASTPATH_20260827
 *
 * Bit-equivalent to:
 *   int32 core PCM -> (Int16) temporary -> OutputSamplesMono()
 *
 * Narrowing happens before interpolation exactly as before. Same 32050 Hz
 * rate, Q15 linear phase, truncation, carried previous sample and overflow
 * behaviour. No other core/rate uses this function.
 */
Bool AudMixBuffer::OutputFceummMonoInt32(const Int32 *pSamples, Int32 nSamples)
{
    Int32 i = 0;

    if (!pSamples || nSamples <= 0)
        return TRUE;

    if (m_uSampleRate != 32050)
        return FALSE;

    if (m_bLinearHavePrev && m_iLinearPrev[0] != m_iLinearPrev[1])
        return FALSE;

    if (!m_bLinearHavePrev)
    {
        const Int32 first = (Int16)pSamples[0];
        m_iLinearPrev[0] = m_iLinearPrev[1] = first;
        m_uLinearResamplePhase = 0;
        m_bLinearHavePrev = TRUE;
        i = 1;
    }

    for (; i < nSamples; ++i)
    {
        const Int32 s0 = m_iLinearPrev[0];
        const Int32 s1 = (Int16)pSamples[i];

        while (m_uLinearResamplePhase < 48000U &&
               m_nOutSamples < AUDMIXBUFFER_MAXENQUEUE)
        {
            const Int32 frac = (Int32)(
                (m_uLinearResamplePhase * 32768U) / 48000U);
            const Int16 v = (Int16)(
                s0 + (((s1 - s0) * frac) >> 15));

            m_OutData[0][m_nOutSamples] = v;
            m_OutData[1][m_nOutSamples] = v;
            ++m_nOutSamples;
            m_uLinearResamplePhase += m_uSampleRate;
        }

        if (m_uLinearResamplePhase >= 48000U)
            m_uLinearResamplePhase -= 48000U;

        m_iLinearPrev[0] = m_iLinearPrev[1] = s1;
    }

    return TRUE;
}

void AudMixBuffer::OutputSamplesMono(Int16 *pSamples, Int32 nSamples)
{
    /* AURORA_ALLCORES_PERF_V5_20260824
     * QuickNES is mono and the native SNES mixer can also select mono.  When
     * both histories match, the historical left/right cubic calls are
     * mathematically identical. Run one and byte-copy its exact Int16 result.
     * A mismatched history (for example immediately after a stereo source)
     * takes the old path once and naturally converges the two histories. */
    if (m_uSampleRate == 32000 && pSamples && nSamples >= 2 &&
        m_iPrevSample[0] == m_iPrevSample[1])
    {
        Int32 estimated = nSamples * 6 / 4;
        Int32 nIn = nSamples;
        Int32 nOut;
        Int16 *left;
        Int16 *right;

        /* Preserve OutputSamplesStereo's pre-clamp overflow decision. */
        if (m_nOutSamples + estimated > AUDMIXBUFFER_MAXENQUEUE)
            return;
        if (nIn > AUDMIXBUFFER_MAXENQUEUE * 2 / 3)
            nIn = AUDMIXBUFFER_MAXENQUEUE * 2 / 3;

        left = m_OutData[0] + m_nOutSamples;
        right = m_OutData[1] + m_nOutSamples;
        PROF_ENTER("Aud_Convert");
        if (s_fastResample)
            nOut = AudMixConvertSamples2to3Fast(
                left, pSamples, nIn, &m_iPrevSample[0]);
        else
            nOut = ConvertSamples2to3(
                left, pSamples, nIn, &m_iPrevSample[0]);
        PROF_LEAVE("Aud_Convert");

        if (nOut > 0)
            memcpy(right, left, (size_t)nOut * sizeof(left[0]));
        m_iPrevSample[1] = m_iPrevSample[0];
        m_nOutSamples += nOut;
        return;
    }

    /* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827: mono arbitrary-rate fast path (FDS is 32050 Hz). */
    if (pSamples && nSamples > 0 && m_uSampleRate != 32000)
    {
        if (m_uSampleRate == 48000)
        {
            if (m_nOutSamples + nSamples <= AUDMIXBUFFER_MAXENQUEUE)
            {
                memcpy(m_OutData[0] + m_nOutSamples, pSamples, (size_t)nSamples * sizeof(Int16));
                memcpy(m_OutData[1] + m_nOutSamples, pSamples, (size_t)nSamples * sizeof(Int16));
                m_nOutSamples += nSamples;
            }
            return;
        }
        if (!m_bLinearHavePrev || m_iLinearPrev[0] == m_iLinearPrev[1])
        {
            Int32 i = 0;
            if (!m_bLinearHavePrev)
            {
                m_iLinearPrev[0] = m_iLinearPrev[1] = pSamples[0];
                m_uLinearResamplePhase = 0;
                m_bLinearHavePrev = TRUE;
                i = 1;
            }
            for (; i < nSamples; ++i)
            {
                Int32 s0 = m_iLinearPrev[0], s1 = pSamples[i];
                if (m_uSampleRate == 16000)
                {
                    Int16 v;
                    if (m_nOutSamples + 3 > AUDMIXBUFFER_MAXENQUEUE) break;
                    v=(Int16)s0; m_OutData[0][m_nOutSamples]=m_OutData[1][m_nOutSamples++]=v;
                    v=(Int16)((2*s0+s1)/3); m_OutData[0][m_nOutSamples]=m_OutData[1][m_nOutSamples++]=v;
                    v=(Int16)((s0+2*s1)/3); m_OutData[0][m_nOutSamples]=m_OutData[1][m_nOutSamples++]=v;
                }
                else if (m_uSampleRate == 24000)
                {
                    Int16 v;
                    if (m_nOutSamples + 2 > AUDMIXBUFFER_MAXENQUEUE) break;
                    v=(Int16)s0; m_OutData[0][m_nOutSamples]=m_OutData[1][m_nOutSamples++]=v;
                    v=(Int16)((s0+s1)/2); m_OutData[0][m_nOutSamples]=m_OutData[1][m_nOutSamples++]=v;
                }
                else
                {
                    while (m_uLinearResamplePhase < 48000U && m_nOutSamples < AUDMIXBUFFER_MAXENQUEUE)
                    {
                        Int32 frac=(Int32)((m_uLinearResamplePhase*32768U)/48000U);
                        Int16 v=(Int16)(s0+(((s1-s0)*frac)>>15));
                        m_OutData[0][m_nOutSamples]=m_OutData[1][m_nOutSamples++]=v;
                        m_uLinearResamplePhase += m_uSampleRate;
                    }
                    if (m_uLinearResamplePhase >= 48000U) m_uLinearResamplePhase -= 48000U;
                }
                m_iLinearPrev[0] = m_iLinearPrev[1] = s1;
            }
            return;
        }
    }
    OutputSamplesStereo(pSamples, pSamples, nSamples);
}
