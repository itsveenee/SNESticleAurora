/* mainloop_globals.cpp
 *
 * Single home for the *definitions* of every cross-file symbol that
 * the rest of the mainloop_*.cpp tree reaches for through
 * mainloop_shared.h.
 *
 * No logic lives here -- only object definitions. Anything with
 * file-static linkage stays inside the .cpp that uses it (e.g.
 * mainloop_init.cpp, mainloop_process.cpp), and #if 0 dead code from
 * the original mainloop.cpp stays in mainloop.cpp.
 *
 * Extracted from mainloop.cpp during the Batch 3 split. No values,
 * initialisers, or attribute lists changed.
 */

#include "types.h"
#include "mainloop_shared.h"

#include "snes.h"
#include "snstate.h"
#include "snrom.h"
#include "nessystem.h"
/* AURORA_FCEUMM_FDS_V0_5_GLOBAL */
#include "nes/fceumm/fdssystem.h"
#include "nesrom.h"
#include "nesstate.h"
#include "segasystem.h"
#include "segarom.h"
/* AURORA_PCE_EXPERIMENTAL_V1 */
#include "pcesystem.h"
#include "pcerom.h"
#include "emusys.h"
#include "emumovie.h"
#include "rendersurface.h"
#include "texture.h"
#include "audmixbuffer.h"
#include "wavfile.h"

#include "uiBrowser.h"
#include "uiNetwork.h"
#include "uiMenu.h"
#include "uiLog.h"
#include "uiScreen.h"


/* MAINLOOP_MEMCARD lives in mainloop_shared.h (included above) and
   gates the memcard variants of _SramPath / _MainLoop_SaveTitle below. */


/* ---- UI screens --------------------------------------------------- */

CBrowserScreen *_MainLoop_pBrowserScreen;
CBrowserScreen *_MainLoop_pStateBrowserScreen;
CNetworkScreen *_MainLoop_pNetworkScreen;
CMenuScreen    *_MainLoop_pMenuScreen;
CMenuScreen    *_MainLoop_pStateScreen;
CMenuScreen    *_MainLoop_pStateDeviceScreen;
CMenuScreen    *_MainLoop_pStateConfirmScreen;
CMenuScreen    *_MainLoop_pMemCardFormatScreen;
CLogScreen     *_MainLoop_pLogScreen;
CVideoScreen   *_MainLoop_pVideoScreen;
CScreen        *_MainLoop_pScreen = NULL;


/* ---- Emulator core handles ---------------------------------------- */

SnesSystem *_pSnes;
SnesRom    *_pSnesRom;

/* Phase 2 of the NES integration (feat/nes-infones).  The iaddis-era
   #if 0 around these declarations is now flipped.  They are no longer
   file-static because mainloop_init.cpp and mainloop_load.cpp need to
   reach them through the extern declarations in mainloop_shared.h.
   Disk-swap state stays un-#if'd here but is not yet driven from
   input -- that part of mainloop_input.cpp is still gated for
   Phase 5 (FDS support). */
NesSystem   *_pNes;
FdsSystem   *_pFds; /* AURORA_FCEUMM_FDS_V0_5_GLOBAL */
NesRom      *_pNesRom;
NesFDSBios  *_pNesFDSBios;
NesDisk     *_pNesFDSDisk;
Int32        _MainLoop_iDisk          = 0;
Bool         _MainLoop_bDiskInserted  = FALSE;

/* AURORA_PICODRIVE_STAGE2 */
SegaSystem  *_pSega;
SegaRom     *_pSegaRom;
PceSystem   *_pPce;
PceRom      *_pPceRom;
GambatteSystem *_pGb; /* AURORA_GAMBATTE_STANDALONE_V2_20260908 */

Char _RomName[256];
Char _RomPath[1024];

#if MAINLOOP_MEMCARD
Char _SramPath[256] = "mc0:/SNESticle";
Char _MainLoop_SaveTitle[] = "SNESticle Aurora";
#else
Char _SramPath[256] = "host0:/cygdrive/d/emu/";
#endif

Emu::System *_pSystem;


/* ---- ROM / framebuffer / audio buffers ---------------------------- */

CRenderSurface *_fbTexture[2];

TextureT _OutTex;
Uint32 _MainLoop_uOutTexTBP  = 0;
Uint32 _MainLoop_uBlenderTBP = 0;
#ifdef DEBUG
CWavFile _WavFile;
#endif

/* AURORA_DYNAMIC_ROM_BUFFER_V1_20260823
 * Frontend-owned ROM backing. NULL/0 while no game is active.
 * SNESticle and PicoDrive can retain pointers into this storage, so it stays
 * alive through the whole cartridge lifetime and is freed only after Unload.
 */
Uint8 *_RomData = NULL;
Uint32 _RomDataCapacity = 0;

SnesStateT		_SnesState;
NesStateT		_NesState;

Emu::MovieClip *s_pMovieClip;


/* ---- SRAM / save bookkeeping -------------------------------------- */

Uint32 _MainLoop_SRAMChecksum;
Uint32 _MainLoop_SaveCounter   = 0;
Uint32 _MainLoop_AutoSaveTime  = 8 * 60;
Bool   _MainLoop_SRAMUpdated   = FALSE;
Bool   _bStateSaved            = FALSE;
Float32 _MainLoop_fOutputIntensity = 0.8f;

AudMixBuffer *_AudMix;


/* ---- Flags / counters --------------------------------------------- */

Bool _bMenu = FALSE;

Char  _MainLoop_ModalStr[256];
Int32 _MainLoop_ModalCount = 0;

Char  _MainLoop_StatusStr[256];
Int32 _MainLoop_StatusCount = 0;

Bool   _MainLoop_BlackScreen   = FALSE;
Uint32 _MainLoop_uDebugDisplay = 0;

Uint32 _uInputFrame;
Uint32 _uInputChecksum[5];

