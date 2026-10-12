/* AURORA_BENCHMARK_CHAINED_V3_20261010
 * Pure menu-driven batch chaining. Keep the existing per-game menu return.
 * Wait a fixed THREE SECONDS of vsynced UI ticks after menu entry, then
 * switch to the next listed ROM. Never interfere with normal profiler saving.
 * No core-frame counts, no time-out for gameplay, no file checks for TXT.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <libpad.h>

#include "aurora_benchmark.h"
#include "aurora_frontend_profiler.h"
#include "mainloop_shared.h"
#include "mainloop_load.h"
#include "mainloop_state.h"
#include "mainloop_menu.h"
#include "mainloop_ui.h"
#include "mainloop_bgm.h"
#include "gskit_backend.h"
#include "sega/picodrive/picodrive_bridge.h"
extern "C" {
#include "audio.h"
}

#if AURORA_FRONTEND_PROFILER
namespace {
static const char *kListPath = "mass0:/Benchmark/BENCHMARK.LST";
static const char *kLogPath  = "mass0:/Benchmark/BENCHMARK.LOG";
enum { MAX_ENTRIES = 64, MAX_REL = 480, MAX_LINE = 1024 };
static char s_roms[MAX_ENTRIES][MAX_REL];
static int s_count=0, s_next=0, s_current=-1;
static enum { OFF, START_NEXT, RUNNING, MENU_GRACE, FINISHED } s_phase=OFF;
static Emu::System *s_activeSystem=NULL;
static unsigned s_uiTicks=0, s_uiTarget=180, s_failures=0;

/* One menu tick is one vsynced, unskipped host frame. GSK reports
 * rational refresh; NTSC 60000/1001 -> 180 ticks; PAL 50/1 -> 150.
 * Slower UI ticks only extend the margin, never truncate the writer. */
static unsigned ThreeSecondUiTicks()
{
    Uint32 num=60000, den=1001;
    GSK_GetRefreshRate(&num,&den);
    if (!num || !den) return 180;
    const unsigned whole=num/den;
    const unsigned rem=num%den;
    if (whole < 40 || whole > 120 || den>1000000) return 180;
    const unsigned n=whole*3u + (rem*3u+den-1u)/den;
    return n>=120 && n<=360 ? n : 300;
}

static int ParseEntry(const char *line, char *out, size_t cap)
{
    size_t n=0;
    int quote=0;
    bool started=false, escaped=false;
    if (!line || !out || cap < 2) return -1;
    out[0]=0;
    while (*line && isspace((unsigned char)*line)) ++line;
    if (!*line || *line=='#') return 0;
    for (; *line; ++line)
    {
        const char c=*line;
        if (escaped)
        {
            escaped=false;
            if (c=='\r' || c=='\n') return -1;
            if (n+1>=cap) return -1;
            out[n++]=c;
            started=true;
        }
        else if (c=='\\') escaped=true;
        else if (c=='\'' || c=='"')
        {
            if (quote==0) { quote=c; started=true; }
            else if (quote==c) quote=0;
            else { if(n+1>=cap) return -1; out[n++]=c; }
        }
        else if (c=='\r' || c=='\n') break;
        else if (!quote && isspace((unsigned char)c))
        {
            while (isspace((unsigned char)line[1])) ++line;
            if (line[1] && line[1]!='#' && line[1]!='\r' && line[1]!='\n')
                return -1;
            break;
        }
        else
        {
            if (n+1>=cap) return -1;
            out[n++]=c;
            started=true;
        }
    }
    if (!started || !n || quote || escaped) return -1;
    out[n]=0;
    if (out[0]=='/' || strchr(out, ':') || strchr(out, '\\')) return -1;
    /* Reject any dotdot path component and non-ROM extensions. */
    for (const char *p=out; *p; )
    {
        const char *seg=p;
        while (*p && *p!='/') ++p;
        const size_t len=(size_t)(p-seg);
        if (!len || (len==1 && seg[0]=='.') ||
            (len==2 && seg[0]=='.' && seg[1]=='.')) return -1;
        if (*p) ++p;
    }
    const char *ext=strrchr(out,'.');
    if (!ext) return -1;
    static const char * const known[] = {
        ".smc", ".sfc", ".nes", ".fds", ".pce", ".md", ".bin",
        ".sms", ".gg", ".gb", ".gbc", ".gba", ".zip", ".cue", ".chd"
    };
    for (size_t i=0;i<sizeof(known)/sizeof(known[0]);++i)
        if (!strcasecmp(ext,known[i])) return 1;
    return -1;
}

static bool Log(const char *fmt, ...)
{
    FILE *fp=fopen(kLogPath,"ab");
    if (!fp) return false;
    va_list ap;
    va_start(ap,fmt);
    const int result=vfprintf(fp,fmt,ap);
    va_end(ap);
    bool ok=(result>=0 && fflush(fp)==0);
    if (fclose(fp)!=0) ok=false;
    return ok;
}

static void Halt(const char *reason)
{
    s_phase=FINISHED;
    MainLoopStatusPrintf(240,"Benchmark: %s",reason);
}

static void StartNext(void)
{
    if (s_next>=s_count)
    {
        if (!Log("END\tprocessed=%d\tfailures=%u\tprofiler_saves_not_verified=1\n",s_count,s_failures))
            Halt("cannot finalize BENCHMARK.LOG");
        else Halt("batch complete; see BENCHMARK.LOG");
        return;
    }
    if (!_bMenu)
    {
        _MenuEnable(TRUE);
        return;
    }
    if (MainLoopSramSaveBusy() || !MainLoopCdUiReady() ||
        !PicoDriveBridge_PrepareGameSwitch()) return;

    s_current=s_next++;
    char rom[sizeof("mass0:/Benchmark/")+MAX_REL];
    const int used=snprintf(rom,sizeof(rom),"mass0:/Benchmark/%s",s_roms[s_current]);
    if (used<=0 || (size_t)used>=sizeof(rom))
    {
        ++s_failures;
        if (!Log("BAD_PATH\t%s\n",s_roms[s_current])) Halt("cannot log bad path");
        return;
    }
    BgmStop();
    if (Aud_IsInitialized()) Aud_Setvol(0);
    if (!_MainLoopExecuteFile(rom,TRUE))
    {
        if (Aud_IsInitialized()) Aud_Setvol(0x3fff);
        ++s_failures;
        if (!Log("ROM_FAILED\t%s\n",s_roms[s_current])) Halt("cannot log ROM failure");
        return;
    }
    if (!MainLoopBenchmarkLoadStateSlot1())
    {
        if (Aud_IsInitialized()) Aud_Setvol(0x3fff);
        ++s_failures;
        if (!Log("STATE_FAILED\t%s\t%s\n",s_roms[s_current],MainLoopStateGetLastMessage()))
            Halt("cannot log state failure");
        return;
    }
    /* Start at zero AFTER loading the state; never use absolute GetFrame(). */
    if (!AuroraFrontendProfilerBatchBegin())
    {
        Halt("profiler not ready to start");
        return;
    }
    s_activeSystem=_pSystem;
    _MenuEnable(FALSE);
    if (_bMenu || !s_activeSystem)
    {
        Halt("could not resume gameplay");
        return;
    }
    s_phase=RUNNING;
    MainLoopStatusPrintf(90,"Benchmark %d/%d: %s",s_current+1,s_count,s_roms[s_current]);
}
} // namespace

void AuroraBenchmarkBoot(void)
{
    FILE *fp=fopen(kListPath,"rb");
    if (!fp) return; /* Normal startup when no manifest is present. */
    s_count=0; s_next=0; s_current=-1;
    char line[MAX_LINE];
    bool invalid=false;
    while (fgets(line,sizeof(line),fp))
    {
        if (!strchr(line,'\n') && !feof(fp))
        {
            int c;
            while ((c=fgetc(fp))!='\n' && c!=EOF) {}
            invalid=true;
            continue;
        }
        char rel[MAX_REL];
        const int result=ParseEntry(line,rel,sizeof(rel));
        if (result<0) {invalid=true;continue;}
        if (!result) continue;
        if (s_count>=MAX_ENTRIES) {invalid=true;continue;}
        snprintf(s_roms[s_count++],MAX_REL,"%s",rel);
    }
    fclose(fp);
    if (invalid || !s_count)
    {
        MainLoopStatusPrintf(240,"Benchmark: invalid/empty BENCHMARK.LST");
        return;
    }
    FILE *log=fopen(kLogPath,"wb");
    if (!log)
    {
        MainLoopStatusPrintf(240,"Benchmark: cannot create BENCHMARK.LOG");
        return;
    }
    const int wrote=fprintf(log,"AURORA_BENCHMARK_CHAINED_V3_20261010\nmode=menu_3s\tentries=%d\n",s_count);
    bool ok=(wrote>=0 && fflush(log)==0);
    if (fclose(log)!=0) ok=false;
    if (!ok) {MainLoopStatusPrintf(240,"Benchmark: log write failed");return;}
    s_phase=START_NEXT;
}

void AuroraBenchmarkTick(void)
{
    switch (s_phase)
    {
        case OFF: case FINISHED: return;
        case START_NEXT: StartNext(); return;
        case RUNNING:
        {
            /* The stable existing game/menu transition owns the endpoint.
             * A UI menu is NOT an error and must NOT abort this batch. */
            if (!_bMenu) return;
            s_uiTicks=0;
            s_uiTarget=ThreeSecondUiTicks();
            s_phase=MENU_GRACE;
            /* Log is independent of profiler dump; no TXT is touched. */
            if (!Log("MENU\t%s\tui_wait_ticks=%u\n",
                     s_roms[s_current],s_uiTarget))
                Halt("cannot log menu return");
            return;
        }
        case MENU_GRACE:
        {
            if (!_bMenu)
            {
                Halt("menu closed during three-second save margin");
                return;
            }
            if (++s_uiTicks < s_uiTarget) return;
            if (!Log("ADVANCE_AFTER_3S\t%s\tprofiler_save_not_checked=1\n",
                     s_roms[s_current]))
            {
                Halt("cannot log advance");
                return;
            }
            s_phase=START_NEXT;
            return;
        }
    }
}
#endif /* AURORA_FRONTEND_PROFILER */
