/* gskit_backend.h
 *
 * gsKit-based backend for SNESticle's GS layer. Owns the GSGLOBAL
 * pointer and exposes helpers used by gs.c, gpfifo.c, gpprim.c and
 * the SNES blender to coexist on the same DMA path.
 *
 * Fase 1 GS->gsKit migration.
 */

#ifndef _GSKIT_BACKEND_H
#define _GSKIT_BACKEND_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fully initialise the GS via gsKit. Mirrors the parameter set used
   by the original GS_InitGraph + GS_SetDispMode + GS_SetEnv calls.

   width/height : framebuffer size in pixels (e.g. 256x240).
   dispx/dispy  : display position (matches MAINLOOP_DISPX/Y).
   psm/psmz     : pixel formats (GS_PSMCT32 / GS_PSMZ16S, etc).
   mode         : 2 = NTSC, 3 = PAL (matches GS_NTSC / GS_PAL).
   interlace    : 0 = non-interlaced, 1 = interlaced.

   After this returns, GSK_GetGlobal() returns a usable GSGLOBAL *.
*/
void GSK_Init(int width, int height,
              int dispx, int dispy,
              int psm, int psmz,
              int mode, int interlace);

/* ---- Video mode + display offset (selectable in the Settings screen) ----
 *
 * Only the two interlaced outputs are supported. Keep their historical IDs
 * so existing video.cfg files remain compatible: removed IDs 0 (240p/288p)
 * and 2 (480p) are rejected and fall back to 480i when settings are loaded. */
#define GSK_VIDMODE_240P  0   /* NTSC/PAL 256x240 progressive (CRT/AV)        */
#define GSK_VIDMODE_480I  1   /* NTSC/PAL 640x480 interlaced source            */
#define GSK_VIDMODE_1080I 3   /* DTV 1280x960 4:3 window in a 1080i raster     */

extern int g_GskVideoMode;    /* one of GSK_VIDMODE_*    */
extern int g_GskDispOffX;     /* horizontal display offset (0 = centred) */
extern int g_GskDispOffY;     /* vertical display offset   (0 = centred) */
extern int g_GskOverscan;     /* 0..100 shrink of the display area (0 = none) */
extern int g_GskWidescreen;   /* 0 = 4:3, 1 = safe mode-specific 16:9           */

/* Set the display offset live (no VRAM realloc) and remember it for the
   next GSK_Init. X is in VCK units, matching FCEUmm-PS2. */
void GSK_SetDisplayOffset(int x, int y);
/* Runtime-only game bias; does not change/save g_GskDispOffY. */
void GSK_SetGameplayYOffsetBias(int y);
/* AURORA_MD_UI256_320FB_V1_20260823
 * Present logical 256-wide UI without fractional scaling while MD keeps
 * its physical 320-wide 240p framebuffer alive. */
void GSK_SetUi256On320Framebuffer(int on);
/* AURORA_PCE_NATIVE_GS_RASTER_V5_20260830
 * 240p physical sample raster:
 * 256 (SNES/NES/common PCE), 320 (MD), 342/512 (PCE dot-clock modes). */
void GSK_Set240pFramebufferWidth(int width);
int GSK_Get240pFramebufferWidth(void);

/* AURORA_PCE_ACTIVEFB_RECONCILE_V14R2_20260830
 * Largura física do framebuffer gsKit atualmente ativo. */
int GSK_GetActiveFramebufferWidth(void);

/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830: hidden runtime probe retained for future diagnostics. */
void GSK_GetPceDebugState(int *fbw, int *winw, int *dw, int *magh,
                          int *startx, int *overscan, int *wide);

/* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
 * Select a 1:1 visible window inside the current 240p framebuffer.
 * Intended for PCE fixed-512 gameplay: source samples remain untouched. */
void GSK_Set240pVisibleWindow(int x, int width);
void GSK_Clear240pVisibleWindow(void);

/* Apply overscan (0..100) live by re-emitting the GS DISPLAY register.
   0 reproduces gsKit's normal output exactly. */
void GSK_SetOverscan(int percent);

/* Toggle the PCRTC 16:9 presentation live (1 = on, 0 = 4:3). */
void GSK_SetWidescreen(int on);

/* NES/SNES 240p pixel-aspect correction. Keeps the 256-pixel framebuffer
   untouched and changes only PCRTC horizontal magnification. */
void GSK_SetNative240pPar(int on);

/* AURORA_GAMBATTE_SQUARE_ASPECT_V13_20260911
 * Square-pixel handheld presentation policy for standalone Gambatte CGB/GB.
 * In 240p this owns only uniform PCRTC pixel width. Interlaced draw scaling is
 * deliberately scoped to the game blit through GSK_SetGbSquarePixelDraw(). */
void GSK_SetGbSquarePixelPresentation(int on);

/* AURORA_GAMBATTE_DRAW_SCOPE_V13_20260911
 * Transient 480i/1080i 2x2 transform for the Gambatte game rectangle only.
 * Must be disabled again before frontend overlays, status text or modals. */
void GSK_SetGbSquarePixelDraw(int on);

/* Tear down and rebuild the GS for the current g_GskVideoMode. The caller
   MUST re-upload any textures it owns afterwards (e.g. FontInit). Intended
   to run once at boot after the saved settings are read from the card. */
void GSK_ReinitVideo(void);

/* The video mode the GS is currently programmed for (set by GSK_Init).
   Differs from g_GskVideoMode after the settings are loaded but before
   GSK_ReinitVideo() runs. */
int GSK_GetActiveVideoMode(void);

/* AURORA_MEGA_V2_GS_REFRESH_DECL: exact presentation clock for audio pacing. */
void GSK_GetRefreshRate(Uint32 *pNumerator, Uint32 *pDenominator);

/* Returns the active gsKit global, or NULL if GSK_Init has not run. */
struct gsGlobal *GSK_GetGlobal(void);

/* Allocate a region of VRAM via gsKit's user buffer pool and return the
   address in TBP units (i.e. byte_offset / 256). 0 on failure. */
Uint32 GSK_VramAllocTBP(Uint32 nBytes);

/* Drain gsKit's draw queue, wait for path-3 DMA to finish, and clear
   the GIF channel. Use this before the SNES blender kicks its own raw
   DMA chain on the GIF channel. */
void GSK_DrainAndWait(void);

/* AURORA_GS_RAWGIF_DRAIN_V1
 * Bridge-only drain: submit gsKit and wait until GIF DMA has finished feeding
 * the same path-3 channel, but do not wait for the GS FINISH token itself.
 * Packet execution order is still preserved by the GIF. */
void GSK_DrainForRawGif(void);

/* AURORA_GS_PARTIAL_GAMEPLAY_CLEAR_V1
 * Hint that the current frame will immediately receive the normal full-width,
 * bottom-reaching gameplay texture blit. When enabled, GSK_ResetFrame limits
 * its black clear to a conservative top strip and restores full scissor before
 * subsequent primitives. Default/off keeps the historical full clear. */
void GSK_SetGameplayFastClear(Bool enabled);
/* AURORA_PD_DIRECT_MD_SKIP_CLEAR_V3_H_20260821 */
void GSK_SetGameplaySkipClear(Bool enabled);

/* Drain gsKit's draw queue and wait. Equivalent to GSK_DrainAndWait
   but kept as a separate name for clarity in the per-frame flush. */
void GSK_FlushFrame(void);

/* Wait for VBlank, swap framebuffers and reset draw queues. Must be
   called once per frame, after GSK_FlushFrame. */
void GSK_SyncFlip(void);

/* Emit FRAME_1 and XYOFFSET_1 register writes into gsKit's queue,
   pointing at the currently active draw buffer and restoring the
   coordinate origin gsKit expects. Use this at the start of a render
   frame so that subsequent gsKit primitives draw to the right buffer
   at the right position.

   Why FRAME_1: the SNES per-scanline blender steers FRAME_1 to its
   render-to-texture target every frame and only restores it through
   the legacy gpfifo chain, which is dispatched *after* gsKit's queue.

   Why XYOFFSET_1: the blender overwrites XYOFFSET_1 on every scanline
   with (0x8000, 0x8000 - iLine*16). Its restore also goes through
   the gpfifo chain, so any gsKit prim queued before gpfifo dispatch
   draws with the blender's stale offset — shifting the sprite
   hundreds of pixels off-screen (the frozen-menu symptom). */
void GSK_ResetFrame(void);

/* Force a TEXFLUSH next time we draw a textured prim. Used by the
   SNES blender after it overwrites texture VRAM via raw DMA. */
void GSK_InvalidateTextureCache(void);

/* Returns the uncached (KSEG1) alias of a kernel-space pointer that
   currently points into cached physical RAM (KSEG0, <256MB). The alias
   is the same byte address with bit 29 set: the EE bus reads/writes
   bypass the data cache, so a peripheral DMA reading the same physical
   line sees stores immediately without a FlushCache.

   Used (or about to be used) by the SNES blender when it patches
   per-scanline parameters into a chain that the GIF DMA is about to
   read. The legacy gslist path achieves the same with
   GSListGetUncachedPtr; this helper is the gsKit-side equivalent that
   does not depend on a gslist context. Returns the input pointer
   unchanged (with an assert in CODE_DEBUG builds) if the address is
   already uncached or otherwise outside physical RAM.

   Fase 1B GS->gsKit migration. */
void *GSK_AsUncached(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* _GSKIT_BACKEND_H */
