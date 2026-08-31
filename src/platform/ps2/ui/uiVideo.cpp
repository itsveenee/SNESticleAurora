
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kernel.h>
#include <libpad.h>
#include "types.h"
#include "font.h"
#include "poly.h"
#include "uiVideo.h"
#include "snrom.h"
#include "snes.h"
extern "C" {
#include "gskit_backend.h"
#include "gpfifo.h"
#include "ps2dma.h"
#include "audio.h"
}
#include "memcard.h"
#include "uiCover.h"
#include "mainloop_bgm.h"
#include "mainloop_state.h"
#include "mainloop_input.h"
#include "input.h"
#include "mainloop_smb.h"
#include "audmixbuffer.h"
#include "embedded_irx.h"   /* HddSupportIsEnabled / HddSupportSetEnabled */
#include "snppucolor.h"
#include "snppurender.h"
#include "nes/quicknes/quicknes_bridge.h" /* QUICKNES_FAMICLONE_HOOK */
#include "sega/picodrive/picodrive_bridge.h"
#include "pce/beetle/pce_bridge.h" /* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
#include "mainloop_safe_frameskip.h" /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */

/* mc0:/SNESticle (defined in mainloop_globals.cpp). */
extern Char _SramPath[256];
extern SnesSystem *_pSnes;
extern Emu::System *_pSystem;
extern AudMixBuffer *_AudMix;

void MainResetEmulator(void);
Bool MainLoopReinitVideoMode(Int32 mode);
/* AURORA_SNES9X2010_V1 */
const Char *MainLoopSnesCoreGetName();
void MainLoopSnesCoreCycleDir(Int32 dir);
/* AURORA_SNES9X2010_V3_MENU_BRIDGE_20260824 */
Int32 MainLoopSnesCoreGetPersisted();
void MainLoopSnesCoreSetPersisted(Int32 value);

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

#define VIDEOCFG_MAGIC   0x53564944u   /* 'SVID' */
#define VIDEOCFG_VERSION 43
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830: v43 appends shared SCD/PCE CD Red Book toggle; old configs default On. */
/* AURORA_PCE_SCALING_LIGHTGUN_TOGGLE_V2_20260830: v42 appends Light Gun; old configs default On. */
/* AURORA_FAMICOM_MIC_CFG41_20260828: v41 keeps the layout and migrates every older Safe Frameskip to 1 once. */
/* AURORA_PCE_VOLUME_V37_20260823
 * v37 appends pcevol only; every v36 field keeps the same offset.
 * Internal 200 == UI 100. */
/* AURORA_AUDIO_SPLIT_VOLUMES_V36_20260823
 * v36 changes NO struct size or field offsets. The former legacy/reserved
 * snesaudiorate slot is now segavol; native SNES synthesis stays fixed 32k. */
/* v35 appends SMS color-border + SMS FM preferences. */
/* AURORA_AUDIO_SPLIT_REVIEW_V1_20260821 */
/* AURORA_PD_MEGA_FIX_20260820 */
/* AURORA_PICODRIVE_CURRENT_UI_V4: v32 appends md6button only. */
/* v31 establishes Menu Volume UI 100 (internal 200) as the migration
 * default for every older config. Once saved as v31+, the user's selected
 * Menu Volume persists normally. */
/* v30 fixes the temporary v29 Menu Audio defaults:
 * Menu Volume defaults to internal 200 (UI 100) and Menu Music to Off.
 * Once saved as v30, both settings persist independently. */
/* v29 changes Menu Volume semantics to match Game Volume:
 * internal 0..400, UI displays /2 (0..200). v28 is byte-compatible. */
/* v27 changes only Game Volume semantics: gamevol is now internal 0..400
 * and the UI displays gamevol/2. v26 remains byte-compatible and migrates. */
/* AURORA_AUG19_BUNDLE_V3: v26 appends Turbo Speed + CPU Overclock. */
/* AURORA_CONFIG_V24_STORAGE_OBJ_LIMIT_MODE
 * v24 appends OBJ limiter mode. v23 imports its limiter/storage fields and
 * defaults the new mode to Per Scanline; v21/v22 retain exact import. */
/* AURORA_V85_SOFTWARE_HACKS_CONFIG
 * v20 only appends renderer-hack preferences. Old configs migrate with all
 * hacks disabled and all layers enabled. */

typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;     /* ganho interno da trilha: 0..400; UI /2 */
	Int32  bgmrate;    /* frequencia de sintese da trilha (Hz)     */
	Int32  gamevol;    /* SNES/QuickNES gain: 0..400; UI /2 */
	Int32  hddenable;  /* suporte ao HD interno (hdd0:): 0=off, 1=on  */
	Int32  mmceenable; /* suporte a MMCE (mmce0/1): 0=off, 1=on       */
	Int32  massenable; /* mass/USB (mass0/1): 0=off, 1=on             */
	Int32  smbenable;  /* historical host slot; now smb: 0=off, 1=on */
	Int32  mx4sioenable; /* MX4SIO (SD via SIO2): 0=off, 1=on         */
	Int32  colorprofile; /* SNPPU_COLOR_PROFILE_*                     */
	Int32  famicloneaudio;
	Int32  fakesramsize;
	Int32  forceregion;
	Int32  sneshacklayersoff;
	Int32  sneshackflags;
	Int32  compatflags;
	Int32  objlimit;
	Int32  sramdevice;
	Int32  objlimitmode;
	Int32  snesmousemode;
	Int32  turbospeed;
	Int32  cpuoverclock;
	Int32  bgmenable;   /* Menu Music: 0=off, 1=on */
	Int32  md6button;   /* bit0=6-button, bit1=BCA */
	Int32  bgmtrack;    /* 1..64 */
	Int32  mdrendering; /* 0=Fast, 1=Good, 2=Accurate */
	Int32  segavol;       /* v36: PicoDrive final gain 0..400; UI /2 */
	Int32  segaaudiorate; /* independent PicoDrive PCM rate */
	Int32  smscolorborder; /* 0=black, 1=SMS VDP backdrop */
	Int32  smsfm;          /* 0=off, 1=Master System YM2413/OPLL */
	Int32  pcevol;         /* v37: Beetle PCE Fast gain 0..400; UI /2 */
	Int32  snescore;       /* v38: persisted SNES core selector */
	Int32  safeframeskip;  /* v41: 0=Off, 1..9=max auto skips; default 1 */
	Int32  ggzoom;         /* v39: GG 160x144 -> 240x216, uniform 3:2 */
	Int32  lightgun;       /* v42: CRC-known NES/Famicom gun; 1=On */
	Int32  cdmusic;        /* v43: SCD/PCE CD Red Book CDDA; 1=On */
} VideoCfgT;
#define VIDEOCFG_V42_BYTES (sizeof(VideoCfgT) - sizeof(Int32))
#define VIDEOCFG_V38_BYTES (sizeof(VideoCfgT) - 4 * sizeof(Int32))
#define VIDEOCFG_V37_BYTES (VIDEOCFG_V38_BYTES - sizeof(Int32))

/* v16 is the exact prefix written by v1.0.4 and by the first video-fix
   test build. Keep it readable so installing this build never resets the
   user's mode, offsets, audio volumes or storage choices. */
typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  hostenable;
	Int32  mx4sioenable;
} VideoCfgV16T;

typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
} VideoCfgV17T;

typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
	Int32  famicloneaudio;
} VideoCfgV18T;

/* Exact on-card layout written by v19. */
typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
	Int32  famicloneaudio;
	Int32  fakesramsize;
	Int32  forceregion;
} VideoCfgV19T;

/* Exact on-card layout written by v20 (V8.5). */
typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
	Int32  famicloneaudio;
	Int32  fakesramsize;
	Int32  forceregion;
	Int32  sneshacklayersoff;
	Int32  sneshackflags;
} VideoCfgV20T;

/* Exact byte layout written by v21 and v22. */
typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
	Int32  famicloneaudio;
	Int32  fakesramsize;
	Int32  forceregion;
	Int32  sneshacklayersoff;
	Int32  sneshackflags;
	Int32  compatflags;
} VideoCfgV22T;


/* Exact byte layout written by Aurora video.cfg v24. */
typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
	Int32  famicloneaudio;
	Int32  fakesramsize;
	Int32  forceregion;
	Int32  sneshacklayersoff;
	Int32  sneshackflags;
	Int32  compatflags;
	Int32  objlimit;
	Int32  sramdevice;
	Int32  objlimitmode;
} VideoCfgV24T;

/* Exact byte layout written by Aurora video.cfg v25. */
typedef struct
{
	VideoCfgV24T v24;
	Int32 snesmousemode;
} VideoCfgV25T;

/* Exact byte layout written by Aurora V1/V1.1 (video.cfg v23). */
typedef struct
{
	Uint32 magic;
	Int32  version;
	Int32  mode;
	Int32  offx;
	Int32  offy;
	Int32  overscan;
	Int32  widescreen;
	Int32  covers;
	Int32  bgmvol;
	Int32  bgmrate;
	Int32  gamevol;
	Int32  hddenable;
	Int32  mmceenable;
	Int32  massenable;
	Int32  smbenable;
	Int32  mx4sioenable;
	Int32  colorprofile;
	Int32  famicloneaudio;
	Int32  fakesramsize;
	Int32  forceregion;
	Int32  sneshacklayersoff;
	Int32  sneshackflags;
	Int32  compatflags;
	Int32  objlimit;
	Int32  sramdevice;
} VideoCfgV23T;

typedef struct
{
	Uint32 magic;
	Int32  version;
} VideoCfgHeaderT;

static void _VideoCfgPath(char *pOut)
{
	strcpy(pOut, _SramPath);
	strcat(pOut, "/video.cfg");
}

static Bool g_FamicloneAudio = FALSE;

/* AURORA_CD_MUSIC_REDBOOK_V3_20260830
 * Shared by Sega CD and PCE CD; Red Book/CDDA only. */
static Bool g_CdMusicEnabled = TRUE;

static void _VideoSetCdMusicEnabled(Bool enabled)
{
	g_CdMusicEnabled = enabled ? TRUE : FALSE;
	PicoDriveBridge_SetCdMusicEnabled(g_CdMusicEnabled ? true : false);
	PceBridge_SetCdMusicEnabled(g_CdMusicEnabled ? true : false);
	if (!g_CdMusicEnabled)
		MainLoopSafeFrameskipCancelCdAudioWindow();
}

/* AURORA_COMPAT_PAGE_V21
 * Zero flags = exact V8.5 host behavior. */
#define VIDEO_COMPAT_GS_FULL_CACHE   (1 << 0)
#define VIDEO_COMPAT_GIF_LONG_WAIT   (1 << 1)
#define VIDEO_COMPAT_AUDIO_SMALL_RPC (1 << 2)
#define VIDEO_COMPAT_AUDIO_DEEP_Q    (1 << 3)
#define VIDEO_COMPAT_ALL             0x0F

static Int32 g_VideoCompatFlags = 0;

static void _VideoApplyCompatFlags(Int32 flags)
{
	g_VideoCompatFlags = flags & VIDEO_COMPAT_ALL;
	GPFifoSetCompatFullCache(
		(g_VideoCompatFlags & VIDEO_COMPAT_GS_FULL_CACHE) ? TRUE : FALSE);
	DmaSetGifCompatLongWait(
		(g_VideoCompatFlags & VIDEO_COMPAT_GIF_LONG_WAIT) ? TRUE : FALSE);
	Aud_SetCompatSmallChunks(
		(g_VideoCompatFlags & VIDEO_COMPAT_AUDIO_SMALL_RPC) ? 1 : 0);
	Aud_SetCompatDeepQueue(
		(g_VideoCompatFlags & VIDEO_COMPAT_AUDIO_DEEP_Q) ? 1 : 0);
}

void VideoSettingsSave(void)
{
	VideoCfgT cfg;
	char      path[300];

	cfg.magic   = VIDEOCFG_MAGIC;
	cfg.version = VIDEOCFG_VERSION;
	cfg.mode    = g_GskVideoMode;
	cfg.offx    = g_GskDispOffX;
	cfg.offy    = g_GskDispOffY;
	cfg.overscan   = g_GskOverscan;
	cfg.widescreen = g_GskWidescreen;
	cfg.covers     = CoverIsEnabled() ? 1 : 0;
	cfg.bgmvol     = BgmGetVolume();
	cfg.bgmenable  = BgmIsEnabled() ? 1 : 0;
	cfg.bgmrate    = BgmGetRate();
	cfg.gamevol    = AudMixGameGetVolume();
	cfg.hddenable  = HddSupportIsEnabled() ? 1 : 0;
	cfg.mmceenable = MmceSupportIsEnabled() ? 1 : 0;
	cfg.massenable = MassStorageIsEnabled() ? 1 : 0;
	cfg.smbenable  = SmbSupportIsEnabled() ? 1 : 0;
	cfg.mx4sioenable = Mx4sioIsEnabled() ? 1 : 0;
		/* AURORA_SNES9X2010_V2_PS2LEAN_20260824: retain the v37 field/layout, but always persist default. */
		cfg.colorprofile = SNPPU_COLOR_PROFILE_ORIGINAL;
	cfg.famicloneaudio = g_FamicloneAudio ? 1 : 0;
	cfg.fakesramsize = g_FakeSRAMSize;
	cfg.forceregion = g_SnesForceRegion;
	cfg.sneshacklayersoff =
		(~SNPPURenderGetSoftwareLayerMask()) &
		(SNESPPU_MASK_BG1 | SNESPPU_MASK_BG2 | SNESPPU_MASK_BG3 |
		 SNESPPU_MASK_BG4 | SNESPPU_MASK_OBJ);
	/* Retire the old fixed SNES frameskip bit; index 36 no longer owns it. */
	cfg.sneshackflags =
		SNPPURenderGetSoftwareHackFlags() & ~SNPPU_HACK_FRAME_SKIP;
	cfg.compatflags = g_VideoCompatFlags & VIDEO_COMPAT_ALL;
	cfg.objlimit = SNPPURenderGetObjLimitLevel();
	cfg.sramdevice = MainLoopSramGetDevice();
	cfg.objlimitmode = SNPPURenderGetObjLimitMode();
	cfg.snesmousemode = (Int32)InputSnesMouseGetMode();
	cfg.turbospeed = (Int32)MainLoopTurboGetSpeed();
	cfg.cpuoverclock = SNCPU_OVERCLOCK_OFF;
	/* AURORA_MD_PAD_LAYOUT_V1: keep video.cfg v32 byte-identical. */
	cfg.md6button =
		(PicoDriveBridge_Get6Button() ? 1 : 0) |
		(MainLoopMdPadGetLayout() == MAINLOOP_MD_PAD_BCA ? 2 : 0);
	cfg.bgmtrack = BgmGetTrackIndex();
	cfg.mdrendering = PicoDriveBridge_GetRenderingMode();
	cfg.segavol = AudMixSegaGetVolume();
	cfg.pcevol = AudMixPceGetVolume();
	cfg.segaaudiorate = (Int32)PicoDriveBridge_GetAudioRate();
	cfg.smscolorborder = PicoDriveBridge_GetSmsColorBorder() ? 1 : 0;
	cfg.smsfm = PicoDriveBridge_GetSmsFm() ? 1 : 0;
	/* AURORA_SNES9X2010_V3_MENU_BRIDGE_20260824 */
	cfg.snescore = MainLoopSnesCoreGetPersisted();
	/* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2: v39 append-only fields. */
	cfg.safeframeskip = MainLoopSafeFrameskipGetLevel();
	cfg.ggzoom = PicoDriveBridge_GetGgZoom() ? 1 : 0;
	cfg.lightgun = QuicknesBridge_GetLightGunEnabled() ? 1 : 0;
	cfg.cdmusic = g_CdMusicEnabled ? 1 : 0; /* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
	_VideoCfgPath(path);
	BgmIOBegin();
	MemCardWriteFile(path, (Uint8 *)&cfg, sizeof(cfg));
	BgmIOEnd();
}

void VideoSettingsLoad(void)
{
	VideoCfgT cfg;
	VideoCfgV16T oldcfg;
	VideoCfgHeaderT header;
	char      path[300];
	Bool      loaded = FALSE;


	/* AURORA_SNES9X2010_V3_MENU_BRIDGE_20260824 */
	MainLoopSnesCoreSetPersisted(0);
	/* AURORA_FAMICOM_MIC_CFG41_20260828: conservative Aurora default. */
	MainLoopSafeFrameskipSetLevel(1);
	PicoDriveBridge_SetGgZoom(false);
	memset(&cfg, 0, sizeof(cfg));
	cfg.lightgun = 1; /* v42 default and all pre-v42 migrations: On */
	cfg.cdmusic = 1;  /* AURORA_CD_MUSIC_REDBOOK_V3_20260830: pre-v43 default On */
	QuicknesBridge_SetLightGunEnabled(true);
	_VideoSetCdMusicEnabled(TRUE);
	SNPPURenderSetSoftwareLayerMask(
		SNESPPU_MASK_BG1 | SNESPPU_MASK_BG2 | SNESPPU_MASK_BG3 |
		SNESPPU_MASK_BG4 | SNESPPU_MASK_OBJ);
	SNPPURenderSetSoftwareHackFlags(SNPPU_HACK_MODE7_HALF);
	SNPPURenderSetObjLimitLevel(SNPPU_OBJ_LIMIT_OFF);
	SNPPURenderSetObjLimitMode(SNPPU_OBJ_LIMIT_MODE_SCANLINE);
	MainLoopSramSetDevice(MAINLOOP_SRAMDEVICE_AUTO);
	InputSnesMouseSetMode(INPUT_SNES_MOUSE_OFF);
	MainLoopTurboSetSpeed(MAINLOOP_TURBO_SPEED_NORMAL);
	SNCPUSetOverclockLevel(_pSnes ? _pSnes->GetCpu() : NULL, SNCPU_OVERCLOCK_OFF);
	_VideoApplyCompatFlags(0);
	_VideoCfgPath(path);

	memset(&header, 0, sizeof(header));
	if (MemCardReadFile(path, (Uint8 *)&header, sizeof(header)) &&
	    header.magic == VIDEOCFG_MAGIC)
	{
		if (header.version == VIDEOCFG_VERSION)
		{
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, sizeof(cfg));
		}
		else if (header.version == 42)
		{
			/* AURORA_CD_MUSIC_REDBOOK_V3_20260830
			 * v42 is the exact v43 prefix before cdmusic; default stays On. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, VIDEOCFG_V42_BYTES);
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 41 || header.version == 40 || header.version == 39)
		{
			/* v39/v40/v41 are the exact v42 prefix before lightgun.
			 * lightgun and cdmusic were preinitialised On. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg,
			                         VIDEOCFG_V42_BYTES - sizeof(cfg.lightgun));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 38)
		{
			/* v38 is the exact prefix before the two v39 fields. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, VIDEOCFG_V38_BYTES);
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 37)
		{
			/* v37 is the exact prefix before snescore. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, VIDEOCFG_V37_BYTES);
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 36)
		{
			/* v36 is exactly the v37 prefix without the appended PCE volume. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg,
			        VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 35)
		{
			/* v35 is byte-identical in size; this slot used to be the
			 * legacy SNES rate value and is migrated below. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, (VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 34)
		{
			loaded = MemCardReadFile(path, (Uint8 *)&cfg,
			        (VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 33)
		{
			loaded = MemCardReadFile(path, (Uint8 *)&cfg,
			        ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 32)
		{
			loaded = MemCardReadFile(path, (Uint8 *)&cfg,
			        ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 31)
		{
			/* AURORA_PICODRIVE_CURRENT_CFG_V32 */
			memset(&cfg, 0, ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)));
			loaded = MemCardReadFile(path, (Uint8 *)&cfg,
			        ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.md6button) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 30)
		{
			/* v30 has the same byte layout as v31. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.md6button) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 29)
		{
			/* v29 was a temporary test config with broken menu-audio defaults.
			 * Import its layout, then reset only those two settings once. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.md6button) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering));
			if (loaded)
			{
				cfg.version = VIDEOCFG_VERSION;
				cfg.bgmvol = 200;   /* UI 100 */
				cfg.bgmenable = 0;  /* Off */
			}
		}
		else if (header.version == 28)
		{
			/* v28 has the same byte layout; only Menu Volume semantics changed. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.md6button) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 27)
		{
			/* v27 is the v28/v29 prefix without bgmenable. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.md6button) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering) - sizeof(Int32));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 26)
		{
			/* v26 has the same byte layout; only Game Volume semantics changed. */
			loaded = MemCardReadFile(path, (Uint8 *)&cfg, ((VIDEOCFG_V37_BYTES - sizeof(cfg.pcevol)) - sizeof(cfg.smscolorborder) - sizeof(cfg.smsfm)) - sizeof(cfg.segavol) - sizeof(cfg.segaaudiorate) - sizeof(cfg.md6button) - sizeof(cfg.bgmtrack) - sizeof(cfg.mdrendering) - sizeof(Int32));
			if (loaded) cfg.version = VIDEOCFG_VERSION;
		}
		else if (header.version == 25)
		{
			VideoCfgV25T oldcfg25;
			memset(&oldcfg25, 0, sizeof(oldcfg25));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg25, sizeof(oldcfg25)))
			{
				memcpy(&cfg, &oldcfg25, sizeof(oldcfg25));
				cfg.version = VIDEOCFG_VERSION;
				switch (oldcfg25.snesmousemode)
				{
					case 1: cfg.snesmousemode = INPUT_SNES_MOUSE_CONTROLLER; break;
					case 3: cfg.snesmousemode = INPUT_SNES_MOUSE_USB; break;
					default: cfg.snesmousemode = INPUT_SNES_MOUSE_OFF; break;
				}
				cfg.turbospeed = MAINLOOP_TURBO_SPEED_NORMAL;
				cfg.cpuoverclock = SNCPU_OVERCLOCK_OFF;
				loaded = TRUE;
			}
		}
		else if (header.version == 24)
		{
			VideoCfgV24T oldcfg24;
			memset(&oldcfg24, 0, sizeof(oldcfg24));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg24, sizeof(oldcfg24)))
			{
				memcpy(&cfg, &oldcfg24, sizeof(oldcfg24));
				cfg.version = VIDEOCFG_VERSION;
				cfg.snesmousemode = INPUT_SNES_MOUSE_OFF;
				loaded = TRUE;
			}
		}
		else if (header.version == 23)
		{
			VideoCfgV23T oldcfg23;
			memset(&oldcfg23, 0, sizeof(oldcfg23));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg23, sizeof(oldcfg23)))
			{
				memcpy(&cfg, &oldcfg23, sizeof(oldcfg23));
				cfg.version = VIDEOCFG_VERSION;
				cfg.objlimitmode = SNPPU_OBJ_LIMIT_MODE_SCANLINE;
				loaded = TRUE;
			}
		}
		else if (header.version == 22 || header.version == 21)
		{
			VideoCfgV22T oldcfg22;
			memset(&oldcfg22, 0, sizeof(oldcfg22));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg22, sizeof(oldcfg22)))
			{
				memcpy(&cfg, &oldcfg22, sizeof(oldcfg22));
				cfg.version = VIDEOCFG_VERSION;
				cfg.objlimit = SNPPU_OBJ_LIMIT_OFF;
				cfg.sramdevice = MAINLOOP_SRAMDEVICE_AUTO;
				cfg.objlimitmode = SNPPU_OBJ_LIMIT_MODE_SCANLINE;
				loaded = TRUE;
			}
		}
		else if (header.version == 20)
		{
			VideoCfgV20T oldcfg20;
			memset(&oldcfg20, 0, sizeof(oldcfg20));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg20, sizeof(oldcfg20)))
			{
				memcpy(&cfg, &oldcfg20, sizeof(oldcfg20));
				cfg.version = VIDEOCFG_VERSION;
				cfg.compatflags = 0;
				loaded = TRUE;
			}
		}
		else if (header.version == 19)
		{
			VideoCfgV19T oldcfg19;

			memset(&oldcfg19, 0, sizeof(oldcfg19));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg19, sizeof(oldcfg19)))
			{
				memcpy(&cfg, &oldcfg19, sizeof(oldcfg19));
				cfg.version = VIDEOCFG_VERSION;
				cfg.sneshacklayersoff = 0;
				cfg.sneshackflags = 0;
				loaded = TRUE;
			}
		}
		else if (header.version == 18)
		{
			VideoCfgV18T oldcfg18;

			memset(&oldcfg18, 0, sizeof(oldcfg18));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg18, sizeof(oldcfg18)))
			{
				memcpy(&cfg, &oldcfg18, sizeof(oldcfg18));
				cfg.version = VIDEOCFG_VERSION;
				cfg.fakesramsize = 0;
				cfg.forceregion = SNES_FORCE_REGION_OFF;
				loaded = TRUE;
			}
		}
		else if (header.version == 17)
		{
	VideoCfgV17T oldcfg17;

	memset(&oldcfg17, 0, sizeof(oldcfg17));
	if (MemCardReadFile(path, (Uint8 *)&oldcfg17, sizeof(oldcfg17)))
	{
		memcpy(&cfg, &oldcfg17, sizeof(oldcfg17));
		cfg.version = VIDEOCFG_VERSION;
		cfg.famicloneaudio = 0;
		loaded = TRUE;
	}
}
		else if (header.version == 16)
		{
			memset(&oldcfg, 0, sizeof(oldcfg));
			if (MemCardReadFile(path, (Uint8 *)&oldcfg, sizeof(oldcfg)))
			{
				/* VideoCfgT only appends colorprofile to the v16 prefix. */
				memcpy(&cfg, &oldcfg, sizeof(oldcfg));
				cfg.version = VIDEOCFG_VERSION;
				cfg.colorprofile = SNPPU_COLOR_PROFILE_ORIGINAL;
				loaded = TRUE;
			}
		}
	}

	/* v27 and older had no independent Menu Music switch.
	 * New default policy is Off; Menu Volume remains independent. */
	if (loaded && header.version <= 27)
		cfg.bgmenable = 0;

	/* v26 and older stored Game Volume as the old 0..100 UI value.
	 * Preserve the audible setting (old 100 -> internal 200 -> UI 100). */
	if (loaded && header.version <= 26 &&
	    cfg.gamevol >= 0 && cfg.gamevol <= 100)
		cfg.gamevol *= 2;

	/* v28 and older stored Menu Volume directly as 0..100.
	 * v29 uses the same convention as Game Volume:
	 * old 100 -> internal 200 -> UI still shows 100. */
	if (loaded && header.version <= 28 &&
	    cfg.bgmvol >= 0 && cfg.bgmvol <= 100)
		cfg.bgmvol *= 2;

	/* v31 establishes a clean Menu Volume default for every older config.
	 * UI 100 == internal 200. From v31 onward, preserve the saved value. */
	if (loaded && header.version <= 30)
		cfg.bgmvol = 200;

	if (loaded && header.version <= 32)
	{
		cfg.bgmtrack = 1;
		cfg.mdrendering = 1;
	}

	/* AURORA_AUDIO_SPLIT_VOLUMES_V36_20260823
	 * v35 and older had one final Game Volume shared by every core.
	 * Copy the already-migrated gamevol into SEGA volume so old configs keep
	 * exactly the same audible balance after upgrading. */
	if (loaded && header.version <= 35)
		cfg.segavol = cfg.gamevol;

	/* PCE used Game Volume through v36. Preserve exactly that audible
	 * level on upgrade: old UI 100/internal 200 -> PCE UI 100. */
	if (loaded && header.version <= 36)
		cfg.pcevol = cfg.gamevol;
	if (loaded && header.version <= 33)
		cfg.segaaudiorate = 16000;

	/* Preserve the pre-v35 behaviour when importing any old config. */
	if (loaded && header.version <= 34)
	{
		cfg.smscolorborder = 1;
		cfg.smsfm = 0;
	}

	/* The fixed 1-in-2 SNES frameskip is retired in v39. Clear it even
	 * when importing a v38 file that had the old Performance row On. */
	if (loaded)
		cfg.sneshackflags &= ~SNPPU_HACK_FRAME_SKIP;

	/* AURORA_FAMICOM_MIC_CFG41_20260828
	 * Preserve v42 Safe Frameskip when v43 only adds CD music.
	 * Older migrations retain the previous one-time conservative default. */
	if (loaded && header.version < 42)
		cfg.safeframeskip = 1;

	/* New policy applies exactly once to every pre-v22 config. Once the
	 * user saves v22, a manual Full selection remains persistent. */
	if (loaded && header.version < 22)
		cfg.sneshackflags |= SNPPU_HACK_MODE7_HALF;

	if (loaded && cfg.magic == VIDEOCFG_MAGIC)
	{
		if (cfg.snescore >= 0 && cfg.snescore < 2)
			MainLoopSnesCoreSetPersisted(cfg.snescore);
		/* v1.0.2 allowed both SIO2 storage hooks to be saved at once.
		   Prefer MMCE when importing such a legacy config; all new changes
		   are mutually exclusive in the setters below. */
		if (cfg.mmceenable == 1 && cfg.mx4sioenable == 1)
			cfg.mx4sioenable = 0;

		if (cfg.mode == GSK_VIDMODE_240P ||
            cfg.mode == GSK_VIDMODE_480I ||
            cfg.mode == GSK_VIDMODE_1080I ||
            cfg.mode == GSK_VIDMODE_1080P)
			g_GskVideoMode = cfg.mode;
		else
			g_GskVideoMode = GSK_VIDMODE_480I;

		if (cfg.offx >= -64 && cfg.offx <= 64) g_GskDispOffX = cfg.offx;
		if (cfg.offy >= -64 && cfg.offy <= 64) g_GskDispOffY = cfg.offy;
		if (cfg.overscan >= 0 && cfg.overscan <= 100) g_GskOverscan = cfg.overscan;
		if (cfg.widescreen == 0 || cfg.widescreen == 1) g_GskWidescreen = cfg.widescreen;
		if (cfg.covers == 0 || cfg.covers == 1) CoverSetEnabled(cfg.covers ? TRUE : FALSE);
		if (cfg.smscolorborder == 0 || cfg.smscolorborder == 1)
			PicoDriveBridge_SetSmsColorBorder(cfg.smscolorborder != 0);
		if (header.version >= 39 && header.version <= VIDEOCFG_VERSION &&
		    cfg.safeframeskip >= 0 && cfg.safeframeskip <= 9)
			MainLoopSafeFrameskipSetLevel(cfg.safeframeskip);
		if (cfg.ggzoom == 0 || cfg.ggzoom == 1)
			PicoDriveBridge_SetGgZoom(cfg.ggzoom != 0);
		if (cfg.lightgun == 0 || cfg.lightgun == 1)
			QuicknesBridge_SetLightGunEnabled(cfg.lightgun != 0);
		if (cfg.cdmusic == 0 || cfg.cdmusic == 1)
			_VideoSetCdMusicEnabled(cfg.cdmusic ? TRUE : FALSE);
		if (cfg.smsfm == 0 || cfg.smsfm == 1)
			PicoDriveBridge_SetSmsFm(cfg.smsfm != 0);
		if (cfg.bgmvol >= 0 && cfg.bgmvol <= 400) BgmSetVolume(cfg.bgmvol);
		if (cfg.bgmenable == 0 || cfg.bgmenable == 1) BgmSetEnabled(cfg.bgmenable);
		if (cfg.bgmrate >= 8000 && cfg.bgmrate <= 48000)
			BgmSetRate(cfg.bgmrate);
		/* SNES is always native 32 kHz; v36 reuses the old persisted rate
		 * slot for SEGA volume without changing VideoCfgT size. */
		SnesAudioSetRate(SNSPCDSP_SAMPLERATE);
		if (cfg.segaaudiorate >= 8000 && cfg.segaaudiorate <= 48000)
			PicoDriveBridge_SetAudioRate(cfg.segaaudiorate);
		if (cfg.gamevol >= 0 && cfg.gamevol <= 400) AudMixGameSetVolume(cfg.gamevol);
		if (cfg.segavol >= 0 && cfg.segavol <= 400) AudMixSegaSetVolume(cfg.segavol);
		if (cfg.pcevol >= 0 && cfg.pcevol <= 400) AudMixPceSetVolume(cfg.pcevol);
		if (cfg.bgmtrack >= 1 && cfg.bgmtrack <= 64) BgmSetTrackIndex(cfg.bgmtrack);
		if (cfg.mdrendering >= 0 && cfg.mdrendering <= 2)
			PicoDriveBridge_SetRenderingMode(cfg.mdrendering);
		if (cfg.hddenable == 0 || cfg.hddenable == 1) HddSupportSetEnabled(cfg.hddenable);
		if (cfg.mmceenable == 0 || cfg.mmceenable == 1) MmceSupportSetEnabled(cfg.mmceenable);
		if (cfg.massenable == 0 || cfg.massenable == 1) MassStorageSetEnabled(cfg.massenable);
		if (cfg.smbenable == 0 || cfg.smbenable == 1) SmbSupportSetEnabled(cfg.smbenable);
		if (cfg.mx4sioenable == 0 || cfg.mx4sioenable == 1) Mx4sioSetEnabled(cfg.mx4sioenable);
		/* AURORA_SNES9X2010_V2_PS2LEAN_20260824: ignore legacy Composite configs; default is fixed. */
		SNPPUColorSetProfile(SNPPU_COLOR_PROFILE_ORIGINAL);
if (cfg.famicloneaudio == 0 || cfg.famicloneaudio == 1)
{
	g_FamicloneAudio = cfg.famicloneaudio ? TRUE : FALSE;
	QuicknesBridge_SetDutySwap(g_FamicloneAudio ? true : false);
}
		if (cfg.fakesramsize == 0 ||
		    cfg.fakesramsize == 8 ||
		    cfg.fakesramsize == 16 ||
		    cfg.fakesramsize == 32 ||
		    cfg.fakesramsize == 64 ||
		    cfg.fakesramsize == 128 ||
		    cfg.fakesramsize == 256 ||
		    cfg.fakesramsize == 512 ||
		    cfg.fakesramsize == 1024 ||
		    cfg.fakesramsize == 2048)
		{
			g_FakeSRAMSize = cfg.fakesramsize;
		}
		if (cfg.forceregion == SNES_FORCE_REGION_OFF ||
		    cfg.forceregion == SNES_FORCE_REGION_NTSC_U ||
		    cfg.forceregion == SNES_FORCE_REGION_NTSC_J ||
		    cfg.forceregion == SNES_FORCE_REGION_PAL)
		{
			g_SnesForceRegion = cfg.forceregion;
			PicoDriveBridge_SetRegion(g_SnesForceRegion);
		}

		const Int32 uLayerBits =
			SNESPPU_MASK_BG1 | SNESPPU_MASK_BG2 | SNESPPU_MASK_BG3 |
			SNESPPU_MASK_BG4 | SNESPPU_MASK_OBJ;
		if ((cfg.sneshacklayersoff & ~uLayerBits) == 0)
			SNPPURenderSetSoftwareLayerMask(
				(Uint8)(uLayerBits & ~cfg.sneshacklayersoff));
		if ((cfg.sneshackflags & ~SNPPU_HACK_ALL) == 0)
			SNPPURenderSetSoftwareHackFlags((Uint8)cfg.sneshackflags);
		if (cfg.objlimit >= SNPPU_OBJ_LIMIT_OFF && cfg.objlimit < SNPPU_OBJ_LIMIT_NUM)
			SNPPURenderSetObjLimitLevel((Uint8)cfg.objlimit);
		if (cfg.objlimitmode >= SNPPU_OBJ_LIMIT_MODE_SCANLINE &&
		    cfg.objlimitmode < SNPPU_OBJ_LIMIT_MODE_NUM)
			SNPPURenderSetObjLimitMode((Uint8)cfg.objlimitmode);
		if (cfg.sramdevice >= MAINLOOP_SRAMDEVICE_AUTO &&
		    cfg.sramdevice < MAINLOOP_SRAMDEVICE_NUM)
			MainLoopSramSetDevice((MainLoopSramDeviceE)cfg.sramdevice);
		if ((cfg.compatflags & ~VIDEO_COMPAT_ALL) == 0)
			_VideoApplyCompatFlags(cfg.compatflags);
		if (cfg.snesmousemode >= INPUT_SNES_MOUSE_OFF &&
		    cfg.snesmousemode < INPUT_SNES_MOUSE_MODE_NUM)
			InputSnesMouseSetMode(
				(InputSnesMouseModeE)cfg.snesmousemode);
		if (cfg.turbospeed >= MAINLOOP_TURBO_SPEED_NORMAL &&
		    cfg.turbospeed < MAINLOOP_TURBO_SPEED_NUM)
			MainLoopTurboSetSpeed((MainLoopTurboSpeedE)cfg.turbospeed);
		/* AURORA_PICODRIVE_CURRENT_CFG_APPLY
		 * AURORA_MD_PAD_LAYOUT_V1: valores antigos 0/1 = ABC. */
		if (cfg.md6button >= 0 && cfg.md6button <= 3)
		{
			PicoDriveBridge_Set6Button((cfg.md6button & 1) != 0);
			MainLoopMdPadSetLayout(
				(cfg.md6button & 2) ? MAINLOOP_MD_PAD_BCA
				                     : MAINLOOP_MD_PAD_ABC);
		}
		SNCPUSetOverclockLevel(_pSnes ? _pSnes->GetCpu() : NULL,
		                         SNCPU_OVERCLOCK_OFF);
	}
}

/* AURORA_MOUSE_VIDEO_COMPAT_V1 */
/* SNES Mouse lives on Video Config 4/4 and uses Left/Right. */

/* ------------------------------------------------------------------ */
/* Screen                                                              */
/* ------------------------------------------------------------------ */

CVideoScreen::CVideoScreen()
{
	m_iSelect = 0;
}

void CVideoScreen::Process()
{
}

static void _VideoCenter(int x, int y, const char *pStr)
{
	FontPuts(x - FontGetStrWidth(pStr) / 2, y, pStr);
}

static void _VideoRow(int vy, int idx, int sel, const char *pLabel, const char *pValue)
{
	if (idx == sel)
	{
		PolyColor4f(0.0f, 0.5f, 0.0f, 0.5f);
		PolyRect(48, vy - 1, 160, FontGetHeight() + 2);
	}

	FontColor4f(0.5f, 0.5f, 0.5f, 1.0f);
	FontPuts(56, vy, pLabel);

	FontColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	FontPuts(150, vy, pValue);
}

static void _VideoHeader(int vy, const char *pStr)
{
	PolyColor4f(0.0f, 0.2f, 0.2f, 0.5f);
	PolyRect(32, vy, 256 - 64, 9);
	FontColor4f(0.0f, 0.8f, 0.8f, 1.0f);
	_VideoCenter(128, vy, pStr);
}

static const char *_VideoMmceStatus()
{
	int slots;

	if (!MmceSupportIsEnabled()) return "Off";
	if (MmceNeedsRestart())      return "Restart";
	if (MmceGetLastError() < 0)  return "Driver Error";
	if (!MmceIsLoaded())         return "On";

	slots = MmceGetAvailableSlots();
	if (slots == 1) return "Slot 1";
	if (slots == 2) return "Slot 2";
	if (slots == 3) return "Slots 1+2";
	return "Not Found";
}

static const char *_VideoSafeFrameskipStatus()
{
    static const char *const names[10] =
        { "Off", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
    Int32 level = MainLoopSafeFrameskipGetLevel();
    if (level < 0 || level > 9) level = 1;
    return names[level];
}

static const char *_VideoFakeSRAMStatus()
{
   switch (g_FakeSRAMSize)
{
case 8:    return "1 KB";
case 16:   return "2 KB";
case 32:   return "4 KB";
case 64:   return "8 KB";
case 128:  return "16 KB";
case 256:  return "32 KB";
case 512:  return "64 KB";
case 1024: return "128 KB";
case 2048: return "256 KB";
default:   return "Auto";
}
}

static const char *_VideoForceRegionStatus()
{
    switch (g_SnesForceRegion)
    {
        case SNES_FORCE_REGION_NTSC_U:
            return "NTSC-U";

        case SNES_FORCE_REGION_NTSC_J:
            return "NTSC-J";

        case SNES_FORCE_REGION_PAL:
            return "PAL";

        case SNES_FORCE_REGION_OFF:
        default:
            return "Auto";
    }
}

static const char *_VideoFamicloneAudioStatus()
{
	return g_FamicloneAudio ? "On" : "Off";
}

static const char *_VideoHackLayerStatus(Uint8 uLayer)
{
	return (SNPPURenderGetSoftwareLayerMask() & uLayer) ? "On" : "Off";
}

static const char *_VideoHackAccurateStatus(Uint8 uFlag)
{
	return (SNPPURenderGetSoftwareHackFlags() & uFlag) ? "Off" : "Accurate";
}

static const char *_VideoHackMode7Status()
{
	return (SNPPURenderGetSoftwareHackFlags() & SNPPU_HACK_MODE7_HALF)
		? "Half" : "Full";
}

static const char *_VideoHackSpriteLimiterStatus()
{
	/* AURORA_V15_MULTICORE_SPRITE_LIMIT_20260824
	 * SNES values are OBJ-tile budgets. Other machines use different native
	 * units/limits, so don't display a misleading SNES number there. */
	const Bool bSnesUnits = (_pSystem == _pSnes) ? TRUE : FALSE;
	switch (SNPPURenderGetObjLimitLevel())
	{
		case SNPPU_OBJ_LIMIT_LIGHT:   return bSnesUnits ? "Light (28)"   : "Light";
		case SNPPU_OBJ_LIMIT_MEDIUM:  return bSnesUnits ? "Medium (24)"  : "Medium";
		case SNPPU_OBJ_LIMIT_STRONG:  return bSnesUnits ? "Strong (20)"  : "Strong";
		case SNPPU_OBJ_LIMIT_EXTREME: return bSnesUnits ? "Extreme (16)" : "Extreme";
		case SNPPU_OBJ_LIMIT_HEAVY:   return bSnesUnits ? "Heavy (12)"   : "Heavy";
		case SNPPU_OBJ_LIMIT_INSANE:  return bSnesUnits ? "Insane (8)"   : "Insane";
		default:                      return bSnesUnits ? "Off (34)"      : "Off";
	}
}

static const char *_VideoHackSpriteLimiterModeStatus()
{
	return SNPPURenderGetObjLimitMode() == SNPPU_OBJ_LIMIT_MODE_SCREEN
		? "Per Screen" : "Per Scanline";
}

static const char *_VideoHackCpuOverclockStatus()
{
	switch (SNCPUGetOverclockLevel())
	{
		case SNCPU_OVERCLOCK_120: return "120%";
		case SNCPU_OVERCLOCK_150: return "150%";
		case SNCPU_OVERCLOCK_200: return "200%";
		case SNCPU_OVERCLOCK_300: return "300%";
		default:                  return "Off";
	}
}

static const char *_VideoCompatProfileStatus()
{
	if (g_VideoCompatFlags == 0) return "Standard";
	if (g_VideoCompatFlags == VIDEO_COMPAT_ALL) return "Conservative";
	return "Custom";
}
static const char *_VideoCompatGsCacheStatus()
{
	return (g_VideoCompatFlags & VIDEO_COMPAT_GS_FULL_CACHE) ? "Full" : "Range";
}
static const char *_VideoCompatGifWaitStatus()
{
	return (g_VideoCompatFlags & VIDEO_COMPAT_GIF_LONG_WAIT) ? "Long" : "Normal";
}
static const char *_VideoCompatAudioRpcStatus()
{
	return (g_VideoCompatFlags & VIDEO_COMPAT_AUDIO_SMALL_RPC) ? "1 KB" : "4 KB";
}
static const char *_VideoCompatAudioQueueStatus()
{
	return (g_VideoCompatFlags & VIDEO_COMPAT_AUDIO_DEEP_Q) ? "Deep" : "Normal";
}


/* AURORA_AUDIO_SPLIT_REVIEW_V1_20260821 */
static Int32 _VideoCycleSystemAudioRate(Int32 hz, int dir)
{
    static const Int32 rates[] =
        { 16000, 22050, 24000, 32000, 38000, 44100, 48000 };
    const Int32 count = (Int32)(sizeof(rates) / sizeof(rates[0]));
    Int32 i, idx = 0;

    for (i = 0; i < count; ++i)
    {
        if (rates[i] == hz)
        {
            idx = i;
            break;
        }
    }

    idx += (dir < 0) ? -1 : 1;
    if (idx < 0) idx = count - 1;
    if (idx >= count) idx = 0;
    return rates[idx];
}

static const char *_VideoMdRenderingStatus()
{
	switch (PicoDriveBridge_GetRenderingMode())
	{
		case 0: return "Fast";
		case 1: return "Good";
		default: return "Accurate";
	}
}

static const char *_VideoMx4sioStatus()
{
	if (!Mx4sioIsEnabled())       return "Off";
	if (Mx4sioNeedsRestart())     return "Restart";
	if (Mx4sioGetLastError() < 0) return "Driver Error";
	return Mx4sioIsLoaded() ? "On" : "Enabled";
}

typedef struct
{
	Int32 mode;
	const char *name;
} VideoModeChoiceT;

static const VideoModeChoiceT _VideoModes[] =
{
	{ GSK_VIDMODE_240P,  "240p/288p (CRT)" },
	{ GSK_VIDMODE_480I,  "480i (default)" },
	{ GSK_VIDMODE_1080I, "1080i" }
};

static Int32 _VideoModeIndex(Int32 mode)
{
	Int32 i;
	for (i = 0; i < (Int32)(sizeof(_VideoModes) / sizeof(_VideoModes[0])); i++)
		if (_VideoModes[i].mode == mode)
			return i;
	return 0;
}

void CVideoScreen::Draw()
{
	Int32 vy = 15;
	char  buf[16];
	int   m = _VideoModeIndex(g_GskVideoMode);
	const char *pMode = _VideoModes[m].name;
	/* AURORA_V85_SOFTWARE_HACKS_PAGE
	 * AURORA_MD_MENU_MAPPING_SRAM_FIX_V4: controller page dedicada.
	 * Display order: 0..9, 30..36, 40..44, 20..29, 10..19. */
	int   iPage = (m_iSelect >= 50) ? 1 :
	              ((m_iSelect >= 40) ? 3 :
	              ((m_iSelect >= 31) ? 2 :
	              ((m_iSelect >= 20) ? 4 :
	              ((m_iSelect >= 10) ? 5 : 0))));
	const char *pWide = "Off";
	/* AURORA_SNES9X2010_V2_PS2LEAN_20260824: SNES colour profile is intentionally fixed to
	 * the original/default palette on PS2; no menu row is exposed. */

	if (g_GskWidescreen)
		pWide = "On";

	FontSelect(0);

	_VideoHeader(vy,
		iPage == 0 ? "Settings Menu (1/6)" :
		iPage == 1 ? "Settings Menu (2/6)" :
		iPage == 2 ? "Settings Menu (3/6)" :
		iPage == 3 ? "Settings Menu (4/6)" :
		iPage == 4 ? "Settings Menu (5/6)" :
		             "Settings Menu (6/6)");
	vy += 18;

	if (iPage == 0) {
	_VideoHeader(vy, "Screen");
	vy += 14;

	_VideoRow(vy, 0, m_iSelect, "Video Mode", pMode);  vy += 12;

	_VideoRow(vy, 1, m_iSelect, "Widescreen", pWide); vy += 12;

	snprintf(buf, sizeof(buf), "%d", g_GskOverscan);
	_VideoRow(vy, 2, m_iSelect, "Overscan", buf);      vy += 12;

	snprintf(buf, sizeof(buf), "%d", g_GskDispOffX);
	_VideoRow(vy, 3, m_iSelect, "Offset X", buf);      vy += 12;

	snprintf(buf, sizeof(buf), "%d", g_GskDispOffY);
	_VideoRow(vy, 4, m_iSelect, "Offset Y", buf);      vy += 12;

	_VideoRow(vy, 5, m_iSelect, "Cover Art", CoverIsEnabled() ? "On" : "Off"); vy += 12;
	_VideoRow(vy, 6, m_iSelect, "SMS VDP border",
	          PicoDriveBridge_GetSmsColorBorder() ? "On" : "Off"); vy += 12;
	_VideoRow(vy, 7, m_iSelect, "GG Zoom",
	          PicoDriveBridge_GetGgZoom() ? "On" : "Off"); vy += 12;
	_VideoRow(vy, 8, m_iSelect, "Safe Frameskip",
	          _VideoSafeFrameskipStatus()); vy += 12;

	}
	else if (iPage == 1)
	{
		_VideoHeader(vy, "Audio"); vy += 14;
		_VideoRow(vy, 50, m_iSelect, "Menu Music",
		          BgmIsEnabled() ? "ON" : "OFF"); vy += 12;
		snprintf(buf, sizeof(buf), "%d", BgmGetVolume() / 2);
		_VideoRow(vy, 51, m_iSelect, "Menu volume", buf); vy += 12;
		snprintf(buf, sizeof(buf), "%d", AudMixGameGetVolume() / 2);
		_VideoRow(vy, 52, m_iSelect, "SNES volume", buf); vy += 12;
		snprintf(buf, sizeof(buf), "%d", AudMixSegaGetVolume() / 2);
		_VideoRow(vy, 53, m_iSelect, "SEGA volume", buf); vy += 12;
		snprintf(buf, sizeof(buf), "%d", AudMixPceGetVolume() / 2);
		_VideoRow(vy, 54, m_iSelect, "PCE volume", buf); vy += 12;
		_VideoRow(vy, 55, m_iSelect, "SNES audio", "32 kHz native"); vy += 12;
		snprintf(buf, sizeof(buf), "%d kHz", (PicoDriveBridge_GetAudioRate() + 500) / 1000);
		_VideoRow(vy, 56, m_iSelect, "SEGA audio", buf); vy += 12;
		_VideoRow(vy, 57, m_iSelect, "SMS FM audio",
		          PicoDriveBridge_GetSmsFm() ? "Enable" : "Disable"); vy += 12;
		_VideoRow(vy, 58, m_iSelect, "CD music",
		          g_CdMusicEnabled ? "ON" : "OFF"); vy += 12; /* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
	}
	else if (iPage == 5)
	{
		_VideoHeader(vy, "Storage / Devices"); vy += 14;

		_VideoRow(vy, 10, m_iSelect, "Mass / USB",
		          MassStorageIsEnabled() ? "On" : "Off"); vy += 12;
		_VideoRow(vy, 11, m_iSelect, "HDD Support",
		          HddSupportIsEnabled() ? "On" : "Off"); vy += 12;
		_VideoRow(vy, 12, m_iSelect, "MMCE Cards",
		          _VideoMmceStatus()); vy += 12;
		_VideoRow(vy, 13, m_iSelect, "SMB (Network)",
		          SmbGetStatusText()); vy += 12;
		_VideoRow(vy, 14, m_iSelect, "MX4SIO (SD)",
		          _VideoMx4sioStatus()); vy += 12;

_VideoHeader(vy, "Misc."); vy += 14;

_VideoRow(vy, 15, m_iSelect, "SRAM Size",
          _VideoFakeSRAMStatus()); vy += 12;

_VideoRow(vy, 16, m_iSelect, "Force Region",
          _VideoForceRegionStatus()); vy += 12;

_VideoRow(vy, 17, m_iSelect, "Famiclone Audio",
          _VideoFamicloneAudioStatus()); vy += 12;

_VideoRow(vy, 18, m_iSelect, "Reset emulator", ""); vy += 12;
_VideoRow(vy, 19, m_iSelect, "Exit to OSD", ""); vy += 12;

	}
	else if (iPage == 4)
	{
		_VideoHeader(vy, "SNES Hacks"); vy += 14;
		/* AURORA_SNES9X2010_V1_RUNTIMEFIX_20260824 -- runtime-only in V1. */
		_VideoRow(vy, 30, m_iSelect, "SNES Core",
			MainLoopSnesCoreGetName()); vy += 12;
		_VideoRow(vy, 20, m_iSelect, "BG1 Layer",
			_VideoHackLayerStatus(SNESPPU_MASK_BG1)); vy += 12;
		_VideoRow(vy, 21, m_iSelect, "BG2 Layer",
			_VideoHackLayerStatus(SNESPPU_MASK_BG2)); vy += 12;
		_VideoRow(vy, 22, m_iSelect, "BG3 Layer",
			_VideoHackLayerStatus(SNESPPU_MASK_BG3)); vy += 12;
		_VideoRow(vy, 23, m_iSelect, "BG4 Layer",
			_VideoHackLayerStatus(SNESPPU_MASK_BG4)); vy += 12;
		_VideoRow(vy, 24, m_iSelect, "Sprites / OBJ",
			_VideoHackLayerStatus(SNESPPU_MASK_OBJ)); vy += 12;
		_VideoRow(vy, 25, m_iSelect, "Color Math",
			_VideoHackAccurateStatus(SNPPU_HACK_COLOR_MATH_OFF)); vy += 12;
		_VideoRow(vy, 26, m_iSelect, "Window Effects",
			_VideoHackAccurateStatus(SNPPU_HACK_WINDOWS_OFF)); vy += 12;
		_VideoRow(vy, 27, m_iSelect, "Mode 7 Quality",
			_VideoHackMode7Status()); vy += 12;
		_VideoRow(vy, 28, m_iSelect, "Sprite Limiter",
			_VideoHackSpriteLimiterStatus()); vy += 12;
		_VideoRow(vy, 29, m_iSelect, "Limiter Mode",
			_VideoHackSpriteLimiterModeStatus()); vy += 12;
	}
	else if (iPage == 2)
	{
		_VideoHeader(vy, "Performance"); vy += 14;
		_VideoRow(vy, 31, m_iSelect, "Profile",
			_VideoCompatProfileStatus()); vy += 12;
		_VideoRow(vy, 32, m_iSelect, "GS Cache Sync",
			_VideoCompatGsCacheStatus()); vy += 12;
		_VideoRow(vy, 33, m_iSelect, "GIF DMA Wait",
			_VideoCompatGifWaitStatus()); vy += 12;
		_VideoRow(vy, 34, m_iSelect, "Audio RPC Chunk",
			_VideoCompatAudioRpcStatus()); vy += 12;
		_VideoRow(vy, 35, m_iSelect, "Audio Queue",
			_VideoCompatAudioQueueStatus()); vy += 12;
		_VideoRow(vy, 37, m_iSelect, "MD rendering",
			_VideoMdRenderingStatus()); vy += 12;

	}
	else if (iPage == 3)
	{
		/* AURORA_MD_MENU_MAPPING_SRAM_FIX_V4 */
		_VideoHeader(vy, "Controller options"); vy += 14;
		_VideoRow(vy, 40, m_iSelect, "Use mouse",
			InputSnesMouseGetModeName()); vy += 12;
		_VideoRow(vy, 41, m_iSelect, "MD pad",
			PicoDriveBridge_Get6Button() ? "6-button" : "3-button"); vy += 12;
		_VideoRow(vy, 42, m_iSelect, "MD mapping",
			MainLoopMdPadGetLayoutName()); vy += 12;
		_VideoRow(vy, 43, m_iSelect, "Turbo Speed",
			MainLoopTurboGetSpeedName()); vy += 12;
		_VideoRow(vy, 44, m_iSelect, "Light Gun",
			QuicknesBridge_GetLightGunEnabled() ? "On" : "Off"); vy += 12;
	}

	/* controls / hints (clear of the vy=215 footer) */
	vy = 184;
	FontColor4f(0.6f, 0.6f, 0.6f, 1.0f);
	_VideoCenter(128, vy, "Up/Dn: select   L/R: change   X: save"); vy += 12;
	_VideoCenter(128, vy, "O: next   Square: previous"); vy += 12;

	if (g_GskVideoMode != GSK_GetActiveVideoMode())
	{
		FontColor4f(1.0f, 0.88f, 0.46f, 1.0f);
		_VideoCenter(128, vy, "mode applies after reboot");
	}
	else if (MmceNeedsRestart() || Mx4sioNeedsRestart())
	{
		FontColor4f(1.0f, 0.88f, 0.46f, 1.0f);
		_VideoCenter(128, vy, "storage applies after reboot");
	}
}

void CVideoScreen::Input(Uint32 buttons, Uint32 trigger)
{
	int dir = 0;

	/* AURORA_PD_MEGA_FIX_20260820: Screen -> Audio -> Performance -> Controller -> Hacks -> Devices. */
	if (trigger & PAD_CIRCLE)
	{
		if (m_iSelect < 10)        m_iSelect = 50;
		else if (m_iSelect < 20)   m_iSelect = 0;
		else if (m_iSelect <= 30)  m_iSelect = 10;
		else if (m_iSelect < 40)   m_iSelect = 40;
		else if (m_iSelect < 50)   m_iSelect = 20;
		else                       m_iSelect = 31;
	}

	{
		int lo, hi;
		if (m_iSelect < 10)       { lo = 0;  hi = 8;  }
		else if (m_iSelect < 20)  { lo = 10; hi = 19; }
		else if (m_iSelect <= 30) { lo = 20; hi = 30; }
		else if (m_iSelect < 40)  { lo = 31; hi = 37; }
		else if (m_iSelect < 50)  { lo = 40; hi = 44; }
		else                      { lo = 50; hi = 58; } /* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
		if (trigger & PAD_UP)
		{
			m_iSelect--;
			if (m_iSelect < lo) m_iSelect = hi;
			/* Keep retired index 36 unreachable without renumbering 37. */
			if (m_iSelect == 36) m_iSelect = 35;
		}
		if (trigger & PAD_DOWN)
		{
			m_iSelect++;
			if (m_iSelect > hi) m_iSelect = lo;
			if (m_iSelect == 36) m_iSelect = 37;
		}
	}

	if (trigger & PAD_LEFT)  dir = -1;
	if (trigger & PAD_RIGHT) dir = +1;

	if (dir != 0)
	{
		switch (m_iSelect)
		{
		case 0: /* video mode (live GS reinit) */
			{
				Int32 count = (Int32)(sizeof(_VideoModes) / sizeof(_VideoModes[0]));
				Int32 modeIndex = _VideoModeIndex(g_GskVideoMode) + dir;
				if (modeIndex < 0)      modeIndex = count - 1;
				if (modeIndex >= count) modeIndex = 0;
				MainLoopReinitVideoMode(_VideoModes[modeIndex].mode);
			}
			break;

		case 1: /* widescreen on/off (live) */
			g_GskWidescreen = !g_GskWidescreen;
			GSK_SetWidescreen(g_GskWidescreen);
			break;

		case 2: /* overscan 0..100 (live, step 5) */
			g_GskOverscan += dir * 5;
			if (g_GskOverscan < 0)   g_GskOverscan = 0;
			if (g_GskOverscan > 100) g_GskOverscan = 100;
			GSK_SetOverscan(g_GskOverscan);
			break;

		case 3: /* offset X (live) */
			g_GskDispOffX += dir;
			if (g_GskDispOffX < -64) g_GskDispOffX = -64;
			if (g_GskDispOffX >  64) g_GskDispOffX =  64;
			GSK_SetDisplayOffset(g_GskDispOffX, g_GskDispOffY);
			break;

		case 4: /* offset Y (live) */
			g_GskDispOffY += dir;
			if (g_GskDispOffY < -64) g_GskDispOffY = -64;
			if (g_GskDispOffY >  64) g_GskDispOffY =  64;
			GSK_SetDisplayOffset(g_GskDispOffX, g_GskDispOffY);
			break;

		case 5: /* cover art on/off (live; persisted on X like the rest) */
			CoverToggle();
			break;

		case 6: /* SMS VDP border color / black, live */
			PicoDriveBridge_SetSmsColorBorder(
				!PicoDriveBridge_GetSmsColorBorder());
			break;
		case 7: /* Game Gear exact 3:2 zoom, 160x144 -> 240x216 */
			PicoDriveBridge_SetGgZoom(!PicoDriveBridge_GetGgZoom());
			break;
		case 8: /* Safe Frameskip: Off, On */
		{
			Int32 level = MainLoopSafeFrameskipGetLevel();
			level += (dir > 0) ? 1 : -1;
			if (level > 9) level = 0;
			if (level < 0) level = 9;
			MainLoopSafeFrameskipSetLevel(level);
			break;
		}

		case 50: /* Menu Music ON/OFF. */
			BgmSetEnabled(!BgmIsEnabled());
			break;
		case 51: /* Menu volume: UI 0..200, internal 0..400. */
			{ int v = BgmGetVolume() + dir * 2; if (v < 0) v = 0; if (v > 400) v = 400; BgmSetVolume(v); }
			break;
		case 52: /* SNES volume: SNESticle + QuickNES, UI 0..200. */
			{ int v = AudMixGameGetVolume() + dir * 2; if (v < 0) v = 0; if (v > 400) v = 400; AudMixGameSetVolume(v); }
			break;
		case 53: /* SEGA volume: PicoDrive only, UI 0..200. */
			{ int v = AudMixSegaGetVolume() + dir * 2; if (v < 0) v = 0; if (v > 400) v = 400; AudMixSegaSetVolume(v); }
			break;
		case 54: /* PCE volume: Beetle PCE Fast only, UI 0..200. */
			{ int v = AudMixPceGetVolume() + dir * 2; if (v < 0) v = 0; if (v > 400) v = 400; AudMixPceSetVolume(v); }
			break;
		case 55:
			/* SNES stays native 32 kHz; row retained as informational. */
			SnesAudioSetRate(SNSPCDSP_SAMPLERATE);
			if (_pSystem == _pSnes && _AudMix)
				_AudMix->SetSampleRate(SNSPCDSP_SAMPLERATE);
			break;
		case 56:
			PicoDriveBridge_SetAudioRate(
				_VideoCycleSystemAudioRate(PicoDriveBridge_GetAudioRate(), dir));
			break;
		case 57: /* Master System YM2413/OPLL */
			PicoDriveBridge_SetSmsFm(!PicoDriveBridge_GetSmsFm());
			break;
		case 58: /* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
			_VideoSetCdMusicEnabled(!g_CdMusicEnabled);
			break;

		case 10: /* Mass / USB on/off -- lista mass0:/mass1: (USB).  O USB core
		           sobe no boot de qualquer forma (seguro); isto controla a
		           listagem.  O MX4SIO agora tem toggle proprio (case 14). */
			MassStorageSetEnabled(!MassStorageIsEnabled());
			break;

		case 11: /* HDD interno (hdd0:) on/off -- lista + carga preguicosa. */
			HddSupportSetEnabled(!HddSupportIsEnabled());
			break;

		case 12: /* MMCE (mmce0/1) on/off -- lista + carga preguicosa. */
			MmceSupportSetEnabled(!MmceSupportIsEnabled());
			if (MmceSupportIsEnabled())
			{
				BgmIOBegin();
				MmceProbeAvailableSlots();
				BgmIOEnd();
			}
			break;

		case 13: /* SMB on/off. Driver/network stay lazy until smb: is opened. */
			if (SmbSupportIsEnabled())
			{
				BgmIOBegin();
				SmbDisconnect();
				BgmIOEnd();
				SmbSupportSetEnabled(0);
			}
			else
			{
				SmbSupportSetEnabled(1);
			}
			break;

		case 14: /* MX4SIO (SD via SIO2) on/off -- carga preguicosa (deferida).
		            Padrao OFF: quem nao tem o adaptador evita o flood de
		            sondagem do SIO2.  Independente do Mass/USB. */
			Mx4sioSetEnabled(!Mx4sioIsEnabled());
			if (Mx4sioIsEnabled())
			{
				BgmIOBegin();
				Mx4sioLoadIfEnabled();
				BgmIOEnd();
			}
			break;

case 15: /* SRAM Size */
    switch (g_FakeSRAMSize)
    {
        case 0:
            g_FakeSRAMSize = (dir > 0) ? 8 : 2048;
            break;

        case 8:
            g_FakeSRAMSize = (dir > 0) ? 16 : 0;
            break;

        case 16:
            g_FakeSRAMSize = (dir > 0) ? 32 : 8;
            break;

        case 32:
            g_FakeSRAMSize = (dir > 0) ? 64 : 16;
            break;

        case 64:
            g_FakeSRAMSize = (dir > 0) ? 128 : 32;
            break;

        case 128:
            g_FakeSRAMSize = (dir > 0) ? 256 : 64;
            break;

        case 256:
            g_FakeSRAMSize = (dir > 0) ? 512 : 128;
            break;

        case 512:
            g_FakeSRAMSize = (dir > 0) ? 1024 : 256;
            break;

        case 1024:
            g_FakeSRAMSize = (dir > 0) ? 2048 : 512;
            break;

        case 2048:
        default:
            g_FakeSRAMSize = (dir > 0) ? 0 : 1024;
            break;
    }
    break;

case 16: /* Force Region */
    if (dir > 0)
    {
        switch (g_SnesForceRegion)
        {
            case SNES_FORCE_REGION_OFF:
                g_SnesForceRegion = SNES_FORCE_REGION_NTSC_U;
                break;

            case SNES_FORCE_REGION_NTSC_U:
                g_SnesForceRegion = SNES_FORCE_REGION_NTSC_J;
                break;

            case SNES_FORCE_REGION_NTSC_J:
                g_SnesForceRegion = SNES_FORCE_REGION_PAL;
                break;

            case SNES_FORCE_REGION_PAL:
            default:
                g_SnesForceRegion = SNES_FORCE_REGION_OFF;
                break;
        }
    }
    else
    {
        switch (g_SnesForceRegion)
        {
            case SNES_FORCE_REGION_OFF:
                g_SnesForceRegion = SNES_FORCE_REGION_PAL;
                break;

            case SNES_FORCE_REGION_NTSC_U:
                g_SnesForceRegion = SNES_FORCE_REGION_OFF;
                break;

            case SNES_FORCE_REGION_NTSC_J:
                g_SnesForceRegion = SNES_FORCE_REGION_NTSC_U;
                break;

            case SNES_FORCE_REGION_PAL:
            default:
                g_SnesForceRegion = SNES_FORCE_REGION_NTSC_J;
                break;
        }
    }
    PicoDriveBridge_SetRegion(g_SnesForceRegion);
    break;
case 17: /* Famiclone Audio */
    g_FamicloneAudio = !g_FamicloneAudio;
    QuicknesBridge_SetDutySwap(g_FamicloneAudio ? true : false);
    break;

		case 20: case 21: case 22: case 23: case 24:
		{
			static const Uint8 kLayers[5] = {
				SNESPPU_MASK_BG1, SNESPPU_MASK_BG2, SNESPPU_MASK_BG3,
				SNESPPU_MASK_BG4, SNESPPU_MASK_OBJ
			};
			Uint8 uMask = SNPPURenderGetSoftwareLayerMask();
			uMask ^= kLayers[m_iSelect - 20];
			SNPPURenderSetSoftwareLayerMask(uMask);
			break;
		}
		case 25:
			SNPPURenderSetSoftwareHackFlags(
				SNPPURenderGetSoftwareHackFlags() ^ SNPPU_HACK_COLOR_MATH_OFF);
			break;
		case 26:
			SNPPURenderSetSoftwareHackFlags(
				SNPPURenderGetSoftwareHackFlags() ^ SNPPU_HACK_WINDOWS_OFF);
			break;
		case 27:
			SNPPURenderSetSoftwareHackFlags(
				SNPPURenderGetSoftwareHackFlags() ^ SNPPU_HACK_MODE7_HALF);
			break;
		case 28:
			{
				Int32 level = (Int32)SNPPURenderGetObjLimitLevel() + dir;
				if (level < 0) level = SNPPU_OBJ_LIMIT_NUM - 1;
				if (level >= SNPPU_OBJ_LIMIT_NUM) level = 0;
				SNPPURenderSetObjLimitLevel((Uint8)level);
			}
			break;
		case 29:
			{
				Int32 mode = (Int32)SNPPURenderGetObjLimitMode() + dir;
				if (mode < 0) mode = SNPPU_OBJ_LIMIT_MODE_NUM - 1;
				if (mode >= SNPPU_OBJ_LIMIT_MODE_NUM) mode = 0;
				SNPPURenderSetObjLimitMode((Uint8)mode);
			}
			break;
		case 30: /* AURORA_SNES9X2010_V1_RUNTIMEFIX_20260824 -- applies on next SNES launch. */
			MainLoopSnesCoreCycleDir(dir);
			break;
		case 31:
			_VideoApplyCompatFlags(
				g_VideoCompatFlags == VIDEO_COMPAT_ALL ? 0 : VIDEO_COMPAT_ALL);
			break;
		case 32:
			_VideoApplyCompatFlags(
				g_VideoCompatFlags ^ VIDEO_COMPAT_GS_FULL_CACHE);
			break;
		case 33:
			_VideoApplyCompatFlags(
				g_VideoCompatFlags ^ VIDEO_COMPAT_GIF_LONG_WAIT);
			break;
		case 34:
			_VideoApplyCompatFlags(
				g_VideoCompatFlags ^ VIDEO_COMPAT_AUDIO_SMALL_RPC);
			break;
		case 35:
			_VideoApplyCompatFlags(
				g_VideoCompatFlags ^ VIDEO_COMPAT_AUDIO_DEEP_Q);
			break;
		case 37:
			{ int v = PicoDriveBridge_GetRenderingMode() + dir; if (v < 0) v = 2; if (v > 2) v = 0; PicoDriveBridge_SetRenderingMode(v); }
			break;
		case 40:
			InputSnesMouseCycleModeDir(dir);
			break;
		case 41:
			/* AURORA_PICODRIVE_CURRENT_6BUTTON */
			PicoDriveBridge_Set6Button(!PicoDriveBridge_Get6Button());
			break;
		case 42:
			MainLoopMdPadCycleLayoutDir(dir);
			break;
		case 43:
			MainLoopTurboCycleSpeedDir(dir);
			break;
		case 44:
			QuicknesBridge_SetLightGunEnabled(
				!QuicknesBridge_GetLightGunEnabled());
			break;
		}


	}

	/* Square: previous page in the displayed six-page order. */
	if (trigger & PAD_SQUARE)
	{
		if (m_iSelect >= 50)      m_iSelect = 0;
		else if (m_iSelect >= 40) m_iSelect = 31;
		else if (m_iSelect >= 31) m_iSelect = 50;
		else if (m_iSelect >= 20) m_iSelect = 40;
		else if (m_iSelect >= 10) m_iSelect = 20;
		else                      m_iSelect = 10;
	}

/* Cross / Start: persist ordinary settings; immediate actions never save. */
if (trigger & (PAD_CROSS | PAD_START))
{
    if (m_iSelect == 18)
    {
        if (Aud_IsInitialized()) Aud_Setvol(0);
        MainResetEmulator();
    }
    else if (m_iSelect == 19)
    {
        if (Aud_IsInitialized()) Aud_Setvol(0);
        ExecOSD(0, NULL);
    }
    else
        VideoSettingsSave();
}
}

