
#ifndef _AUDMIXBUFFER_H
#define _AUDMIXBUFFER_H


#include "mixbuffer.h"


#define AUDMIXBUFFER_MAXENQUEUE (800*5)

class AudMixBuffer : public CMixBuffer
{
    Int16   m_OutData[2][AUDMIXBUFFER_MAXENQUEUE] _ALIGN(16);
    Int32   m_nOutSamples;

    Int32   m_iPrevSample[2];
    Uint32  m_uSampleRate;
    Bool    m_bAsync;
    Uint32  m_uFrameSamplePhase;
    Uint32  m_uLinearResamplePhase;
    Int32   m_iLinearPrev[2];
    Bool    m_bLinearHavePrev;
    /* AURORA_MEGA_V2_AUDIO_CLOCK_FIELDS */
    Uint32  m_uFrameRateNum;
    Uint32  m_uFrameRateDen;

	Uint32	m_uLastOutput;

    Int32   ConvertSamples2to3(Int16 *pOut, Int16 *pIn, Int32 nSamples, Int32 *pPrevSample);
    Int32   ConvertSamplesStereo_32000(Int16 *pLeftSamples, Int16 *pRightSamples, Int16 *pOutLeft, Int16 *pOutRight, Int32 nInSamples);
    Int32   ConvertSamplesStereo_Linear48(Int16 *pLeftSamples, Int16 *pRightSamples, Int16 *pOutLeft, Int16 *pOutRight, Int32 nInSamples, Int32 nMaxOut);


public:
    AudMixBuffer(Uint32 uSampleRate = 48000, Bool bAsync = FALSE);

    void SetSampleRate(Uint32 uSampleRate)
    {
        m_uSampleRate = uSampleRate;
        m_uFrameSamplePhase = 0;
        m_uLinearResamplePhase = 0;
        m_iLinearPrev[0] = m_iLinearPrev[1] = 0;
        m_bLinearHavePrev = FALSE;
        m_iPrevSample[0] = m_iPrevSample[1] = 0;
    }
    void SetFrameRateRational(Uint32 uNumerator, Uint32 uDenominator)
    {
        if (!uNumerator || !uDenominator) return;
        if (m_uFrameRateNum != uNumerator || m_uFrameRateDen != uDenominator)
        {
            m_uFrameRateNum = uNumerator;
            m_uFrameRateDen = uDenominator;
            m_uFrameSamplePhase = 0;
        }
    }
    /* AURORA_PD_AUDIO_RATE_QUERY_V6_20260821 */
    Uint32 GetSampleRate() const { return m_uSampleRate; }
	Uint32 GetLastOutput() {return m_uLastOutput;}
    void Reset();

    virtual void GetFormat(Uint32 *puSampleRate, Uint32 *pnSampleBits, Uint32 *pnChannels);
    virtual Int32 GetOutputSamples();
    /* AURORA_PS2_PERF_V4_20260824 */
    Bool OutputLibretroInterleaved(const Int16 *pStereo, Int32 nFrames);
    /* AURORA_PD_AUDIO_INTERLEAVED_FAST_V2_H_20260821 */
    Bool OutputPicoDriveInterleaved150(const Int16 *pStereo, Int32 nFrames);
    /* AURORA_PD_AUDIO_32K_DIRECT_V5_H_20260821 */
    Bool OutputPicoDriveInterleaved32000(
        const Int16 *pPrefixLeft, const Int16 *pPrefixRight,
        Int32 nPrefixFrames, const Int16 *pStereo, Int32 nStereoFrames);
    /* AURORA_FCEUMM_FDS_V14_INT32_MONO_FASTPATH_20260827
     * FCEUmm exposes native mono as int32_t even though Aurora consumes it
     * as Int16. Fuse that narrowing with the existing 32050->48k mono path.
     * FALSE means: use the historical bridge fallback unchanged. */
    Bool OutputFceummMonoInt32(const Int32 *pSamples, Int32 nSamples);
    virtual void OutputSamplesStereo(Int16 *pLeftSamples, Int16 *pRightSamples, Int32 nSamples);
    virtual void OutputSamplesMono(Int16 *pSamples, Int32 nSamples);
    virtual void Flush();
};


/* Emulator audio gains: internal 0..400 percent, default 200.
   Video Config displays internal/2 (0..200).
   "SNES volume" is shared by SNESticle + QuickNES.
   "SEGA volume" is used only while PicoDrive is active. */
#ifdef __cplusplus
extern "C" {
#endif
void AudMixGameSetVolume(int vol);
int  AudMixGameGetVolume(void);
void AudMixSegaSetVolume(int vol);
int  AudMixSegaGetVolume(void);
void AudMixPceSetVolume(int vol);
int  AudMixPceGetVolume(void);
void AudMixSetSegaVolumeMode(int enabled);
void AudMixSetPceVolumeMode(int enabled);
/* Experimental fast 32->48 kHz path used only by PicoDrive. */
void AudMixSetFastResample(int enabled);
#ifdef __cplusplus
}
#endif


#endif
