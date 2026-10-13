/* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
#include <string.h>
#include <strings.h> /* AURORA_SWC_FLOPPY_V2_20260831: strcasecmp/strncasecmp */
/* AURORA_SWC_FLOPPY_V3_20260831 */
/* AURORA_SWC_FLOPPY_V4_20260831: writable IMG + isolated states. */
static char s_SwcExternalCartPath[1024] = {0}; /* AURORA_SWC_FLOPPY_V5_20260831 */
static char s_SmdExternalCartPath[1024] = {0}; /* AURORA_SUPER_MAGIC_DRIVE_V1_20260902 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <dirent.h> /* AURORA_SWC_FLOPPY_V1_20260831 */
#include <sys/stat.h>
#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <fileXio.h>
#include <fileXio_rpc.h>
#undef NEWLIB_PORT_AWARE
#include "types.h"
#include "console.h"
#include "file.h"
#include "dataio.h"
#include "pathext.h"
#include "snppucolor.h"
#include "emurom.h"
#include "mainloop.h"
#include "mainloop_shared.h"
/* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827 */
#include "nes/quicknes/quicknes_bridge.h"
#include "nes/fceumm/fceumm_fds_bridge.h"
#include "sega/picodrive/picodrive_bridge.h"
/* AURORA_PCE_EXPERIMENTAL_V1 */
#include "pce/beetle/pce_bridge.h"
#include "mainloop_state.h"
#include "mainloop_ui.h"
#include "uiVideo.h" /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: SGB BIOS selector */
#include "mainloop_bgm.h" /* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830 */
#include "snes.h"
#include "rendersurface.h"
#include "texture.h"
#include "audmixbuffer.h"
#include "emumovie.h"
#include "mainloop_load.h"
#include "mainloop_menu.h"
#include "embedded_irx.h"   /* HddMapPath (hdd0:/PART -> pfs0:) */
/* AURORA_HDD_FULL_STORAGE_SAFE_V2_20261013: used by partition-local BIOS discovery. */
extern char *MainGetBootPath();
#include "sndbglog.h"

/* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914
 *
 * Keep only the active emulator System resident. Rom objects remain frontend
 * descriptors/backings because browser routing already owns them separately.
 * Reset() after construction preserves the old boot-time contract.
 */
static Bool _MainLoopEnsureSnesSystem()
{
    if (_pSnes) return TRUE;
    _pSnes = new SnesSystem();
    if (!_pSnes) return FALSE;
    _pSnes->Reset();
    return TRUE;
}

static Bool _MainLoopEnsureNesSystem()
{
    if (_pNes) return TRUE;
    _pNes = new NesSystem();
    if (!_pNes) return FALSE;
    _pNes->Reset();
    return TRUE;
}

static Bool _MainLoopEnsureFdsSystem()
{
    if (_pFds) return TRUE;
    _pFds = new FdsSystem();
    if (!_pFds) return FALSE;
    _pFds->Reset();
    return TRUE;
}

static Bool _MainLoopEnsureSegaSystem()
{
    if (_pSega) return TRUE;
    _pSega = new SegaSystem();
    if (!_pSega) return FALSE;
    _pSega->Reset();
    return TRUE;
}

static Bool _MainLoopEnsurePceSystem()
{
    if (_pPce) return TRUE;
    _pPce = new PceSystem();
    if (!_pPce) return FALSE;
    _pPce->Reset();
    return TRUE;
}

static Bool _MainLoopEnsureGbSystem()
{
    if (_pGb) return TRUE;
    _pGb = new GambatteSystem();
    if (!_pGb) return FALSE;
    _pGb->Reset();
    return TRUE;
}

static Bool _MainLoopEnsureGbaSystem()
{
    if (_pGba) return TRUE;
    _pGba = new GpSPSystem();
    if (!_pGba) return FALSE;
    _pGba->Reset();
    return TRUE;
}

static Bool _MainLoopEnsureSystemForType(PathExtTypeE eType)
{
    switch (eType)
    {
        case MAINLOOP_ENTRYTYPE_SNESROM:
            return _MainLoopEnsureSnesSystem();
        case MAINLOOP_ENTRYTYPE_SNESWCBIOS:
        case MAINLOOP_ENTRYTYPE_SNESWCDISK:
            return TRUE;
        case MAINLOOP_ENTRYTYPE_NESROM:
            return _MainLoopEnsureNesSystem();
        case MAINLOOP_ENTRYTYPE_NESFDSDISK:
        case MAINLOOP_ENTRYTYPE_NESFDSBIOS:
            return _MainLoopEnsureFdsSystem();
        case MAINLOOP_ENTRYTYPE_SEGAROM:
            return _MainLoopEnsureSegaSystem();
        case MAINLOOP_ENTRYTYPE_PCEROM:
            return _MainLoopEnsurePceSystem();
        case MAINLOOP_ENTRYTYPE_GBROM:
            return _MainLoopEnsureGbSystem();
        case MAINLOOP_ENTRYTYPE_GBAROM:
            return _MainLoopEnsureGbaSystem();
        default:
            return TRUE;
    }
}

extern "C" {
#include "miniz.h"
#include "miniz_compat.h"
}

void _MainLoopGetName(Char *pName, const Char *pPath)
{
        const Char *pFileName;

        pFileName = strrchr(pPath, '/');
        if (pFileName==NULL)
        {
                pFileName = pPath;
        } else
        {
                // skip /
                pFileName = pFileName + 1;
        }
        strcpy(pName, pFileName);
}

/* AURORA_MD_PRELOAD_RASTER_PROBE_V2
 * Decide the 240p physical raster before PicoDrive is initialised.
 *
 * This is deliberately conservative. Known 8-bit/32X images stay on
 * Aurora's 256x240 raster. Standard MD images use 320x240. Ambiguous
 * .bin images get one additional 68000-vector probe (V4) before the safe
 * 256 fallback, so 240p does not unnecessarily lose PicoDrive's direct-T8
 * path while the same title can use it in 480i. */
static int _MainLoopAsciiLower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static Bool _MainLoopSegaExtEquals(const char *pName, const char *pExt)
{
    const char *p;

    if (!pName || !pExt)
        return FALSE;

    p = strrchr(pName, '.');
    if (!p || !p[1])
        return FALSE;
    ++p;

    while (*p && *pExt)
    {
        if (_MainLoopAsciiLower((unsigned char)*p) !=
            _MainLoopAsciiLower((unsigned char)*pExt))
            return FALSE;
        ++p;
        ++pExt;
    }

    return (*p == '\0' && *pExt == '\0') ? TRUE : FALSE;
}

/* AURORA_GB_FINAL_R1_STRICT_EXTENSION_ROUTING_20260909
 * The frontend selector names hardware policy, but the cartridge extension
 * still has one hard rule: .gbc is always a CGB image and never enters the
 * SNES/SGB shell. A .gb selected as GBC remains standalone Gambatte in DMG
 * mode with Aurora's SGB title palette database. ZIP already exposes its
 * member name here; accept the common .gbc.gz spelling as well. */
static Bool _MainLoopGbContentIsGbc(const char *pName)
{
    const char *begin, *end, *ext;
    size_t n;

    if (!pName || !*pName)
        return FALSE;

    begin = pName;
    end = pName + strlen(pName);
    n = (size_t)(end - begin);
    if (n >= 3U && !strncasecmp(end - 3, ".gz", 3))
        end -= 3;

    ext = end;
    while (ext > begin && ext[-1] != '.' && ext[-1] != '/' &&
           ext[-1] != '\\' && ext[-1] != ':')
        --ext;

    return (ext > begin && ext[-1] == '.' &&
            (size_t)(end - ext) == 3U &&
            !strncasecmp(ext, "gbc", 3)) ? TRUE : FALSE;
}

/* AURORA_MD_PRELOAD_VECTOR_PROBE_V4_2
 *
 * Mega Drive begins with big-endian 68000 reset vectors. A normal initial
 * stack pointer is in $FF0000-$FFFFFF, and the even reset PC points into the
 * cartridge image. This is only a fallback for ambiguous .bin content:
 * explicit SMS/GG/SG/SC/32X tests below still win first.
 */
static Uint32 _MainLoopReadBE32(const Uint8 *p)
{
    return ((Uint32)p[0] << 24) |
           ((Uint32)p[1] << 16) |
           ((Uint32)p[2] << 8)  |
           (Uint32)p[3];
}

static Bool _MainLoopLooksLikeMegaDriveVectors(
    const Uint8 *pData, Int32 nBytes, Uint32 uFileBase)
{
    Uint32 sp, pc, payloadBytes;

    if (!pData || nBytes <= 0 ||
        uFileBase > (Uint32)nBytes ||
        (Uint32)nBytes - uFileBase < 8U)
        return FALSE;

    payloadBytes = (Uint32)nBytes - uFileBase;
    sp = _MainLoopReadBE32(pData + uFileBase);
    pc = _MainLoopReadBE32(pData + uFileBase + 4U);

    if ((sp & 0xFFFF0000U) != 0x00FF0000U)
        return FALSE;

    if ((pc & 1U) != 0U || pc < 0x100U || pc >= payloadBytes)
        return FALSE;

    return TRUE;
}

/* AURORA_PCE_CDRDAO_TOC_SUPPORT_V4_8_20260830
 *
 * Load-time-only syntax sniff. cdrdao TOC and MMC CUE are different
 * grammars even when somebody has accidentally named the TOC ".cue".
 *
 * Strong TOC-only signatures:
 *   CD_ROM / CD_DA / CD_ROM_XA
 *   DATAFILE / AUDIOFILE / ZERO
 *   TRACK <format>        (cdrdao)
 * versus CUE:
 *   TRACK <number> <format>
 *
 * No frame-time checks and no HuCard path changes.
 */
static Bool _MainLoopLooksLikeCdrdaoToc(const char *pPath)
{
    FILE *fp;
    char line[1024];
    int lines = 0;

    if (!pPath || !*pPath)
        return FALSE;

    if (_MainLoopSegaExtEquals(pPath, "toc"))
        return TRUE;

    fp = fopen(pPath, "rb");
    if (!fp)
        return FALSE;

    while (lines++ < 48 && fgets(line, sizeof(line), fp))
    {
        char *p = line;
        char token[64];
        char *comment;

        if (lines == 1 &&
            (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF)
            p += 3;

        while (*p == ' ' || *p == '\t' ||
               *p == '\r' || *p == '\n')
            ++p;

        comment = strstr(p, "//");
        if (comment)
            *comment = '\0';

        if (!*p)
            continue;

        if (!strncasecmp(p, "CD_ROM", 6) &&
            (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' ||
             p[6] == '\r' || p[6] == '\n'))
        {
            fclose(fp);
            return TRUE;
        }
        if (!strncasecmp(p, "CD_DA", 5) &&
            (p[5] == '\0' || p[5] == ' ' || p[5] == '\t' ||
             p[5] == '\r' || p[5] == '\n'))
        {
            fclose(fp);
            return TRUE;
        }
        if (!strncasecmp(p, "CD_ROM_XA", 9) &&
            (p[9] == '\0' || p[9] == ' ' || p[9] == '\t' ||
             p[9] == '\r' || p[9] == '\n'))
        {
            fclose(fp);
            return TRUE;
        }

        if (!strncasecmp(p, "DATAFILE", 8) &&
            (p[8] == ' ' || p[8] == '\t'))
        {
            fclose(fp);
            return TRUE;
        }
        if (!strncasecmp(p, "AUDIOFILE", 9) &&
            (p[9] == ' ' || p[9] == '\t'))
        {
            fclose(fp);
            return TRUE;
        }
        if (!strncasecmp(p, "ZERO", 4) &&
            (p[4] == ' ' || p[4] == '\t'))
        {
            fclose(fp);
            return TRUE;
        }

        if (!strncasecmp(p, "TRACK", 5) &&
            (p[5] == ' ' || p[5] == '\t'))
        {
            token[0] = '\0';
            if (sscanf(p + 5, "%63s", token) == 1 &&
                (token[0] < '0' || token[0] > '9'))
            {
                fclose(fp);
                return TRUE;
            }
        }
    }

    fclose(fp);
    return FALSE;
}

/* AURORA_LARGE_MD_COMPACT_V2_20260914
 * Pre-allocation gate only. Full payload is revalidated by the bridge/core.
 */
static Bool _MainLoopSegaCanUseCompactLargeBacking(
    const Uint8 *pData, Int32 nBytes, const char *pName)
{
    static const Uint32 smsHeaderOffsets[] = { 0x7ff0U, 0x3ff0U, 0x1ff0U };
    const char *ext;

    if (!pData || !pName || nBytes <= 0x400000)
        return FALSE;
    if (((Uint32)nBytes & 0x3FFFU) == 0x0200U)
        return FALSE;

    ext = strrchr(pName, '.');
    if (!ext || !ext[1])
        return FALSE;
    ++ext;
    if (strcasecmp(ext, "md") && strcasecmp(ext, "gen") &&
        strcasecmp(ext, "bin"))
        return FALSE;

    if (nBytes >= 0x109 &&
        (!memcmp(pData + 0x100, "SEGA 32X", 8) ||
         !memcmp(pData + 0x100, "SEGA PICO", 9)))
        return FALSE;

    for (unsigned i = 0;
         i < sizeof(smsHeaderOffsets) / sizeof(smsHeaderOffsets[0]); ++i)
    {
        Uint32 off = smsHeaderOffsets[i];
        if ((Uint32)nBytes >= off + 8U &&
            !memcmp(pData + off, "TMR SEGA", 8))
            return FALSE;
    }

    if (nBytes >= 0x104 &&
        (!memcmp(pData + 0x100, "SEGA", 4) ||
         !memcmp(pData + 0x100, " SEG", 4)))
        return TRUE;

    return _MainLoopLooksLikeMegaDriveVectors(pData, nBytes, 0);
}


static Bool _MainLoopSegaWantsNative320(
    const Uint8 *pData, Int32 nBytes, const char *pName)
{
    static const Uint32 smsHeaderOffsets[] =
    {
        0x7ff0U, 0x3ff0U, 0x1ff0U
    };

    /* Explicit extensions can choose the raster before ROM allocation. */
    if (_MainLoopSegaExtEquals(pName, "sms") ||
        _MainLoopSegaExtEquals(pName, "gg")  ||
        _MainLoopSegaExtEquals(pName, "sg")  ||
        _MainLoopSegaExtEquals(pName, "sc")  ||
        _MainLoopSegaExtEquals(pName, "pco"))
        return FALSE;

    /* AURORA_SEGA_CD_32X_MD_SCALING_V2R1_20260828
     * 32X uses the same H32/H40 physical presentation as Mega Drive. */
    if (_MainLoopSegaExtEquals(pName, "md")  ||
        _MainLoopSegaExtEquals(pName, "gen") ||
        _MainLoopSegaExtEquals(pName, "smd") ||
        _MainLoopSegaExtEquals(pName, "32x"))
        return TRUE;

    if (!pData || nBytes <= 0)
        return FALSE;

    /* A .bin may actually be SMS/GG. Mirror PicoDrive's TMR SEGA test
     * before assuming Mega Drive. Also accept a conventional 0x200-byte
     * copier header when probing ambiguous .bin content. */
    for (unsigned i = 0;
         i < sizeof(smsHeaderOffsets) / sizeof(smsHeaderOffsets[0]);
         ++i)
    {
        Uint32 off = smsHeaderOffsets[i];

        if ((Uint32)nBytes >= off + 8U &&
            memcmp(pData + off, "TMR SEGA", 8) == 0)
            return FALSE;

        if ((Uint32)nBytes >= off + 0x200U + 8U &&
            memcmp(pData + off + 0x200U, "TMR SEGA", 8) == 0)
            return FALSE;
    }

    /* 32X/Pico use Mega-Drive-style headers too, so reject their explicit
     * signatures before the generic SEGA/vector tests. Check both raw and
     * conventional +0x200 header locations. */
    if ((nBytes >= 0x108 &&
         memcmp(pData + 0x100, "SEGA 32X", 8) == 0) ||
        (nBytes >= 0x308 &&
         memcmp(pData + 0x300, "SEGA 32X", 8) == 0))
        return FALSE;

    if ((nBytes >= 0x109 &&
         memcmp(pData + 0x100, "SEGA PICO", 9) == 0) ||
        (nBytes >= 0x309 &&
         memcmp(pData + 0x300, "SEGA PICO", 9) == 0))
        return FALSE;

    /* Normal commercial .bin MD dumps identify themselves here. */
    if (_MainLoopSegaExtEquals(pName, "bin") &&
        nBytes >= 0x104 &&
        (memcmp(pData + 0x100, "SEGA", 4) == 0 ||
         memcmp(pData + 0x100, " SEG", 4) == 0))
        return TRUE;

    /* AURORA_MD_PRELOAD_VECTOR_PROBE_V4_2
     *
     * Some valid/headerless MD .bin files do not carry the SEGA console
     * string. 480i can learn they are MD after PicoDrive starts and still
     * use direct T8; 240p must know before core init so its physical raster
     * is already 320 wide.
     *
     * Try raw 68000 vectors first. A conventional +0x200 copier header is
     * accepted only when the file-size shape also matches +0x200.
     * Ambiguous content that fails both probes deliberately stays 256/RGBA.
     */
    if (_MainLoopSegaExtEquals(pName, "bin"))
    {
        if (_MainLoopLooksLikeMegaDriveVectors(pData, nBytes, 0))
            return TRUE;

        if ((((Uint32)nBytes & 0x3FFFU) == 0x0200U) &&
            _MainLoopLooksLikeMegaDriveVectors(
                pData, nBytes, 0x200U))
        {
            return TRUE;
        }
    }

    /* Safe fallback: 256/RGBA is preferable to rebuilding the GS
     * after PicoDrive has already allocated its machine. */
    return FALSE;
}


/* AURORA_V9_FINAL_TRICKY_TIMING_20260915
 * AURORA_V9_LOADING_STATIC_20260915
 * Cartridge loading stays synchronous and uses the same I/O/decompression
 * paths, but the UI is rendered exactly once. Repeated progress formatting
 * and MainLoopRender() calls were host overhead and did not affect emulation. */
/* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915
 * Fases bloqueantes de core/BIOS reutilizam uma única renderização estática.
 * Não cria loop de progresso nem leitura adicional. */
static void _MainLoopLoadPhaseBegin()
{
    MainLoopStatusPrintf(120, "Loading game...");
    MainLoopRender();
}

static void _MainLoopLoadProgressBegin(Int32 total)
{
    if (total <= 0)
        return;
    _MainLoopLoadPhaseBegin();
}

static void _MainLoopLoadProgressFinish(Bool complete)
{
    (void)complete;
    _MainLoop_StatusCount = 0;
    _MainLoop_StatusStr[0] = 0;
}

/* AURORA_LOADING_FAST_CLEANUP_20260915
 * Keep the one static loading frame, but do not split fileXio reads merely
 * for progress reporting.  Request the complete remaining payload and still
 * handle legal short reads exactly like _MainLoopReadBinaryData(). */
static Int32 _MainLoopReadBinaryDataProgress(
    Uint8 *pBuffer, Int32 nExpectedBytes, const char *pRomFile)
{
    int fd;
    Int32 total = 0;

    if (!pBuffer || nExpectedBytes <= 0 || !pRomFile)
        return -1;

    fd = fileXioOpen(pRomFile, FIO_O_RDONLY, 0);
    if (fd < 0)
        return -1;

    _MainLoopLoadProgressBegin(nExpectedBytes);

    while (total < nExpectedBytes)
    {
        Int32 want = nExpectedBytes - total;
        int n = fileXioRead(fd, pBuffer + total, (int)want);

        if (n <= 0)
        {
            fileXioClose(fd);
            _MainLoopLoadProgressFinish(FALSE);
            return n < 0 ? -1 : total;
        }

        total += (Int32)n;
    }

    fileXioClose(fd);
    _MainLoopLoadProgressFinish(total == nExpectedBytes ? TRUE : FALSE);
    return total;
}

int _MainLoopReadBinaryData(Uint8 *pBuffer, Int32 nBufferBytes, const char *pRomFile)
{
        int fd;
        int total = 0;

        if (!pBuffer || nBufferBytes <= 0)
                return -1;

        fd = fileXioOpen(pRomFile, FIO_O_RDONLY, 0);
        if (fd < 0)
                return -1;

        /* AURORA_DYNAMIC_ROM_BUFFER_V1_20260823
         * fileXioRead may legally return short; finish the payload. */
        while (total < nBufferBytes)
        {
                int n = fileXioRead(fd, pBuffer + total, nBufferBytes - total);
                if (n < 0)
                {
                        fileXioClose(fd);
                        return -1;
                }
                if (n == 0)
                        break;
                total += n;
        }

        fileXioClose(fd);
        return total;
}

int _MainLoopReadGZData(Uint8 *pBuffer, Int32 nBufferBytes, const char *pRomFile)
{
        return MinizReadGZToBuffer(pRomFile, pBuffer, nBufferBytes);
}

/* AURORA_PD_MEGA_FIX_20260820
 * ZIP selection must be stricter than "PathExtResolve knows the suffix".
 * Otherwise a recognised auxiliary file can win MinizReadZipFirstMatch()
 * before the actual cartridge. */
static int _MainLoopZipNameIsRom(const char *pName)
{
        PathExtTypeE eType;

        if (!pName || !PathExtResolve((char *)pName, &eType, FALSE))
                return 0;

        switch (eType)
        {
                case MAINLOOP_ENTRYTYPE_SNESROM:
                case MAINLOOP_ENTRYTYPE_NESROM:
                case MAINLOOP_ENTRYTYPE_NESFDSDISK:
                case MAINLOOP_ENTRYTYPE_NESFDSBIOS:
                case MAINLOOP_ENTRYTYPE_SEGAROM:
                case MAINLOOP_ENTRYTYPE_PCEROM:
                case MAINLOOP_ENTRYTYPE_GBROM:
                case MAINLOOP_ENTRYTYPE_GBAROM: /* AURORA_ROM_LIFETIME_RAMONLY_ZIP_V2_20260913 */
                        return 1;
                default:
                        return 0;
        }
}


/* AURORA_DYNAMIC_ROM_BUFFER_V1_20260823 */
#define MAINLOOP_LEGACY_ROM_MAX_BYTES (8U * 1024U * 1024U + 1024U)
#define MAINLOOP_SEGA_ROM_MAX_BYTES   (16U * 1024U * 1024U)
/* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: não regredir cartuchos MD compactos >=10 MiB. */
#if MAINLOOP_SEGA_ROM_MAX_BYTES < (10U * 1024U * 1024U)
#error Aurora PicoDrive must retain support for at least 10 MiB Mega Drive ROMs
#endif
#define MAINLOOP_PCE_ROM_MAX_BYTES    (4U * 1024U * 1024U + 512U)
#define MAINLOOP_GBA_ZIP_MAX_BYTES    (32U * 1024U * 1024U) /* AURORA_ROM_LIFETIME_RAMONLY_ZIP_V2_20260913: RAM-only */
#define MAINLOOP_SEGA_PROBE_BYTES     0x8200U
/* AURORA_FDS_V4_ZIP_LIMITS_20260828 */
#define MAINLOOP_FDS_SIDE_BYTES       65500U
#define MAINLOOP_FDS_MAX_SIDES        8U
#define MAINLOOP_FDS_ZIP_MIN_BYTES    MAINLOOP_FDS_SIDE_BYTES
#define MAINLOOP_FDS_ZIP_MAX_BYTES    (16U + MAINLOOP_FDS_MAX_SIDES * MAINLOOP_FDS_SIDE_BYTES)


/* AURORA_FDS_ZIP_FULLPATH_TMP_V2_20260828
 * This pinned FCEUmm advertises need_fullpath=true and retro_load_game()
 * calls FCEUI_LoadGame(game->path), ignoring game->data. ZIP therefore has
 * to become a real temporary .fds path for the lifetime of the loaded disk.
 */
static Char s_FdsZipTempPath[1024] = {0};

static void _MainLoopRemoveFdsZipTemp(void)
{
    if (s_FdsZipTempPath[0])
    {
        fileXioRemove(s_FdsZipTempPath);
        s_FdsZipTempPath[0] = 0;
    }
}

/* AURORA_ZIP_FILEONLY_MARIO_BIOS_TSUKURU8M_V3_20260913_ZIP_FILEONLY
 * ZIP cartridge/disk policy for path-based cores.
 *
 * Never hold a complete inflated GBA/FDS member in EE RAM. miniz writes the
 * selected member directly to SYSTEM/UNZIP, then the normal path-based core
 * loader owns that file for its complete lifetime.
 */
static Char s_GbaZipTempPath[1024] = {0};

static void _MainLoopRemoveGbaZipTemp(void)
{
    if (s_GbaZipTempPath[0])
    {
        fileXioRemove(s_GbaZipTempPath);
        s_GbaZipTempPath[0] = 0;
    }
}

/* AURORA_ZIP_MC_EXTRACT_GUARD_V1_1_20260927
 * V1.1: keep the guard definitions before every caller. */
/* AURORA_ZIP_MC_EXTRACT_GUARD_V1_20260927
 * ZIP payloads may be read from a memory card, but Aurora must never create
 * an extracted payload there. Keep the policy at the file-output boundary
 * so in-memory ZIP loading remains unchanged. */
static Bool s_ZipExtractBlockedOnMemoryCard = FALSE;

static Bool _MainLoopZipPathIsMemoryCard(const Char *pPath)
{
    if (!pPath)
        return FALSE;

    return ((pPath[0] == 'm' || pPath[0] == 'M') &&
            (pPath[1] == 'c' || pPath[1] == 'C') &&
            (pPath[2] == '0' || pPath[2] == '1') &&
             pPath[3] == ':') ? TRUE : FALSE;
}


/* AURORA_ZIP_ISSUE36_V3_1_20261008
 * ZIP temporary files are always storage-backed: USB/massN, or MX4SIO
 * when its driver exposes a writable massN:/ or mx4sio:/ device. A bare
 * memory card, SRAM fallback and full-ROM EE-RAM staging are forbidden.
 * This policy does not affect firmware lookup or the SRAM destination.
 */
static Bool _MainLoopZipEnsureStorageDir(const Char *pDir)
{
    struct stat st;
    int fd;
    if (mkdir(pDir, 0777) == 0)
        return TRUE;
    if (stat(pDir, &st) == 0 && S_ISDIR(st.st_mode))
        return TRUE;
    /* Some iomanX-only devices do not expose newlib stat(). */
    fd = fileXioDopen(pDir);
    if (fd < 0)
        return FALSE;
    fileXioDclose(fd);
    return TRUE;
}

static Bool _MainLoopZipTryStorageRoot(Char *pOut, Int32 nOutBytes,
                                        const Char *pRoot,
                                        const Char *pLeaf)
{
    Char dir[768];
    int n, fd, closeResult;
    const Char *parts[] = { "/SNESticle", "/SNESticle/SYSTEM",
                            "/SNESticle/SYSTEM/UNZIP" };
    int i;
    if (!pOut || !pRoot || !*pRoot || !pLeaf || !*pLeaf)
        return FALSE;
    /* Only the two storage driver namespaces; never mcN: or mmceN:. */
    /* AURORA_HDD_ISSUE38_V3_20261013 : pfs0: is admitted ONLY after source partition mount. */
    if (strncasecmp(pRoot, "mass", 4) != 0 &&
        strcasecmp(pRoot, "mx4sio:") != 0 &&
        strcmp(pRoot, "pfs0:") != 0)
        return FALSE;
    for (i = 0; i < 3; ++i)
    {
        n = snprintf(dir, sizeof(dir), "%s%s", pRoot, parts[i]);
        if (n < 0 || n >= (int)sizeof(dir) ||
            !_MainLoopZipEnsureStorageDir(dir))
            return FALSE;
    }
    n = snprintf(pOut, (size_t)nOutBytes, "%s/%s", dir, pLeaf);
    if (n < 0 || n >= nOutBytes)
    {
        pOut[0] = 0;
        return FALSE;
    }
    /* Check actual write support, not just mkdir/stat on an existing path.
     * The same fixed filename is overwritten by the extractor immediately. */
    fd = fileXioOpen(pOut, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (fd < 0)
        return FALSE;
    closeResult = fileXioClose(fd);
    if (closeResult < 0)
    {
        fileXioRemove(pOut);
        return FALSE;
    }
    /* After extraction SNES, FDS and GBA use path-based core loaders,
     * including CFileIO::Open/fopen. Check that too before choosing a disk.
     * This avoids placing a valid file on a device that those loaders cannot
     * reopen, while never holding its decompressed contents in EE memory. */
    {
        FILE *check = fopen(pOut, "rb");
        if (!check)
        {
            fileXioRemove(pOut);
            return FALSE;
        }
        fclose(check);
    }
    fileXioRemove(pOut);
    return TRUE;
}

static Bool _MainLoopZipSourceStorageRoot(const Char *pZipPath,
                                          Char *pOut, size_t outBytes)
{
    const Char *colon;
    size_t n, i;
    if (!pZipPath || !pOut || outBytes < 8)
        return FALSE;
    colon = strchr(pZipPath, ':');
    if (!colon) return FALSE;
    n = (size_t)(colon - pZipPath) + 1;
    if (n >= outBytes) return FALSE;
    if (n == 7 && !strncasecmp(pZipPath, "mx4sio:", 7))
    {
        memcpy(pOut, pZipPath, n);
        pOut[n] = 0;
        return TRUE;
    }
    if (n < 5 || n > 10 || strncasecmp(pZipPath, "mass", 4))
        return FALSE;
    for (i = 4; i + 1 < n; ++i)
        if (pZipPath[i] < '0' || pZipPath[i] > '9')
            return FALSE;
    memcpy(pOut, pZipPath, n);
    pOut[n] = 0;
    return TRUE;
}

static Bool _MainLoopPrepareZipTempPath(
    Char *pOut, Int32 nOutBytes, const Char *pLeaf,
    const Char *pZipSource)
{
    Char root[32];
    Int32 i;
    if (!pOut || nOutBytes <= 0 || !pLeaf || !*pLeaf)
        return FALSE;
    pOut[0] = 0;
    s_ZipExtractBlockedOnMemoryCard = FALSE;

    /* AURORA_HDD_ISSUE38_V3_20261013 : HDD source -> corresponding writable PFS partition.
     * Never create a ZIP temporary on a memory card or in EE RAM. */
    if (pZipSource && HddSupportIsEnabled())
    {
        if (!strncmp(pZipSource, "hdd0:", 5) &&
            HddLoadEmbeddedIrx() >= 0)
        {
            char sourceMapped[1024];
            if (HddMapPath(pZipSource, sourceMapped,
                           sizeof(sourceMapped)) == 1 &&
                _MainLoopZipTryStorageRoot(
                    pOut, nOutBytes, "pfs0:", pLeaf))
                return TRUE;
        }
        else if (!strncmp(pZipSource, "pfs0:", 5) &&
                 _MainLoopZipTryStorageRoot(
                     pOut, nOutBytes, "pfs0:", pLeaf))
            return TRUE; /* PFS already mounted by FHDB/launcher. */
    }

    /* First try the device containing the ZIP: includes mass2:, mass3:,
     * ... without assuming the source is always a plugged-in USB stick. */
    if (_MainLoopZipSourceStorageRoot(pZipSource, root, sizeof(root)) &&
        _MainLoopZipTryStorageRoot(pOut, nOutBytes, root, pLeaf))
        return TRUE;

    /* Writable USB/MX4SIO fallback, never memory card, even if SRAM uses MC. */
    for (i = 0; i < 10; ++i)
    {
        snprintf(root, sizeof(root), "mass%d:", (int)i);
        if (_MainLoopZipTryStorageRoot(pOut, nOutBytes, root, pLeaf))
            return TRUE;
    }
    if (_MainLoopZipTryStorageRoot(pOut, nOutBytes, "mass:", pLeaf) ||
        _MainLoopZipTryStorageRoot(pOut, nOutBytes, "mx4sio:", pLeaf))
        return TRUE;

    pOut[0] = 0;
    printf("[ZIP] no writable USB/MX4SIO storage for %s\n",
           pZipSource ? pZipSource : "UNKNOWN");
    return FALSE;
}

/* AURORA_HDD_ISSUE38_V3_20261013
 * Exclusive filesystem scope during HDD game loading (including ZIP
 * extraction). The BGM worker must not remount shared pfs0: in this scope.
 * There is no IOP reset, module preload or change to audio synthesis. */
class AuroraIssue38HddLoadScope
{
    Bool m_active;
public:
    explicit AuroraIssue38HddLoadScope(const char *source)
      : m_active(source && (!strncmp(source, "hdd0:", 5) ||
                            !strncmp(source, "pfs0:", 5)))
    {
        if (m_active) BgmIOBegin();
    }
    ~AuroraIssue38HddLoadScope()
    {
        if (m_active) BgmIOEnd();
    }
};

/* AURORA_LOADER_REVIEW_V2_20260915
 * ZIP preflight already uses fileXio random access. Keep extraction on the
 * same PS2 input backend rather than reopening the archive via miniz stdio. */
typedef struct MainLoopZipReaderT
{
    int fd;
    Int32 size;
    Int32 pos;
} MainLoopZipReaderT;

static Bool _MainLoopZipReaderOpen(
    const Char *pPath, MainLoopZipReaderT *pReader)
{
    int size;

    if (!pPath || !*pPath || !pReader)
        return FALSE;

    pReader->fd = -1;
    pReader->size = 0;
    pReader->pos = 0;

    pReader->fd = fileXioOpen(pPath, FIO_O_RDONLY, 0);
    if (pReader->fd < 0)
        return FALSE;

    size = fileXioLseek(pReader->fd, 0, FIO_SEEK_END);
    if (size <= 0 || fileXioLseek(pReader->fd, 0, FIO_SEEK_SET) < 0)
    {
        fileXioClose(pReader->fd);
        pReader->fd = -1;
        return FALSE;
    }

    pReader->size = (Int32)size;
    pReader->pos = 0;
    return TRUE;
}

static void _MainLoopZipReaderClose(MainLoopZipReaderT *pReader)
{
    if (!pReader)
        return;
    if (pReader->fd >= 0)
        fileXioClose(pReader->fd);
    pReader->fd = -1;
    pReader->size = 0;
    pReader->pos = 0;
}

static size_t _MainLoopZipReaderRead(
    void *opaque, mz_uint64 fileOfs, void *pBuffer, size_t nBytes)
{
    MainLoopZipReaderT *pReader = (MainLoopZipReaderT *)opaque;
    Uint8 *pDst = (Uint8 *)pBuffer;
    size_t total = 0;
    size_t available;

    if (!pReader || pReader->fd < 0 || !pBuffer || !nBytes)
        return 0;
    if (fileOfs > 0x7fffffffULL ||
        fileOfs >= (mz_uint64)(Uint32)pReader->size)
        return 0;

    available = (size_t)((mz_uint64)(Uint32)pReader->size - fileOfs);
    if (nBytes > available)
        nBytes = available;

    if (pReader->pos != (Int32)fileOfs)
    {
        int pos = fileXioLseek(pReader->fd, (int)fileOfs, FIO_SEEK_SET);
        /* AURORA_ZIP_ISSUE36_V3_1_20261008: no partial seek accepted. */
        if (pos != (int)fileOfs)
            return 0;
        pReader->pos = (Int32)pos;
    }

    while (total < nBytes)
    {
        size_t remain = nBytes - total;
        int want = remain > 0x7fffffffU ? 0x7fffffff : (int)remain;
        int got = fileXioRead(pReader->fd, pDst + total, want);
        if (got <= 0 || got > want)
            break;
        total += (size_t)got;
        pReader->pos += (Int32)got;
    }

    return total;
}

/* AURORA_ZIP_ISSUE36_V3_1_20261008: use the same iomanX/fileXio
 * backend for ZIP output as the input reader. No fopen/fwrite on MX4SIO.
 * Complete short writes, bound the decompressed stream and preserve CRC. */
typedef struct MainLoopZipOutputT
{
    int fd;
    Int32 expected;
    Int32 written;
} MainLoopZipOutputT;

static size_t _MainLoopZipOutputWrite(
    void *opaque, mz_uint64 fileOfs, const void *pBuffer, size_t nBytes)
{
    MainLoopZipOutputT *pOut = (MainLoopZipOutputT *)opaque;
    const Uint8 *src = (const Uint8 *)pBuffer;
    size_t total = 0;
    if (!pOut || pOut->fd < 0 || !pBuffer ||
        fileOfs != (mz_uint64)(Uint32)pOut->written ||
        pOut->written < 0 || pOut->written > pOut->expected ||
        nBytes > (size_t)(pOut->expected - pOut->written))
        return 0;

    while (total < nBytes)
    {
        const size_t remain = nBytes - total;
        const int want = remain > 65536U ? 65536 : (int)remain;
        const int got = fileXioWrite(pOut->fd, src + total, want);
        if (got <= 0 || got > want)
            return 0;
        total += (size_t)got;
    }
    pOut->written += (Int32)nBytes;
    return nBytes;
}


static Bool _MainLoopExtractZipEntryToFile(
    const Char *pZipPath,
    const Char *pMemberName,
    unsigned int uZipIndex,
    Int32 nExpectedBytes,
    const Char *pOutPath,
    Uint32 *pOutCRC = NULL)
{
    MainLoopZipReaderT reader;
    MainLoopZipOutputT output;
    mz_zip_archive zip;
    mz_zip_archive_file_stat zst;
    int verifiedFd = -1;
    const char *failedAt = "preflight";
    Bool readerOpen = FALSE;
    Bool zipOpen = FALSE;
    Bool loadingShown = FALSE;
    Bool ok = FALSE;

    if (pOutCRC)
        *pOutCRC = 0;

    if (!pZipPath || !*pZipPath ||
        !pMemberName || !*pMemberName ||
        !pOutPath || !*pOutPath || nExpectedBytes <= 0)
        return FALSE;

    s_ZipExtractBlockedOnMemoryCard = FALSE;
    if (_MainLoopZipPathIsMemoryCard(pOutPath))
    {
        s_ZipExtractBlockedOnMemoryCard = TRUE;
        MainLoopModalPrintf(60 * 4,
            "can't extract zip file on memory card");
        return FALSE;
    }

    memset(&reader, 0, sizeof(reader));
    reader.fd = -1;
    memset(&output, 0, sizeof(output));
    output.fd = -1;
    memset(&zip, 0, sizeof(zip));
    memset(&zst, 0, sizeof(zst));

    if (!_MainLoopZipReaderOpen(pZipPath, &reader))
    {
        printf("[ZIP] failed: source_open (%s)\n", pZipPath);
        return FALSE;
    }
    readerOpen = TRUE;
    failedAt = "zip_directory";

    zip.m_pRead = _MainLoopZipReaderRead;
    zip.m_pIO_opaque = &reader;
    if (!mz_zip_reader_init(&zip, (mz_uint64)(Uint32)reader.size, 0))
        goto done;
    zipOpen = TRUE;

    failedAt = "member_index";
    if (uZipIndex >= mz_zip_reader_get_num_files(&zip) ||
        !mz_zip_reader_file_stat(&zip, (mz_uint)uZipIndex, &zst))
        goto done;

    if (zst.m_is_directory || !zst.m_is_supported ||
        zst.m_uncomp_size != (mz_uint64)(Uint32)nExpectedBytes ||
        strcmp(zst.m_filename, pMemberName) != 0)
        goto done;

    failedAt = "destination_open";
    fileXioRemove(pOutPath);
    output.fd = fileXioOpen(pOutPath,
        FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
    if (output.fd < 0)
        goto done;

    output.expected = nExpectedBytes;
    output.written = 0;

    _MainLoopLoadProgressBegin(nExpectedBytes);
    loadingShown = TRUE;

    failedAt = "decompress_or_crc";
    if (!mz_zip_reader_extract_to_callback(
            &zip, (mz_uint)uZipIndex,
            _MainLoopZipOutputWrite, &output, 0))
        goto done;

    failedAt = "written_size";
    if (output.written != nExpectedBytes)
        goto done;

    failedAt = "destination_close";
    {
        const int closeResult = fileXioClose(output.fd);
        output.fd = -1;
        if (closeResult < 0)
            goto done;
    }

    /* Reopen through fileXio, not newlib stat(): validate the real length. */
    failedAt = "verify_file_size";
    verifiedFd = fileXioOpen(pOutPath, FIO_O_RDONLY, 0);
    if (verifiedFd < 0)
        goto done;
    {
        const int actual = fileXioLseek(verifiedFd, 0, FIO_SEEK_END);
        const int closeResult = fileXioClose(verifiedFd);
        verifiedFd = -1;
        if (actual != nExpectedBytes || closeResult < 0)
            goto done;
    }

    if (pOutCRC)
        *pOutCRC = (Uint32)zst.m_crc32;
    ok = TRUE;

done:
    if (output.fd >= 0)
    {
        fileXioClose(output.fd);
        output.fd = -1;
    }
    if (verifiedFd >= 0)
        fileXioClose(verifiedFd);
    if (zipOpen)
        mz_zip_reader_end(&zip);
    if (readerOpen)
        _MainLoopZipReaderClose(&reader);
    if (loadingShown)
        _MainLoopLoadProgressFinish(ok);
    if (!ok)
    {
        printf("[ZIP] failed: %s (member=%s, index=%u, dest=%s)\n",
               failedAt, pMemberName, uZipIndex, pOutPath);
        fileXioRemove(pOutPath);
    }
    return ok;
}


/* AURORA_SNES_OWNED_ZIP_V1_20260914
 * CRC do arquivo bruto em streaming. Mantém a mesma identidade usada pelo
 * loader antigo (antes de header removal/deinterleave), sem staging completo.
 */
static Bool _MainLoopFileCRC32Exact(
    const Char *pPath, Int32 nExpectedBytes, Uint32 *pCRC,
    Bool bShowProgress)
{
    FILE *fp;
    Uint8 buf[8192];
    Uint32 crc = MZ_CRC32_INIT;
    Int32 total = 0;
    Bool ok = FALSE;

    if (!pPath || !*pPath || nExpectedBytes <= 0 || !pCRC)
        return FALSE;

    fp = fopen(pPath, "rb");
    if (!fp)
        return FALSE;

    if (bShowProgress)
        _MainLoopLoadProgressBegin(nExpectedBytes);

    while (total < nExpectedBytes)
    {
        Int32 want = nExpectedBytes - total;
        size_t got;

        if (want > (Int32)sizeof(buf))
            want = (Int32)sizeof(buf);

        got = fread(buf, 1, (size_t)want, fp);
        if (got != (size_t)want)
            goto done;

        crc = (Uint32)mz_crc32(
            crc, (const unsigned char *)buf, got);
        total += (Int32)got;
    }

    {
        int extra = fgetc(fp);
        if (extra != EOF || ferror(fp))
            goto done;
    }

    *pCRC = crc;
    ok = TRUE;

done:
    fclose(fp);
    if (bShowProgress)
        _MainLoopLoadProgressFinish(ok);
    return ok;
}


/* AURORA_PCE_SSF2_FINAL_R4_CLEANUP_20260913_NORMAL_ALLOCATOR
 * R4: no speculative cross-core SSF2 reservation.  Allocate only when the
 * selected cartridge actually requests its backing. */
static void _MainLoopFreeRomBuffer(void)
{
    if (_RomData)
        free(_RomData);
    _RomData = NULL;
    _RomDataCapacity = 0;
}

static Bool _MainLoopAllocRomBuffer(Uint32 capacity)
{
    Uint8 *p;
    if (capacity == 0 || _RomData || _RomDataCapacity)
        return FALSE;

    p = (Uint8 *)memalign(64, (size_t)capacity);
    if (!p)
    {
        /* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910
         * ROM backing is CPU memory, not a GS DMA surface. */
        p = (Uint8 *)memalign(16, (size_t)capacity);
    }
    if (!p)
        return FALSE;

    _RomData = p;
    _RomDataCapacity = capacity;
    return TRUE;
}

static Int32 _MainLoopGetBinarySize(const char *pRomFile)
{
    int fd = fileXioOpen(pRomFile, FIO_O_RDONLY, 0);
    int size;
    if (fd < 0)
        return -1;
    size = fileXioLseek(fd, 0, FIO_SEEK_END);
    fileXioClose(fd);
    return (size > 0) ? (Int32)size : -1;
}

static Int32 _MainLoopReadBinaryPrefix(
    Uint8 *pBuffer, Int32 nBufferBytes, const char *pRomFile)
{
    int fd, total = 0;
    if (!pBuffer || nBufferBytes <= 0)
        return -1;

    fd = fileXioOpen(pRomFile, FIO_O_RDONLY, 0);
    if (fd < 0)
        return -1;

    while (total < nBufferBytes)
    {
        int n = fileXioRead(fd, pBuffer + total, nBufferBytes - total);
        if (n <= 0)
            break;
        total += n;
    }
    fileXioClose(fd);
    return total;
}

static Uint32 _MainLoopRomPayloadLimit(PathExtTypeE eType)
{
    if (eType == MAINLOOP_ENTRYTYPE_SEGAROM) return MAINLOOP_SEGA_ROM_MAX_BYTES;
    if (eType == MAINLOOP_ENTRYTYPE_PCEROM) return MAINLOOP_PCE_ROM_MAX_BYTES;
    return MAINLOOP_LEGACY_ROM_MAX_BYTES;
}

static int _MainLoopZipDynamicEntryFilter(
    const char *pName, unsigned int nBytes)
{
    PathExtTypeE eType;
    if (!pName || !PathExtResolve((char *)pName, &eType, FALSE))
        return 0;

    switch (eType)
    {
        case MAINLOOP_ENTRYTYPE_SNESROM:
        case MAINLOOP_ENTRYTYPE_NESROM:
        case MAINLOOP_ENTRYTYPE_NESFDSBIOS:
        case MAINLOOP_ENTRYTYPE_GBROM:
            return nBytes <= MAINLOOP_LEGACY_ROM_MAX_BYTES;
        case MAINLOOP_ENTRYTYPE_NESFDSDISK:
            /* AURORA_FDS_V4_ZIP_FILTER_20260828 */
            return nBytes >= MAINLOOP_FDS_ZIP_MIN_BYTES &&
                   nBytes <= MAINLOOP_FDS_ZIP_MAX_BYTES;
        case MAINLOOP_ENTRYTYPE_SEGAROM:
            return nBytes <= MAINLOOP_SEGA_ROM_MAX_BYTES;
        case MAINLOOP_ENTRYTYPE_PCEROM:
            return nBytes <= MAINLOOP_PCE_ROM_MAX_BYTES;
        case MAINLOOP_ENTRYTYPE_GBAROM:
            return nBytes > 0U && nBytes <= MAINLOOP_GBA_ZIP_MAX_BYTES; /* AURORA_ROM_LIFETIME_RAMONLY_ZIP_V2_20260913 */
        default:
            return 0;
    }
}

static void _MainLoopAbortPreCoreLoad(void)
{
    _MainLoopFreeRomBuffer();
    if (!MainLoopEnsureGameplayRasterWidth(256))
        printf("[video] warning: could not restore 256 raster after load failure\n");
}


int _MainLoopReadZipData(Uint8 *pBuffer, Int32 nBufferBytes, const char *pZipFile, char *pFileName)
{
        int nBytes;

        nBytes = MinizReadZipFirstMatch(
                pZipFile,
                pBuffer,
                nBufferBytes,
                pFileName,
                256,
                _MainLoopZipNameIsRom);

        if (nBytes > 0)
        {
                printf("ZIP: read %s (%d)\n", pFileName ? pFileName : "", nBytes);
        }
        else
        {
                printf("ZIP: no compatible entry in %s\n", pZipFile ? pZipFile : "");
        }
        return nBytes;
}

Bool _MainLoopLoadRomData(Emu::Rom *pRom, Uint8 *pRomData, Int32 nRomBytes)
{
        CMemFileIO romfile;
        Emu::Rom::LoadErrorE eError;

        // open memoryfile for rom data
        romfile.Open(pRomData, nRomBytes);

        // load rom
        /* AURORA_PICODRIVE_STAGE2_ROM_BUFFER: Sega attaches Aurora's
           already-loaded _RomData directly. No duplicate buffer and no
           in-place CMemFileIO memcpy. SNES/NES retain the old path. */
        if (pRom == _pSegaRom)
            eError = _pSegaRom->AttachBuffer(
                pRomData, (Uint32)nRomBytes, _RomDataCapacity);
        else if (pRom == _pPceRom)
            eError = _pPceRom->AttachBuffer(
                pRomData, (Uint32)nRomBytes, _RomDataCapacity);
        else
            eError = pRom->LoadRom(&romfile);
        romfile.Close();

        if (eError!=Emu::Rom::LoadErrorE::LOADERROR_NONE)
        {
                ConPrint("ERROR: loading rom %d\n", eError);
                return FALSE;
        }
        return TRUE;
}

Bool _MainLoopLoadBios(Emu::Rom *pRom, const Char *pFilePath)
{
        CFileIO romfile;
        Emu::Rom::LoadErrorE eError;

        // open memoryfile for rom data
        if (!romfile.Open(pFilePath, "rb"))
        {
                ConPrint("ERROR: loading fds bios!\n");
                return FALSE;
        }

        // load rom
        eError = pRom->LoadRom(&romfile);
        romfile.Close();

        if (eError!=Emu::Rom::LoadErrorE::LOADERROR_NONE)
        {
                ConPrint("ERROR: loading rom %d\n", eError);
                return FALSE;
        }
        return TRUE;
}

/* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827: shared standard NES .pal, exactly 64 RGB triples. */
Bool _MainLoopLoadNesPalette(const char *pFileName)
{
    Uint8 rgb[64 * 3], extra;
    FILE *fp;
    size_t got;
    if (!pFileName) return FALSE;
    fp = fopen(pFileName, "rb");
    if (!fp) { MainLoopModalPrintf(60 * 3, "ERROR: cannot open NES palette"); return FALSE; }
    got = fread(rgb, 1, sizeof(rgb), fp);
    if (got != sizeof(rgb) || fread(&extra, 1, 1, fp) != 0)
    {
        fclose(fp);
        MainLoopModalPrintf(60 * 3, "ERROR: NES .pal must be exactly 192 bytes");
        return FALSE;
    }
    fclose(fp);
    if (!QuicknesBridge_SetPalette(rgb) || !FceummFdsBridge_SetPalette(rgb))
    {
        MainLoopModalPrintf(60 * 3, "ERROR: could not apply NES palette");
        return FALSE;
    }
    MainLoopStatusPrintf(120, "NES palette loaded.");
    return TRUE;
}

Bool _MainLoopLoadSnesPalette(const char *pFileName)
{
        Uint32 *pPalData;
        pPalData = SNPPUColorGetPalette();

        return _MainLoopReadBinaryData((Uint8 *)pPalData, SNPPUCOLOR_NUM * sizeof(Uint32), pFileName) > 0;
}


/* AURORA_V4_12_PRIVATE_FILEXIO_CDDA_PCE_TOC2CUE_20260830 */
static char s_PceCdrdaoTempCue[1024];

/* AURORA_SWC_32MBIT_CART_BACKING_V1_20260914
 * AURORA_SWC_LAZY_CART_BACKING_V3_20260915
 * AURORA_V2_TRICKY_ACCURACY_20260915
 *
 * A classic 32-Mbit SWC needs two DISTINCT memories when a full-size
 * external cartridge is inserted: 4 MiB copier DRAM + up to 4 MiB Game Pak
 * ROM. They cannot be aliased because SWC Mode 0 can expose both at once.
 *
 * Game Pak backing stays lazy: empty firmware and disk-only boot allocate
 * only real copier DRAM. On insertion V2 reserves exactly the physical
 * cartridge payload size, up to 4 MiB.
 *
 * This backing is never used as copier DRAM, B-RAM, FDC media, or SRAM.
 */
enum
{
    /* AURORA_LOADER_REVIEW_V2_20260915
     * SWC DRAM is still its own physical 4 MiB allocation. This is only the
     * distinct passthrough Game Pak backing. Keep a 4 MiB rung for 32-Mbit
     * carts and permit the SNES loader's existing ExLoROM range up to 8 MiB. */
    MAINLOOP_SWC_CART_32MBIT_BYTES = 4 * 1024 * 1024,
    MAINLOOP_SWC_CART_RESERVE_BYTES = 8 * 1024 * 1024,
    MAINLOOP_SWC_DRAM_BYTES = 4 * 1024 * 1024, /* AURORA_SWC_32MBIT_SRAM_FIDELITY_V1_1_20260916 */
    /* AURORA_SNES_32MBIT_HOST_GUARD_FIX_V1_20260918 */
    /* AURORA_SNES_32MBIT_HOST_GUARD_NEWLINE_HOTFIX_V2_20260918: literal \\n tokens repaired/validated. */
    MAINLOOP_SNES_HOST_READ_GUARD_BYTES = 16,
    MAINLOOP_SWC_ARENA_BYTES =
        MAINLOOP_SWC_DRAM_BYTES +
        MAINLOOP_SWC_CART_32MBIT_BYTES +
        MAINLOOP_SNES_HOST_READ_GUARD_BYTES
};

static Uint8 *s_pSwcCartReserve = NULL;
static Uint32 s_uSwcCartReserveBytes = 0;

/* AURORA_SWC_32MBIT_SRAM_FIDELITY_V1_1_20260916
 * Keep copier DRAM separate from the hot Game Pak backing at the emulated
 * hardware level. AURORA_SWC_32MBIT_ARENA_V1_2_20260916 may source both from one HOST allocation, but the
 * two 4 MiB slices never overlap and remain separate physical memories. */
static Uint8 *s_pSwcDramReserve = NULL;
static Uint32 s_uSwcDramReserveBytes = 0;
static Bool s_bSwcDramReserveLoaned = FALSE; /* AURORA_SWC_32MBIT_SRAM_FIDELITY_V1_1_20260916 */

/* AURORA_SWC_32MBIT_ARENA_V1_2_20260916
 * Host-only arena. Ownership belongs exclusively to the frontend. The SWC
 * may borrow the first half and SnesRom may borrow the second half. */
static Uint8 *s_pSwcArena = NULL;
static Uint32 s_uSwcArenaBytes = 0;

static Bool _MainLoopSwcCartReserveIsArenaSlice()
{
    return s_pSwcArena &&
           s_pSwcCartReserve == s_pSwcArena + MAINLOOP_SWC_DRAM_BYTES
        ? TRUE : FALSE;
}

static Bool _MainLoopSwcDramReserveIsArenaSlice()
{
    return s_pSwcArena && s_pSwcDramReserve == s_pSwcArena
        ? TRUE : FALSE;
}

static void _MainLoopSwcMaybeReleaseArena()
{
    if (!s_pSwcArena || s_bSwcDramReserveLoaned ||
        s_pSwcDramReserve || s_pSwcCartReserve)
        return;

    free(s_pSwcArena);
    s_pSwcArena = NULL;
    s_uSwcArenaBytes = 0;
}

static void _MainLoopSwcReleaseDramReserve()
{
    /* Never invalidate live copier DRAM. */
    if (s_bSwcDramReserveLoaned)
    {
        if (_pSnes && _pSnes->IsSuperWildCard())
            return;
        s_bSwcDramReserveLoaned = FALSE;
    }

    if (s_pSwcDramReserve)
    {
        if (!_MainLoopSwcDramReserveIsArenaSlice())
            free(s_pSwcDramReserve);
        s_pSwcDramReserve = NULL;
    }
    s_uSwcDramReserveBytes = 0;
    _MainLoopSwcMaybeReleaseArena();
}

static void _MainLoopSwcReleaseCartReserve()
{
    if (s_pSwcCartReserve)
    {
        if (!_MainLoopSwcCartReserveIsArenaSlice())
            free(s_pSwcCartReserve);
        s_pSwcCartReserve = NULL;
    }
    s_uSwcCartReserveBytes = 0;
    _MainLoopSwcMaybeReleaseArena();
}

static Bool _MainLoopSwcEnsureArena()
{
    if (s_pSwcArena && s_uSwcArenaBytes >= MAINLOOP_SWC_ARENA_BYTES)
    {
        s_pSwcDramReserve = s_pSwcArena;
        s_uSwcDramReserveBytes = MAINLOOP_SWC_DRAM_BYTES;
        s_pSwcCartReserve = s_pSwcArena + MAINLOOP_SWC_DRAM_BYTES;
        s_uSwcCartReserveBytes =
            MAINLOOP_SWC_CART_32MBIT_BYTES +
            MAINLOOP_SNES_HOST_READ_GUARD_BYTES;
        return TRUE;
    }

    if (s_pSwcDramReserve || s_pSwcCartReserve || s_bSwcDramReserveLoaned)
        return FALSE;

    s_pSwcArena = (Uint8 *)malloc((size_t)MAINLOOP_SWC_ARENA_BYTES);
    if (!s_pSwcArena)
        return FALSE;

    s_uSwcArenaBytes = MAINLOOP_SWC_ARENA_BYTES;
    s_pSwcDramReserve = s_pSwcArena;
    s_uSwcDramReserveBytes = MAINLOOP_SWC_DRAM_BYTES;
    s_pSwcCartReserve = s_pSwcArena + MAINLOOP_SWC_DRAM_BYTES;
    s_uSwcCartReserveBytes =
            MAINLOOP_SWC_CART_32MBIT_BYTES +
            MAINLOOP_SNES_HOST_READ_GUARD_BYTES;
    printf("[SWC] reserved one %u-byte arena (4 MiB DRAM + 4 MiB cart)\n",
           (unsigned)s_uSwcArenaBytes);
    return TRUE;
}

static Bool _MainLoopSwcEnsureDramReserve()
{
    if (s_pSwcDramReserve &&
        s_uSwcDramReserveBytes >= MAINLOOP_SWC_DRAM_BYTES)
        return TRUE;

    if (s_pSwcArena && s_uSwcArenaBytes >= MAINLOOP_SWC_ARENA_BYTES)
    {
        s_pSwcDramReserve = s_pSwcArena;
        s_uSwcDramReserveBytes = MAINLOOP_SWC_DRAM_BYTES;
        return TRUE;
    }

    _MainLoopSwcReleaseDramReserve();
    s_pSwcDramReserve = (Uint8 *)malloc((size_t)MAINLOOP_SWC_DRAM_BYTES);
    if (!s_pSwcDramReserve)
    {
        printf("[SWC] pre-core 4 MiB DRAM reserve unavailable; "
               "using internal-allocation fallback\n");
        return FALSE;
    }
    s_uSwcDramReserveBytes = MAINLOOP_SWC_DRAM_BYTES;
    return TRUE;
}

static Bool _MainLoopSwcEnsureCartReserve(Uint32 nBytes)
{
    Uint32 nHostBytes;

    if (!nBytes || nBytes > MAINLOOP_SWC_CART_RESERVE_BYTES)
        return FALSE;
    if (nBytes > 0xffffffffu - MAINLOOP_SNES_HOST_READ_GUARD_BYTES)
        return FALSE;

    nHostBytes = nBytes + MAINLOOP_SNES_HOST_READ_GUARD_BYTES;

    if (s_pSwcCartReserve && s_uSwcCartReserveBytes >= nHostBytes)
        return TRUE;

    /* AURORA_SNES_32MBIT_HOST_GUARD_FIX_V1_20260918
     * Logical cart remains <=32 Mbit; the host slice gets only the tiny
     * readable tail required by the R5900 direct-read fast path. */
    if (nBytes <= MAINLOOP_SWC_CART_32MBIT_BYTES &&
        s_pSwcArena && s_uSwcArenaBytes >= MAINLOOP_SWC_ARENA_BYTES)
    {
        s_pSwcCartReserve = s_pSwcArena + MAINLOOP_SWC_DRAM_BYTES;
        s_uSwcCartReserveBytes =
            MAINLOOP_SWC_CART_32MBIT_BYTES +
            MAINLOOP_SNES_HOST_READ_GUARD_BYTES;
        return TRUE;
    }

    _MainLoopSwcReleaseCartReserve();
    s_pSwcCartReserve = (Uint8 *)malloc((size_t)nHostBytes);
    if (!s_pSwcCartReserve)
    {
        printf("[SWC] %u-byte cart backing (+guard) unavailable; "
               "using normal fallback\n", (unsigned)nBytes);
        return FALSE;
    }
    s_uSwcCartReserveBytes = nHostBytes;
    return TRUE;
}

static Uint32 _MainLoopSwcPrepareCartHeadroom()
{
    /* AURORA_SWC_32MBIT_ARENA_V1_2_20260916
     * A normal BIOS launch and a post-L2+R3 relaunch must not depend on
     * allocator history. First ask for one 8 MiB block and split it 4+4.
     * If that exact block is unavailable, fall back to v1_1's independent
     * 4 MiB allocations instead of making the BIOS fail. */
    if (_pSnes && _pSnes->IsSuperWildCard())
        return s_uSwcCartReserveBytes;

    s_bSwcDramReserveLoaned = FALSE;
    _MainLoopSwcReleaseCartReserve();
    _MainLoopSwcReleaseDramReserve();

    if (_MainLoopSwcEnsureArena())
        return MAINLOOP_SWC_CART_32MBIT_BYTES;

    if (!_MainLoopSwcEnsureDramReserve())
        return 0;

    if (_MainLoopSwcEnsureCartReserve(MAINLOOP_SWC_CART_32MBIT_BYTES))
        return MAINLOOP_SWC_CART_32MBIT_BYTES;

    return 0;
}

static Bool _MainLoopEnsureSnesSystemWithSwcHeadroom()
{
    if (_MainLoopEnsureSnesSystem())
        return TRUE;

    /* AURORA_SWC_32MBIT_ARENA_V1_2_20260916
     * An arena cannot release only its second half back to malloc. If the
     * core itself cannot be constructed with the full 8 MiB parked, release
     * the whole host arena and preserve the historical low-memory fallback. */
    if (s_pSwcArena)
    {
        _MainLoopSwcReleaseCartReserve();
        _MainLoopSwcReleaseDramReserve();
        if (_MainLoopEnsureSnesSystem())
            return TRUE;
        return FALSE;
    }

    if (s_uSwcCartReserveBytes)
    {
        _MainLoopSwcReleaseCartReserve();
        if (_MainLoopEnsureSnesSystem())
            return TRUE;
    }

    if (s_pSwcDramReserve)
    {
        _MainLoopSwcReleaseDramReserve();
        if (_MainLoopEnsureSnesSystem())
            return TRUE;
    }
    return FALSE;
}

static Bool _MainLoopEnsureSwcRasterWithHeadroom()
{
    if (MainLoopEnsureGameplayRasterWidth(256))
        return TRUE;

    /* AURORA_SWC_32MBIT_ARENA_V1_2_20260916: if a rare video reinit needs EE heap, an unopened SWC may
     * discard its arena as one unit. A live borrowed DRAM is never freed. */
    if (s_pSwcArena && (!_pSnes || !_pSnes->IsSuperWildCard()))
    {
        _MainLoopSwcReleaseCartReserve();
        _MainLoopSwcReleaseDramReserve();
        return MainLoopEnsureGameplayRasterWidth(256);
    }

    if (s_uSwcCartReserveBytes)
    {
        _MainLoopSwcReleaseCartReserve();
        if (MainLoopEnsureGameplayRasterWidth(256))
            return TRUE;
    }

    if (!_pSnes || !_pSnes->IsSuperWildCard())
        _MainLoopSwcReleaseDramReserve();
    return FALSE;
}

static Bool _MainLoopSwcHeadroomMemoryError(const char *pError)
{
    static const char kPrefix[] = "not enough EE memory for ";
    return pError &&
           !strncmp(pError, kPrefix, sizeof(kPrefix) - 1)
        ? TRUE : FALSE;
}

/* AURORA_SWC_32MBIT_ARENA_V1_2_20260916: borrowed DRAM may be the first half of the arena. */
static Bool _MainLoopSwcLoadBorrowedDram(
    const char *pFirmwarePath, const char *pDiskPath)
{
    Bool bLoaded;

    if (!_pSnes)
        return FALSE;

    /* A failed SNSuperWildCard::Load() has already Shutdown() its previous
     * mapping.  Publish the loan only after the new load fully succeeds. */
    s_bSwcDramReserveLoaned = FALSE;
    bLoaded = _pSnes->LoadSuperWildCard(
        pFirmwarePath, pDiskPath,
        s_pSwcDramReserve, s_uSwcDramReserveBytes);

    if (bLoaded && s_pSwcDramReserve &&
        s_uSwcDramReserveBytes >= MAINLOOP_SWC_DRAM_BYTES)
        s_bSwcDramReserveLoaned = TRUE;

    return bLoaded;
}

static Bool _MainLoopLoadSuperWildCardWithHeadroom(
    const char *pFirmwarePath, const char *pDiskPath)
{
    const char *pError;

    if (!_pSnes)
        return FALSE;

    if (_MainLoopSwcLoadBorrowedDram(
            pFirmwarePath, pDiskPath))
        return TRUE;

    pError = _pSnes->GetSuperWildCardError();
    if (!_MainLoopSwcHeadroomMemoryError(pError))
        return FALSE;

    /* SNSuperWildCard::Load() starts with Shutdown(), so a failed memory
     * attempt can safely be retried after reducing parked cart headroom. */
    if (s_uSwcCartReserveBytes > MAINLOOP_SWC_CART_32MBIT_BYTES)
    {
        _MainLoopSwcReleaseCartReserve();
        (void)_MainLoopSwcEnsureCartReserve(
            MAINLOOP_SWC_CART_32MBIT_BYTES);

        if (_MainLoopSwcLoadBorrowedDram(
            pFirmwarePath, pDiskPath))
            return TRUE;

        pError = _pSnes->GetSuperWildCardError();
        if (!pError || !strstr(pError, "not enough EE memory for copier DRAM"))
            return FALSE;
    }

    if (s_uSwcCartReserveBytes)
    {
        _MainLoopSwcReleaseCartReserve();
        if (_MainLoopSwcLoadBorrowedDram(
            pFirmwarePath, pDiskPath))
            return TRUE;
    }

    return FALSE;
}


static Uint32 _MainLoopSwcCartPayloadBytes(const char *pPath)
{
    struct stat st;
    Uint32 nFileBytes;
    Uint32 nHeaderBytes;

    if (!pPath || !*pPath || stat(pPath, &st) != 0 ||
        S_ISDIR(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > 0xffffffffULL)
        return 0;

    nFileBytes = (Uint32)st.st_size;

    /* Match the normal 512-byte copier-header case at the 32-Mbit boundary. */
    nHeaderBytes = ((nFileBytes & 0x1FFFu) == 512u) ? 512u : 0u;
    return nFileBytes - nHeaderBytes;
}


void _MainLoopUnloadRom()
{
#if AURORA_FRONTEND_PROFILER
    /* AURORA_PROFILER_AUTO20_STATELOAD_V21_20261009: never carry a
     * previous ROM's benchmark deadline into a new core. */
    MainLoopProfilerAutoCloseCancel();
#endif
    /* AURORA_AUDIO_HARDCUT_ROM_UNLOAD_V1 */
    MainLoopAudioHardCut();

    /* AURORA_COPIER_SRAM_FLUSH_V10_16_20260831
     *
     * Classic Front copiers have 256 Kbit / 32 KiB battery-backed SRAM.
     * Aurora deliberately does not poll SNES SRAM during gameplay, so a
     * direct game/core switch could otherwise detach the copier before the
     * menu-open checksum/save path notices the last writes.
     *
     * Flush only the real copier SRAM, only at an unload boundary:
     * - no gameplay polling or periodic I/O;
     * - no effect on ordinary SNES cartridges or any other core;
     * - no write at all when the 32 KiB contents are unchanged.
     *
     * Do this before SetRom(NULL), while _pSystem, _RomName and the live
     * SRAM buffer still identify the departing Magicom/SWC instance.
     */
    if (_pSystem == _pSnes && _pSnes &&
        _pSnes->IsSuperWildCard() &&
        _pSystem->GetSRAMBytes() == 0x8000)
    {
        /* AURORA_FINAL_V1_7_D88_DUAL_SRAM_PERSIST_20260901
         * This is a real power/lifetime boundary. Persist the copier B-RAM
         * and the inserted battery cart SRAM even when neither dirty detector
         * changed since the previous save. */
        (void)_MainLoopForceCheckSRAM();
        {
            Bool bSaved = _MainLoopSaveSRAM(TRUE);
            ConPrint("Copier + cart SRAM unload flush: %s\n",
                     bSaved ? "saved" : "FAILED");
            MainLoopStatusPrintf(
                bSaved ? 120 : 240,
                bSaved ? "Copier/cart SRAM saved."
                       : "WARNING: copier/cart SRAM save failed!");
        }
    }

    /* AURORA_BSXSLOT_MEMORY_PACK_V1_2_IO_WATCH_SGB_STATUS_20260906
     * One cold-path line at ROM lifetime end. The hot Read/Write paths only
     * increment 32-bit counters, so diagnostic visibility does not turn every
     * Memory Pack access into console/file I/O. */
    if (_pSystem == _pSnes && _pSnes && _pSnes->HasBSXMemoryPack())
    {
        ConPrint("[BSX/MPK] io unload: R=%u W=%u prog=%u erase=%u chip=%u status=%u vendor=%u dirty=%d\n",
                 (unsigned)_pSnes->GetBSXMemoryPackReadCount(),
                 (unsigned)_pSnes->GetBSXMemoryPackWriteCount(),
                 (unsigned)_pSnes->GetBSXMemoryPackProgramCount(),
                 (unsigned)_pSnes->GetBSXMemoryPackBlockEraseCount(),
                 (unsigned)_pSnes->GetBSXMemoryPackChipEraseCount(),
                 (unsigned)_pSnes->GetBSXMemoryPackStatusReadCount(),
                 (unsigned)_pSnes->GetBSXMemoryPackVendorReadCount(),
                 _pSnes->IsBSXMemoryPackDirty() ? 1 : 0);
    }

    /* AURORA_BSXSLOT_MEMORY_PACK_V1_1_PERSIST_FIX_20260906
     * Standalone BSC/SA-1 slotted carts own a separate 1 MiB Memory Pack.
     * Flush a dirty pack before SetRom(NULL), while _RomName and the backing
     * still identify the departing game. Ordinary SNES/SGB/copier paths are
     * untouched; a slotted game may own both <game>.srm and <game>.mpk. */
    /* AURORA_BSXSLOT_MEMORY_PACK_V1_3_UNLOAD_BUNDLE_20260906
     * At ROM lifetime end, sample ordinary SRAM even when the flash itself is
     * currently clean. If either member changed, persist the coherent bundle. */
    if (_pSystem == _pSnes && _pSnes &&
        !_pSnes->IsSuperWildCard() &&
        !_pSnes->IsSuperGameBoy() &&
        _pSnes->HasBSXMemoryPack())
    {
        (void)_MainLoopForceCheckSRAM();
        if (_MainLoop_SRAMUpdated || _pSnes->IsBSXMemoryPackDirty())
        {
            Bool bSaved = _MainLoopSaveSRAM(TRUE);
            ConPrint("BS-X Memory Pack unload bundle flush: %s\n",
                     bSaved ? "saved" : "FAILED");
            MainLoopStatusPrintf(
                bSaved ? 120 : 240,
                bSaved ? "Memory Pack saved."
                       : "WARNING: Memory Pack save failed!");
        }
    }

    if (_pSystem == _pSnes && _pSnes && _pSnes->IsSuperGameBoy())
    {
        (void)_MainLoopForceCheckSRAM();
        if (_MainLoop_SRAMUpdated)
        {
            Bool bSaved = _MainLoopSaveSRAM(TRUE);
            ConPrint("SGB savedata unload flush: %s\n", bSaved ? "saved" : "FAILED");
        }
    }

    /* AURORA_GAMBATTE_STANDALONE_V2_20260908 */
    if (_pSystem == _pGb && _pGb && _pGb->IsGameLoaded() &&
        (_pGb->GetSavedataBytes() > 0 || _pGb->HasTurboFile())) /* AURORA_GB_ASCII_TURBO_FILE_R8_20260909 */
    {
        _MainLoop_SRAMUpdated = TRUE;
        Bool bSaved = _MainLoopSaveSRAM(TRUE);
        ConPrint("GB savedata unload flush: %s\n", bSaved ? "saved" : "FAILED");
    }

    /* AURORA_D88_GBA_LIFECYCLE_FIX_V1_20260920: the preceding Gambatte block is the single
     * SRAM/RTC/Turbo File unload persistence boundary. */

    /* AURORA_GPSP_GBA_V1_20260911
     * gpSP exposes one fixed 128 KiB save-RAM window. Persist it before core
     * teardown, following the standalone Gambatte lifetime boundary. */
    if (_pSystem == _pGba && _pGba && _pGba->IsGameLoaded() &&
        _pGba->GetSRAMBytes() > 0)
    {
        _MainLoop_SRAMUpdated = TRUE;
        Bool bSaved = _MainLoopSaveSRAM(TRUE);
        ConPrint("GBA savedata unload flush: %s\n", bSaved ? "saved" : "FAILED");
    }

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901
     * The flush above calls _MainLoopForceCheckSRAM/_MainLoopSaveSRAM while
     * the cart backing and cartridge filename still exist. Detach only now. */
        /* AURORA_SMD_L2_R3_CLOSE_V1_20260903
     * B-RAM is the current Sega SRAM while SMD is active. Save before the
     * bridge/device teardown. */
    if (_pSystem == _pSega && _pSega && _pSega->IsSuperMagicDrive() &&
        _pSystem->GetSRAMBytes() == 0x8000)
    {
        (void)_MainLoopForceCheckSRAM();
        Bool bSaved = _MainLoopSaveSRAM(TRUE);
        ConPrint("SMD B-RAM unload flush: %s\n", bSaved ? "saved" : "FAILED");
        MainLoopStatusPrintf(bSaved ? 120 : 240,
            bSaved ? "SMD B-RAM saved." : "WARNING: SMD B-RAM save failed!");
    }

_MainLoopSwcCartSRAMDetach();

    /* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830
     * BgmStop intentionally retains libxmp for fast menu reopen.
     * A real game switch prefers maximum free EE heap. */
    BgmReleaseDecoderForGameSwitch();

    // stop recording if we are recording
    if (s_pMovieClip->IsRecording())
    {
        printf("Movie: Record End\n");
        s_pMovieClip->RecordEnd();
    } 
    // stop playing if we are playing
    if (s_pMovieClip->IsPlaying())
    {
        printf("Movie: Play End\n");
        s_pMovieClip->PlayEnd();
    } 
    /* V6.2: recordings belong to the departing ROM. Releasing their lazy
     * buffers here also guarantees maximum EE heap before the next core. */
    s_pMovieClip->Discard();

	// unload old rom
	if (_pSnes) _pSnes->SetRom(NULL);
    s_SwcExternalCartPath[0] = 0; /* AURORA_SWC_MEGA_V9_20260831 */
	_pSnesRom->Unload();
    _MainLoopSwcReleaseCartReserve(); /* AURORA_SWC_32MBIT_CART_BACKING_V1_20260914 */

	/* Phase 2: NES unload mirrors the SNES path. NesDisk is unloaded
	   even though disk-swap input is still gated for Phase 5 - the
	   wrapper itself exists and owns memory. */
	if (_pNes) _pNes->SetRom(NULL);
	if (_pFds) _pFds->SetRom(NULL); /* AURORA_FCEUMM_FDS_V0_5_UNLOAD */
    /* AURORA_FDS_ZIP_FULLPATH_TMP_V2_20260828: FCEUmm has closed the
     * disk now, so the extracted full-path backing file is no longer needed. */
    _MainLoopRemoveFdsZipTemp();
	_pNesRom->Unload();
	_pNesFDSDisk->Unload();
	/* AURORA_PICODRIVE_STAGE2_UNLOAD: SetRom(NULL) fully deinitializes PicoDrive. */
	if (_pSega) _pSega->SetRom(NULL);
	if (_pSegaRom) _pSegaRom->Unload();
	if (_pPce) _pPce->SetRom(NULL);
	if (_pPceRom) _pPceRom->Unload();
	if (_pGb) _pGb->UnloadGame(); /* AURORA_GAMBATTE_STANDALONE_V2_20260908 */
	if (_pGba) _pGba->UnloadGame(); /* AURORA_GPSP_GBA_V1_20260911 */
    /* AURORA_ZIP_FILEONLY_MARIO_BIOS_TSUKURU8M_V3_20260913_ZIP_FILEONLY: gpSP paging file is closed now. */
    _MainLoopRemoveGbaZipTemp();

    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914
     * Established SetRom/UnloadGame calls above save/detach media first.
     * Now destruct wrappers so bridge/JIT/core heap cannot remain resident. */
    _pSystem = NULL;
    if (_pSnes) { delete _pSnes; _pSnes = NULL; }
    s_bSwcDramReserveLoaned = FALSE; /* AURORA_SWC_32MBIT_SRAM_FIDELITY_V1_1_20260916: borrower is now gone. */
    _MainLoopSwcReleaseDramReserve();
    if (_pNes)  { delete _pNes;  _pNes  = NULL; }
    if (_pFds)  { delete _pFds;  _pFds  = NULL; }
    if (_pSega) { delete _pSega; _pSega = NULL; }
    if (_pPce)  { delete _pPce;  _pPce  = NULL; }
    if (_pGb)   { delete _pGb;   _pGb   = NULL; }
    if (_pGba)  { delete _pGba;  _pGba  = NULL; }


    if (s_PceCdrdaoTempCue[0])
    {
        remove(s_PceCdrdaoTempCue);
        s_PceCdrdaoTempCue[0] = 0;
    }

    /* The cores are detached now; release ROM RAM before gsKit reallocates. */
    _MainLoopFreeRomBuffer();

    /* AURORA_MD_STABLE_RASTER_V2
     * Only rebuild the 240p GS after PicoDrive has been shut down.
     * Browser/SNES/NES/SMS/GG use Aurora's normal physical raster. */
    /* AURORA_GAME_SWITCH_CLEAR_V1_20260823
     * System-agnostic: every ROM switch starts from black presentation state.
     *
     * The raster helper re-uploads _fbTexture[0] after a gsKit rebuild.
     * Therefore clear BEFORE the rebuild, otherwise the previous game's last
     * frame gets copied into the new _OutTex and flashes during next boot. */
    if (_fbTexture[0]) _fbTexture[0]->Clear();
    if (_fbTexture[1]) _fbTexture[1]->Clear();

    /* Also cover same-raster switches where no GS rebuild occurs. */
    if (_fbTexture[0])
        TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));

    if (!MainLoopEnsureGameplayRasterWidth(256))
        printf("[video] warning: could not restore 256 raster after unload\n");

    _bStateSaved = FALSE;
    _pSystem = NULL;
    _RomPath[0] = 0;
    MainLoopStateOnRomChanged();

}

/* AURORA_V4_12_PRIVATE_FILEXIO_CDDA_PCE_TOC2CUE_20260830 */
typedef struct AuroraPceTocTrack
{
    char cueType[16];
    char dataFile[768];
    long fileOffsetBytes;
    long dataSectors;
    int hasOffset;
    int generatedPregap;
    int startSectors;
    int hasStart;
    int haveData;
} AuroraPceTocTrack;

static int _MainLoopPceParseMsf(const char *s)
{
    unsigned m, sec, f;
    if (!s || sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3 ||
        sec > 59 || f > 74)
        return -1;
    return (int)((m * 60u + sec) * 75u + f);
}

static void _MainLoopPceFormatMsf(long sectors,
                                  char *out, size_t outBytes)
{
    long m, sec, f;
    if (sectors < 0) sectors = 0;
    m = sectors / (60 * 75);
    sectors %= 60 * 75;
    sec = sectors / 75;
    f = sectors % 75;
    snprintf(out, outBytes, "%02ld:%02ld:%02ld", m, sec, f);
}

static const char *_MainLoopPceTocToken(const char *p,
                                        char *out, size_t outBytes)
{
    size_t n = 0;
    if (!p || !out || !outBytes)
        return NULL;

    while (*p == ' ' || *p == '\t')
        ++p;

    if (*p == '"')
    {
        ++p;
        while (*p && *p != '"')
        {
            if (n + 1 < outBytes)
                out[n++] = *p;
            ++p;
        }
        if (*p == '"') ++p;
    }
    else
    {
        while (*p && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n')
        {
            if (n + 1 < outBytes)
                out[n++] = *p;
            ++p;
        }
    }

    out[n] = 0;
    return p;
}

static Bool _MainLoopBuildPceCueFromCdrdao(
    const char *pTocPath,
    const char *pSystemDirectory,
    char *pOutCue,
    size_t nOutCue)
{
    AuroraPceTocTrack tr[99];
    FILE *in = NULL, *out = NULL;
    char line[1536];
    char commonFile[768];
    char binPath[1024];
    char tocDir[1024];
    int trackCount = 0;
    int active = -1;
    long expectedOffset = 0;      /* cdrdao virtual byte cursor */
    long generatedGapBytes = 0;
    long physicalCueSectors = 0;
    long actualBinBytes = -1;
    Bool compactGeneratedGaps = FALSE;
    const char *slash;
    int i;
    Bool ok = FALSE;

    memset(tr, 0, sizeof(tr));
    commonFile[0] = 0;

    if (!pTocPath || !*pTocPath ||
        !pSystemDirectory || !*pSystemDirectory ||
        !pOutCue || nOutCue < 32)
        return FALSE;

    in = fopen(pTocPath, "rb");
    if (!in)
        return FALSE;

    while (fgets(line, sizeof(line), in))
    {
        const char *p = line;
        char *comment = strstr(line, "//");
        char a0[768], a1[128], a2[128];

        if (comment) *comment = 0;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '\r' || *p == '\n')
            continue;

        if (!strncasecmp(p, "TRACK", 5) &&
            (p[5] == ' ' || p[5] == '\t'))
        {
            char fmt[64];
            p = _MainLoopPceTocToken(p + 5, fmt, sizeof(fmt));
            if (!p || trackCount >= 99)
                goto done;

            active = trackCount++;
            if (!strcasecmp(fmt, "AUDIO"))
                strcpy(tr[active].cueType, "AUDIO");
            else if (!strcasecmp(fmt, "MODE1_RAW"))
                strcpy(tr[active].cueType, "MODE1/2352");
            else if (!strcasecmp(fmt, "MODE2_RAW"))
                strcpy(tr[active].cueType, "MODE2/2352");
            else
                goto done;
            continue;
        }

        if (active < 0)
            continue;

        if ((!strncasecmp(p, "ZERO", 4) &&
             (p[4] == ' ' || p[4] == '\t')) ||
            (!strncasecmp(p, "SILENCE", 7) &&
             (p[7] == ' ' || p[7] == '\t')))
        {
            const char *q = strchr(p, ' ');
            int msf = -1;
            if (!q) q = strchr(p, '\t');
            if (!q) goto done;

            q = _MainLoopPceTocToken(q, a0, sizeof(a0));
            q = _MainLoopPceTocToken(q, a1, sizeof(a1));

            if (a1[0] && strchr(a1, ':'))
                msf = _MainLoopPceParseMsf(a1);
            else if (a0[0] && strchr(a0, ':'))
                msf = _MainLoopPceParseMsf(a0);

            if (msf < 0)
                goto done;
            tr[active].generatedPregap += msf;
            continue;
        }

        if (!strncasecmp(p, "START", 5) &&
            (p[5] == ' ' || p[5] == '\t'))
        {
            int msf;
            p = _MainLoopPceTocToken(p + 5, a0, sizeof(a0));
            msf = _MainLoopPceParseMsf(a0);
            if (msf < 0) goto done;
            tr[active].startSectors = msf;
            tr[active].hasStart = 1;
            continue;
        }

        if (!strncasecmp(p, "DATAFILE", 8) &&
            (p[8] == ' ' || p[8] == '\t'))
        {
            const char *q = _MainLoopPceTocToken(
                p + 8, a0, sizeof(a0));
            q = _MainLoopPceTocToken(q, a1, sizeof(a1));
            q = _MainLoopPceTocToken(q, a2, sizeof(a2));

            if (!a0[0])
                goto done;

            snprintf(tr[active].dataFile,
                     sizeof(tr[active].dataFile), "%s", a0);

            if (!commonFile[0])
                snprintf(commonFile, sizeof(commonFile), "%s", a0);
            else if (strcmp(commonFile, a0))
                goto done;

            if (a1[0] == '#')
            {
                char *endp = NULL;
                tr[active].fileOffsetBytes =
                    strtol(a1 + 1, &endp, 10);
                if (!endp || *endp || tr[active].fileOffsetBytes < 0)
                    goto done;
                tr[active].hasOffset = 1;
                tr[active].dataSectors = _MainLoopPceParseMsf(a2);
            }
            else
            {
                tr[active].fileOffsetBytes = expectedOffset;
                tr[active].hasOffset = 0;
                tr[active].dataSectors = _MainLoopPceParseMsf(a1);
            }

            if (tr[active].dataSectors <= 0)
                goto done;

            tr[active].haveData = 1;

            if (tr[active].hasOffset &&
                tr[active].fileOffsetBytes != expectedOffset)
                goto done;

            expectedOffset += tr[active].dataSectors * 2352L;
            continue;
        }

        if (!strncasecmp(p, "INDEX", 5) &&
            (p[5] == ' ' || p[5] == '\t'))
            goto done;
    }

    if (trackCount < 2 || !commonFile[0])
        goto done;

    for (i = 0; i < trackCount; ++i)
    {
        if (!tr[i].haveData)
            goto done;

        if (tr[i].generatedPregap < 0 ||
            tr[i].generatedPregap > tr[i].dataSectors)
            goto done;

        generatedGapBytes +=
            (long)tr[i].generatedPregap * 2352L;
    }

    for (i = 0; i < trackCount; ++i)
    {
        if (tr[i].generatedPregap && tr[i].hasStart &&
            tr[i].generatedPregap != tr[i].startSectors)
            goto done;
    }

    slash = strrchr(pTocPath, '/');
    if (strchr(commonFile, ':') || commonFile[0] == '/')
    {
        snprintf(binPath, sizeof(binPath), "%s", commonFile);
    }
    else
    {
        size_t dirLen;
        if (!slash)
            snprintf(tocDir, sizeof(tocDir), ".");
        else
        {
            dirLen = (size_t)(slash - pTocPath);
            if (dirLen >= sizeof(tocDir))
                goto done;
            memcpy(tocDir, pTocPath, dirLen);
            tocDir[dirLen] = 0;
        }

        if (snprintf(binPath, sizeof(binPath), "%s/%s",
                     tocDir, commonFile) >= (int)sizeof(binPath))
            goto done;
    }

    /* Decide from the real BIN size whether generated ZERO/SILENCE
     * sectors are physically stored or only exist in the logical TOC. */
    {
        FILE *bin = fopen(binPath, "rb");
        const long virtualBytes = expectedOffset;
        const long compactBytes = virtualBytes - generatedGapBytes;

        if (!bin)
            goto done;
        if (fseek(bin, 0, SEEK_END) != 0)
        {
            fclose(bin);
            goto done;
        }

        actualBinBytes = ftell(bin);
        fclose(bin);

        if (actualBinBytes == compactBytes)
            compactGeneratedGaps = TRUE;
        else if (actualBinBytes == virtualBytes)
            compactGeneratedGaps = FALSE;
        else
        {
            printf("[PCE/CD] cdrdao size mismatch: actual=%ld virtual=%ld compact=%ld\n",
                   actualBinBytes, virtualBytes, compactBytes);
            goto done;
        }

        printf("[PCE/CD] cdrdao geometry: actual=%ld virtual=%ld compact=%ld mode=%s\n",
               actualBinBytes, virtualBytes, compactBytes,
               compactGeneratedGaps ? "compact-gaps" : "stored-gaps");
    }

    if (snprintf(pOutCue, nOutCue,
                 "%s/__AURORA_PCE_CDRDAO_TMP__.cue",
                 pSystemDirectory) >= (int)nOutCue)
        goto done;

    remove(pOutCue);
    out = fopen(pOutCue, "wb");
    if (!out)
        goto done;

    fprintf(out, "FILE \"%s\" BINARY\n", binPath);

    /* AURORA_V4_15_PCE_CDRDAO_AUDIO_ENDIAN_20260830
     * Ordinary CUE has no flag for cdrdao raw AUDIO byte order.
     * cdrdao raw PCM is MSB-first; preserve it explicitly. */
    fprintf(out, "REM AURORA_CDRDAO_RAW_AUDIO_MSB_FIRST\n");

    physicalCueSectors = 0;
    for (i = 0; i < trackCount; ++i)
    {
        char msf0[32], msf1[32], pg[32];
        long index0 = physicalCueSectors;
        long index1 = index0;
        long storedSectors = tr[i].dataSectors;

        fprintf(out, "  TRACK %02d %s\n", i + 1, tr[i].cueType);

        if (tr[i].generatedPregap > 0)
        {
            if (compactGeneratedGaps)
            {
                _MainLoopPceFormatMsf(
                    tr[i].generatedPregap, pg, sizeof(pg));
                fprintf(out, "    PREGAP %s\n", pg);
                storedSectors -= tr[i].generatedPregap;
            }
            else
            {
                /* AURORA_V4_14_FILEXIO_OPENFLAG_PCE_STORED_PREGAPS_20260830
                 * Gap sectors are physically stored in this single BIN.
                 * Represent them as INDEX 00 -> INDEX 01. */
                index1 += tr[i].generatedPregap;
                _MainLoopPceFormatMsf(index0, msf0, sizeof(msf0));
                fprintf(out, "    INDEX 00 %s\n", msf0);
            }
        }
        else if (tr[i].hasStart && tr[i].startSectors > 0)
        {
            index1 += tr[i].startSectors;
            _MainLoopPceFormatMsf(index0, msf0, sizeof(msf0));
            fprintf(out, "    INDEX 00 %s\n", msf0);
        }

        if (storedSectors < 0)
            goto done;

        _MainLoopPceFormatMsf(index1, msf1, sizeof(msf1));
        fprintf(out, "    INDEX 01 %s\n", msf1);

        printf("[PCE/CD] cue track %02d: INDEX01=%s logical=%ld stored=%ld pregap=%d\n",
               i + 1, msf1, tr[i].dataSectors,
               storedSectors, tr[i].generatedPregap);

        physicalCueSectors += storedSectors;
    }

    if (physicalCueSectors * 2352L != actualBinBytes)
    {
        printf("[PCE/CD] generated CUE geometry mismatch: sectors=%ld bytes=%ld actual=%ld\n",
               physicalCueSectors,
               physicalCueSectors * 2352L,
               actualBinBytes);
        goto done;
    }

    if (fflush(out) != 0 || fclose(out) != 0)
    {
        out = NULL;
        remove(pOutCue);
        goto done;
    }
    out = NULL;

    printf("[PCE/CD] cdrdao single-BIN converted to CUE: %s -> %s\n",
           pTocPath, pOutCue);
    ok = TRUE;

done:
    if (out) fclose(out);
    if (in) fclose(in);
    if (!ok && pOutCue && *pOutCue)
    {
        remove(pOutCue);
        pOutCue[0] = 0;
    }
    return ok;
}

/* AURORA_HDD_ISSUE38_V3_20261013
 * During a game switch _RomPath is cleared before FDS/SCD/PCECD firmware
 * preflight. Always resolve from the NEW game path, not the prior ROM.
 * The existing v2 function returns the partition-local mounted PFS root. */
static Bool AuroraIssue38FirmwareDirectory(
    const char *source, char *out, Int32 outBytes)
{
    char root[64];
    int n;
    if (!out || outBytes <= 0) return FALSE;
    out[0] = 0;
    if (!source) return FALSE;
    if (!strncmp(source, "pfs0:", 5))
    {
        /* FHDB provided only an already mounted PFS path. */
        if (!HddSupportIsEnabled()) return FALSE;
        n = snprintf(out, (size_t)outBytes, "pfs0:/SNESticle/SYSTEM");
        return n >= 0 && n < outBytes ? TRUE : FALSE;
    }
    if (strncmp(source, "hdd0:", 5) ||
        !MainLoopHddSystemRootForSource(source, root, sizeof(root)))
        return FALSE;
    n = snprintf(out, (size_t)outBytes, "%s/SYSTEM", root);
    if (n < 0 || n >= outBytes) { out[0] = 0; return FALSE; }
    return TRUE;
}

static Bool AuroraIssue38FindFirmware(
    const char *source, const char *name, char *out, Int32 outBytes)
{
    char dir[512];
    char path[1024];
    FILE *f;
    int n;
    if (!name || !*name ||
        !AuroraIssue38FirmwareDirectory(source, dir, sizeof(dir)))
        return FALSE;
    n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof(path)) return FALSE;
    f = fopen(path, "rb");
    if (!f) return FALSE;
    fclose(f);
    n = snprintf(out, (size_t)outBytes, "%s", dir);
    if (n < 0 || n >= outBytes) return FALSE;
    return TRUE;
}

static Bool AuroraIssue38EnsureSystem(
    const char *source, char *out, Int32 outBytes)
{
    char dir[512];
    char root[512];
    if (!AuroraIssue38FirmwareDirectory(source, dir, sizeof(dir)))
        return FALSE;
    if (snprintf(root, sizeof(root), "%s", dir) >= (int)sizeof(root))
        return FALSE;
    char *slash = strrchr(root, '/');
    if (!slash) return FALSE;
    *slash = 0;
    /* Reuse the ZIP storage directory helper: mkdir/stat and fileXioDopen
     * fallback for iomanX devices that do not implement newlib stat. */
    if (!_MainLoopZipEnsureStorageDir(root))
        return FALSE;
    if (!_MainLoopZipEnsureStorageDir(dir))
        return FALSE;
    if (snprintf(out, (size_t)outBytes, "%s", dir) >= outBytes)
        return FALSE;
    return TRUE;
}

/* AURORA_CD_SRAM_NOTICES_20260824
 * A CUE and its BIN/audio tracks stay on storage. This path allocates no
 * frontend ROM buffer, which is essential inside the EE's 32 MiB budget. */
static Bool _MainLoopExecuteDisc(const char *pMappedPath,
                                 const char *pOriginalPath,
                                 const char *pBasePath,
                                 Bool bLoadSRAM)
{
    Char SystemDirectory[512];
    Emu::System *pSystem = NULL;
    const char *pName;
    const char *pPceLoadPath = pMappedPath;
    int eDisc;
    Bool bLoaded;

    if (!pMappedPath || !*pMappedPath ||
        !pOriginalPath || !pBasePath)
        return FALSE;

    if (!AuroraIssue38EnsureSystem(
            pOriginalPath, SystemDirectory, (Int32)sizeof(SystemDirectory)) &&
        !MainLoopEnsureSystemDirectory(
            SystemDirectory, (Int32)sizeof(SystemDirectory)))
    {
        MainLoopModalPrintf(60 * 4,
            "ERROR: cannot create SNESticle/SYSTEM");
        return FALSE;
    }

    /* AURORA_PCE_CDRDAO_TOC_SUPPORT_V4_8_20260830
     * PicoDrive's classifier is intentionally CUE-only. A cdrdao TOC is a
     * PCE-supported image format and must reach Beetle without being parsed
     * by PicoDrive first. Mislabeled TOC-as-.cue is accepted by syntax too. */
    if (_MainLoopLooksLikeCdrdaoToc(pMappedPath))
        eDisc = 0;
    else
        eDisc = PicoDriveBridge_ProbeSegaCd(pMappedPath);

    if (eDisc < 0)
    {
        MainLoopModalPrintf(60 * 4,
            "ERROR: CUE/TOC or its first track is unreadable");
        return FALSE;
    }

    if (eDisc == 0 && _MainLoopLooksLikeCdrdaoToc(pMappedPath))
    {
        s_PceCdrdaoTempCue[0] = 0;
        if (_MainLoopBuildPceCueFromCdrdao(
                pMappedPath, SystemDirectory,
                s_PceCdrdaoTempCue,
                sizeof(s_PceCdrdaoTempCue)))
        {
            pPceLoadPath = s_PceCdrdaoTempCue;
        }
        else
        {
            pPceLoadPath = pMappedPath;
            printf("[PCE/CD] cdrdao TOC not convertible; using native parser\n");
        }
    }

    /* AURORA_SEGA_CD_32X_MD_SCALING_V2R1_20260828
     * Sega CD uses the same Genesis/MD H32/H40 output. Choose the native
     * 320-wide raster before PicoDrive allocates MCD; PCE CD stays 256. */
    /* AURORA_PCE_PRECORE512_V13_20260830
     * Sega CD keeps MD's 320 raster; PCE CD uses the same fixed 512 storage
     * raster as HuCards before Beetle is initialized. */
    if (!MainLoopEnsureGameplayRasterWidth(eDisc > 0 ? 320 : 512))
    {
        MainLoopModalPrintf(60 * 3,
            "ERROR: cannot configure CD video raster");
        return FALSE;
    }

    _MainLoop_fOutputIntensity = 1.0f;
    /* AURORA_CD_BIOS_SYSTEM_RESOLVE_V15_20260911
     * Keep SystemDirectory as the writable frontend work directory.
     * Resolve a second directory only for CD firmware lookup.
     */
    Char FirmwareSystemDirectory[512];
    Char CandidateSystemDirectory[512];

    if (snprintf(FirmwareSystemDirectory,
                 sizeof(FirmwareSystemDirectory),
                 "%s", SystemDirectory) >=
        (int)sizeof(FirmwareSystemDirectory))
    {
        MainLoopModalPrintf(60 * 4, "ERROR: SYSTEM path is too long");
        return FALSE;
    }

    CandidateSystemDirectory[0] = 0;

    if (eDisc > 0)
    {
        static const Char *const kSegaCdSystemFiles[] = {
            "bios_CD_U.bin",
            "bios_CD_E.bin",
            "bios_CD_J.bin",
            "bios_CD_U.zip",
            "bios_CD_E.zip",
            "bios_CD_J.zip",

            "us_scd2_9306.bin",
            "SegaCDBIOS9303.bin",
            "us_scd1_9210.bin",
            "eu_mcd2_9306.bin",
            "eu_mcd2_9303.bin",
            "eu_mcd1_9210.bin",
            "jp_mcd2_921222.bin",
            "jp_mcd1_9112.bin",
            "jp_mcd1_9111.bin",

            "us_scd2_9306.zip",
            "SegaCDBIOS9303.zip",
            "us_scd1_9210.zip",
            "eu_mcd2_9306.zip",
            "eu_mcd2_9303.zip",
            "eu_mcd1_9210.zip",
            "jp_mcd2_921222.zip",
            "jp_mcd1_9112.zip",
            "jp_mcd1_9111.zip"
        };

        for (Uint32 i = 0;
             i < sizeof(kSegaCdSystemFiles) /
                 sizeof(kSegaCdSystemFiles[0]);
             ++i)
        {
            CandidateSystemDirectory[0] = 0;

            if (!AuroraIssue38FindFirmware(
                    pOriginalPath, kSegaCdSystemFiles[i],
                    CandidateSystemDirectory,
                    (Int32)sizeof(CandidateSystemDirectory)) &&
                !MainLoopFindSystemFileDirectory(
                    CandidateSystemDirectory,
                    (Int32)sizeof(CandidateSystemDirectory),
                    kSegaCdSystemFiles[i]))
                continue;

            if (snprintf(FirmwareSystemDirectory,
                         sizeof(FirmwareSystemDirectory),
                         "%s", CandidateSystemDirectory) >=
                (int)sizeof(FirmwareSystemDirectory))
            {
                MainLoopModalPrintf(
                    60 * 4, "ERROR: Sega CD SYSTEM path is too long");
                return FALSE;
            }

            ConPrint("Sega CD firmware SYSTEM: %s (%s)\n",
                     FirmwareSystemDirectory,
                     kSegaCdSystemFiles[i]);
            break;
        }
    }
    else
    {
        if (AuroraIssue38FindFirmware(
                pOriginalPath, "syscard3.pce",
                CandidateSystemDirectory,
                (Int32)sizeof(CandidateSystemDirectory)) ||
            MainLoopFindSystemFileDirectory(
                CandidateSystemDirectory,
                (Int32)sizeof(CandidateSystemDirectory),
                "syscard3.pce"))
        {
            if (snprintf(FirmwareSystemDirectory,
                         sizeof(FirmwareSystemDirectory),
                         "%s", CandidateSystemDirectory) >=
                (int)sizeof(FirmwareSystemDirectory))
            {
                MainLoopModalPrintf(
                    60 * 4, "ERROR: PCE CD SYSTEM path is too long");
                return FALSE;
            }

            ConPrint("PCE CD firmware SYSTEM: %s (syscard3.pce)\n",
                     FirmwareSystemDirectory);
        }
    }

    if (eDisc > 0)
    {
        if (!_MainLoopEnsureSegaSystem())
        {
            MainLoopModalPrintf(60 * 3,
                "ERROR: not enough memory for Sega CD core");
            return FALSE;
        }
        pSystem = _pSega;
        PicoDriveBridge_SetRegion((int)g_SnesForceRegion);
        /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: BIOS + disco entram juntos no retro_load_game. */
        _MainLoopLoadPhaseBegin();
        bLoaded = _pSega && _pSega->LoadDisc(
            pMappedPath, FirmwareSystemDirectory);
        _MainLoopLoadProgressFinish(bLoaded);
    }
    else
    {
        if (!_MainLoopEnsurePceSystem())
        {
            MainLoopModalPrintf(60 * 3,
                "ERROR: not enough memory for PCE CD core");
            return FALSE;
        }
        pSystem = _pPce;
        /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: System Card + disco entram juntos no retro_load_game. */
        _MainLoopLoadPhaseBegin();
        bLoaded = _pPce && _pPce->LoadDisc(
            pPceLoadPath, FirmwareSystemDirectory);
        _MainLoopLoadProgressFinish(bLoaded);
    }

    if (!bLoaded || !pSystem ||
        (pSystem == _pSega && !_pSega->IsRomReady()) ||
        (pSystem == _pPce && !_pPce->IsRomReady()))
    {
        _MainLoopUnloadRom();
        if (eDisc > 0)
            MainLoopModalPrintf(60 * 5,
                "ERROR: Sega CD failed; put its region BIOS .bin in SYSTEM");
        else
            MainLoopModalPrintf(60 * 5,
                "ERROR: PCE CD failed; put syscard3.pce in SYSTEM");
        return FALSE;
    }

    _pSystem = pSystem;
    pSystem->Reset();

    pName = strrchr(pBasePath, '/');
    if (!pName)
        pName = strrchr(pBasePath, '\\');
    pName = pName ? pName + 1 : pBasePath;
    snprintf(_RomName, sizeof(_RomName), "%s", pName);
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalPath);
    MainLoopStateOnRomChanged();

    ConPrint("CD Loaded: %s (%s)\n", pMappedPath,
             pSystem == _pSega ? "Sega CD" : "PC Engine CD");
    _MainLoopSetSampleRate(pSystem->GetSampleRate());
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    _fbTexture[0]->Clear();
    TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));
    return TRUE;
}


/* AURORA_FCEUMM_FDS_V0_5_PATH_BOOT
 * FCEUmm's pinned libretro frontend requires a real full path. Keep the FDS
 * image on storage and point the core at Aurora's existing SYSTEM directory.
 */
static Bool _MainLoopExecuteFdsPath(const char *pMappedPath,
                                    const char *pOriginalPath,
                                    Bool bLoadSRAM)
{
    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914:_MainLoopExecuteFdsPath */
    if (!_MainLoopEnsureFdsSystem())
    {
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for FDS core");
        return FALSE;
    }

    Char SystemDirectory[512];
    Char BiosPath[1024];
    FILE *pBios;
    long nBiosBytes;

    if (!pMappedPath || !*pMappedPath || !pOriginalPath || !*pOriginalPath || !_pFds)
        return FALSE;

    if (!MainLoopEnsureGameplayRasterWidth(256))
    {
        MainLoopModalPrintf(60 * 3, "ERROR: cannot configure FDS video raster");
        return FALSE;
    }

    /* AURORA_SYSTEM_BIOS_PATH_FIX_V1_20260826
     * Prefer an already-existing disksys.rom, with mass0:/SNESticle/SYSTEM
     * first. Only if no BIOS is found do we fall back to ensuring the normal
     * firmware directory, so the subsequent error reports the intended path.
     */
    if (!AuroraIssue38FindFirmware(
            pOriginalPath, "disksys.rom",
            SystemDirectory, (Int32)sizeof(SystemDirectory)) &&
        !MainLoopFindSystemFileDirectory(
            SystemDirectory, (Int32)sizeof(SystemDirectory), "disksys.rom") &&
        !AuroraIssue38EnsureSystem(
            pOriginalPath, SystemDirectory, (Int32)sizeof(SystemDirectory)) &&
        !MainLoopEnsureSystemDirectory(
            SystemDirectory, (Int32)sizeof(SystemDirectory)))
    {
        MainLoopModalPrintf(60 * 4, "ERROR: cannot create SNESticle/SYSTEM");
        return FALSE;
    }

    if (snprintf(BiosPath, sizeof(BiosPath), "%s/disksys.rom", SystemDirectory) >=
        (int)sizeof(BiosPath))
    {
        MainLoopModalPrintf(60 * 4, "ERROR: FDS BIOS path is too long");
        return FALSE;
    }

    printf("[FDS] BIOS probe: %s\n", BiosPath);
    pBios = fopen(BiosPath, "rb");
    if (!pBios)
    {
        printf("[FDS] BIOS fopen failed: %s\n", BiosPath);
        MainLoopModalPrintf(60 * 5, "ERROR: put disksys.rom in SNESticle/SYSTEM");
        return FALSE;
    }
    if (fseek(pBios, 0, SEEK_END) != 0)
    {
        fclose(pBios);
        MainLoopModalPrintf(60 * 5, "ERROR: cannot read SYSTEM/disksys.rom");
        return FALSE;
    }
    nBiosBytes = ftell(pBios);
    fclose(pBios);
    if (nBiosBytes != 8192)
    {
        MainLoopModalPrintf(60 * 5, "ERROR: disksys.rom must be exactly 8192 bytes");
        return FALSE;
    }

    _MainLoop_fOutputIntensity = 0.8f;
    /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: BIOS + FDS são uma única fase bloqueante do FCEUmm. */
    _MainLoopLoadPhaseBegin();
    Bool bFdsLoaded =
        _pFds->LoadDisk(pMappedPath, SystemDirectory) &&
        _pFds->IsRomReady();
    _MainLoopLoadProgressFinish(bFdsLoaded);
    if (!bFdsLoaded)
    {
        _MainLoopUnloadRom();
        MainLoopModalPrintf(60 * 5,
            "ERROR: FCEUmm could not boot FDS; check SYSTEM/disksys.rom");
        return FALSE;
    }

    _pSystem = _pFds;
    _pFds->Reset();
    _MainLoopGetName(_RomName, pOriginalPath);
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalPath);
    MainLoopStateOnRomChanged();
    _MainLoopSetSampleRate(_pFds->GetSampleRate());
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    if (_fbTexture[0]) _fbTexture[0]->Clear();
    if (_fbTexture[1]) _fbTexture[1]->Clear();
    if (_fbTexture[0]) TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));

    _MainLoop_iDisk = 0;
    _MainLoop_bDiskInserted = TRUE;
    ConPrint("FDS Loaded via FCEUmm: %s\n", pMappedPath);
    return TRUE;
}


/* AURORA_FDS_V4_ZIP_LOADER_20260828 */
/* AURORA_FDS_D88_TFA_SWC_V3_20260913 */
/* AURORA_FDS_ZIP_RAMONLY_V3_20260913
 * ZIP storage is input-only. miniz expands the selected .fds member into a
 * small EE-RAM buffer and the existing FdsSystem/FCEUmm borrowed-memory path
 * consumes it synchronously. FDSLoad copies all disk sides before returning,
 * so this frontend buffer is released immediately after LoadDiskMemory().
 * No temporary file is created on USB, memory card, HDD, or source storage. */
/* AURORA_ZIP_FILEONLY_MARIO_BIOS_TSUKURU8M_V3_20260913_ZIP_FILEONLY: FDS ZIP -> SYSTEM/UNZIP -> FCEUmm full path. */
static Bool _MainLoopExecuteFdsZip(const char *pZipPath,
                                   const char *pOriginalZipPath,
                                   const char *pMemberName,
                                   unsigned int uZipIndex,
                                   Int32 nExpectedBytes,
                                   Bool bLoadSRAM)
{
    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914:_MainLoopExecuteFdsZip */
    if (!_MainLoopEnsureFdsSystem())
    {
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for FDS core");
        return FALSE;
    }

    Char tempPath[1024];

    if (!pZipPath || !*pZipPath ||
        !pOriginalZipPath || !*pOriginalZipPath ||
        !pMemberName || !*pMemberName ||
        !_pFds ||
        nExpectedBytes < (Int32)MAINLOOP_FDS_ZIP_MIN_BYTES ||
        nExpectedBytes > (Int32)MAINLOOP_FDS_ZIP_MAX_BYTES)
    {
        MainLoopModalPrintf(
            60 * 4, "ERROR: invalid FDS image in ZIP");
        return FALSE;
    }

    _MainLoopRemoveFdsZipTemp();

    if (!_MainLoopPrepareZipTempPath(
            tempPath, (Int32)sizeof(tempPath),
            "aurora_fds_zip.fds", pOriginalZipPath))
    {
        if (!s_ZipExtractBlockedOnMemoryCard)
            MainLoopModalPrintf(
                60 * 4, "ZIP requires writable USB/MX4SIO/HDD");
        return FALSE;
    }

    if (!_MainLoopExtractZipEntryToFile(
            pZipPath, pMemberName, uZipIndex,
            nExpectedBytes, tempPath))
    {
        if (!s_ZipExtractBlockedOnMemoryCard)
            MainLoopModalPrintf(
                60 * 4, "ERROR: Cannot extract complete FDS ZIP");
        return FALSE;
    }

    if (snprintf(s_FdsZipTempPath,
                 sizeof(s_FdsZipTempPath), "%s", tempPath) >=
        (int)sizeof(s_FdsZipTempPath))
    {
        fileXioRemove(tempPath);
        s_FdsZipTempPath[0] = 0;
        MainLoopModalPrintf(60 * 4, "ERROR: FDS temp path too long");
        return FALSE;
    }

    /* FCEUmm need_fullpath keeps this .fds alive. The existing FDS cleanup
     * runs only after SetRom(NULL), so eject/insert and side changes can keep
     * using the same backing file. */
    if (!_MainLoopExecuteFdsPath(
            s_FdsZipTempPath, pOriginalZipPath, FALSE))
    {
        _MainLoopRemoveFdsZipTemp();
        return FALSE;
    }

    _MainLoopGetName(_RomName, pMemberName);
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalZipPath);
    MainLoopStateOnRomChanged();
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    ConPrint("FDS ZIP file-backed: %s -> %s -> %s (%d bytes)\n",
             pZipPath, pMemberName, s_FdsZipTempPath,
             nExpectedBytes);
    return TRUE;
}



/* AURORA_SWC_FLOPPY_V1_20260831 */
static Bool _MainLoopSwcIsFirmwareName(const char *pName);

static const char *_MainLoopSwcBaseName(const char *pPath)
{
    const char *p;
    const char *pName;

    if (!pPath)
        return NULL;

    p = pPath;
    pName = pPath;
    while (*p)
    {
        if (*p == '/' || *p == '\\')
            pName = p + 1;
        ++p;
    }
    return pName;
}

/* AURORA_SWC_FLOPPY_V5_20260831 */
static Bool _MainLoopSwcPathIsFirmware(const char *pPath)
{
    const char *pName = _MainLoopSwcBaseName(pPath);

    if (!pName)
        return FALSE;

    if (!strcasecmp(pName, "dsk.rom") ||
        !strcasecmp(pName, "dsk.sfc") ||
        !strcasecmp(pName, "dsk.smc"))
        return TRUE;

    return _MainLoopSwcIsFirmwareName(pName);
}

static Bool _MainLoopSwcIsFirmwareName(const char *pName)
{
    const char *pExt;
    if (!pName) return FALSE;

    if (strncasecmp(pName, "Super Wild Card", 15) != 0)
        return FALSE;

    /* AURORA_CLASSIC_COPIER_FIRMWARE_V10_15B_20260831
     * DX/DX2 are later hardware families, not BIOS revisions of this model. */
    if (!strncasecmp(pName, "Super Wild Card DX", 18))
        return FALSE;

    pExt = strrchr(pName, '.');
    if (!pExt) return FALSE;

    return (!strcasecmp(pExt, ".rom") ||
            !strcasecmp(pExt, ".sfc") ||
            !strcasecmp(pExt, ".smc")) ? TRUE : FALSE;
}

/* AURORA_V6_MAGICOM_FRONT_FAREAST_20260831: classic Super Magicom V1H/V31 frontend recognition. */
static Bool _MainLoopMagicomNameIsV3H(const char *pName)
{
    if (!pName) return FALSE;
    for (const char *p=pName; p[0] && p[1] && p[2]; ++p)
        if (!strncasecmp(p,"V3H",3)) return TRUE;
    return FALSE;
}

static Bool _MainLoopMagicomPathIsFirmware(const char *pPath)
{
    const char *pName=_MainLoopSwcBaseName(pPath);
    const char *pExt;
    if (!pName) return FALSE;
    if (!strcasecmp(pName,"magicom.rom") ||
        !strcasecmp(pName,"magicom.sfc") ||
        !strcasecmp(pName,"magicom.smc")) return TRUE;
    if (strncasecmp(pName,"Super Magicom",13)!=0) return FALSE;
    pExt=strrchr(pName,'.');
    if (!pExt) return FALSE;
    return (!strcasecmp(pExt,".rom") || !strcasecmp(pExt,".sfc") ||
            !strcasecmp(pExt,".smc")) ? TRUE : FALSE;
}

static Bool _MainLoopMagicomFirmwareFileLooksValid(const char *pPath)
{
    FILE *fp; long nBytes,nBase; Uint8 v[2];
    const char *pName=_MainLoopSwcBaseName(pPath);
    if (!pPath || !*pPath || !pName || !_MainLoopMagicomPathIsFirmware(pPath) ||
        _MainLoopMagicomNameIsV3H(pName)) return FALSE;
    fp=fopen(pPath,"rb"); if (!fp) return FALSE;
    if (fseek(fp,0,SEEK_END)!=0) { fclose(fp); return FALSE; }
    nBytes=ftell(fp);
    if (nBytes==0x2000L) nBase=0;
    else if (nBytes==0x2200L) nBase=0x200L;
    else if (nBytes==0x8000L) nBase=0x6000L;
    else { fclose(fp); return FALSE; }
    if (fseek(fp,nBase+0x1FFCL,SEEK_SET)!=0 || fread(v,1,2,fp)!=2)
    { fclose(fp); return FALSE; }
    fclose(fp);
    {
        Uint16 rv=(Uint16)v[0]|((Uint16)v[1]<<8);
        return (rv>=0xE000 && rv!=0xFFFF) ? TRUE : FALSE;
    }
}

/* AURORA_SWC_MEGA_V9_20260831: validate SWC media before destructive unload. */
/* AURORA_CLASSIC_MEMORY_MODE_V10_15B_20260831 */
/* AURORA_CLASSIC_COPIER_FIRMWARE_V10_15B_20260831 */
static Bool _MainLoopClassicSwcVectorOK(const Uint8 *p, Uint32 nBytes)
{
    Uint16 rv;
    if (!p || nBytes < 0x2000u)
        return FALSE;
    rv = (Uint16)p[0x1FFCu] | ((Uint16)p[0x1FFDu] << 8);
    return (rv >= 0xE000u && rv != 0xFFFFu) ? TRUE : FALSE;
}

static Bool _MainLoopSwcFirmwareFileLooksValid(const char *pPath)
{
    FILE *fp;
    long nBytes;
    long nSkip = 0;
    Uint8 v[2];

    if (!pPath || !*pPath)
        return FALSE;

    fp = fopen(pPath, "rb");
    if (!fp)
        return FALSE;

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return FALSE;
    }
    nBytes = ftell(fp);

    if (nBytes == 0x4200L)
        nSkip = 512;
    else if (nBytes == 0x4000L)
        nSkip = 0;
    else if (nBytes == 0x10000L)
    {
        Uint8 firstBlock[0x4000];
        Uint8 block[0x4000];
        Int32 first = -1;
        Int32 nValid = 0;
        Bool identical = TRUE;

        for (Int32 q = 0; q < 4; ++q)
        {
            long off = (long)q * 0x4000L;
            if (fseek(fp, off, SEEK_SET) != 0 ||
                fread(block, 1, sizeof(block), fp) != sizeof(block))
            {
                fclose(fp);
                return FALSE;
            }

            if (_MainLoopClassicSwcVectorOK(block, (Uint32)sizeof(block)))
            {
                if (first < 0)
                {
                    first = q;
                    memcpy(firstBlock, block, sizeof(block));
                }
                else if (memcmp(firstBlock, block, sizeof(block)) != 0)
                {
                    identical = FALSE;
                }
                ++nValid;
            }
        }

        if (nValid == 0 || (nValid > 1 && !identical))
        {
            fclose(fp);
            return FALSE;
        }

        nSkip = (long)first * 0x4000L;
    }
    else
    {
        fclose(fp);
        return FALSE;
    }

    if (fseek(fp, nSkip + 0x1FFCL, SEEK_SET) != 0 ||
        fread(v, 1, 2, fp) != 2)
    {
        fclose(fp);
        return FALSE;
    }
    fclose(fp);

    {
        Uint16 resetVector = (Uint16)v[0] | ((Uint16)v[1] << 8);
        return (resetVector != 0x0000 && resetVector != 0xFFFF)
            ? TRUE : FALSE;
    }
}

/* AURORA_COPIER_DSK_PRIORITY_AUTO_DISK1_V10_17_20260901
 * DSK.* is the canonical user override for classic copier autoboot. It wins over
 * every named BIOS. Determine classic model from payload, not filename.
 */

/* AURORA_SUPER_MAGIC_DRIVE_V1_20260902 */
static const char *_MainLoopSmdBaseName(const char*p){const char*a,*b;if(!p)return"";a=strrchr(p,'/');b=strrchr(p,'\\');if(!a||(b&&b>a))a=b;return a?a+1:p;}
static Bool _MainLoopIsSmdBiosPath(const char*p){const char*n=_MainLoopSmdBaseName(p);return n&&strcasecmp(n,"mdpbios.bin")==0?TRUE:FALSE;}
static Bool _MainLoopSmdBiosLooksValid(const char*p){struct stat st;if(!p||stat(p,&st)!=0||S_ISDIR(st.st_mode)||st.st_size!=0x2000)return FALSE;return TRUE;}
static Bool _MainLoopExecuteSmdFirmware(const char*p,const char*orig,Bool loadSram){
    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914:_MainLoopExecuteSmdFirmware */
    if (!_MainLoopEnsureSegaSystem())
    {
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for Sega core");
        return FALSE;
    }
if(!_pSega||!_MainLoopIsSmdBiosPath(p)||!_MainLoopSmdBiosLooksValid(p))return FALSE;if(!MainLoopEnsureGameplayRasterWidth(256))return FALSE; /* AURORA_SMD_V1_7_RECOVERY_BOOT_20260903 */if(!_pSega->LoadSuperMagicDrive(p,NULL)){MainLoopModalPrintf(60*5,"SMD boot failed: %s",_pSega->GetSuperMagicDriveError());return FALSE;}_pSystem=_pSega;s_SmdExternalCartPath[0]=0;snprintf(_RomName,sizeof(_RomName),"%s","Super Magic Drive");snprintf(_RomPath,sizeof(_RomPath),"%s",orig&&*orig?orig:p);MainLoopStateOnRomChanged();_MainLoopSetSampleRate(_pSega->GetSampleRate());if(loadSram)_MainLoopLoadSRAM();MainLoopStatusPrintf(180,"Super Magic Drive V3: no disk / no cart");return TRUE;}
static void _MainLoopSmdPut16(Uint8*p,Uint16 v){p[0]=(Uint8)v;p[1]=(Uint8)(v>>8);}static void _MainLoopSmdPut32(Uint8*p,Uint32 v){p[0]=(Uint8)v;p[1]=(Uint8)(v>>8);p[2]=(Uint8)(v>>16);p[3]=(Uint8)(v>>24);}
static Bool _MainLoopSmdCreateD88(const char*p){
 enum{HB=0x2b0,TRACKS=80,HEADS=2,SPT=18,SLOTS=20,SEC=512,SH=16,SLOT=SLOTS*(SH+SEC)};Uint8 header[HB],*track=NULL,logical[SEC];FILE*fp=NULL;Uint32 total=HB+(Uint32)(TRACKS*HEADS)*SLOT,lba=0;Bool ok=FALSE;if(!p||!*p)return FALSE;fp=fopen(p,"wb");if(!fp)return FALSE;track=(Uint8*)malloc(SLOT);if(!track)goto done;memset(header,0,sizeof(header));memcpy(header,"AURORA SMD",10);header[0x1b]=0x20;_MainLoopSmdPut32(header+0x1c,total);for(Uint32 t=0;t<TRACKS*HEADS;t++)_MainLoopSmdPut32(header+0x20+t*4,HB+t*SLOT);if(fwrite(header,1,sizeof(header),fp)!=sizeof(header))goto done;
 for(Uint32 ti=0;ti<TRACKS*HEADS;ti++){Uint8 c=(Uint8)(ti>>1),h=(Uint8)(ti&1);Uint32 pos=0;memset(track,0,SLOT);for(Uint8 r=1;r<=SPT;r++,lba++){Uint8*sh=track+pos,*data=sh+SH;memset(logical,0,sizeof(logical));if(lba==0){logical[0]=0xeb;logical[1]=0x3c;logical[2]=0x90;memcpy(logical+3,"AURORASMD",9);_MainLoopSmdPut16(logical+11,512);logical[13]=1;_MainLoopSmdPut16(logical+14,1);logical[16]=2;_MainLoopSmdPut16(logical+17,224);_MainLoopSmdPut16(logical+19,2880);logical[21]=0xf0;_MainLoopSmdPut16(logical+22,9);_MainLoopSmdPut16(logical+24,18);_MainLoopSmdPut16(logical+26,2);logical[510]=0x55;logical[511]=0xaa;}else if(lba==1||lba==10){logical[0]=0xf0;logical[1]=logical[2]=0xff;}sh[0]=c;sh[1]=h;sh[2]=r;sh[3]=2;_MainLoopSmdPut16(sh+4,SPT);sh[13]=1;_MainLoopSmdPut16(sh+14,SEC);memcpy(data,logical,SEC);pos+=SH+SEC;}if(fwrite(track,1,SLOT,fp)!=SLOT)goto done;}ok=fflush(fp)==0?TRUE:FALSE;done:if(track)free(track);if(fp)fclose(fp);if(!ok)remove(p);return ok;}
static Bool _MainLoopSmdStem(char*out,Int32 cap){const char*n,*dot;size_t z;if(!out||cap<=1)return FALSE;n=s_SmdExternalCartPath[0]?_MainLoopSmdBaseName(s_SmdExternalCartPath):"SMD";dot=strrchr(n,'.');z=dot?(size_t)(dot-n):strlen(n);if(!z||z>=(size_t)cap)return FALSE;memcpy(out,n,z);out[z]=0;for(size_t i=0;i<z;i++)if((unsigned char)out[i]<0x20||strchr("\\/:*?\"<>|",out[i]))out[i]='_';return TRUE;}
static Bool _MainLoopSmdDiskPath(char*out,Int32 cap,unsigned idx){Char dir[512],stem[256];int n;if(!out||cap<=0||!idx||!_MainLoopSmdStem(stem,sizeof(stem))||!MainLoopEnsureSwcDirectory(dir,sizeof(dir)))return FALSE;n=snprintf(out,cap,"%s/%s_%u.d88",dir,stem,idx);return n>=0&&n<cap?TRUE:FALSE;}
static Bool _MainLoopSmdNumberedIndex(const char*p,unsigned*out){const char*n=_MainLoopSmdBaseName(p),*dot=strrchr(n,'.'),*q;unsigned v=0,m=1;if(!dot||strcasecmp(dot,".d88")!=0||dot==n)return FALSE;q=dot;while(q>n&&q[-1]>='0'&&q[-1]<='9'){--q;v+=(unsigned)(*q-'0')*m;if(m>100000000u)return FALSE;m*=10;}if(q==dot||q==n||q[-1]!='_'||!v)return FALSE;if(out)*out=v;return TRUE;}
static Bool _MainLoopSmdInsertDisk(const char*p){if(!_pSega||!_pSega->IsSuperMagicDrive()||!p||!*p)return FALSE;MainLoopStatusPrintf(180,"Inserting SMD disk...");MainLoopRender();MainLoopRender();if(!_pSega->SwapSuperMagicDriveDisk(p)){MainLoopStatusPrintf(240,"SMD disk failed: %s",_pSega->GetSuperMagicDriveError());return FALSE;}MainLoopStateOnRomChanged();MainLoopStatusPrintf(120,"SMD disk inserted: %s",_MainLoopSmdBaseName(p));return TRUE;}
Bool MainLoopSmdCreateNextDisk(void){Char p[1024];struct stat st;if(!_pSega||!_pSega->IsSuperMagicDrive())return FALSE;for(unsigned i=1;i<1000;i++){if(!_MainLoopSmdDiskPath(p,sizeof(p),i))return FALSE;if(stat(p,&st)!=0){MainLoopStatusPrintf(180,"Creating SMD disk %u...",i);MainLoopRender();MainLoopRender();if(!_MainLoopSmdCreateD88(p)){MainLoopStatusPrintf(240,"Could not create SMD D88");return FALSE;}return _MainLoopSmdInsertDisk(p);}}return FALSE;}
Bool MainLoopSmdSwapNextDisk(void){Char p[1024];struct stat st;unsigned cur=0,next;(void)_MainLoopSmdNumberedIndex(_pSega&&_pSega->IsSuperMagicDrive()?_pSega->GetSuperMagicDriveDiskPath():NULL,&cur);if(!_pSega||!_pSega->IsSuperMagicDrive())return FALSE;next=cur?cur+1:1;if(_MainLoopSmdDiskPath(p,sizeof(p),next)&&stat(p,&st)==0&&!S_ISDIR(st.st_mode))return _MainLoopSmdInsertDisk(p);if(_MainLoopSmdDiskPath(p,sizeof(p),1)&&stat(p,&st)==0&&!S_ISDIR(st.st_mode))return _MainLoopSmdInsertDisk(p);MainLoopStatusPrintf(120,"No numbered SMD disk found");return FALSE;}
static Bool _MainLoopSmdInsertCartridge(const char*p){
 Int32 nBytes;size_t req;Emu::Rom::LoadErrorE er;Uint32 crc;if(!_pSega||!_pSegaRom||!_pSega->IsSuperMagicDrive()||!_pSega->IsSuperMagicDriveFirmwareMode()||!p||!*p)return FALSE;nBytes=_MainLoopGetBinarySize(p);if(nBytes<=0)return FALSE;req=PicoDriveBridge_RequiredRomCapacity((size_t)nBytes);if(!req||req>0xffffffffU)return FALSE;
 if(_pSega->HasSuperMagicDriveCartridge()){_pSega->EjectSuperMagicDriveCartridge();_pSegaRom->Unload();_MainLoopFreeRomBuffer();}
 if(!_MainLoopAllocRomBuffer((Uint32)req)){MainLoopStatusPrintf(240,"Not enough memory for SMD cartridge");return FALSE;}if(_MainLoopReadBinaryData(_RomData,(Int32)_RomDataCapacity,p)!=nBytes){_MainLoopFreeRomBuffer();return FALSE;}crc=(Uint32)mz_crc32(MZ_CRC32_INIT,_RomData,(size_t)nBytes);er=_pSegaRom->AttachBuffer(_RomData,(Uint32)nBytes,_RomDataCapacity);if(er!=Emu::Rom::LOADERROR_NONE){_MainLoopFreeRomBuffer();return FALSE;}_pSegaRom->SetSourceName(p);if(!_pSega->InsertSuperMagicDriveCartridge(_pSegaRom)){_pSegaRom->Unload();_MainLoopFreeRomBuffer();MainLoopStatusPrintf(240,"SMD cartridge insertion failed: %s",_pSega->GetSuperMagicDriveError());return FALSE;}snprintf(s_SmdExternalCartPath,sizeof(s_SmdExternalCartPath),"%s",p);MainLoopStateOnRomChanged();MainLoopStatePrimeRomIdentityCRC(crc);
 if(!_pSega->HasSuperMagicDriveDisk()){Char d1[1024];struct stat st;if(_MainLoopSmdDiskPath(d1,sizeof(d1),1)){if(stat(d1,&st)!=0&&!_MainLoopSmdCreateD88(d1)){MainLoopStatusPrintf(210,"SMD cart inserted; could not create disk 1");return TRUE;}(void)_MainLoopSmdInsertDisk(d1);}}
 MainLoopStatusPrintf(180,"SMD cartridge inserted: %s",_MainLoopSmdBaseName(p));return TRUE;}

static Bool _MainLoopDskMagicomPayloadLooksValid(const char *pPath)
{
    FILE *fp;
    long nBytes, nBase;
    Uint8 v[2];

    if (!pPath || !*pPath)
        return FALSE;

    fp = fopen(pPath, "rb");
    if (!fp)
        return FALSE;
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return FALSE;
    }

    nBytes = ftell(fp);
    if (nBytes == 0x2000L) nBase = 0;
    else if (nBytes == 0x2200L) nBase = 0x200L;
    else if (nBytes == 0x8000L) nBase = 0x6000L;
    else
    {
        fclose(fp);
        return FALSE;
    }

    if (fseek(fp, nBase + 0x1FFCL, SEEK_SET) != 0 ||
        fread(v, 1, 2, fp) != 2)
    {
        fclose(fp);
        return FALSE;
    }
    fclose(fp);

    {
        Uint16 rv = (Uint16)v[0] | ((Uint16)v[1] << 8);
        return (rv >= 0xE000 && rv != 0xFFFF) ? TRUE : FALSE;
    }
}

static Bool _MainLoopFindPriorityDskFirmware(
    char *pOut, Int32 nOutBytes, Bool *pMagicom)
{
    static const char *kDskNames[] =
    {
        "DSK.SFC", "DSK.SMC", "DSK.ROM",
        "dsk.sfc", "dsk.smc", "dsk.rom"
    };
    Char Directory[512];

    if (!pOut || nOutBytes <= 0 || !pMagicom)
        return FALSE;

    pOut[0] = 0;
    *pMagicom = FALSE;

    for (Uint32 i = 0;
         i < sizeof(kDskNames) / sizeof(kDskNames[0]); ++i)
    {
        if (!MainLoopFindSystemFileDirectory(
                Directory, (Int32)sizeof(Directory), kDskNames[i]))
            continue;

        int n = snprintf(
            pOut, (size_t)nOutBytes,
            "%s/%s", Directory, kDskNames[i]);
        if (n < 0 || n >= nOutBytes)
        {
            pOut[0] = 0;
            continue;
        }

        if (_MainLoopSwcFirmwareFileLooksValid(pOut))
        {
            *pMagicom = FALSE;
            return TRUE;
        }

        if (_MainLoopDskMagicomPayloadLooksValid(pOut))
        {
            *pMagicom = TRUE;
            return TRUE;
        }

        pOut[0] = 0;
    }

    return FALSE;
}

/* AURORA_SWC_D88_ONLY_V5_20260901 */
static Uint32 _MainLoopD88Get32(const Uint8 *p)
{
    return (Uint32)p[0] |
           ((Uint32)p[1] << 8) |
           ((Uint32)p[2] << 16) |
           ((Uint32)p[3] << 24);
}

static Bool _MainLoopSwcD88FileLooksValid(const char *pPath, long nBytes)
{
    enum { HeaderMin = 0x2A0, HeaderMax = 0x2B0 };
    Uint8 Header[HeaderMin];
    Uint32 diskBytes;
    Uint32 firstTrack = 0;
    FILE *fp;

    if (!pPath || nBytes < HeaderMin)
        return FALSE;

    fp = fopen(pPath, "rb");
    if (!fp)
        return FALSE;

    if (fread(Header, 1, sizeof(Header), fp) != sizeof(Header))
    {
        fclose(fp);
        return FALSE;
    }
    fclose(fp);

    diskBytes = _MainLoopD88Get32(Header + 0x1C);
    /* AURORA_SWC_D88_ONLY_V5_2_20260901:
     * a completed in-BIOS reformat may make the same D88 2DD or 2HD. */
    if (diskBytes != (Uint32)nBytes ||
        (Header[0x1B] != 0x10 && Header[0x1B] != 0x20))
        return FALSE;

    for (Uint32 i = 0; i < 160; ++i)
    {
        Uint32 off = _MainLoopD88Get32(Header + 0x20 + i * 4);
        if (off)
        {
            firstTrack = off;
            break;
        }
    }

    return (firstTrack == HeaderMin || firstTrack == HeaderMax)
        ? TRUE : FALSE;
}

static Bool _MainLoopSwcDiskFileLooksValid(const char *pPath)
{
    struct stat st;
    long n;
    const char *pExt;

    if (!pPath || !*pPath || stat(pPath, &st) != 0 ||
        S_ISDIR(st.st_mode))
        return FALSE;

    pExt = strrchr(pPath, '.');
    if (!pExt || strcasecmp(pExt, ".d88") != 0)
        return FALSE;

    n = (long)st.st_size;
    return _MainLoopSwcD88FileLooksValid(pPath, n);
}

static Bool _MainLoopFindSwcFirmware(char *pOut, Int32 nOutBytes)
{
    static const char *kNames[] =
    {
        /* AURORA_V5_COPIER_LOADER_MEDIA_FLOW_20260831
         * AURORA_SWC_D88_ONLY_V5_20260901: canonical copier BIOS fallback first. */
        "DSK.SFC",
        "DSK.SMC",
        "DSK.ROM",
        "dsk.sfc",
        "dsk.smc",
        "dsk.rom",
        "Super Wild Card V2.8CC 06-08-94 BIOS [!].smc",
        "Super Wild Card V2.8CC 06-08-94 BIOS.smc",
        "Super Wild Card V2.8CC 06-28-94 BIOS [!].smc",
        "Super Wild Card V2.8CC 06-28-94 BIOS.smc"
    };
    Char Directory[512];

    if (!pOut || nOutBytes <= 0)
        return FALSE;
    pOut[0] = 0;

    for (Uint32 i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
    {
        if (MainLoopFindSystemFileDirectory(
                Directory, (Int32)sizeof(Directory), kNames[i]))
        {
            int n = snprintf(pOut, (size_t)nOutBytes,
                             "%s/%s", Directory, kNames[i]);
            if (n >= 0 && n < nOutBytes &&
                _MainLoopSwcFirmwareFileLooksValid(pOut))
                return TRUE;
            pOut[0] = 0;
        }
    }

    if (MainLoopEnsureSystemDirectory(
            Directory, (Int32)sizeof(Directory)))
    {
        DIR *pDir = opendir(Directory);
        if (pDir)
        {
            struct dirent *pEntry;
            while ((pEntry = readdir(pDir)) != NULL)
            {
                if (_MainLoopSwcIsFirmwareName(pEntry->d_name))
                {
                    int n = snprintf(pOut, (size_t)nOutBytes,
                                     "%s/%s", Directory, pEntry->d_name);
                    if (n >= 0 && n < nOutBytes &&
                        _MainLoopSwcFirmwareFileLooksValid(pOut))
                    {
                        closedir(pDir);
                        return TRUE;
                    }
                    pOut[0] = 0;
                }
            }
            closedir(pDir);
        }
    }

    return FALSE;
}


static Bool _MainLoopExecuteSwcFirmware(const char *pFirmwarePath,
                                        const char *pOriginalPath,
                                        Bool bLoadSRAM)
{
    if (!pFirmwarePath || !*pFirmwarePath ||
        !_MainLoopSwcPathIsFirmware(pFirmwarePath))
        return FALSE;

    (void)_MainLoopSwcPrepareCartHeadroom();

    if (!_MainLoopEnsureSnesSystemWithSwcHeadroom())
    {
        _MainLoopSwcReleaseCartReserve();
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for SNES core");
        return FALSE;
    }

    if (!_MainLoopEnsureSwcRasterWithHeadroom())
    {
        _MainLoopSwcReleaseCartReserve();
        return FALSE;
    }

    if (!_MainLoopLoadSuperWildCardWithHeadroom(pFirmwarePath, NULL))
    {
        _MainLoopSwcReleaseCartReserve();
        MainLoopModalPrintf(
            60 * 5, "SWC boot failed: %s",
            _pSnes->GetSuperWildCardError());
        return FALSE;
    }

    _pSystem = _pSnes;
    s_SwcExternalCartPath[0] = 0;
    snprintf(_RomName, sizeof(_RomName), "%s", "Super Wild Card");
    snprintf(
        _RomPath, sizeof(_RomPath), "%s",
        (pOriginalPath && *pOriginalPath) ? pOriginalPath : pFirmwarePath);
    MainLoopStateOnRomChanged();

    _MainLoopSetSampleRate(_pSnes->GetSampleRate());
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    MainLoopStatusPrintf(150, "Super Wild Card: no disk inserted");
    return TRUE;
}

static Bool _MainLoopExecuteMagicomFirmware(
    const char *pFirmwarePath, const char *pOriginalPath, Bool bLoadSRAM)
{
    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914:_MainLoopExecuteMagicomFirmware */
    if (!_MainLoopEnsureSnesSystem())
    {
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for SNES core");
        return FALSE;
    }

    if (!pFirmwarePath || !*pFirmwarePath ||
        !_MainLoopMagicomPathIsFirmware(pFirmwarePath) ||
        !_MainLoopMagicomFirmwareFileLooksValid(pFirmwarePath) || !_pSnes)
        return FALSE;
    if (!MainLoopEnsureGameplayRasterWidth(256)) return FALSE;
    if (!_pSnes->LoadSuperMagicom(pFirmwarePath,NULL))
    {
        MainLoopModalPrintf(60*5,"Magicom boot failed: %s",_pSnes->GetSuperWildCardError());
        return FALSE;
    }
    _pSystem=_pSnes;
    s_SwcExternalCartPath[0]=0;
    snprintf(_RomName,sizeof(_RomName),"%s","Super Magicom");
    snprintf(_RomPath,sizeof(_RomPath),"%s",
        (pOriginalPath && *pOriginalPath) ? pOriginalPath : pFirmwarePath);
    MainLoopStateOnRomChanged();
    _MainLoopSetSampleRate(_pSnes->GetSampleRate());
    if (bLoadSRAM) _MainLoopLoadSRAM();
    MainLoopStatusPrintf(150,"Super Magicom: no disk inserted");
    return TRUE;
}


/* AURORA_COPIER_DSK_PRIORITY_AUTO_DISK1_V10_17_20260901 */
static Bool _MainLoopSwcBuildCartDiskPath(
    char *pOut, Int32 nOutBytes, unsigned nDisk);
/* AURORA_D88_RAM_IO_PERF_V1_5_MULTIDISK_20260901 */
/* AURORA_D88_V1_6_MULTIDISK_1600_20260901 */
static Bool _MainLoopSwcCreateFat12D88(const char *pPath);
static Bool _MainLoopSwcNumberedDiskIndex(
    const char *pPath, unsigned *pIndex);

static Bool _MainLoopSwcInsertCartridge(const char *pPath)
{
    CFileIO romfile;
    Emu::Rom::LoadErrorE eError;
    Uint32 nPayloadBytes;
    if (!_pSnes || !_pSnes->IsSuperWildCard() ||
        !pPath || !*pPath || !_pSnesRom)
        return FALSE;

    /* AURORA_SWC_MEDIA_FLOW_V2_20260902 */
    if (_pSnes->HasSuperWildCardCartridge())
    {
        if (!_pSnes->IsSuperWildCardFirmwareMode())
            return FALSE;

        (void)_MainLoopForceCheckSRAM();
        (void)_MainLoopSaveSRAM(TRUE);
        _MainLoopSwcCartSRAMDetach();
        _pSnes->EjectSuperWildCardCartridge();
        _pSnesRom->Unload();
        s_SwcExternalCartPath[0] = 0;
    }

    /* AURORA_D88_RAM_IO_PERF_V1_1_20260901
     * Two renders cover both GS buffers before synchronous work. */
    /* AURORA_D88_RAM_IO_PERF_V1_5_MULTIDISK_20260901:
     * MainLoopStatusPrintf is the exact blue/left path used by "SRAM saved.". */
    MainLoopStatusPrintf(180, "Inserting cartridge...");
    MainLoopRender();
    MainLoopRender();

    /* AURORA_SWC_HOTCART_HEADROOM_V1_20260914
     * Hot cart insert bypasses _MainLoopUnloadRom(), então o decoder de BGM
     * opcional poderia continuar ocupando heap antes do malloc contíguo da
     * ROM externa. Libere só esse scratch do frontend. DRAM/B-RAM/FDC/D88,
     * cart SRAM e latches de Memory Mode permanecem vivos e intocados.
     */
    BgmReleaseDecoderForGameSwitch();

    if (!romfile.Open(pPath, "rb"))
    {
        MainLoopStatusPrintf(180, "Cannot open cartridge");
        return FALSE;
    }

    nPayloadBytes = _MainLoopSwcCartPayloadBytes(pPath);

    if (nPayloadBytes && nPayloadBytes <= MAINLOOP_SWC_CART_RESERVE_BYTES)
    {
        /* AURORA_V2_TRICKY_ACCURACY_20260915 */
        (void)_MainLoopSwcEnsureCartReserve(nPayloadBytes);
    }
    else if (nPayloadBytes > MAINLOOP_SWC_CART_RESERVE_BYTES)
    {
        _MainLoopSwcReleaseCartReserve();
    }

    /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: inserção hot do Game Pak também é load de ROM. */
    _MainLoopLoadPhaseBegin();
    if (s_pSwcCartReserve &&
        nPayloadBytes &&
        s_uSwcCartReserveBytes >= nPayloadBytes)
    {
        eError = _pSnesRom->LoadRom(
            &romfile,
            s_pSwcCartReserve,
            s_uSwcCartReserveBytes);
    }
    else
    {
        eError = _pSnesRom->LoadRom(&romfile);
    }

    romfile.Close();
    _MainLoopLoadProgressFinish(
        eError == Emu::Rom::LOADERROR_NONE ? TRUE : FALSE);
    SnesRomResetRuntimeCompatForExternalDevice();

    if (eError == Emu::Rom::LOADERROR_NONE &&
        s_pSwcCartReserve &&
        _pSnesRom->GetData() != s_pSwcCartReserve)
        _MainLoopSwcReleaseCartReserve();

    if (eError != Emu::Rom::LOADERROR_NONE)
    {
        /* AURORA_SWC_CART_OOM_REPORT_V3_20260913
         * Do not collapse an allocation failure into "Invalid cartridge".
         * V3 also removes the resident D88 mirror, so the usual cause of this
         * false error releases ~1.69 MiB before the cartridge is parsed. */
        _pSnesRom->Unload();
        /* AURORA_SWC_LAZY_CART_BACKING_V3_20260915 */
        _MainLoopSwcReleaseCartReserve();
        if (eError == Emu::Rom::LOADERROR_OUTOFSPACE)
            MainLoopStatusPrintf(240,
                "Not enough EE memory for Wild Card cartridge");
        else
            MainLoopStatusPrintf(240,
                "Invalid SNES cartridge (load error %d)", (int)eError);
        return FALSE;
    }

    if (!_pSnes->InsertSuperWildCardCartridge(_pSnesRom))
    {
        _pSnesRom->Unload();
        /* AURORA_SWC_LAZY_CART_BACKING_V3_20260915 */
        _MainLoopSwcReleaseCartReserve();
        MainLoopStatusPrintf(180, "Cartridge insertion failed");
        return FALSE;
    }

    snprintf(
        s_SwcExternalCartPath, sizeof(s_SwcExternalCartPath),
        "%s", pPath);

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901 */
    _MainLoopSwcCartSRAMAttach(pPath);
    MainLoopStateOnRomChanged();

    /* AURORA_COPIER_DSK_PRIORITY_AUTO_DISK1_V10_17_20260901
     * If this cartridge has no _1 D88 yet, create the same logical 1.44-MiB
     * D88 media used by L2+Square and hot-insert it immediately. Existing _1
     * media is never overwritten or auto-inserted.
     */
    {
        Char AutoDiskPath[1024];
        struct stat AutoDiskStat;

        if (_MainLoopSwcBuildCartDiskPath(
                AutoDiskPath, sizeof(AutoDiskPath), 1))
        {
            Bool bExists =
                stat(AutoDiskPath, &AutoDiskStat) == 0 &&
                !S_ISDIR(AutoDiskStat.st_mode);

            if (!bExists)
            {
                MainLoopStatusPrintf(180, "Creating disk 1...");
                MainLoopRender();
                MainLoopRender();

                if (!_MainLoopSwcCreateFat12D88(AutoDiskPath))
                {
                    MainLoopStatusPrintf(
                        210,
                        "Cartridge inserted; could not create _1 disk");
                    return TRUE;
                }
            }

            MainLoopStatusPrintf(180, "Inserting disk 1...");
            MainLoopRender();
            MainLoopRender();

            if (!_pSnes->SwapSuperWildCardDisk(AutoDiskPath))
            {
                if (!bExists)
                    remove(AutoDiskPath);
                MainLoopStatusPrintf(
                    210,
                    "Cartridge inserted; _1 disk insert failed");
                return TRUE;
            }

            MainLoopStateOnRomChanged();
            MainLoopStatusPrintf(
                180, "Cartridge + %s inserted",
                _MainLoopSwcBaseName(AutoDiskPath));
            return TRUE;
        }
    }

    MainLoopStatusPrintf(
        180, "SWC cartridge inserted: %s",
        _MainLoopSwcBaseName(pPath));
    return TRUE;
}


static Bool _MainLoopSwcCartStem(char *pOut, Int32 nOutBytes)
{
    const char *pName;
    const char *pExt;
    const char *pSource;
    Bool bDiskStem = FALSE;
    size_t n;

    if (!pOut || nOutBytes <= 1)
        return FALSE;

    pSource = s_SwcExternalCartPath;

    /* AURORA_SWC_BROWSER_DISK_STEM_V1_20260902:
     * With no donor/external cartridge, inherit the currently mounted D88
     * family instead of creating Dummy_N.  GAME_3.d88 therefore continues
     * as GAME_4.d88 (subject to the normal first-free numbered scan). */
    if (!pSource[0])
    {
        pSource = (_pSnes && _pSnes->IsSuperWildCard())
                    ? _pSnes->GetSuperWildCardDiskPath()
                    : NULL;
        if (!pSource || !*pSource)
        {
            int nChars = snprintf(pOut, (size_t)nOutBytes, "%s", "Dummy");
            return nChars >= 0 && nChars < nOutBytes ? TRUE : FALSE;
        }
        bDiskStem = TRUE;
    }

    pName = _MainLoopSwcBaseName(pSource);
    if (!pName || !*pName)
        return FALSE;

    pExt = strrchr(pName, '.');
    n = pExt ? (size_t)(pExt - pName) : strlen(pName);

    if (bDiskStem)
    {
        size_t end = n;
        while (end > 0 && pName[end - 1] >= '0' && pName[end - 1] <= '9')
            --end;
        if (end < n && end > 0 && pName[end - 1] == '_')
            n = end - 1;
    }

    if (!n || n >= (size_t)nOutBytes)
        return FALSE;

    memcpy(pOut, pName, n);
    pOut[n] = 0;

    for (size_t i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char)pOut[i];
        if (c < 0x20 || strchr("\\/:*?\"<>|", c))
            pOut[i] = '_';
    }
    while (n > 0 && (pOut[n - 1] == ' ' || pOut[n - 1] == '.'))
        pOut[--n] = '_';

    return pOut[0] ? TRUE : FALSE;
}

static Bool _MainLoopSwcBuildCartDiskPath(char *pOut,
                                           Int32 nOutBytes,
                                           unsigned nDisk)
{
    Char Directory[512];
    Char Stem[256];
    int n;

    if (!pOut || nOutBytes <= 0 || nDisk == 0 ||
        !_MainLoopSwcCartStem(Stem, sizeof(Stem)) ||
        !MainLoopEnsureSwcDirectory(
            Directory, (Int32)sizeof(Directory)))
        return FALSE;

    /* AURORA_SWC_D88_ONLY_V5_20260901 */
    n = snprintf(
        pOut, (size_t)nOutBytes,
        "%s/%s_%u.d88", Directory, Stem, nDisk);
    return n >= 0 && n < nOutBytes ? TRUE : FALSE;
}

static Bool _MainLoopSwcFindFirstExistingCartDisk(
    char *pOut, Int32 nOutBytes)
{
    Char Directory[512];
    Char Stem[256];
    Char Prefix[300];
    DIR *pDir;
    struct dirent *pEntry;
    unsigned best = 0;
    Char BestName[512] = "";

    if (!pOut || nOutBytes <= 0 ||
        !_MainLoopSwcCartStem(Stem, sizeof(Stem)) ||
        !MainLoopEnsureSwcDirectory(
            Directory, (Int32)sizeof(Directory)))
        return FALSE;

    if (snprintf(Prefix, sizeof(Prefix), "%s_", Stem) >=
        (int)sizeof(Prefix))
        return FALSE;

    pDir = opendir(Directory);
    if (!pDir)
        return FALSE;

    while ((pEntry = readdir(pDir)) != NULL)
    {
        const char *p = pEntry->d_name;
        const char *pDigits;
        unsigned value = 0;
        Bool overflow = FALSE;

        if (strncasecmp(p, Prefix, strlen(Prefix)) != 0)
            continue;

        pDigits = p + strlen(Prefix);
        if (*pDigits < '0' || *pDigits > '9')
            continue;

        p = pDigits;
        while (*p >= '0' && *p <= '9')
        {
            unsigned digit = (unsigned)(*p - '0');

            /* AURORA_SWC_SWAP_INDEX_BOUNDS_V4_4_20260901 */
            if (value > ((~0u) - digit) / 10u)
            {
                overflow = TRUE;
                break;
            }

            value = value * 10u + digit;
            ++p;
        }

        if (overflow || value == 0 ||
            strcasecmp(p, ".d88") != 0)
            continue;

        if (!best || value < best)
        {
            best = value;
            snprintf(BestName, sizeof(BestName), "%s", pEntry->d_name);
        }
    }
    closedir(pDir);

    if (!best)
        return FALSE;

    {
        struct stat st;
        int n = snprintf(
            pOut, (size_t)nOutBytes,
            "%s/%s", Directory, BestName);
        if (n < 0 || n >= nOutBytes ||
            stat(pOut, &st) != 0 || S_ISDIR(st.st_mode))
            return FALSE;
    }
    return TRUE;
}


/* AURORA_SWC_DEFAULT_FLOPPY_1440_V4_1_20260901
 * Standard 1.44 MiB FAT12: 2880 sectors, 18 sectors/track, 9 sectors/FAT. */
/* AURORA_SWC_DEFAULT_FLOPPY_1440_V4_1_20260901
 * AURORA_SWC_D88_ONLY_V5_20260901
 * AURORA_SWC_D88_ONLY_V5_1_20260901
 * The D88 container reserves 20 sector records per physical track while
 * declaring only 18 active sectors. Logical format is standard FAT12 1.44
 * MiB; the same file can later be FORMAT TRACK'ed to 20 SPT / 1.6 MiB. */
/* AURORA_D88_WRITE_HELPERS_COMPILEFIX_20260901
 * D88 stores integer fields little-endian. */
static void _MainLoopD88Put16(Uint8 *p, Uint16 v)
{
    p[0] = (Uint8)(v & 0xFFu);
    p[1] = (Uint8)((v >> 8) & 0xFFu);
}

static void _MainLoopD88Put32(Uint8 *p, Uint32 v)
{
    p[0] = (Uint8)(v & 0xFFu);
    p[1] = (Uint8)((v >> 8) & 0xFFu);
    p[2] = (Uint8)((v >> 16) & 0xFFu);
    p[3] = (Uint8)((v >> 24) & 0xFFu);
}

/* AURORA_SWC_DEFAULT_FLOPPY_1440_V4_1_20260901
 * AURORA_SWC_D88_ONLY_V5_20260901
 * AURORA_SWC_D88_ONLY_V5_1_20260901
 * AURORA_D88_RAM_IO_PERF_V1_1_20260901
 *
 * Build the complete fixed-capacity D88 in EE RAM and emit it with one
 * sequential fwrite. Logical FAT12 remains 1.44 MiB / 18 SPT; each track
 * still reserves physical room for 20 SPT so BIOS FORMAT can reach 1.6 MiB. */
/* AURORA_SWC_DEFAULT_FLOPPY_1440_V4_1_20260901
 * AURORA_SWC_D88_ONLY_V5_20260901
 * AURORA_SWC_D88_ONLY_V5_1_20260901
 * AURORA_D88_RAM_IO_PERF_V1_1_20260901
 * AURORA_D88_V1_6_MULTIDISK_1600_20260901
 *
 * NEW media default is restored to 1.6 MiB:
 *   80 cylinders x 2 heads x 20 sectors x 512 = 1,638,400 logical bytes.
 * FAT12 requires 10 sectors/FAT at this geometry.
 *
 * The D88 container is streamed one physical track at a time. Peak temporary
 * creation RAM is only 20 x (16+512) = 10,560 bytes, important because the
 * currently inserted D88 is already cached in EE RAM. */
static Bool _MainLoopSwcCreateFat12D88(const char *pPath)
{
    enum
    {
        HeaderBytes = 0x2B0,
        Tracks = 80,
        Heads = 2,
        ActiveSectors = 20,
        D88SlotSectors = 20,
        SectorBytes = 512,
        SectorHeaderBytes = 16,
        SlotBytes =
            D88SlotSectors * (SectorHeaderBytes + SectorBytes)
    };

    Uint8 Header[HeaderBytes];
    Uint8 *Track = NULL;
    const Uint32 totalBytes =
        HeaderBytes + (Uint32)Tracks * Heads * SlotBytes;
    FILE *fp = NULL;
    Bool ok = FALSE;

    if (!pPath || !*pPath)
        return FALSE;

    Track = (Uint8 *)malloc(SlotBytes);
    if (!Track)
        return FALSE;

    fp = fopen(pPath, "wb");
    if (!fp)
        goto done;

    memset(Header, 0, sizeof(Header));
    memcpy(Header, "AURORA SWC", 10);
    Header[0x1A] = 0x00;
    Header[0x1B] = 0x20;
    _MainLoopD88Put32(Header + 0x1C, totalBytes);

    for (Uint32 t = 0; t < (Uint32)(Tracks * Heads); ++t)
        _MainLoopD88Put32(
            Header + 0x20 + t * 4,
            HeaderBytes + t * SlotBytes);

    if (fwrite(Header, 1, sizeof(Header), fp) != sizeof(Header))
        goto done;

    for (Uint32 c = 0; c < Tracks; ++c)
    {
        for (Uint32 h = 0; h < Heads; ++h)
        {
            memset(Track, 0, SlotBytes);

            for (Uint32 r = 1; r <= ActiveSectors; ++r)
            {
                Uint32 lba =
                    ((c * Heads + h) * ActiveSectors) + (r - 1);
                Uint32 rec =
                    (r - 1) * (SectorHeaderBytes + SectorBytes);
                Uint8 *sh = Track + rec;
                Uint8 *sector = sh + SectorHeaderBytes;

                sh[0] = (Uint8)c;
                sh[1] = (Uint8)h;
                sh[2] = (Uint8)r;
                sh[3] = 2;
                _MainLoopD88Put16(
                    sh + 4, (Uint16)ActiveSectors);
                sh[6] = 0x00;
                sh[7] = 0x00;
                sh[8] = 0x00;
                sh[13] = 1;
                _MainLoopD88Put16(
                    sh + 14, SectorBytes);

                if (lba == 0)
                {
                    sector[0] = 0xEB;
                    sector[1] = 0x3C;
                    sector[2] = 0x90;
                    memcpy(sector + 3, "AURORASW", 8);

                    sector[11] = 0x00;
                    sector[12] = 0x02;
                    sector[13] = 0x01;
                    sector[14] = 0x01;
                    sector[15] = 0x00;
                    sector[16] = 0x02;
                    sector[17] = 0xE0;
                    sector[18] = 0x00;

                    sector[19] = 0x80;
                    sector[20] = 0x0C;
                    sector[21] = 0xF0;
                    sector[22] = 0x0A;
                    sector[23] = 0x00;
                    sector[24] = 0x14;
                    sector[25] = 0x00;
                    sector[26] = 0x02;
                    sector[27] = 0x00;

                    sector[38] = 0x29;
                    sector[39] = 0x41;
                    sector[40] = 0x55;
                    sector[41] = 0x52;
                    sector[42] = 0x36;
                    memcpy(sector + 43, "NO NAME    ", 11);
                    memcpy(sector + 54, "FAT12   ", 8);
                    sector[510] = 0x55;
                    sector[511] = 0xAA;
                }
                else if (lba == 1 || lba == 11)
                {
                    sector[0] = 0xF0;
                    sector[1] = 0xFF;
                    sector[2] = 0xFF;
                }
            }

            if (fwrite(Track, 1, SlotBytes, fp) != SlotBytes)
                goto done;
        }
    }

    if (fflush(fp) != 0 ||
        ftell(fp) != (long)totalBytes)
        goto done;

    ok = TRUE;

done:
    if (fp)
        fclose(fp);
    if (Track)
        free(Track);

    if (!ok)
    {
        remove(pPath);
        return FALSE;
    }

    if (!_MainLoopSwcD88FileLooksValid(
            pPath, (long)totalBytes))
    {
        remove(pPath);
        return FALSE;
    }

    return TRUE;
}

/* AURORA_V5_COPIER_LOADER_MEDIA_FLOW_20260831: no automatic floppy provisioning. */

Bool MainLoopSwcCreateNextDisk(void)
{
    Char Path[1024];
    struct stat st;
    unsigned nDisk;

    if (!_pSnes || !_pSnes->IsSuperWildCard())
        return FALSE;

    /* AURORA_D88_RAM_IO_PERF_V1_5_MULTIDISK_20260901
     * Manual L2+Square creation is CREATE-ONLY. It must never eject/replace
     * the currently inserted disk. The sole automatic insertion remains the
     * cartridge auto-provision path for a missing _1.d88. */
    for (nDisk = 1; nDisk < 10000; ++nDisk)
    {
        if (!_MainLoopSwcBuildCartDiskPath(Path, sizeof(Path), nDisk))
            return FALSE;

        if (stat(Path, &st) != 0)
            break;
    }

    if (nDisk >= 10000)
    {
        MainLoopStatusPrintf(180, "Too many numbered disks");
        return FALSE;
    }

    MainLoopStatusPrintf(180, "Creating disk %u...", nDisk);
    MainLoopRender();
    MainLoopRender();

    if (!_MainLoopSwcCreateFat12D88(Path))
    {
        MainLoopStatusPrintf(180, "Could not create disk image");
        return FALSE;
    }

    MainLoopStatusPrintf(
        150,
        "Created disk %u: %s",
        nDisk,
        _MainLoopSwcBaseName(Path));
    return TRUE;
}

static Bool _MainLoopSwcInsertDisk(const char *pPath)
{
    const char *pExt;
    unsigned shown = 0;

    if (!_pSnes || !_pSnes->IsSuperWildCard() ||
        !pPath || !*pPath)
        return FALSE;

    pExt = strrchr(pPath, '.');
    if (!pExt || strcasecmp(pExt, ".d88") != 0)
    {
        MainLoopStatusPrintf(
            180, "Unsupported D88 floppy image");
        return FALSE;
    }

    /* AURORA_D88_RAM_IO_PERF_V1_5_MULTIDISK_20260901
     * Do not pre-open/pre-read the D88 here. MountDisk already performs the
     * authoritative full RAM-cache validation, so the old frontend pre-probe
     * was duplicate host I/O on every browser insertion. */
    if (_MainLoopSwcNumberedDiskIndex(pPath, &shown))
        MainLoopStatusPrintf(180, "Inserting disk %u...", shown);
    else
        MainLoopStatusPrintf(
            180,
            "Inserting disk: %s...",
            _MainLoopSwcBaseName(pPath));

    MainLoopRender();
    MainLoopRender();

    if (!_pSnes->SwapSuperWildCardDisk(pPath))
    {
        MainLoopStatusPrintf(
            180, "SWC disk insert failed: %s",
            _pSnes->GetSuperWildCardError());
        return FALSE;
    }

    MainLoopStateOnRomChanged();

    MainLoopStatusPrintf(
        180, "SWC floppy mounted %s: %s",
        _pSnes->IsSuperWildCardDiskWritable() ? "RW" : "READ-ONLY",
        _MainLoopSwcBaseName(pPath));
    return TRUE;
}


/* AURORA_D88_GBA_LIFECYCLE_FIX_V1_20260920
 * Standalone D88 is disk-only at boot. Reserve only the copier's real
 * 4 MiB DRAM here; Game Pak backing remains lazy until a cartridge is
 * actually inserted. This restores the contract documented by the SWC
 * backing layer and avoids parking the 8 MiB DRAM+future-cart arena before
 * the first SnesSystem construction. */
static Bool _MainLoopSwcPrepareDiskOnlyDram()
{
    if (_pSnes && _pSnes->IsSuperWildCard())
        return TRUE;

    s_bSwcDramReserveLoaned = FALSE;
    _MainLoopSwcReleaseCartReserve();
    _MainLoopSwcReleaseDramReserve();
    return _MainLoopSwcEnsureDramReserve();
}

/* A standalone .d88 reaches _MainLoopExecuteSwcDisk only after the normal
 * launcher teardown. If cold boot fails after recreating SnesSystem, undo
 * that partial lifetime here instead of returning with core/DRAM resident. */
static void _MainLoopSwcAbortStandaloneDiskBoot()
{
    _pSystem = NULL;

    if (_pSnes)
    {
        delete _pSnes;
        _pSnes = NULL;
    }

    s_bSwcDramReserveLoaned = FALSE;
    _MainLoopSwcReleaseCartReserve();
    _MainLoopSwcReleaseDramReserve();
    s_SwcExternalCartPath[0] = 0;
}

static Bool _MainLoopExecuteSwcDisk(const char *pMappedPath,
                                    const char *pOriginalPath,
                                    Bool bLoadSRAM)
{
    Char FirmwarePath[1024];

    if (!pMappedPath || !*pMappedPath || !pOriginalPath || !*pOriginalPath)
        return FALSE;

    Bool bMagicom = FALSE;

    /* AURORA_COPIER_DSK_PRIORITY_AUTO_DISK1_V10_17_20260901
     * Canonical DSK.* always wins over every named copier BIOS. */
    if (!_MainLoopFindPriorityDskFirmware(
            FirmwarePath, sizeof(FirmwarePath), &bMagicom))
    {
        if (!_MainLoopFindSwcFirmware(FirmwarePath, sizeof(FirmwarePath)))
        {
            Char Directory[512];
            DIR *d = NULL;
            struct dirent *e;

            if (MainLoopEnsureSystemDirectory(
                    Directory, sizeof(Directory)))
                d = opendir(Directory);

            if (d)
            {
                while ((e = readdir(d)) != NULL)
                {
                    if (_MainLoopMagicomPathIsFirmware(e->d_name))
                    {
                        int n = snprintf(
                            FirmwarePath, sizeof(FirmwarePath),
                            "%s/%s", Directory, e->d_name);
                        if (n > 0 &&
                            n < (int)sizeof(FirmwarePath) &&
                            _MainLoopMagicomFirmwareFileLooksValid(
                                FirmwarePath))
                        {
                            bMagicom = TRUE;
                            break;
                        }
                    }
                }
                closedir(d);
            }

            if (!bMagicom)
            {
                MainLoopModalPrintf(
                    60 * 5,
                    "Classic copier BIOS missing in SYSTEM");
                return FALSE;
            }
        }
    }

    /* AURORA_D88_GBA_LIFECYCLE_FIX_V1_20260920: a bare D88 has no Game Pak yet. */
    if (!bMagicom && !_MainLoopSwcPrepareDiskOnlyDram())
    {
        MainLoopModalPrintf(
            60 * 4, "ERROR: not enough memory for SWC copier DRAM");
        return FALSE;
    }

    if (!(bMagicom
            ? _MainLoopEnsureSnesSystem()
            : _MainLoopEnsureSnesSystemWithSwcHeadroom()))
    {
        _MainLoopSwcAbortStandaloneDiskBoot();
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for SNES core");
        return FALSE;
    }

    if (!(bMagicom
            ? MainLoopEnsureGameplayRasterWidth(256)
            : _MainLoopEnsureSwcRasterWithHeadroom()))
    {
        _MainLoopSwcAbortStandaloneDiskBoot();
        MainLoopModalPrintf(60 * 3, "ERROR: cannot configure SWC SNES raster");
        return FALSE;
    }

    /* _MainLoopEnsureSnesSystemWithSwcHeadroom() and the raster fallback are
     * allowed to surrender parked memory in order to construct/reconfigure
     * the core. Before handing the DRAM pointer to the copier, restore only
     * the 4 MiB physical DRAM requirement -- never the future cart half. */
    if (!bMagicom && !_MainLoopSwcEnsureDramReserve())
    {
        _MainLoopSwcAbortStandaloneDiskBoot();
        MainLoopModalPrintf(
            60 * 4, "ERROR: not enough memory for SWC copier DRAM");
        return FALSE;
    }

    /* AURORA_D88_STANDALONE_REENTRY_FIX_V1_20260916
     * The copier raster is ready now. Render the blocking-load UI in that
     * raster before BIOS+D88 I/O instead of leaving the old browser frame
     * stretched/corrupted on screen. */
    _MainLoopLoadPhaseBegin();
    Bool bD88Booted =
        bMagicom ? _pSnes->LoadSuperMagicom(FirmwarePath, pMappedPath)
                 : _MainLoopLoadSuperWildCardWithHeadroom(
                       FirmwarePath, pMappedPath);
    _MainLoopLoadProgressFinish(bD88Booted);

    if (!bD88Booted)
    {
        Char ErrorText[256];
        snprintf(
            ErrorText, sizeof(ErrorText), "%s",
            _pSnes ? _pSnes->GetSuperWildCardError()
                   : "unknown copier boot error");
        _MainLoopSwcAbortStandaloneDiskBoot();
        MainLoopModalPrintf(
            60 * 5, "SWC boot failed: %s", ErrorText);
        return FALSE;
    }

    _pSystem = _pSnes;
    snprintf(_RomName, sizeof(_RomName), "%s", bMagicom ? "Super Magicom" : "Super Wild Card");
    s_SwcExternalCartPath[0] = 0; /* AURORA_SWC_FLOPPY_V5_20260831 */
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalPath);
    MainLoopStateOnRomChanged();

    _MainLoopSetSampleRate(_pSnes->GetSampleRate());
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    if (_fbTexture[0]) _fbTexture[0]->Clear();
    if (_fbTexture[1]) _fbTexture[1]->Clear();
    if (_fbTexture[0])
        TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));

    ConPrint("Super Wild Card V4: BIOS=%s disk=%s\n",
             FirmwarePath, pMappedPath);
    /* AURORA_SWC_MEDIA_PROBE_V10_2_20260831: cold-boot media confirmation. */
    MainLoopStatusPrintf(
        210, "SWC cold-boot floppy mounted %s: %s",
        _pSnes->IsSuperWildCardDiskWritable() ? "RW" : "READ-ONLY",
        _MainLoopSwcBaseName(pMappedPath));
    return TRUE;
}

/* AURORA_SWC_SWAP_REPORT_REAL_INDEX_V4_2_20260901
 * AURORA_SWC_SWAP_REPORT_STRICT_INDEX_V4_3_20260901
 * AURORA_SWC_SWAP_INDEX_BOUNDS_V4_4_20260901
 * AURORA_SWC_D88_ONLY_V5_20260901
 * Parse basename_<n>.d88 without unsigned wrap. */
static Bool _MainLoopSwcNumberedDiskIndex(
    const char *pPath, unsigned *pIndex)
{
    const Char *pName;
    const Char *pExt;
    const Char *pDigits;
    const Char *p;
    unsigned value = 0;

    if (!pPath || !*pPath || !pIndex)
        return FALSE;

    pName = _MainLoopSwcBaseName(pPath);
    if (!pName || !*pName)
        return FALSE;

    pExt = strrchr(pName, '.');
    if (!pExt || strcasecmp(pExt, ".d88") != 0)
        return FALSE;

    pDigits = pExt;
    while (pDigits > pName &&
           pDigits[-1] >= '0' && pDigits[-1] <= '9')
        --pDigits;

    if (pDigits == pExt || pDigits <= pName || pDigits[-1] != '_')
        return FALSE;

    for (p = pDigits; p < pExt; ++p)
    {
        unsigned digit = (unsigned)(*p - '0');
        if (value > ((~0u) - digit) / 10u)
            return FALSE;
        value = value * 10u + digit;
    }

    if (value == 0)
        return FALSE;

    *pIndex = value;
    return TRUE;
}

Bool MainLoopSwcSwapNextDisk(void)
{
    const Char *pCurrent;
    const Char *pExt;
    const Char *pUnderscore;
    const Char *pDigits;
    Char Prefix[1024];
    Char NextPath[1024];
    struct stat Status;
    unsigned nDisk = 0;
    unsigned nDigits = 0;

    if (!_pSnes || !_pSnes->IsSuperWildCard())
        return FALSE;

    pCurrent = _pSnes->GetSuperWildCardDiskPath();
    if (!pCurrent || !*pCurrent)
    {
        Char FirstPath[1024];
        struct stat FirstStatus;

        if (!_MainLoopSwcFindFirstExistingCartDisk(
                FirstPath, sizeof(FirstPath)) ||
            stat(FirstPath, &FirstStatus) != 0 ||
            S_ISDIR(FirstStatus.st_mode))
        {
            MainLoopStatusPrintf(
                120, "No existing disk to insert");
            return FALSE;
        }

        /* AURORA_SWC_MEGA_V9_20260831: lowest existing numbered image. */
        {
            unsigned shown = 0;
            if (_MainLoopSwcNumberedDiskIndex(FirstPath, &shown))
                MainLoopStatusPrintf(180, "Inserting disk %u...", shown);
            else
                MainLoopStatusPrintf(
                    180,
                    "Inserting disk: %s...",
                    _MainLoopSwcBaseName(FirstPath));
        }
        MainLoopRender();
        MainLoopRender();

        if (!_pSnes->SwapSuperWildCardDisk(FirstPath))
            return FALSE;

        MainLoopStateOnRomChanged();

        {
            unsigned shown;
            if (_MainLoopSwcNumberedDiskIndex(FirstPath, &shown))
                MainLoopStatusPrintf(90, "Disk %u inserted", shown);
            else
                MainLoopStatusPrintf(
                    90, "Floppy inserted: %s",
                    _MainLoopSwcBaseName(FirstPath));
        }
        return TRUE;
    }

    pExt = strrchr(pCurrent, '.');
    if (!pExt || strcasecmp(pExt, ".d88"))
    {
        MainLoopStatusPrintf(120, "Current disk is not .d88");
        return FALSE;
    }

    pUnderscore = pExt;
    while (pUnderscore > pCurrent && pUnderscore[-1] != '_' &&
           pUnderscore[-1] != '/' && pUnderscore[-1] != '\\')
        --pUnderscore;

    if (pUnderscore <= pCurrent || pUnderscore[-1] != '_')
    {
        MainLoopStatusPrintf(150, "Disk name must end in _1.d88, _2.d88, ...");
        return FALSE;
    }

    pDigits = pUnderscore;
    while (pDigits < pExt && *pDigits >= '0' && *pDigits <= '9')
    {
        ++nDigits;
        ++pDigits;
    }
    if (pDigits != pExt || nDigits == 0 ||
        !_MainLoopSwcNumberedDiskIndex(pCurrent, &nDisk))
    {
        MainLoopStatusPrintf(
            150, "Disk name must end in _1.d88, _2.d88, ...");
        return FALSE;
    }

    {
        size_t nPrefix = (size_t)((pUnderscore - 1) - pCurrent);
        if (nPrefix >= sizeof(Prefix))
            return FALSE;
        memcpy(Prefix, pCurrent, nPrefix);
        Prefix[nPrefix] = 0;
    }

    {
        Bool bHaveNext = FALSE;

        /* AURORA_SWC_SWAP_INDEX_BOUNDS_V4_4_20260901
         * Preserve legacy next-disk behavior for every representable index.
         * Only UINT_MAX-equivalent (~0u) skips +1, preventing wrap to _0. */
        if (nDisk != ~0u)
        {
            if (snprintf(NextPath, sizeof(NextPath), "%s_%0*u%s",
                         Prefix, (int)nDigits, nDisk + 1u, pExt) >=
                (int)sizeof(NextPath))
                return FALSE;

            if (stat(NextPath, &Status) == 0 &&
                !S_ISDIR(Status.st_mode))
                bHaveNext = TRUE;
        }

        if (!bHaveNext)
        {
        unsigned nFirst;
        Bool bFoundFirst = FALSE;

        /* AURORA_V7_FRONT_COPIER_MEDIA_CART_RESET_20260831
         * Numbered media is circular. Search from _1 upward using the same
         * prefix/extension; when only _1 exists it is a valid wrap target too. */
        for (nFirst = 1; nFirst < 10000; ++nFirst)
        {
            if (snprintf(NextPath, sizeof(NextPath), "%s_%0*u%s",
                         Prefix, (int)nDigits, nFirst, pExt) >=
                (int)sizeof(NextPath))
                return FALSE;

            if (stat(NextPath, &Status) == 0 &&
                !S_ISDIR(Status.st_mode))
            {
                bFoundFirst = TRUE;
                break;
            }
        }

            if (!bFoundFirst)
            {
                MainLoopStatusPrintf(
                    120, "No numbered floppy image found");
                return FALSE;
            }
        }
    }

    {
        unsigned shown = 0;
        if (_MainLoopSwcNumberedDiskIndex(NextPath, &shown))
            MainLoopStatusPrintf(180, "Inserting disk %u...", shown);
        else
            MainLoopStatusPrintf(
                180,
                "Inserting disk: %s...",
                _MainLoopSwcBaseName(NextPath));
    }
    MainLoopRender();
    MainLoopRender();

    if (!_pSnes->SwapSuperWildCardDisk(NextPath))
    {
        MainLoopStatusPrintf(180, "SWC disk swap failed: %s",
                             _pSnes->GetSuperWildCardError());
        return FALSE;
    }

    MainLoopStateOnRomChanged(); /* AURORA_SWC_MEGA_V9_20260831 */

    {
        unsigned shown;
        if (_MainLoopSwcNumberedDiskIndex(NextPath, &shown))
            MainLoopStatusPrintf(90, "Disk %u inserted", shown);
        else
            MainLoopStatusPrintf(
                90, "Floppy inserted: %s",
                _MainLoopSwcBaseName(NextPath));
    }
    return TRUE;
}


/* AURORA_SGB_RUNTIME_V0_4_20260904 */
static void _MainLoopSgbCompact(const Char *pIn, Char *pOut, Int32 nOut)
{
    Int32 n = 0;
    if (!pOut || nOut <= 0) return;
    while (pIn && *pIn && n + 1 < nOut)
    {
        Char c = *pIn++;
        if (c >= 'a' && c <= 'z') c = (Char)(c - ('a' - 'A'));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) pOut[n++] = c;
    }
    pOut[n] = 0;
}

/* AURORA_SGB_FIRMWARE_TRACE_V0_6_2_20260904 */
static void _MainLoopSgbBootTrace(const Char *pText);

/* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
 * Strict SGB firmware identity.
 *
 * Only basename families sgb.* and sgb2.* are eligible. Descriptive No-Intro
 * names are intentionally ignored. CRC32 is calculated by this frontend so
 * firmware validation does not depend on miniz's CRC ABI. */
static Uint32 _MainLoopSgbCRC32Update(
    Uint32 crc, const Uint8 *pData, Uint32 nBytes)
{
    Uint32 bit;
    while (nBytes--)
    {
        crc ^= *pData++;
        for (bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^
                (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

static Bool _MainLoopSgbFileCRC32(
    const Char *pPath, Uint32 *pCRC, Uint32 *pBytes)
{
    FILE *fp;
    Uint8 buf[4096];
    Uint32 crc = 0xFFFFFFFFU;
    Uint32 total = 0;
    size_t got;

    if (!pPath || !pCRC || !pBytes)
        return FALSE;
    fp = fopen(pPath, "rb");
    if (!fp)
        return FALSE;

    while ((got = fread(buf, 1, sizeof(buf), fp)) != 0)
    {
        if (total > 0x80000U ||
            got > (size_t)(0x80000U - total))
        {
            fclose(fp);
            return FALSE;
        }
        crc = _MainLoopSgbCRC32Update(
            crc, buf, (Uint32)got);
        total += (Uint32)got;
    }
    if (ferror(fp))
    {
        fclose(fp);
        return FALSE;
    }
    fclose(fp);

    *pCRC = crc ^ 0xFFFFFFFFU;
    *pBytes = total;
    return TRUE;
}

static Bool _MainLoopSgbProgramCRCAllowed(
    Bool bSgb2, Uint32 crc, Uint32 bytes)
{
    if (bSgb2)
        return (bytes == 0x80000U &&
                crc == 0xCB176E45U) ? TRUE : FALSE;

    if (bytes != 0x40000U)
        return FALSE;

    return (crc == 0x2E35EDBBU || /* SGB 1.0 */
            crc == 0x27A03C98U || /* SGB Rev 1 */
            crc == 0x8A4A174FU)   /* SGB Rev 2 */
        ? TRUE : FALSE;
}

static Bool _MainLoopSgbBootCRCAllowed(
    Uint32 crc, Uint32 bytes)
{
    return (bytes == 0x100U &&
            (crc == 0xEC8A83B9U ||
             crc == 0x53D0DD63U)) ? TRUE : FALSE;
}

static Bool _MainLoopSgbStemMatch(
    const Char *pName, const Char *pStem)
{
    size_t n;
    if (!pName || !pStem)
        return FALSE;
    n = strlen(pStem);
    return !strncasecmp(pName, pStem, n) &&
           pName[n] == '.' && pName[n + 1] != '\0'
        ? TRUE : FALSE;
}

/* Return 1 = exactly one valid file in this SYSTEM directory,
 * 0 = none, -1 = ambiguous (two or more valid files for the same stem).
 * An invalid matching filename is ignored after a diagnostic CRC print. */
static Int32 _MainLoopScanSgbStemDirectory(
    const Char *pDirectory, const Char *pStem, Int32 kind,
    Char *pChosen, Uint32 nChosen)
{
    DIR *d;
    struct dirent *de;
    Int32 matches = 0;
    Char first[1024];

    if (!pDirectory || !*pDirectory || !pStem || !*pStem ||
        !pChosen || !nChosen)
        return 0;

    first[0] = 0;
    d = opendir(pDirectory);
    if (!d)
        return 0;

    while ((de = readdir(d)) != NULL)
    {
        Char path[1024];
        Uint32 crc, bytes;
        Bool valid;

        if (!_MainLoopSgbStemMatch(de->d_name, pStem))
            continue;
        if (snprintf(path, sizeof(path), "%s/%s",
                     pDirectory, de->d_name) >= (int)sizeof(path))
            continue;
        if (!_MainLoopSgbFileCRC32(path, &crc, &bytes))
            continue;

        if (kind == 1)
            valid = _MainLoopSgbProgramCRCAllowed(FALSE, crc, bytes);
        else if (kind == 2)
            valid = _MainLoopSgbProgramCRCAllowed(TRUE, crc, bytes);
        else if (kind == 4)
            valid = (bytes == 0x900U && crc == 0x41884E46U) ? TRUE : FALSE; /* AURORA_SGB_GAMBATTE_R8_COMPLETE_20260909 */
        else
            valid = _MainLoopSgbBootCRCAllowed(crc, bytes);

        if (!valid)
        {
            ConPrint("Rejected %s: CRC32 %08X, %u bytes\n",
                     path, (unsigned)crc, (unsigned)bytes);
            continue;
        }

        ++matches;
        if (matches == 1)
        {
            snprintf(first, sizeof(first), "%s", path);
            ConPrint("Accepted %s.*: %s (CRC32 %08X)\n",
                     pStem, path, (unsigned)crc);
        }
        else
        {
            ConPrint("Ambiguous %s.* in %s: more than one valid image\n",
                     pStem, pDirectory);
        }
    }

    closedir(d);
    if (matches == 1)
    {
        snprintf(pChosen, nChosen, "%s", first);
        return 1;
    }
    return matches > 1 ? -1 : 0;
}

/* AURORA_HDD_FULL_STORAGE_SAFE_V2_20261013
 * Protect the selected PFS partition for the entire strict firmware lookup
 * while the menu background music thread could otherwise switch mounts. */
class AuroraHddFirmwareIOGuard
{
    Bool m_bActive;
public:
    AuroraHddFirmwareIOGuard(const Char *pSource)
      : m_bActive((pSource && !strncmp(pSource, "hdd0:", 5)) ||
                  !strncmp(_RomPath, "hdd0:", 5))
    {
        if (m_bActive) BgmIOBegin();
    }
    ~AuroraHddFirmwareIOGuard()
    {
        if (m_bActive) BgmIOEnd();
    }
};

/* The supplied ROM path is needed because _RomPath is assigned only AFTER
 * the GBC firmware preflight succeeds. Probe ONLY the ROM's partition,
 * then the known previous/boot partitions; never enumerate arbitrary HDDs. */
static Int32 _MainLoopScanHddSgbStem(
    const Char *pSourcePath, const Char *pStem, Int32 kind,
    Char *pChosen, Uint32 nChosen)
{
    Char root[32];
    Char dir[64];
    if (!MainLoopHddSystemRootForSource(
            pSourcePath, root, sizeof(root)))
        return 0;
    if (snprintf(dir, sizeof(dir), "%s/SYSTEM", root) >=
        (int)sizeof(dir))
        return 0;
    return _MainLoopScanSgbStemDirectory(
        dir, pStem, kind, pChosen, nChosen);
}

static Bool _MainLoopFindStrictSgbStem(
    const Char *pStem, Int32 kind,
    Char *pChosen, Uint32 nChosen,
    const Char *pHddRomSource = NULL)
{
    AuroraHddFirmwareIOGuard hddFirmwareIO(pHddRomSource);
    static const Char *const preferred[] = {
        "mass0:/SNESticle/SYSTEM",
        "mass1:/SNESticle/SYSTEM",
        "mass:/SNESticle/SYSTEM",
        NULL
    };
    static const Char *const fallback[] = {
        "mc0:/SNESticle/SYSTEM",
        "mc1:/SNESticle/SYSTEM",
        "mmce0:/SNESticle/SYSTEM",
        "mmce1:/SNESticle/SYSTEM",
        NULL
    };
    const Char *pActiveRoot;
    Char active[1024];
    Int32 i, result;

    if (!pChosen || !nChosen)
        return FALSE;
    pChosen[0] = 0;

    for (i = 0; preferred[i]; ++i)
    {
        result = _MainLoopScanSgbStemDirectory(
            preferred[i], pStem, kind, pChosen, nChosen);
        if (result > 0) return TRUE;
        if (result < 0) return FALSE;
    }

    if (pHddRomSource && !strncmp(pHddRomSource, "hdd0:", 5))
    {
        result = _MainLoopScanHddSgbStem(
            pHddRomSource, pStem, kind, pChosen, nChosen);
        if (result > 0) return TRUE;
        if (result < 0) return FALSE;
    }

    pActiveRoot = MainLoopSramGetBrowseRoot();
    if (pActiveRoot && *pActiveRoot &&
        snprintf(active, sizeof(active), "%s/SYSTEM",
                 pActiveRoot) < (int)sizeof(active))
    {
        result = _MainLoopScanSgbStemDirectory(
            active, pStem, kind, pChosen, nChosen);
        if (result > 0) return TRUE;
        if (result < 0) return FALSE;
    }

    /* AURORA_HDD_FULL_STORAGE_SAFE_V2_20261013 : do this AFTER the legacy USB + active root checks;
     * no change to existing firmware priorities or CRC requirements. */
    {
        const Char *sources[] = { pHddRomSource, _RomPath,
                                  MainGetBootPath(), NULL };
        for (i = 0; i < 3; ++i)
        {
            if (!sources[i] || !sources[i][0])
                continue;
            result = _MainLoopScanHddSgbStem(
                sources[i], pStem, kind, pChosen, nChosen);
            if (result > 0) return TRUE;
            if (result < 0) return FALSE;
        }
    }

    for (i = 0; fallback[i]; ++i)
    {
        result = _MainLoopScanSgbStemDirectory(
            fallback[i], pStem, kind, pChosen, nChosen);
        if (result > 0) return TRUE;
        if (result < 0) return FALSE;
    }

    return FALSE;
}

static Bool _MainLoopLoadRequiredSgbFirmware(
    const Char *pPath, Bool bSgb2)
{
    CFileIO f;
    Emu::Rom::LoadErrorE e;
    Uint32 crc, bytes;

    if (!pPath || !*pPath || !_pSnesRom ||
        !_MainLoopSgbFileCRC32(pPath, &crc, &bytes) ||
        !_MainLoopSgbProgramCRCAllowed(bSgb2, crc, bytes))
        return FALSE;

    if (!f.Open(pPath, "rb"))
        return FALSE;

    _pSnesRom->Unload();
    e = _pSnesRom->LoadRom(&f);
    f.Close();
    if (e != Emu::Rom::LoadErrorE::LOADERROR_NONE)
    {
        _pSnesRom->Unload();
        SnesRomResetRuntimeCompatForExternalDevice();
        return FALSE;
    }

    /* CRC + strict sgb./sgb2. stem establishes physical SGB identity.
     * Preserve the existing SGB shell path even if a ROM header does not
     * advertise the legacy E3 type exactly as Aurora expects. */
    _pSnesRom->m_Flags |= SNROM_FLAG_GAMEBOY;
    ConPrint("SGB program selected: %s\n",
             bSgb2 ? "SGB2" : "SGB1");
    return TRUE;
}

static Bool _MainLoopFindAndLoadSgbFirmware(
    Bool *pbSgb2, Char *pChosen, Int32 nChosen)
{
    Bool wantSgb2;
    Char selected[1024];
    const Char *stem;
    Int32 kind;

    if (!pbSgb2 || !pChosen || nChosen <= 0)
        return FALSE;
    pChosen[0] = 0;

    wantSgb2 = (VideoGetGameBoyMode() == 2) ? TRUE : FALSE;
    stem = wantSgb2 ? "sgb2" : "sgb";
    kind = wantSgb2 ? 2 : 1;

    /* AURORA_GB_MODE_GBC_SGB1_SGB2_R8_20260909:
     * preflight only the accessory actually selected by the user. */
    if (!_MainLoopFindStrictSgbStem(stem, kind, selected, sizeof(selected)))
        return FALSE;
    if (!_MainLoopLoadRequiredSgbFirmware(selected, wantSgb2))
        return FALSE;

    *pbSgb2 = wantSgb2;
    snprintf(pChosen, nChosen, "%s", selected);
    return TRUE;
}

/* AURORA_SGB_BOOT_TRACE_V0_6_1_20260904
 * Breadcrumbs rendered before synchronous boot operations. If the PS2 wedges,
 * the last visible line identifies the blocking boundary. */
static void _MainLoopSgbBootTrace(const Char *pText)
{
    if (!pText)
        return;

    /* AURORA_SGB_PUBLIC_OSD_V1_3_20260906
     * Public build: SGB breadcrumbs (Hxx, 4x, SBx and related diagnostics)
     * remain in ConPrint only and never replace the normal frontend status.
     * "Loading BIOS and ROM" remains the sole user-facing SGB loading line. */
    ConPrint("%s\n", pText);
}

/* AURORA_NOSGB_TRACE_BRIDGE_V4_1O_20260921
 * SGB is not linked in V4. Keep the compatibility no-op owner in snsgb.cpp
 * and retire this historical mainloop diagnostics bridge to avoid a second
 * strong definition of AuroraSgbBootTrace at final ELF link. */

/* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
 * Mandatory sgb_bios.*. SGB1 and SGB2 SM83 boot ROMs are bit-identical except
 * byte $00FD (01 for SGB1, FF for SGB2). Accept either authentic dump by CRC,
 * then normalize that one byte in RAM to the configured model. */
static Bool _MainLoopLoadGbBios(
    Uint8 *pOut, Uint32 nOutBytes, Bool bSgb2,
    Char *pChosen, Uint32 nChosenBytes)
{
    Char path[1024];
    FILE *fp;
    Uint32 inputCRC, inputBytes;
    Uint32 normalizedCRC, expectedCRC;
    size_t got;

    if (!pOut || nOutBytes < 0x100U)
        return FALSE;
    if (pChosen && nChosenBytes)
        pChosen[0] = 0;

    if (!_MainLoopFindStrictSgbStem(
            "sgb_bios", 3, path, sizeof(path)))
        return FALSE;

    if (!_MainLoopSgbFileCRC32(
            path, &inputCRC, &inputBytes) ||
        !_MainLoopSgbBootCRCAllowed(inputCRC, inputBytes))
        return FALSE;

    fp = fopen(path, "rb");
    if (!fp)
        return FALSE;
    got = fread(pOut, 1, 0x100U, fp);
    fclose(fp);
    if (got != 0x100U)
        return FALSE;

    pOut[0xFD] = bSgb2 ? 0xFFU : 0x01U;

    expectedCRC = bSgb2 ? 0x53D0DD63U : 0xEC8A83B9U;
    normalizedCRC =
        _MainLoopSgbCRC32Update(
            0xFFFFFFFFU, pOut, 0x100U) ^ 0xFFFFFFFFU;
    if (normalizedCRC != expectedCRC)
        return FALSE;

    if (pChosen && nChosenBytes)
        snprintf(pChosen, nChosenBytes, "%s", path);

    ConPrint("SGB SM83 BIOS: %s input=%08X active=%08X (%s)\n",
             path, (unsigned)inputCRC, (unsigned)normalizedCRC,
             bSgb2 ? "SGB2" : "SGB1");
    return TRUE;
}

/* AURORA_GB_CGB_BIOS_PALETTE_R7_20260909
 * Standalone default is a real CGB. CGB-capable games and ordinary DMG games
 * therefore require the authentic 0x900-byte CGB bootstrap. The latter are
 * run in CGB compatibility mode, letting the Nintendo BIOS choose its normal
 * title palette and accept its boot-time palette button combinations. */
static Bool _MainLoopLoadRequiredGbcBios(
    Uint8 *pOut, Uint32 nOutBytes,
    Char *pChosen, Uint32 nChosenBytes,
    const Char *pHddRomSource)
{
    /* Keep the selected PFS partition pinned through CRC AND final fread. */
    AuroraHddFirmwareIOGuard hddReadIO(pHddRomSource);
    static const Char *kStems[] = { "cgb_boot", "gbc_bios", "cgb_bios", NULL };
    Char path[1024];
    FILE *fp;
    Uint32 crc = 0, bytes = 0;
    size_t got;
    Int32 i;

    if (!pOut || nOutBytes < 0x900U)
        return FALSE;
    if (pChosen && nChosenBytes) pChosen[0] = 0;
    path[0] = 0;

    for (i = 0; kStems[i]; ++i)
    {
        if (_MainLoopFindStrictSgbStem(kStems[i], 4, path, sizeof(path),
                                       pHddRomSource))
            break;
        path[0] = 0;
    }
    if (!path[0]) return FALSE;

    if (!_MainLoopSgbFileCRC32(path, &crc, &bytes) ||
        bytes != 0x900U || crc != 0x41884E46U)
        return FALSE;

    fp = fopen(path, "rb");
    if (!fp) return FALSE;
    got = fread(pOut, 1, 0x900U, fp);
    fclose(fp);
    if (got != 0x900U) return FALSE;

    if (pChosen && nChosenBytes)
        snprintf(pChosen, nChosenBytes, "%s", path);
    ConPrint("CGB BIOS: %s CRC32=%08X\n", path, (unsigned)crc);
    return TRUE; /* AURORA_SGB_GAMBATTE_R8_COMPLETE_20260909 */
}

static Bool _MainLoopBootSuperGameBoy(const Uint8 *pGbData, Uint32 nGbBytes,
                                      const Char *pOriginalPath,
                                      Uint32 uGbCRC, Bool bLoadSRAM)
{
    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914:_MainLoopBootSuperGameBoy */
    if (!_MainLoopEnsureSnesSystem())
    {
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for SGB/SNES core");
        return FALSE;
    }

    Bool bSgb2 = FALSE;
    Char firmware[1024];
    Uint8 gbBios[0x100];
    Char gbBiosPath[1024];
    if (!pGbData || nGbBytes < 0x150U || !_pSnes || !_pSnesRom) return FALSE;
    _MainLoopSgbBootTrace("SGB 1/6: firmware search");
    if (!_MainLoopFindAndLoadSgbFirmware(&bSgb2, firmware, sizeof(firmware)))
    {
        MainLoopModalPrintf(60 * 7,
            VideoGetGameBoyMode() == 2
                ? "SGB2 requires valid sgb2.* in SNESticle/SYSTEM."
                : "SGB1 requires valid sgb.* in SNESticle/SYSTEM."); /* AURORA_GB_MODE_GBC_SGB1_SGB2_R8_20260909 */
        return FALSE;
    }

    _MainLoopSgbBootTrace("SGB 2/6: firmware OK");
    gbBiosPath[0] = 0;
    if (!_MainLoopLoadGbBios(
            gbBios, sizeof(gbBios), bSgb2,
            gbBiosPath, sizeof(gbBiosPath)))
    {
        _pSnesRom->Unload();
        MainLoopModalPrintf(60 * 7,
            "SGB requires a valid sgb_bios.* 256-byte SGB boot ROM in SYSTEM.");
        return FALSE;
    }
    _MainLoopSgbBootTrace("SGB 2B: real SM83 BIOS ready");
    _MainLoopSgbBootTrace("SGB 3/6: SNES shell");
    _pSnes->SetSnesRom(_pSnesRom);
    _MainLoopSgbBootTrace("SGB 4/6: GB core attach");
    if (!_pSnes->AttachSuperGameBoyGame(
            pGbData, nGbBytes, bSgb2,
            gbBios, sizeof(gbBios)))
    {
        _pSnes->SetSnesRom(NULL);
        _pSnesRom->Unload();
        MainLoopModalPrintf(60 * 4,
            "ERROR: Could not initialize Game Boy image through Super Game Boy.");
        return FALSE;
    }
    _MainLoopSgbBootTrace("SGB 5/6: SNES reset");
    _pSnes->Reset();
    _MainLoopSgbBootTrace("SGB 6/6: ready");
    _pSystem = _pSnes;
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalPath ? pOriginalPath : "");
    MainLoopStateOnRomChanged();
    /* State identity is resolved lazily from the active GB payload via
       GetSuperGameBoyGameCRC/GetSuperGameBoyGameBytes, never from firmware. */
    (void)uGbCRC;
    ConPrint("SGB Loaded: %s via %s + %s (%s)\n",
             _RomName, firmware, gbBiosPath,
             bSgb2 ? "SGB2" : "SGB1"); /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
    _MainLoopSetSampleRate(_pSnes->GetSampleRate());
    if (bLoadSRAM) _MainLoopLoadSRAM();
    if (_fbTexture[0])
    {
        _fbTexture[0]->Clear();
        TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));
    }
    /* SGB ready: do not let the preboot "Loading BIOS and game file..."
       survive for its original 30-second status timeout. Internal ConPrint
       breadcrumbs remain untouched. */
    _MainLoop_StatusCount = 0;
    _MainLoop_StatusStr[0] = 0;
    return TRUE;
}

/* AURORA_GPSP_GBA_V1_20260911
 * gpSP is path-based on PS2. Keep .gba/.agb on storage and let its existing
 * 1 MiB-block cartridge loader own ROM RAM; never allocate Aurora's generic
 * cartridge buffer as a second copy. */
static Bool _MainLoopExecuteGbaPath(const char *pMappedPath,
                                    const char *pOriginalPath,
                                    Bool bLoadSRAM,
                                    Uint32 uKnownCRC = 0,
                                    Uint32 nKnownBytes = 0)
{
    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914:_MainLoopExecuteGbaPath */
    if (!_MainLoopEnsureGbaSystem())
    {
        MainLoopModalPrintf(60 * 3, "ERROR: not enough memory for GBA core");
        return FALSE;
    }

    Char SystemDirectory[512];

    if (!pMappedPath || !*pMappedPath || !pOriginalPath || !*pOriginalPath || !_pGba)
        return FALSE;

    if (!MainLoopEnsureGameplayRasterWidth(256))
    {
        MainLoopModalPrintf(60 * 3, "ERROR: cannot configure GBA video raster");
        return FALSE;
    }

    if (!MainLoopEnsureSystemDirectory(SystemDirectory, (Int32)sizeof(SystemDirectory)))
    {
        MainLoopModalPrintf(60 * 4, "ERROR: cannot create SNESticle/SYSTEM");
        return FALSE;
    }

    _MainLoop_fOutputIntensity = 1.0f;

    /* AURORA_GBA_STATE_LIFECYCLE_V2_20260916
     * Reserve the full state workspace before gpSP retro_init() calls
     * init_gamepak_buffer(), whose 1 MiB cache allocations run until malloc
     * fails. On a tight heap the ROM LRU therefore becomes one block smaller
     * instead of starving Save/Load later. */
    {
        Int32 nGbaStateBytes = _pGba->GetStateSize();
        if (nGbaStateBytes <= 0 ||
            !MainLoopStateReserveGbaScratch((Uint32)nGbaStateBytes))
        {
            MainLoopModalPrintf(
                60 * 5,
                "ERROR: not enough memory for GBA state workspace");
            return FALSE;
        }
    }

    /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: fase real de inicialização/paging do gpSP. */
    _MainLoopLoadPhaseBegin();
    Bool bGbaLoaded =
        _pGba->LoadGame(
            pMappedPath, SystemDirectory,
            uKnownCRC, nKnownBytes) &&
        _pGba->IsGameLoaded();
    _MainLoopLoadProgressFinish(bGbaLoaded);
    if (!bGbaLoaded)
    {
        _pGba->UnloadGame();
        MainLoopStateReleaseGbaScratch();
        MainLoopModalPrintf(60 * 5,
            "ERROR: gpSP could not run this GBA image"); /* AURORA_GPSP_GBA_V13_SAFE_PERF_20260911: ROMs larger than cache use gpSP paging */
        return FALSE;
    }

    _pSystem = _pGba;
    _MainLoopGetName(_RomName, pOriginalPath);
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalPath);
    MainLoopStateOnRomChanged();
    _MainLoopSetSampleRate(_pGba->GetSampleRate());
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    if (_fbTexture[0]) _fbTexture[0]->Clear();
    if (_fbTexture[1]) _fbTexture[1]->Clear();
    if (_fbTexture[0]) TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));

    ConPrint("GBA Loaded via gpSP: %s [audio=%u Hz, viewport=240x160/native-square, bios=auto+boot, upload=explicit, blend=on, cc=on, safefs=latched-core-skip/resync, input=mask, jit=4M/512K, rtc=system, turboL=R2+L1, turboR=R2+L2, TFA=%s, crc=%08X]\n",
             pMappedPath, (unsigned)_pGba->GetSampleRate(),
             _pGba->HasTurboFileAdvance() ? "on" : "off",
             (unsigned)_pGba->GetGameCRC()); /* AURORA_GPSP_GBA_V13_SAFE_PERF_20260911  | AURORA_GPSP_GBA_V14_VISIBLE_BIOS_SQUARE_20260911 */
    return TRUE;
}


/* AURORA_ROM_LIFETIME_RAMONLY_ZIP_V2_20260913
 * GBA ZIP policy: storage is read-only. The selected member is decompressed
 * only into EE RAM, then synchronously copied into gpSP's existing cartridge
 * buffers. No mass:/, mc0:/, mc1:/, pfs:/ or source-directory temp is ever
 * created. If the two in-RAM representations cannot coexist during load, the
 * launch fails cleanly instead of spilling to storage. */
/* AURORA_ZIP_FILEONLY_MARIO_BIOS_TSUKURU8M_V3_20260913_ZIP_FILEONLY: GBA ZIP -> SYSTEM/UNZIP -> gpSP file pager. */
static Bool _MainLoopExecuteGbaZipMemory(const char *pZipPath,
                                         const char *pOriginalPath,
                                         const char *pMemberName,
                                         unsigned int uZipIndex,
                                         Int32 nExpectedBytes,
                                         Bool bLoadSRAM)
{
    Char tempPath[1024];
    Uint32 uZipCRC = 0; /* AURORA_LOADER_REVIEW_V2_20260915 */

    /* AURORA_GBA_ZIP_LAZY_CORE_FIX_V1_20260915
     * _MainLoopUnloadRom() deliberately destroys inactive core wrappers.
     * A ZIP-backed GBA must therefore not require _pGba before extraction:
     * _MainLoopExecuteGbaPath() recreates gpSP after the file exists. */
    if (!pZipPath || !*pZipPath ||
        !pOriginalPath || !*pOriginalPath ||
        !pMemberName || !*pMemberName ||
        nExpectedBytes <= 0 ||
        (Uint32)nExpectedBytes > MAINLOOP_GBA_ZIP_MAX_BYTES)
    {
        MainLoopModalPrintf(
            60 * 4, "ZIP file too large for decompress");
        return FALSE;
    }

    _MainLoopRemoveGbaZipTemp();

    if (!_MainLoopPrepareZipTempPath(
            tempPath, (Int32)sizeof(tempPath),
            "aurora_gba_zip.gba", pOriginalPath))
    {
        if (!s_ZipExtractBlockedOnMemoryCard)
            MainLoopModalPrintf(
                60 * 4, "ZIP requires writable USB/MX4SIO/HDD");
        return FALSE;
    }

    if (!_MainLoopExtractZipEntryToFile(
            pZipPath, pMemberName, uZipIndex,
            nExpectedBytes, tempPath, &uZipCRC))
    {
        if (!s_ZipExtractBlockedOnMemoryCard)
            MainLoopModalPrintf(
                60 * 4, "ERROR: Cannot extract complete GBA ZIP");
        return FALSE;
    }

    if (snprintf(s_GbaZipTempPath,
                 sizeof(s_GbaZipTempPath), "%s", tempPath) >=
        (int)sizeof(s_GbaZipTempPath))
    {
        fileXioRemove(tempPath);
        s_GbaZipTempPath[0] = 0;
        MainLoopModalPrintf(60 * 4, "ERROR: GBA temp path too long");
        return FALSE;
    }

    /* Load with SRAM deferred: save identity must use the ZIP member name,
     * not the fixed temporary filename. gpSP keeps the file open for ROM
     * paging, so s_GbaZipTempPath is retained until unload. */
    if (!_MainLoopExecuteGbaPath(
            s_GbaZipTempPath, pOriginalPath, FALSE,
            uZipCRC, (Uint32)nExpectedBytes))
    {
        _MainLoopRemoveGbaZipTemp();
        return FALSE;
    }

    _MainLoopGetName(_RomName, pMemberName);
    snprintf(_RomPath, sizeof(_RomPath), "%s", pOriginalPath);
    MainLoopStateOnRomChanged();
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    ConPrint("GBA ZIP file-backed: %s -> %s -> %s (%d bytes)\n",
             pZipPath, pMemberName, s_GbaZipTempPath,
             nExpectedBytes);
    return TRUE;
}


Bool _MainLoopExecuteFile(const char *pFileName, Bool bLoadSRAM)
{
    PathExtTypeE eType, eSourceType;
    Emu::Rom *pRom = NULL;
    Emu::System *pSystem = NULL;
    Emu::Rom *pBios = NULL;
    char FileName[1024], OriginalPath[1024], SegaContentName[1024];
    char hddPath[1024], ZipMemberName[512];
    unsigned int uZipIndex = 0;
    Bool bZipIndexValid = FALSE;
    Bool bSnesOwnedFileLoaded = FALSE; /* AURORA_SNES_OWNED_ZIP_V1_20260914 */
    Bool bSegaCompactLargeBacking = FALSE; /* AURORA_LARGE_MD_COMPACT_V2_20260914 */
    Int32 nExpectedRomBytes = 0, nRomBytes = 0;
    Int32 rasterWidth = 256; /* AURORA_LOAD_BYTE_PROGRESS_V1_20260915 */
    Uint32 uRomIdentityCRC = 0;
#if SNDBG_LOG
    Uint32 uRomCRC = 0;
#endif

    if (!pFileName)
        return FALSE;

    snprintf(OriginalPath, sizeof(OriginalPath), "%s", pFileName);
    AuroraIssue38HddLoadScope hddLoadScope(OriginalPath);
    if (HddMapPath(pFileName, hddPath, sizeof(hddPath)) == 1)
        pFileName = hddPath;

    snprintf(FileName, sizeof(FileName), "%s", pFileName);
    snprintf(SegaContentName, sizeof(SegaContentName), "%s", pFileName);
    ZipMemberName[0] = 0;

    if (!PathExtResolve(FileName, &eType, TRUE))
        return FALSE;

    if (eType == MAINLOOP_ENTRYTYPE_SNESPALETTE)
        return _MainLoopLoadSnesPalette(pFileName);
    if (eType == MAINLOOP_ENTRYTYPE_NESPALETTE)
        return _MainLoopLoadNesPalette(pFileName); /* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827 */

    /* AURORA_SUPER_MAGIC_DRIVE_V1_20260902 */
    if (_pSega && _pSega->IsSuperMagicDrive())
    {
        if (eType == MAINLOOP_ENTRYTYPE_SNESWCDISK)
            return _MainLoopSmdInsertDisk(pFileName);
        if (eType == MAINLOOP_ENTRYTYPE_SEGAROM && !_MainLoopIsSmdBiosPath(pFileName) && _pSega->IsSuperMagicDriveFirmwareMode())
            return _MainLoopSmdInsertCartridge(pFileName);
    }
    if (eType == MAINLOOP_ENTRYTYPE_SEGAROM && _MainLoopIsSmdBiosPath(pFileName) && !_MainLoopSmdBiosLooksValid(pFileName))
    {
        MainLoopModalPrintf(60*5,"mdpbios.bin must be the 8192-byte Super Magic Drive BIOS");
        return FALSE;
    }

    /* AURORA_V6_MAGICOM_FRONT_FAREAST_20260831: identify copier before destructive unload. */
    if (eType == MAINLOOP_ENTRYTYPE_SNESWCBIOS)
    {
        if (_MainLoopMagicomPathIsFirmware(pFileName))
        {
            if (!_MainLoopMagicomFirmwareFileLooksValid(pFileName))
            {
                MainLoopModalPrintf(60*5,
                    "Requires classic Super Magicom V1H/V31 BIOS (V3H soft-upgrade unsupported)");
                return FALSE;
            }
        }
        else if (_MainLoopSwcPathIsFirmware(pFileName))
        {
            if (!_MainLoopSwcFirmwareFileLooksValid(pFileName))
            {
                MainLoopModalPrintf(60*5,"Requires classic 16 KiB Super Wild Card BIOS");
                return FALSE;
            }
        }
        else
        {
            MainLoopModalPrintf(60*4,"Unrecognized classic copier .rom");
            return FALSE;
        }
    }
    else if (eType == MAINLOOP_ENTRYTYPE_SNESROM &&
             _MainLoopMagicomPathIsFirmware(pFileName))
    {
        if (!_MainLoopMagicomFirmwareFileLooksValid(pFileName))
        {
            MainLoopModalPrintf(60*5,
                "Requires classic Super Magicom V1H/V31 BIOS (V3H soft-upgrade unsupported)");
            return FALSE;
        }
    }
    else if (eType == MAINLOOP_ENTRYTYPE_SNESROM &&
             _MainLoopSwcPathIsFirmware(pFileName))
    {
        if (!_MainLoopSwcFirmwareFileLooksValid(pFileName))
        {
            MainLoopModalPrintf(60*5,"Requires classic 16 KiB Super Wild Card BIOS");
            return FALSE;
        }
    }
    else if (eType == MAINLOOP_ENTRYTYPE_SNESWCDISK)
    {
        if (!_MainLoopSwcDiskFileLooksValid(pFileName))
        {
            MainLoopModalPrintf(60*4,"Unsupported or invalid D88 floppy image");
            return FALSE;
        }
        /* AURORA_SWC_D88_ONLY_V5_20260901: bare D88 retains DSK.SFC fallback. */
        if (!(_pSnes && _pSnes->IsSuperWildCard()))
        {
            Char FirmwarePath[1024];
            if (!_MainLoopFindSwcFirmware(FirmwarePath,sizeof(FirmwarePath)))
            {
                MainLoopModalPrintf(60*5,"SWC BIOS missing/invalid in SNESticle/SYSTEM");
                return FALSE;
            }
        }
    }

    /* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830
     * Defensive guard for non-browser launch paths. */
    if (MainLoopSramSaveBusy())
        return FALSE;

    /* AURORA_FINAL_V1_2_STORAGE_CLOSURE_20260901
     * One teardown boundary for both CD cores. Browser launches normally
     * already own this hold through the menu; non-browser callers acquire it
     * here. PCE needs the same protection Sega CD already had before its
     * cdstream objects can be destroyed under an in-flight async read. */
    if (!MainLoopCdUiQuiesce())
    {
        MainLoopStatusPrintf(120, "CD I/O busy; try again.");
        return FALSE;
    }

    /* AURORA_V6_MAGICOM_FRONT_FAREAST_20260831: shared loader/cart/disk workflow. */
    /* AURORA_D88_STANDALONE_REENTRY_FIX_V1_20260916
     * A .d88 selected through _MainLoopExecuteFile is a standalone launch,
     * not a hot insert into a possibly stale copier left resident after
     * L2+R3. Let it fall through _MainLoopUnloadRom() below; then
     * _MainLoopExecuteSwcDisk() recreates the copier and mounts the disk from
     * a clean state. Explicit in-copier swap/create paths remain untouched. */
    if (_pSnes && _pSnes->IsSuperWildCard() &&
        (!_pSnes->HasSuperWildCardCartridge() ||
         _pSnes->IsSuperWildCardFirmwareMode()) &&
        eType == MAINLOOP_ENTRYTYPE_SNESROM &&
        !_MainLoopSwcPathIsFirmware(pFileName) &&
        !_MainLoopMagicomPathIsFirmware(pFileName))
    {
        return _MainLoopSwcInsertCartridge(pFileName);
    }

    _MainLoopUnloadRom();

    /* AURORA_FINAL_V1_2_STORAGE_CLOSURE_20260901
     * Old CD objects are gone now. Release Beetle's pause gate before any new
     * content is loaded; PicoDrive requires no explicit native resume. */
    MainLoopCdUiResume();

#if MAINLOOP_HISTORY
    _MainLoopResetHistory();
#endif
    _MainLoopResetInputChecksums();

    /* AURORA_GPSP_GBA_V1_20260911: direct full-path launch, before generic ROM allocation. */
    if (eType == MAINLOOP_ENTRYTYPE_GBAROM)
        return _MainLoopExecuteGbaPath(pFileName, OriginalPath, bLoadSRAM);

    /* AURORA_SUPER_MAGIC_DRIVE_V1_20260902 */
    if (eType == MAINLOOP_ENTRYTYPE_SEGAROM && _MainLoopIsSmdBiosPath(pFileName))
        return _MainLoopExecuteSmdFirmware(pFileName, OriginalPath, bLoadSRAM);

    /* AURORA_FCEUMM_FDS_V0_5_PATH_BOOT: only a plain .fds full path enters FCEUmm. */
    /* AURORA_V6_MAGICOM_FRONT_FAREAST_20260831: explicit loader ROM selects its hardware model. */
    if ((eType == MAINLOOP_ENTRYTYPE_SNESWCBIOS ||
         eType == MAINLOOP_ENTRYTYPE_SNESROM) &&
        _MainLoopMagicomPathIsFirmware(pFileName))
        return _MainLoopExecuteMagicomFirmware(
            pFileName, OriginalPath, bLoadSRAM);

    if (eType == MAINLOOP_ENTRYTYPE_SNESWCBIOS ||
        (eType == MAINLOOP_ENTRYTYPE_SNESROM &&
         _MainLoopSwcPathIsFirmware(pFileName)))
        return _MainLoopExecuteSwcFirmware(
            pFileName, OriginalPath, bLoadSRAM);

    /* AURORA_SWC_D88_ONLY_V5_20260901: .d88 bypasses cartridge allocation. */
    if (eType == MAINLOOP_ENTRYTYPE_SNESWCDISK)
        return _MainLoopExecuteSwcDisk(pFileName, OriginalPath, bLoadSRAM);

    if (eType == MAINLOOP_ENTRYTYPE_NESFDSDISK)
        return _MainLoopExecuteFdsPath(pFileName, OriginalPath, bLoadSRAM);

    /* AURORA_CD_SRAM_NOTICES_20260824: never size/read a CUE or its tracks as a cartridge. */
    if (eType == MAINLOOP_ENTRYTYPE_CDIMAGE)
        return _MainLoopExecuteDisc(
            pFileName, OriginalPath, FileName, bLoadSRAM);

    eSourceType = eType;

    /* Preflight size/name with no large frontend ROM allocation. */
    if (eSourceType == MAINLOOP_ENTRYTYPE_GZ)
    {
        snprintf(SegaContentName, sizeof(SegaContentName), "%s", FileName);
        if (!PathExtResolve(FileName, &eType, TRUE))
            return FALSE;
        nExpectedRomBytes = MinizGetGZUncompressedSize(pFileName);
    }
    else if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP)
    {
        nExpectedRomBytes = MinizProbeZipFirstMatchInfo(
            pFileName, &uZipIndex,
            ZipMemberName, (int)sizeof(ZipMemberName),
            _MainLoopZipDynamicEntryFilter);

        if (nExpectedRomBytes > 0)
        {
            bZipIndexValid = TRUE;
            snprintf(SegaContentName, sizeof(SegaContentName),
                     "%s", ZipMemberName);
            snprintf(FileName, sizeof(FileName), "%s", ZipMemberName);
            if (!PathExtResolve(FileName, &eType, TRUE))
                return FALSE;
        }
    }
    else
    {
        nExpectedRomBytes = _MainLoopGetBinarySize(pFileName);
    }

    /* AURORA_ROM_LIFETIME_RAMONLY_ZIP_V2_20260913: compressed GBA may only enter through RAM. */
    if (eType == MAINLOOP_ENTRYTYPE_GBAROM)
    {
        if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP && bZipIndexValid)
            return _MainLoopExecuteGbaZipMemory(
                pFileName, OriginalPath, ZipMemberName,
                uZipIndex, nExpectedRomBytes, bLoadSRAM);
        MainLoopModalPrintf(60 * 4,
            "Compressed GBA must be a RAM-only ZIP");
        return FALSE;
    }

    /* AURORA_FDS_V4_ZIP_DISPATCH_20260828 */
    if (eType == MAINLOOP_ENTRYTYPE_NESFDSDISK)
    {
        if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP && bZipIndexValid)
            return _MainLoopExecuteFdsZip(
                pFileName, OriginalPath, ZipMemberName,
                uZipIndex, nExpectedRomBytes, bLoadSRAM);

        MainLoopModalPrintf(60 * 4,
            "FDS in GZ is not supported; use plain .fds or ZIP");
        return FALSE;
    }

    if (nExpectedRomBytes <= 0 ||
        (Uint32)nExpectedRomBytes > _MainLoopRomPayloadLimit(eType))
    {
        MainLoopModalPrintf(60 * 3,
            eSourceType == MAINLOOP_ENTRYTYPE_ZIP
                ? "ZIP file too large for decompress"
                : "ERROR: ROM is too large or unreadable");
        return FALSE;
    }

    switch (eType)
    {
        case MAINLOOP_ENTRYTYPE_NESROM:
            pSystem = _pNes; pRom = _pNesRom; pBios = NULL;
            _MainLoop_fOutputIntensity = 0.8f;
            break;
        case MAINLOOP_ENTRYTYPE_NESFDSDISK:
            pSystem = _pNes; pRom = _pNesFDSDisk; pBios = _pNesFDSBios;
            _MainLoop_fOutputIntensity = 0.8f;
            break;
        case MAINLOOP_ENTRYTYPE_NESFDSBIOS:
            pSystem = _pNes; pRom = NULL; pBios = _pNesFDSBios;
            _MainLoop_fOutputIntensity = 0.8f;
            break;
        case MAINLOOP_ENTRYTYPE_SEGAROM:
            pSystem = _pSega; pRom = _pSegaRom; pBios = NULL;
            _MainLoop_fOutputIntensity = 1.0f;
            break;
        case MAINLOOP_ENTRYTYPE_SNESROM:
            pSystem = _pSnes; pRom = _pSnesRom; pBios = NULL;
            _MainLoop_fOutputIntensity = 1.0f;
            break;
        case MAINLOOP_ENTRYTYPE_GBROM:
            /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909
             * GB/GBC never selects the SNES core here.  SGB1/SGB2 are now
             * standalone Gambatte presentation modes only. */
            pSystem = (Emu::System *)_pGb;
            pRom = NULL; pBios = NULL;
            _MainLoop_fOutputIntensity = 1.0f;
            break;
        case MAINLOOP_ENTRYTYPE_PCEROM:
            pSystem = _pPce; pRom = _pPceRom; pBios = NULL;
            _MainLoop_fOutputIntensity = 1.0f;
            break;
        default:
            return FALSE;
    }

    /* AURORA_SNES_OWNED_ZIP_V1_20260914
     * Plain SNES and ZIP SNES avoid the generic full-ROM staging allocation.
     * ZIP is extracted directly to SYSTEM/UNZIP; CFileIO then forces SnesRom
     * to malloc/read its single resident copy. GZ stays memory-backed below.
     */
    if (eType == MAINLOOP_ENTRYTYPE_SNESROM &&
        (eSourceType == MAINLOOP_ENTRYTYPE_SNESROM ||
         (eSourceType == MAINLOOP_ENTRYTYPE_ZIP && bZipIndexValid)))
    {
        Char tempPath[1024];
        const Char *pOwnedPath = pFileName;
        Bool bTemp = FALSE;
        CFileIO romfile;
        Emu::Rom::LoadErrorE eError;
        Bool bIdentityReady = FALSE; /* AURORA_SNES_SINGLE_IO_IDENTITY_V1_20260915 */

        tempPath[0] = 0;
        if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP)
        {
            if (!_MainLoopPrepareZipTempPath(
                    tempPath, (Int32)sizeof(tempPath),
                    "aurora_snes_zip.sfc", OriginalPath))
            {
                if (!s_ZipExtractBlockedOnMemoryCard)
                    MainLoopModalPrintf(60 * 4,
                        "ZIP requires writable USB/MX4SIO/HDD");
                return FALSE;
            }
            if (!_MainLoopExtractZipEntryToFile(
                    pFileName, ZipMemberName, uZipIndex,
                    nExpectedRomBytes, tempPath))
            {
                if (!s_ZipExtractBlockedOnMemoryCard)
                    MainLoopModalPrintf(60 * 4,
                        "ERROR: cannot extract complete SNES ZIP");
                return FALSE;
            }
            pOwnedPath = tempPath;
            bTemp = TRUE;
        }

        /* AURORA_SNES_SINGLE_IO_IDENTITY_V1_20260915
         * Do not scan the whole file here. SnesRom computes the pristine/raw
         * file CRC from the same bytes it must read for emulation. */

        if (!romfile.Open(pOwnedPath, "rb"))
        {
            if (bTemp) fileXioRemove(tempPath);
            MainLoopModalPrintf(60 * 3, "ERROR: cannot open SNES ROM");
            return FALSE;
        }

        _pSnesRom->Unload();
        /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: SNES direto/ZIP passa por SnesRom fora do loader genérico. */
        _MainLoopLoadPhaseBegin();
        eError = _pSnesRom->LoadRom(&romfile);
        romfile.Close();
        _MainLoopLoadProgressFinish(
            eError == Emu::Rom::LOADERROR_NONE ? TRUE : FALSE);

        if (eError == Emu::Rom::LOADERROR_NONE)
        {
            /* Fast path for every normal/headered SNES image: no second host
             * read.  Preserve the historical raw-file identity exactly. */
            if (_pSnesRom->GetRawFileBytes() == (Uint32)nExpectedRomBytes)
            {
                uRomIdentityCRC = _pSnesRom->GetRawFileCRC32();
                bIdentityReady = TRUE;
            }
            else
            {
                /* Conservative compatibility fallback for malformed/legacy
                 * shapes whose parser intentionally ignores an odd remainder. */
                bIdentityReady = _MainLoopFileCRC32Exact(
                    pOwnedPath, nExpectedRomBytes, &uRomIdentityCRC, FALSE);
            }
        }

        if (bTemp) fileXioRemove(tempPath);

        if (eError != Emu::Rom::LOADERROR_NONE)
        {
            _pSnesRom->Unload();
            if (eError == Emu::Rom::LOADERROR_OUTOFSPACE)
                MainLoopModalPrintf(60 * 4,
                    eSourceType == MAINLOOP_ENTRYTYPE_ZIP
                        ? "ERROR: not enough EE memory for SNES ZIP ROM"
                        : "ERROR: not enough EE memory for SNES ROM");
            else
                MainLoopModalPrintf(60 * 4,
                    "ERROR: cannot load SNES ROM (%d)", (int)eError);
            return FALSE;
        }

        if (!bIdentityReady)
        {
            _pSnesRom->Unload();
            MainLoopModalPrintf(60 * 4,
                "ERROR: cannot verify complete SNES ROM");
            return FALSE;
        }
#if SNDBG_LOG
        uRomCRC = uRomIdentityCRC;
#endif

        nRomBytes = nExpectedRomBytes;
        bSnesOwnedFileLoaded = TRUE;
    }

    /* AURORA_MEMORY_RASTER_REGRESSION_FIX_V3_20260913_ROM_BEFORE_RASTER
     * Memory-regression fix: decide the target raster first, but do NOT
     * rebuild gsKit before the largest EE allocation of this load.
     *
     * After gpSP/GBA enlarged the resident ELF, rebuilding 256->320/512
     * before reserving a 5 MiB+ cartridge could fragment/consume the last
     * sufficiently large contiguous heap block. SSF2 then failed here with
     * "not enough memory for ROM" even after its backing was reduced from
     * 8 MiB to ~5 MiB.
     *
     * Reserve the ROM while the old UI raster is still intact. Only after
     * that succeeds may gsKit change raster. On raster failure the existing
     * abort helper frees the ROM before restoring 256. */
    {
        /* AURORA_PCE_PRECORE512_V13_20260830
         * PCE Fast host surface has a 512-pixel pitch and may expose
         * 256/352/512 visible widths. Enter the core with the GS already on
         * one aligned 512-wide storage raster; later presentation selects only
         * the visible window, never rebuilds to a non-64-aligned width. */
        if (eType == MAINLOOP_ENTRYTYPE_PCEROM)
            rasterWidth = 512;

        if (eType == MAINLOOP_ENTRYTYPE_SEGAROM)
        {
            Bool native320 = FALSE;

            if (_MainLoopSegaExtEquals(SegaContentName, "bin"))
            {
                Uint32 nProbe = (Uint32)nExpectedRomBytes;
                Uint8 *pProbe;
                Int32 got;

                if (nProbe > MAINLOOP_SEGA_PROBE_BYTES)
                    nProbe = MAINLOOP_SEGA_PROBE_BYTES;

                pProbe = (Uint8 *)malloc((size_t)nProbe);
                if (!pProbe)
                {
                    MainLoopModalPrintf(60 * 3,
                        "ERROR: not enough memory for ROM probe");
                    return FALSE;
                }

                if (eSourceType == MAINLOOP_ENTRYTYPE_GZ)
                    got = MinizReadGZPrefix(
                        pFileName, pProbe, (Int32)nProbe);
                else if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP)
                    got = bZipIndexValid
                        ? MinizReadZipEntryPrefix(
                            pFileName, uZipIndex, pProbe, (Int32)nProbe)
                        : -1;
                else
                    got = _MainLoopReadBinaryPrefix(
                        pProbe, (Int32)nProbe, pFileName);

                if (got != (Int32)nProbe)
                {
                    free(pProbe);
                    MainLoopModalPrintf(60 * 3,
                        "ERROR: cannot probe Sega ROM");
                    return FALSE;
                }

                native320 = _MainLoopSegaWantsNative320(
                    pProbe, nExpectedRomBytes, SegaContentName);
                free(pProbe);
            }
            else
            {
                native320 = _MainLoopSegaWantsNative320(
                    NULL, nExpectedRomBytes, SegaContentName);
            }

            if (native320)
                rasterWidth = 320;
        }

        /* AURORA_LARGE_MD_COMPACT_V2_20260914
         * Decide compact backing before reserving the large ROM. The bridge
         * will validate the full payload again before PicoDrive can borrow it.
         */
        if (eType == MAINLOOP_ENTRYTYPE_SEGAROM &&
            nExpectedRomBytes > 0x400000)
        {
            Uint32 nProbe = (Uint32)nExpectedRomBytes;
            Uint8 *pProbe;
            Int32 got;

            if (nProbe > MAINLOOP_SEGA_PROBE_BYTES)
                nProbe = MAINLOOP_SEGA_PROBE_BYTES;
            pProbe = (Uint8 *)malloc((size_t)nProbe);
            if (!pProbe)
            {
                MainLoopModalPrintf(60 * 3,
                    "ERROR: not enough memory for ROM probe");
                return FALSE;
            }

            if (eSourceType == MAINLOOP_ENTRYTYPE_GZ)
                got = MinizReadGZPrefix(
                    pFileName, pProbe, (Int32)nProbe);
            else if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP)
                got = bZipIndexValid
                    ? MinizReadZipEntryPrefix(
                        pFileName, uZipIndex, pProbe, (Int32)nProbe)
                    : -1;
            else
                got = _MainLoopReadBinaryPrefix(
                    pProbe, (Int32)nProbe, pFileName);

            if (got != (Int32)nProbe)
            {
                free(pProbe);
                MainLoopModalPrintf(60 * 3,
                    "ERROR: cannot probe large Mega Drive ROM");
                return FALSE;
            }

            bSegaCompactLargeBacking =
                _MainLoopSegaCanUseCompactLargeBacking(
                    pProbe, nExpectedRomBytes, SegaContentName);
            free(pProbe);

            /* AURORA_LOADER_REVIEW_V2_3_FINAL_OVER_V2_20260915: só diagnóstico do caminho compacto já existente. */
            if (bSegaCompactLargeBacking &&
                (Uint32)nExpectedRomBytes >= 10U * 1024U * 1024U)
            {
                ConPrint(
                    "[PicoDrive] 10MiB+ compact MD: %u -> %u bytes backing\n",
                    (unsigned)nExpectedRomBytes,
                    (unsigned)PicoDriveBridge_CompactMegaDriveCapacity(
                        (size_t)nExpectedRomBytes));
            }
        }

        /* Allocate exact frontend backing policy for this cartridge. */
        {
            /* AURORA_SNES_OWNED_ZIP_V1_20260914: SnesRom already owns direct/ZIP data. */
            if (!bSnesOwnedFileLoaded)
            {
                size_t required;
                if (eType == MAINLOOP_ENTRYTYPE_SEGAROM)
                    required = bSegaCompactLargeBacking
                        ? PicoDriveBridge_CompactMegaDriveCapacity(
                            (size_t)nExpectedRomBytes)
                        : PicoDriveBridge_RequiredRomCapacity(
                            (size_t)nExpectedRomBytes);
                else
                    required = (size_t)nExpectedRomBytes + 1024U;

                if (required < (size_t)nExpectedRomBytes ||
                    required > 0xFFFFFFFFU ||
                    !_MainLoopAllocRomBuffer((Uint32)required))
                {
                    _MainLoopAbortPreCoreLoad();
                    MainLoopModalPrintf(60 * 3,
                        eSourceType == MAINLOOP_ENTRYTYPE_ZIP
                            ? "ERROR: not enough EE memory for ZIP ROM"
                            : "ERROR: not enough memory for ROM");
                    return FALSE;
                }
            }
        }

        /* AURORA_LOAD_BYTE_PROGRESS_V1_20260915
         * Keep the normal 256-wide browser raster through synchronous I/O.
         * Gameplay raster is committed only after the payload is complete. */
    }

    if (!bSnesOwnedFileLoaded)
    {
        if (eSourceType == MAINLOOP_ENTRYTYPE_GZ)
        {
            _MainLoopLoadProgressBegin(nExpectedRomBytes);
            nRomBytes = MinizReadGZToBuffer(
                pFileName, _RomData, nExpectedRomBytes);
            _MainLoopLoadProgressFinish(
                nRomBytes == nExpectedRomBytes ? TRUE : FALSE);
        }
        else if (eSourceType == MAINLOOP_ENTRYTYPE_ZIP)
        {
            char loadedName[512];
            loadedName[0] = 0;

            _MainLoopLoadProgressBegin(nExpectedRomBytes);
            nRomBytes = bZipIndexValid
                ? MinizReadZipEntryToBuffer(
                    pFileName, uZipIndex,
                    _RomData, nExpectedRomBytes,
                    loadedName, (int)sizeof(loadedName))
                : -1;
            _MainLoopLoadProgressFinish(
                nRomBytes == nExpectedRomBytes ? TRUE : FALSE);

            if (nRomBytes > 0 &&
                strcmp(loadedName, ZipMemberName) != 0)
                nRomBytes = -1;
        }
        else
        {
            nRomBytes = _MainLoopReadBinaryDataProgress(
                _RomData, nExpectedRomBytes, pFileName);
        }

    if (nRomBytes != nExpectedRomBytes)
    {
        _MainLoopAbortPreCoreLoad();
        MainLoopModalPrintf(60 * 3, "ERROR: Cannot read complete ROM");
        return FALSE;
    }

    /* Save-state identity: same pristine payload boundary as before. */
    uRomIdentityCRC = (Uint32)mz_crc32(
        MZ_CRC32_INIT, _RomData, (size_t)nRomBytes);
#if SNDBG_LOG
    uRomCRC = (Uint32)mz_crc32(
        MZ_CRC32_INIT, _RomData, (size_t)nRomBytes);
#endif

        {
            Int32 guard = (Int32)(_RomDataCapacity - (Uint32)nRomBytes);
            if (guard > 1024) guard = 1024;
            if (guard > 0)
                memset(_RomData + nRomBytes, 0, (size_t)guard);
        }
    }

    if (bSnesOwnedFileLoaded)
        printf("ROM data read: %s (%d bytes, SnesRom-owned)\n",
               pFileName, nRomBytes);
    else
        printf("ROM data read: %s (%d/%u bytes backing)\n",
               pFileName, nRomBytes, (unsigned)_RomDataCapacity);
    _MainLoopGetName(_RomName, FileName);
    printf("ROMName: '%s'\n", _RomName);

    /* AURORA_LOAD_BYTE_PROGRESS_V1_20260915
     * All blocking ROM I/O is complete while the browser raster is still
     * intact. Switch to the target gameplay raster only now. */
    if (!MainLoopEnsureGameplayRasterWidth(rasterWidth))
    {
        if (bSnesOwnedFileLoaded)
            _pSnesRom->Unload();
        _MainLoopAbortPreCoreLoad();
        MainLoopModalPrintf(
            60 * 3, "ERROR: cannot configure video raster");
        return FALSE;
    }

    /* AURORA_WILDCARD_32MBIT_CORE_LIFECYCLE_V1_20260914
     * Deliberately late: secure/read the large ROM backing before
     * constructing a core, preserving contiguous EE heap. */
    if (!_MainLoopEnsureSystemForType(eType))
    {
        _MainLoopAbortPreCoreLoad();
        MainLoopModalPrintf(60 * 3,
            "ERROR: not enough memory for emulator core");
        return FALSE;
    }

    switch (eType)
    {
        case MAINLOOP_ENTRYTYPE_NESROM:  pSystem = _pNes;  break;
        case MAINLOOP_ENTRYTYPE_SEGAROM: pSystem = _pSega; break;
        case MAINLOOP_ENTRYTYPE_SNESROM: pSystem = _pSnes; break;
        case MAINLOOP_ENTRYTYPE_GBROM:   pSystem = _pGb;   break;
        case MAINLOOP_ENTRYTYPE_PCEROM:  pSystem = _pPce;  break;
        default: break;
    }

    if (eType == MAINLOOP_ENTRYTYPE_GBROM)
    {
        Bool bGbcFile;
        const GambatteSystem::StandaloneModeE eGbMode =
            GambatteSystem::STANDALONE_CGB;
        Uint8 gbcBios[0x900];
        Uint32 gbcBiosBytes = 0;
        Char gbcBiosPath[1024];

        /* AURORA_NO_SGB_ELF_V4_20260921: all .gb/.gbc content stays on standalone Gambatte/CGB.
         * No SGB header flag can route into a SNES/ICD2 or dynamic-SGB host. */
        if (!_RomData || nRomBytes < 0x150)
        {
            _MainLoopFreeRomBuffer();
            _MainLoopUnloadRom();
            MainLoopModalPrintf(60 * 4, "ERROR: invalid Game Boy image");
            return FALSE;
        }

        bGbcFile = _MainLoopGbContentIsGbc(FileName) ? TRUE : FALSE;
        gbcBiosPath[0] = 0;
        if (!_MainLoopLoadRequiredGbcBios(
                gbcBios, sizeof(gbcBios), gbcBiosPath, sizeof(gbcBiosPath),
                OriginalPath))
        {
            _MainLoopFreeRomBuffer();
            _MainLoopUnloadRom();
            MainLoopModalPrintf(60 * 7,
                "GBC requires retail CGB BIOS CRC32 41884E46 in SYSTEM");
            return FALSE;
        }
        gbcBiosBytes = sizeof(gbcBios);

        if (!_pGb || !_pGb->LoadGame(
                _RomData, (Uint32)nRomBytes, uRomIdentityCRC,
                eGbMode, gbcBios, gbcBiosBytes))
        {
            _MainLoopFreeRomBuffer();
            _MainLoopUnloadRom();
            MainLoopModalPrintf(60 * 4,
                "ERROR: Gambatte cannot run this Game Boy image");
            return FALSE;
        }

        _pSystem = _pGb;
        snprintf(_RomPath, sizeof(_RomPath), "%s", OriginalPath);
        MainLoopStateOnRomChanged();
        MainLoopStatePrimeRomIdentityCRC(uRomIdentityCRC);
        _MainLoopSetSampleRate(_pGb->GetSampleRate());
        if (bLoadSRAM) _MainLoopLoadSRAM();

        _fbTexture[0]->Clear();
        TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));
        ConPrint("GB Loaded: %s [standalone: CGB%s%s]\n",
                 pFileName,
                 bGbcFile ? ", .gbc" : "",
                 _pGb->HasTurboFile() ? ", ASCII Turbo File" : "");
        _MainLoopFreeRomBuffer();
        return TRUE;
    }


    if (pBios)
    {
        if (pRom == NULL)
        {
            if (!_MainLoopLoadBios(pBios, pFileName))
            {
                _MainLoopUnloadRom();
                MainLoopModalPrintf(60 * 5,
                    "ERROR: Cannot load disksys.rom");
                return FALSE;
            }
        }
        else if (!pBios->IsLoaded())
        {
            char diskrompath[1024];
            Char *pDiskFileName;

            snprintf(diskrompath, sizeof(diskrompath), "%s", FileName);
            pDiskFileName = strrchr(diskrompath, '/');
            if (!pDiskFileName)
                pDiskFileName = strrchr(diskrompath, ':');
            if (!pDiskFileName)
            {
                _MainLoopUnloadRom();
                return FALSE;
            }

            strcpy(pDiskFileName + 1, "disksys.rom");
            printf("FDSRom: '%s'\n", diskrompath);

            if (!_MainLoopLoadBios(pBios, diskrompath))
            {
                _MainLoopUnloadRom();
                MainLoopModalPrintf(60 * 5,
                    "ERROR: Cannot load disksys.rom");
                return FALSE;
            }
        }
    }

    if (pRom && !bSnesOwnedFileLoaded &&
        !_MainLoopLoadRomData(pRom, _RomData, nRomBytes))
    {
        _MainLoopUnloadRom();
        return FALSE;
    }

    /* AURORA_SNES_OWNED_ZIP_V1_20260914
     * NesRom copies from CMemFileIO. SnesRom may retain ReadPtr(), so a GZ
     * SNES image must keep _RomData until unload. Plain/ZIP SNES use the
     * owned CFileIO path and never allocated frontend staging here. */
    if (pRom == _pNesRom)
        _MainLoopFreeRomBuffer();

    if (pBios)
    {
        pSystem->SetRom(pBios);
        if (pSystem == _pNes)
            _pNes->SetNesDisk(_pNesFDSDisk);
        else
            _pSnes->SetSnesRom(_pSnesRom);
    }
    else
    {
        if (pSystem == _pSega && _pSegaRom)
        {
            _pSegaRom->SetSourceName(SegaContentName);
            PicoDriveBridge_SetRegion((int)g_SnesForceRegion);
        }
        else if (pSystem == _pPce && _pPceRom)
            _pPceRom->SetSourceName(SegaContentName);
        pSystem->SetRom(pRom);
    }

    pSystem->Reset();

    if (pSystem == _pNes && !_pNes->IsRomReady())
    {
        printf("[QuickNES] ROM rejected; aborting launch before mainloop\n");
        _MainLoopUnloadRom();
        MainLoopModalPrintf(60 * 3,
            "ERROR: QuickNES cannot run this NES image (format/mapper/FDS)");
        return FALSE;
    }

    if (pSystem == _pSega && !_pSega->IsRomReady())
    {
        printf("[PicoDrive] ROM rejected; aborting launch before mainloop\n");
        _MainLoopUnloadRom();
        MainLoopModalPrintf(60 * 3,
            "ERROR: PicoDrive cannot run this SEGA image");
        return FALSE;
    }

    if (pSystem == _pPce && !_pPce->IsRomReady())
    {
        printf("[PCE] ROM rejected; aborting launch before mainloop\n");
        _MainLoopUnloadRom();
        MainLoopModalPrintf(60 * 3, "ERROR: Beetle PCE Fast cannot run this HuCard");
        return FALSE;
    }

    _pSystem = pSystem;
    snprintf(_RomPath, sizeof(_RomPath), "%s", OriginalPath);
    MainLoopStateOnRomChanged();
    MainLoopStatePrimeRomIdentityCRC(uRomIdentityCRC);

    ConPrint("ROM Loaded: %s\n", pFileName);

    if (pRom)
    {
        int nRegions, iRegion;
        Char *pRomTitle = pRom->GetRomTitle();
        Char *pRomMapper = pRom->GetMapperName();

        if (pRomMapper && !strcmp(pRomMapper, "<unknown>"))
            MainLoopModalPrintf(60, "WARNING: Unsupported NES Mapper");
        if (pRomTitle)
            printf("Rom Title: %s\n", pRomTitle);

#if SNDBG_LOG
        DLog("[rom] file='%s' bytes=%d crc32=%08X title='%s' mapper='%s'",
             _RomName, (int)nRomBytes, (unsigned)uRomCRC,
             pRomTitle ? pRomTitle : "<none>",
             pRomMapper ? pRomMapper : "<none>");
#endif

        nRegions = pRom->GetNumRomRegions();
        for (iRegion = 0; iRegion < nRegions; ++iRegion)
            printf("%s: %d bytes\n",
                   pRom->GetRomRegionName(iRegion),
                   pRom->GetRomRegionSize(iRegion));
    }

    _MainLoopSetSampleRate(pSystem->GetSampleRate());
    if (bLoadSRAM)
        _MainLoopLoadSRAM();

    _fbTexture[0]->Clear();
    TextureUpload(&_OutTex, _fbTexture[0]->GetLinePtr(0));

    if (eType == MAINLOOP_ENTRYTYPE_NESFDSDISK)
    {
        _MainLoop_iDisk = 0;
        _MainLoop_bDiskInserted = TRUE;
    }
    return TRUE;
}

void _MainLoopSetSampleRate(Uint32 uSampleRate)
{
    _AudMix->SetSampleRate(uSampleRate);
}


/* AURORA_PCE_PRECORE512_V13_20260830 */

/* AURORA_PCE_CDRDAO_TOC_SUPPORT_V4_8_20260830 */

/* AURORA_V4_12_PRIVATE_FILEXIO_CDDA_PCE_TOC2CUE_20260830 */

/* AURORA_V4_13_CDDA_STARVATION_PCE_TRACK_GEOMETRY_20260830 */

/* AURORA_V4_14_FILEXIO_OPENFLAG_PCE_STORED_PREGAPS_20260830 */

/* AURORA_V4_14_1_RESUME_COMPILEFIX_20260830 */

/* AURORA_V4_15_PCE_CDRDAO_AUDIO_ENDIAN_20260830 */

/* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830 */
