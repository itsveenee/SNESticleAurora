/* gskit_backend.c
 *
 * gsKit-based replacement for the original direct-GS pipeline.
 * See gskit_backend.h for the public API.
 *
 * Fase 1 GS->gsKit migration.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <gsInline.h>
#include <gsToolkit.h>

#include "types.h"
#include "ps2dma.h"
#include "gs.h"
#include "gskit_backend.h"
#include "gpprim.h"

/* Legacy logical coordinate space the entire UI was written in. Both
   supported outputs use a 640x480 physical framebuffer; 1080i is scaled by
   the PCRTC into a centred 1280x960 4:3 window. */
#define GSK_LOGICAL_W   256
#define GSK_LOGICAL_H   240

/* The original headers use these constants for mode / interlace. They
   live in gs.h but we want this TU to compile without dragging the
   register-level header in, so re-declare the values that match. */
#ifndef GS_NTSC
#define GS_NTSC          2
#define GS_PAL           3
#define GS_INTERLACE     1
#define GS_NONINTERLACED  0
#endif

static GSGLOBAL *_pGsGlobal = NULL;
static int       _gsk_initialised = 0;
static int       _gsk_invalidate_pending = 0;

/* AURORA_GS_LATENCY_V1
 * Off by default: every caller that does not explicitly opt into the gameplay
 * fast-clear path retains the exact historical full-frame clear. */
static Bool      _gsk_gameplay_fast_clear = FALSE;
/* AURORA_PD_DIRECT_MD_SKIP_CLEAR_V3_C_20260821 */
static Bool      _gsk_gameplay_skip_clear = FALSE;

/* Video mode + display offset (selectable in the Settings screen).
   480i is the safe default and 1080i is the only alternate output. */
int g_GskVideoMode = GSK_VIDMODE_480I;
int g_GskDispOffX  = 0;
int g_GskDispOffY  = 0;
int g_GskOverscan  = 0;   /* 0..100 shrink of display area */
int g_GskWidescreen = 0;  /* 0 = 4:3, 1 = safe 16:9 presentation */
static int _gsk_vck         = 4;   /* display-offset VCK units            */
static int _gsk_fb_width    = 640; /* active FB width                     */
static int _gsk_fb_height   = 480; /* active FB height                    */
static int _gsk_active_mode = GSK_VIDMODE_480I; /* mode the GS is in now   */
static int _gsk_native240p_par = 0;
static int _gsk_240p_fb_width = 256;
/* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
 * Optional horizontal scanout window inside the 240p framebuffer.
 * -1 means normal full-framebuffer presentation. */
static int _gsk_240p_window_x = -1;
static int _gsk_240p_window_w = 0;
/* AURORA_MD_UI256_320FB_V1_20260823
 * Keep MD's physical 320-wide framebuffer while its cartridge is alive,
 * but draw/scan Aurora's UI as exactly 256 uniform source pixels. */
static int _gsk_ui256_on_320fb = 0;
static int _gsk_game_y_bias = 0;

/* gsKit's computed DISPLAY params, captured after gsKit_init_screen so
   overscan/widescreen can be recomputed from a clean baseline. */
static int _gsk_base_dw, _gsk_base_dh, _gsk_base_magh, _gsk_base_magv;
static int _gsk_base_startx, _gsk_base_starty;

static void _GskApplyDisplay(void);   /* offset + overscan + widescreen */
static void _GskApplyRenderTransform(void);

/* Saved GSK_Init arguments so GSK_ReinitVideo() can replay them. */
static int _gsk_arg_w, _gsk_arg_h, _gsk_arg_dispx, _gsk_arg_dispy;
static int _gsk_arg_psm, _gsk_arg_psmz, _gsk_arg_mode, _gsk_arg_interlace;

GSGLOBAL *GSK_GetGlobal(void)
{
    return _pGsGlobal;
}

/* AURORA_MEGA_V2_GS_REFRESH_IMPL
 * PS2 NTSC VBlank is the 59.94-family clock, not exactly 60.000 Hz. Feeding
 * this rational to the audio mixer prevents a slow ring-buffer phase drift.
 */
void GSK_GetRefreshRate(Uint32 *pNumerator, Uint32 *pDenominator)
{
    if (!pNumerator || !pDenominator) return;
    if (_pGsGlobal && _pGsGlobal->Mode == GS_MODE_PAL)
    {
        *pNumerator = 50;
        *pDenominator = 1;
    }
    else
    {
        *pNumerator = 60000;
        *pDenominator = 1001;
    }
}


/* Detect the console's TV region from the BIOS ROMVER region byte.
 *
 * This is the well-known, public technique used by uLaunchELF / OPL /
 * Libito (LbFn): the character at BIOS address 0x1FC7FF52 is the region
 * letter of the ROMVER string -- 'E' on European consoles (PAL) and
 * J/A/C/H/K on the NTSC ones (Japan/USA/China/HongKong/Korea).
 *
 * Returns GS_MODE_PAL only when the byte is unambiguously 'E'; anything
 * else (including a BIOS that doesn't expose it) falls back to the safe
 * 60Hz GS_MODE_NTSC.  NTSC consoles therefore keep the EXACT behaviour
 * they had before this was added -- only PAL consoles take the new path.
 */
static int _gsk_DetectTvMode(void)
{
    volatile char region = *(volatile char *)0x1FC7FF52;
    return (region == 'E') ? GS_MODE_PAL : GS_MODE_NTSC;
}

void GSK_Init(int width, int height,
              int dispx, int dispy,
              int psm, int psmz,
              int mode, int interlace)
{
    if (_gsk_initialised) {
        return;
    }

    _gsk_arg_w     = width;  _gsk_arg_h        = height;
    _gsk_arg_dispx = dispx;  _gsk_arg_dispy    = dispy;
    _gsk_arg_psm   = psm;    _gsk_arg_psmz     = psmz;
    _gsk_arg_mode  = mode;   _gsk_arg_interlace = interlace;

    _pGsGlobal = gsKit_init_global();
    if (!_pGsGlobal) {
        return;
    }

    /* Map iaddis-style mode constants to gsKit's enum.
       The original code uses GS_NTSC=2 / GS_PAL=3 which happen to
       match GS_MODE_NTSC / GS_MODE_PAL exactly, but go through the
       check anyway in case a caller passes something else. */
    _pGsGlobal->Mode = (mode == GS_PAL) ? GS_MODE_PAL : GS_MODE_NTSC;

    /* The legacy caller still passes its old interlace argument, but the
       backend now exposes interlaced modes only and owns this choice. */
    (void)interlace;
        switch (g_GskVideoMode)
    {
    case GSK_VIDMODE_1080P: // <-- BLOCO NOVO ADICIONADO
        _pGsGlobal->Mode = GS_MODE_DTV_1080P;
        _pGsGlobal->Interlace = GS_NONINTERLACED;
        _pGsGlobal->Field = GS_FRAME;
        _gsk_fb_width = 640;
        _gsk_fb_height = 480;
        _gsk_vck = 1;
        break;

    case GSK_VIDMODE_1080I:
        _pGsGlobal->Mode = GS_MODE_DTV_1080I;
        _pGsGlobal->Interlace = GS_INTERLACED;
        _pGsGlobal->Field = GS_FIELD;
        _gsk_fb_width = 640;
        _gsk_fb_height = 480;
        _gsk_vck = 1;
        break;
    // ... deixe o restante do switch (240p, 480i) como está

    case GSK_VIDMODE_240P:
        /*
         * NTSC 256x240 progressive / PAL 256x240 inside a 288p raster.
         *
         * This is the native SNES/NES sample grid.  Keep the framebuffer
         * at 256x240 so the renderer's logical 256x240 canvas remains 1:1.
         *
         * Progressive output requires GS_FRAME rather than GS_FIELD.
         */
        _pGsGlobal->Mode      = _gsk_DetectTvMode();
        _pGsGlobal->Interlace = GS_NONINTERLACED;
        _pGsGlobal->Field     = GS_FRAME;
        _gsk_fb_width         = _gsk_240p_fb_width;
        _gsk_fb_height        = 240;
        _gsk_vck              = 4;
        break;

    case GSK_VIDMODE_480I:
    default:
        /* Unsupported or removed saved values are normalised to 480i. PAL
           consoles emit the same source centred in their interlaced raster. */
        g_GskVideoMode        = GSK_VIDMODE_480I;
        _pGsGlobal->Mode      = _gsk_DetectTvMode();
        _pGsGlobal->Interlace = GS_INTERLACED;
        _pGsGlobal->Field     = GS_FIELD;
        _gsk_fb_width         = 640;
        _gsk_fb_height        = 480;
        _gsk_vck              = 4;
        break;
    }
    _gsk_active_mode = g_GskVideoMode;

    /* The caller still describes the legacy 256x240 logical canvas;
       physical dimensions come from the selected output mode above. */
    (void)width;
    (void)height;
    _pGsGlobal->Width  = _gsk_fb_width;
    _pGsGlobal->Height = _gsk_fb_height;
    _pGsGlobal->PSM    = psm;
    _pGsGlobal->PSMZ   = psmz;

    /* Match the original code's behaviour: no Z buffer is needed for
       the 2D blit-style rendering this app does. The Z buffer in the
       old draw_env was only there because GS_SetEnv set it up; the
       actual prims always use TEST_1 with Z disabled. */
    _pGsGlobal->ZBuffering      = GS_SETTING_OFF;
    _pGsGlobal->DoubleBuffering = GS_SETTING_ON;
    _pGsGlobal->PrimAAEnable    = GS_SETTING_OFF;
    _pGsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    _pGsGlobal->Dithering       = GS_SETTING_OFF;
    _pGsGlobal->DrawOrder       = GS_PER_OS;

    /* DMA setup. The SNES blender (snppublend_gs.cpp) also kicks raw
       DMA chains on the GIF channel; gsKit and the blender share the
       same channel, so they must be serialised via GSK_DrainAndWait. */
    /* RCYC=8, no source/dest stall, no MFIFO. The "STS" channel is
       UNSPEC because we do not opt into source-stall behaviour. */
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    /* Reset gsKit's VRAM allocation cursor before init_screen reserves the
       mode-specific framebuffers. (Despite its historical name,
       gsKit_vram_clear resets allocation state; it does not erase 4 MiB.) */
    gsKit_vram_clear(_pGsGlobal);

    gsKit_init_screen(_pGsGlobal);

    /* gsKit maps a 640-wide source across all 1920 pixels in 1080i,
       which turns 4:3 content into a horizontally stretched 16:9 image.
       MAGH=1 reads all 640 source pixels into a 1280-pixel window. The
       640x480 framebuffer is already mapped to 960 output rows (MAGV=1),
       so the resulting centred 1280x960 window has the correct 4:3 ratio.
       Widescreen remains an explicit user choice handled later. */
    if (_gsk_active_mode == GSK_VIDMODE_1080I)
    {
        const int aspect_dw = 1280;
        _pGsGlobal->StartX += (_pGsGlobal->DW - aspect_dw) / 2;
        _pGsGlobal->MagH = 1;
        _pGsGlobal->DW   = aspect_dw;
    }

    /* Capture gsKit's computed DISPLAY params as the baseline for the
       overscan / widescreen transform. */
    _gsk_base_dw     = _pGsGlobal->DW;
    _gsk_base_dh     = _pGsGlobal->DH;
    _gsk_base_magh   = _pGsGlobal->MagH;
    _gsk_base_magv   = _pGsGlobal->MagV;
    _gsk_base_startx = _pGsGlobal->StartX;
    _gsk_base_starty = _pGsGlobal->StartY;

    /* GSK_Init is now usable. Mark it before applying the saved display
       transform: _GskApplyDisplay used to return early here, so offsets,
       overscan and widescreen silently failed whenever a saved mode caused
       a boot-time GS reinitialisation. */
    _gsk_initialised = 1;

    /* Apply offset + overscan + widescreen (0/off = mode baseline). */
    _GskApplyDisplay();

    /* PMODE / DISPLAY1 / DISPLAY2 are now left at the values that
       gsKit_init_screen programmed (PMODE=0x8046 with CRTMD=1,
       DISPLAY1/2 with gsKit's auto-computed magnification). The
       previous code re-emitted those three registers with the
       iaddis legacy layout (PMODE=0xFF61, DW=2560, MagV=0) to
       work around a NetherSX2-only artefact, but that combination
       puts the PCRTC in a non-standard mode (CRTMD=0, EN1=1,
       EN2=0) that the real PS2 silicon does not handle the same
       way as emulators - on real hardware the picture comes up
       small in the centre with vertical-stripe garbage around it,
       because gsKit_sync_flip only updates DISPFB2 and DISPFB1
       (the only one being read with EN1=1, EN2=0) is left at its
       initial value, breaking double buffering.

       picodrive and Open-PS2-Loader both let gsKit handle PMODE
       and DISPLAY entirely, and both render correctly on real
       PS2. We follow the same pattern.

       If the NetherSX2 visual issue resurfaces, gate the override
       behind a build flag (e.g. -DBUILD_FOR_NETHERSX2=1) instead
       of penalising real hardware. */
    (void)dispx;
    (void)dispy;
    (void)width;
    (void)height;

    /* COLCLAMP is re-emitted every frame in GSK_ResetFrame (see
       comment there). The original iaddis pipeline (gs.c) set
       COLCLAMP=1 as part of GS_SetEnv but gsKit_init_screen does
       not touch it, so it sits at the GS reset default (0). */

    gsKit_set_test (_pGsGlobal, GS_ZTEST_OFF);
    gsKit_set_clamp(_pGsGlobal, GS_CMODE_REPEAT);
    gsKit_set_primalpha(_pGsGlobal,
        GS_SETREG_ALPHA(0, 1, 0, 1, 0x80), 0); /* Cs*As + Cd*(1-As) */

    gsKit_TexManager_init(_pGsGlobal);
    gsKit_mode_switch(_pGsGlobal, GS_ONESHOT);

    /* Clear both buffers so the screen starts black. */
    gsKit_clear(_pGsGlobal, 0);
    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
    gsKit_sync_flip(_pGsGlobal);
    gsKit_clear(_pGsGlobal, 0);
    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
    gsKit_sync_flip(_pGsGlobal);

}

/* Map the 256x240 application canvas onto the common 640x480 framebuffer. */
static void _GskApplyRenderTransform(void)
{
    float sx = (float)_gsk_fb_width / (float)GSK_LOGICAL_W;
    float sy = (float)_gsk_fb_height / (float)GSK_LOGICAL_H;

    /* AURORA_MD_UI256_320FB_V1_20260823
     * No 256->320 fractional UI scaling. Draw logical UI columns 1:1. */
    if (_gsk_ui256_on_320fb &&
        _gsk_active_mode == GSK_VIDMODE_240P &&
        _gsk_fb_width == 320)
    {
        sx = 1.0f;
    }

    GPPrimSetTransform(sx, sy, 0.0f, 0.0f);
}

static void _GskApplyDisplay(void)
{
    GSGLOBAL *gs = _pGsGlobal;
    int dw, dh, magh, startx, starty;

    if (!_gsk_initialised || !gs) {
        return;
    }

    dw     = _gsk_base_dw;
    dh     = _gsk_base_dh;
    magh   = _gsk_base_magh;
    startx = _gsk_base_startx;
    starty = _gsk_base_starty;

    /* Overscan: shrink the active area and recentre (adds a border to
       compensate TVs that crop the edges). */
    if (g_GskOverscan > 0)
    {
        int sx = (_gsk_base_dw * g_GskOverscan) / 1300;
        int sy = (_gsk_base_dh * g_GskOverscan) / 1300;
        dw     = _gsk_base_dw - sx * 2;
        dh     = _gsk_base_dh - sy * 2;
        startx = _gsk_base_startx + sx;
        starty = _gsk_base_starty + sy;
    }

    /* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
     * Fixed-512 PCE gameplay: present only the active source-width window.
     * FRAME/DISPFB stride remains 512, so changing 256/342/512 does not
     * require GS reinitialisation. DBX hides stale padding completely.
     *
     * Match the old native-raster physical timing:
     *   256 -> existing native-PAR path
     *   352 -> 8 VCK/sample (full 7 MHz overscan)
     *   512 -> use the 512-mode base geometry
     */
    if (_gsk_active_mode == GSK_VIDMODE_240P &&
        _gsk_240p_window_x >= 0 &&
        _gsk_240p_window_w > 0 &&
        g_GskOverscan == 0)
    {
        const int winw = _gsk_240p_window_w;

        /* AURORA_PCE_MODECLOCK_APERTURE_HALFTEXEL_V10_20260830
         * PCE dot-clock presentation on the fixed 512 storage raster.
         *
         * Keep every source sample intact; only PCRTC sample width changes.
         * 256 and 512 clocks have an exact 2:1 relation, so 10/5 VCK keeps
         * their analogue picture width identical (2560 VCK).
         *
         * The 7 MHz clock ideally falls halfway between two GS integer MAGH
         * steps (~7.5 VCK/sample when the 5 MHz mode is 10). GS cannot express
         * a half step. Prefer 7 rather than 8 so full 352-wide Japanese modes
         * (e.g. R-Type) remain inside the CRT aperture instead of losing side
         * detail. This is a uniform pixel width, never fractional resampling. */
        if (winw == 256)
        {
            const int contentMagH1 = 10;
            const int contentDw = winw * contentMagH1; /* 2560 */
            startx += (dw - contentDw) / 2;
            dw = contentDw;
            magh = contentMagH1 - 1;
        }
        else if (winw == 352)
        {
            const int contentMagH1 = 7;
            const int contentDw = winw * contentMagH1; /* 2464 */
            startx += (dw - contentDw) / 2;
            dw = contentDw;
            magh = contentMagH1 - 1;
        }
        else if (winw == 512)
        {
            const int contentMagH1 = 5;
            const int contentDw = winw * contentMagH1; /* 2560 */
            startx += (dw - contentDw) / 2;
            dw = contentDw;
            magh = contentMagH1 - 1;
        }

        /* Point the read circuit at the active window inside 512 storage.
         * gsKit may rewrite DISPFB during flip, so _GskApplyDisplay is also
         * re-run after sync_flip below. */
        GS_SET_DISPFB2(
            _pGsGlobal->ScreenBuffer[(_pGsGlobal->ActiveBuffer ^ 1) & 1] / 8192,
            _gsk_fb_width / 64,
            _pGsGlobal->PSM,
            _gsk_240p_window_x,
            0);
    }

    /*
     * NES/SNES 240p horizontal PAR correction.
     *
     * Do NOT scale the 256x240 framebuffer. Instead reduce the PCRTC
     * horizontal magnification by one integer step while continuing
     * to scan all 256 framebuffer pixels.
     *
     * In NTSC/PAL 240p with a 256-pixel framebuffer gsKit normally
     * resolves to 11 VCK units per framebuffer pixel (MagH = 10).
     * Use 10 VCK units (MagH = 9) for native 256-wide gameplay. Every source pixel therefore
     * remains exactly the same physical width as every other source pixel:
     * no 256->248 resampling pattern, no uneven columns.
     */
    if (_gsk_native240p_par &&
        _gsk_active_mode == GSK_VIDMODE_240P &&
        g_GskOverscan == 0 &&
        _gsk_base_magh > 0 &&
        /* AURORA_PCE_CRT_OVERSCAN_352_V9_20260830:
         * PCE fixed-512 windows have explicit profiles above. */
        _gsk_240p_window_w <= 0)
    {
        int old_magh1 = _gsk_base_magh + 1;
        int new_magh1 = old_magh1 - 1;
        int srcpix    = _gsk_base_dw / old_magh1;
        int new_dw    = srcpix * new_magh1;

        /*
         * Shift native NES/SNES 240p output two logical pixels left.
         * new_magh1 is 10 here, so 2 source pixels = 20 PCRTC units.
         * Do this at scanout level so no framebuffer columns are clipped.
         */
        startx += (dw - new_dw) / 2 - ((_gsk_fb_width == 256 ? 2 : 0) * new_magh1);
        dw      = new_dw;
        magh    = new_magh1 - 1;

        /*
         * SNESTICLE_NATIVE_240P_Y_BIAS
         *
         * Hardware-tested CRT position.
         * Equivalent to menu Offset Y = +3.
         *
         * Unlike horizontal X, gsKit's vertical display offset is
         * already expressed directly in PCRTC vertical units.
         * Keep this at scanout level: no framebuffer crop and no
         * PolyRect/source-coordinate modification.
         */
        starty += 1;
    }

    /* AURORA_MD_UI256_320FB_V1_20260823
     * DISPLAY.DW/MAGH define how many framebuffer samples PCRTC scans.
     * Choose one integer magnification for exactly 256 source pixels, keeping
     * physical width as close as possible to the active 320 presentation.
     * No source columns are duplicated unevenly. */
    if (_gsk_ui256_on_320fb &&
        _gsk_active_mode == GSK_VIDMODE_240P &&
        _gsk_fb_width == 320)
    {
        int new_magh1 = (dw + (GSK_LOGICAL_W / 2)) / GSK_LOGICAL_W;
        int new_dw;

        if (new_magh1 < 1)  new_magh1 = 1;
        if (new_magh1 > 16) new_magh1 = 16;

        new_dw = GSK_LOGICAL_W * new_magh1;
        startx += (dw - new_dw) / 2;
        dw      = new_dw;
        magh    = new_magh1 - 1;
    }

    /* Widescreen: stretch the picture horizontally to ~16:9 by raising
       the horizontal magnification (MAGH) and the display width (DW)
       together, while still reading the SAME framebuffer pixels.  This
       is the anamorphic path -- on a 16:9 TV the wider picture fills the
       screen; on a 4:3 TV it overscans the left/right edges. */
    if (g_GskWidescreen)
    {
        int magh1  = magh + 1;
        int srcpix = magh1 ? dw / magh1 : dw;
        int new_magh1 = (magh1 * 4 + 1) / 3;   /* ~ x1.333 (4:3 -> 16:9) */
        int new_dw1;

        if (new_magh1 > 16) new_magh1 = 16;    /* MAGH is a 4-bit field  */
        if (new_magh1 < 1)  new_magh1 = 1;
        new_dw1 = new_magh1 * srcpix;

        startx -= (new_dw1 - dw) / 2;          /* keep the picture centred */
        dw   = new_dw1;
        magh = new_magh1 - 1;
    }

    gs->DW     = dw;
    gs->DH     = dh;
    gs->MagH   = magh;
    gs->MagV   = _gsk_base_magv;
    gs->StartX = startx;
    gs->StartY = starty;

    /* Re-emit DISPLAY1/2 (also folds in the user X/Y offset). */
    gsKit_set_display_offset(gs, g_GskDispOffX * _gsk_vck,
                             g_GskDispOffY + _gsk_game_y_bias);
    _GskApplyRenderTransform();
}

/* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830 */
void GSK_Set240pVisibleWindow(int x, int width)
{
    if (x < 0 || width <= 0 || x + width > _gsk_fb_width)
    {
        GSK_Clear240pVisibleWindow();
        return;
    }

    /* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830 */
    if (_gsk_240p_window_x == x && _gsk_240p_window_w == width)
        return;

    _gsk_240p_window_x = x;
    _gsk_240p_window_w = width;
    _GskApplyDisplay();
}

void GSK_Clear240pVisibleWindow(void)
{
    /* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830 */
    if (_gsk_240p_window_x < 0 && _gsk_240p_window_w == 0)
        return;

    _gsk_240p_window_x = -1;
    _gsk_240p_window_w = 0;
    _GskApplyDisplay();
}

void GSK_SetDisplayOffset(int x, int y)
{
    g_GskDispOffX = x;
    g_GskDispOffY = y;
    _GskApplyDisplay();
}

void GSK_SetGameplayYOffsetBias(int y)
{
    if (_gsk_game_y_bias == y)
        return;

    _gsk_game_y_bias = y;
    _GskApplyDisplay();
}

void GSK_SetUi256On320Framebuffer(int on)
{
    on = on ? 1 : 0;
    if (_gsk_ui256_on_320fb == on)
        return;

    _gsk_ui256_on_320fb = on;
    _GskApplyDisplay();
}

void GSK_Set240pFramebufferWidth(int width)
{
        /* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830
     * Legal GS storage rasters. 342/352 are NOT framebuffer strides:
     * PCE medium-dot-clock content lives inside the fixed 512 raster. */
    if (width != 256 && width != 320 && width != 512)
        width = 256;

    _gsk_240p_fb_width = width;
}

int GSK_Get240pFramebufferWidth(void)
{
    return _gsk_240p_fb_width;
}

/* AURORA_PCE_ACTIVEFB_RECONCILE_V14R2_20260830 */
int GSK_GetActiveFramebufferWidth(void)
{
    if (!_gsk_initialised || !_pGsGlobal)
        return 0;
    return _pGsGlobal->Width;
}

/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830 */
void GSK_GetPceDebugState(int *fbw, int *winw, int *dw, int *magh,
                          int *startx, int *overscan, int *wide)
{
    if (fbw)      *fbw = _gsk_fb_width;
    if (winw)     *winw = _gsk_240p_window_w;
    if (dw)       *dw = _pGsGlobal ? _pGsGlobal->DW : -1;
    if (magh)     *magh = _pGsGlobal ? _pGsGlobal->MagH : -1;
    if (startx)   *startx = _pGsGlobal ? _pGsGlobal->StartX : -1;
    if (overscan) *overscan = g_GskOverscan;
    if (wide)     *wide = g_GskWidescreen;
}

void GSK_SetOverscan(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    g_GskOverscan = percent;
    _GskApplyDisplay();
}

void GSK_SetWidescreen(int on)
{
    g_GskWidescreen = on ? 1 : 0;
    _GskApplyDisplay();
}

void GSK_SetNative240pPar(int on)
{
    on = on ? 1 : 0;

    if (_gsk_native240p_par == on)
        return;

    _gsk_native240p_par = on;
    _GskApplyDisplay();
}

void GSK_ReinitVideo(void)
{
    GSGLOBAL *oldGlobal;

    if (!_gsk_initialised || !_pGsGlobal) {
        return;
    }

    /* AURORA_GSK_REINIT_FREE_V1
     * Saved 240p/1080i is applied after the initial 480i boot. The old code
     * created a second GSGLOBAL without destroying the first one. PicoDrive
     * allocates its core lazily at ROM launch, so it is especially sensitive
     * to heap lost during that earlier video reinitialisation. */
    GSK_DrainAndWait();
    oldGlobal = _pGsGlobal;

    _gsk_initialised = 0;
    _pGsGlobal = NULL;
    gsKit_deinit_global(oldGlobal);

    GSK_Init(_gsk_arg_w, _gsk_arg_h, _gsk_arg_dispx, _gsk_arg_dispy,
             _gsk_arg_psm, _gsk_arg_psmz, _gsk_arg_mode, _gsk_arg_interlace);
}

int GSK_GetActiveVideoMode(void)
{
    return _gsk_active_mode;
}

Uint32 GSK_VramAllocTBP(Uint32 nBytes)
{
    u32 addr;

    if (!_gsk_initialised || !_pGsGlobal) {
        return 0;
    }

    addr = gsKit_vram_alloc(_pGsGlobal, nBytes, GSKIT_ALLOC_USERBUFFER);
    if (addr == GSKIT_ALLOC_ERROR) {
        return 0;
    }
    return addr / 256;
}

void GSK_DrainAndWait(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
    DmaSyncGIF();
}

/* AURORA_GS_RAWGIF_DRAIN_V1
 *
 * This is intentionally narrower than GSK_DrainAndWait().
 *
 * gsKit_queue_exec() appends a FINISH command to its queue. Waiting for that
 * FINISH here forces the EE to sit idle until the GS has rasterized every
 * preceding primitive. For a producer switch on the SAME GIF/path-3 channel,
 * that is stronger than necessary: DmaSyncGIF() is sufficient to ensure the
 * previous EE DMA source is no longer active before we start the raw chain.
 * The GIF/GS command stream itself remains FIFO-ordered, so raw commands cannot
 * overtake the earlier gsKit packets.
 *
 * Keep GSK_DrainAndWait() untouched for callers that genuinely request full
 * GS completion. */
void GSK_DrainForRawGif(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_queue_exec(_pGsGlobal);
    DmaSyncGIF();
}

void GSK_SetGameplayFastClear(Bool enabled)
{
    _gsk_gameplay_fast_clear = enabled ? TRUE : FALSE;
}

void GSK_SetGameplaySkipClear(Bool enabled)
{
    _gsk_gameplay_skip_clear = enabled ? TRUE : FALSE;
}

void GSK_FlushFrame(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_queue_exec(_pGsGlobal);
    gsKit_finish();
}

void GSK_SyncFlip(void)
{
    if (!_gsk_initialised) {
        return;
    }

    gsKit_sync_flip(_pGsGlobal);
    /* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
     * gsKit_sync_flip rewrites DISPFB2/DBX; restore active PCE window. */
    if (_gsk_240p_window_x >= 0)
        _GskApplyDisplay();
}

void GSK_ResetFrame(void)
{
    GSGLOBAL *gs;
    u64 *p_data;

    if (!_gsk_initialised || !_pGsGlobal) {
        return;
    }

    gs = _pGsGlobal;

    /* Allocate a four-register A+D GIF tag in gsKit's heap.  The
       queue will dispatch it before any subsequent prim, so FRAME_1,
       XYOFFSET_1, ALPHA_1 and COLCLAMP are all refreshed before
       drawing actually happens.

       XYOFFSET_1 must be restored here because the SNES per-scanline
       blender (snppublend_gs.cpp) overwrites it on every Exec() call
       with a line-specific value (0x8000, 0x8000 - iLine*16).  The
       blender's End() restores it through the GPFifo chain, but that
       chain is dispatched *after* gsKit's queue has already drained
       (see GPFifoPause → GSK_DrainAndWait ordering).  Any gsKit
       textured prim queued between End() and GPFifoFlush therefore
       draws with the blender's stale XYOFFSET, which shifts the
       sprite hundreds of pixels off-screen — the visible symptom is a
       permanently frozen menu image because the game output never
       lands inside the visible framebuffer area.

       ALPHA_1 must be restored here for the symmetric reason:  the
       blender's per-scanline DMA chain rewrites ALPHA_1 several times
       (snppublend_gs.cpp _SNPPUBlendBuildList) and leaves it at
       GS_SET_ALPHA(1, 2, 0, 2, 0x80), i.e. output = (Cd - 0) * As + 0
       = Cd * As.  The blender's End() does *not* restore ALPHA_1, and
       gsKit's prim helpers (gsKit_prim_sprite,
       gsKit_prim_sprite_texture_3d, ...) emit only PRIM / color / XY
       per draw — they never re-emit ALPHA_1.  The gsKit init value
       set via gsKit_set_primalpha (GS_SETREG_ALPHA(0, 1, 0, 1, 0x80)
       = standard (Cs - Cd) * As + Cd) therefore stays clobbered for
       the rest of the session.  Any subsequent gsKit prim drawn with
       ABE = 1 (every font draw, every PolyBlend(TRUE) rect, the menu
       selection bar, the "SRAM saved." modal, ...) ends up computing
       output = Cd, which leaves the framebuffer unchanged and makes
       the entire menu overlay invisible — the visible symptom on the
       L2+R2 game-exit path is a frozen darkened game frame with no
       menu UI on top, while audio and input keep responding.  This
       is the same class of bug PR #60 fixed for FRAME_1 / XYOFFSET_1
       but in the opposite (game → menu) direction. */
    /* COLCLAMP = 1 clamps per-channel alpha-blend / colour-math
       results to 0..255.  The GS reset default is 0 (wrap on
       overflow); the original iaddis pipeline (gs.c) programmed
       COLCLAMP = 1 in GS_SetEnv but the gsKit migration dropped
       that write.  Without it, any final composition draw that
       saturates a channel (sprite or BG2/BG3 region overlapping
       BG1 with alpha) ends up wrapping the high bits, which on
       the visible framebuffer appears as banded/striped corruption
       in those regions while BG1-only pixels (the borders) stay
       intact.  Restoring it per-frame here matches the cadence of
       the FRAME / XYOFFSET / ALPHA restores. */
    p_data = (u64 *)gsKit_heap_alloc(gs, 4, 64, GIF_AD);
    if (!p_data) {
        return;
    }

    *p_data++ = GIF_TAG_AD(4);
    *p_data++ = GIF_AD;
    *p_data++ = GS_SETREG_FRAME_1(
        gs->ScreenBuffer[gs->ActiveBuffer & 1] / 8192,
        gs->Width / 64,
        gs->PSM,
        0);
    *p_data++ = GS_REG_FRAME_1;
    *p_data++ = GS_SETREG_XYOFFSET_1(gs->OffsetX, gs->OffsetY);
    *p_data++ = GS_XYOFFSET_1;
    /* Standard alpha blend: output = (Cs - Cd) * As + Cd.  Matches the
       value gsKit_set_primalpha() programmed at GSK_Init() time. */
    *p_data++ = GS_SETREG_ALPHA(0, 1, 0, 1, 0x80);
    *p_data++ = GS_REG_ALPHA_1;
    /* COLCLAMP = 1 (clamp).  Register 0x46 takes a single bit. */
    *p_data++ = (u64)1;
    *p_data++ = (u64)GS_REG_COLCLAMP;

    /* Clear policy.
     *
     * Menu/boot/black-screen: retain the historical full physical clear.
     *
     * Gameplay: MainLoopRender immediately draws the game texture across the
     * complete framebuffer width and all the way to the bottom edge.
     *
     * AURORA_GS_240P_CLEAR_BUDGET_V4_2
     *
     * The largest uncovered top margin in the current native-240p layouts is
     * 8 framebuffer rows. The same logical margin is 16 framebuffer rows in
     * the 640x480 source modes. Clear exactly that mode-wide maximum instead
     * of the old fixed 32-row safety margin, then restore the full scissor.
     *
     * This changes only a black GS fill that is otherwise overwritten later
     * in the same frame; it does not resample or modify game/source pixels. */
    {
        u8 previous_alpha = gs->PrimAlphaEnable;
        gs->PrimAlphaEnable = GS_SETTING_OFF;

        if (_gsk_gameplay_skip_clear)
        {
            /* Plain MD direct-GS paints an opaque full-frame backdrop
             * immediately after reset, so even the top-strip clear would
             * be overwritten before presentation. */
        }
        else if (_gsk_gameplay_fast_clear && gs->Width > 0 && gs->Height > 0)
        {
            int wanted_rows =
                (_gsk_active_mode == GSK_VIDMODE_240P) ? 8 : 16;
            int clear_rows =
                (gs->Height < wanted_rows) ? gs->Height : wanted_rows;
            u64 top_scissor = GS_SETREG_SCISSOR(
                0, gs->Width - 1, 0, clear_rows - 1);
            u64 full_scissor = GS_SETREG_SCISSOR(
                0, gs->Width - 1, 0, gs->Height - 1);

            gsKit_set_scissor(gs, top_scissor);
            gsKit_clear(gs, 0);
            gsKit_set_scissor(gs, full_scissor);
        }
        else
        {
            gsKit_clear(gs, 0);
        }

        gs->PrimAlphaEnable = previous_alpha;
    }
}

void GSK_InvalidateTextureCache(void)
{
    _gsk_invalidate_pending = 1;
}

/* Internal: callers in gpprim.c should consult this and emit a
   TEXFLUSH register write before binding a texture if a blender
   chain has run since the last bind. */
int GSK_TakeInvalidatePending(void)
{
    int p = _gsk_invalidate_pending;
    _gsk_invalidate_pending = 0;
    return p;
}

void *GSK_AsUncached(void *ptr)
{
    Uint32 addr = (Uint32)ptr;

    /* NULL stays NULL. */
    if (!addr) {
        return ptr;
    }

    /* Only KSEG0 / KUSEG cached pointers (top nibble 0x0..0x1, i.e.
       byte address < 0x20000000) can be aliased through KSEG1 by
       setting bit 29. Anything in 0x20000000+ is already uncached
       (KSEG1) or is a kernel/io segment that must not be touched
       through the alias trick.

       PS2 main RAM is 32MB at 0x00000000-0x01FFFFFF, so any legitimate
       pointer into a buffer the EE allocates falls well below the
       0x10000000 threshold. We assert a tighter bound (<256MB) to
       catch accidental use with stack/scratchpad/IO addresses while
       still allowing future memory-map changes. The assert is
       compile-out in CODE_RELEASE so it has zero hot-path cost. */
    assert((addr & 0xF0000000) == 0 &&
           "GSK_AsUncached: pointer is outside physical RAM (<256MB)");

    return (void *)(addr | 0x20000000);
}

/* AURORA_PCE_CRT_OVERSCAN_352_V9_20260830 */

/* AURORA_PCE_MODECLOCK_APERTURE_HALFTEXEL_V10_20260830 */
