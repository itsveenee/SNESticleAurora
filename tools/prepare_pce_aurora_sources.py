#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

MARK = "AURORA_PS2_THREE_BUG_FIX_V1_20260912_PCE_SOFT_PAUSE_CORE"
STATE_MARK = MARK + "_STATE"
WORKER_MARK = MARK + "_WORKER"
API_MARK = MARK + "_API"


def die(msg: str) -> None:
    raise SystemExit(f"ERROR: {msg}")


def replace_exact(text: str, old: str, new: str, what: str) -> str:
    if new in text:
        return text
    n = text.count(old)
    if n != 1:
        die(f"{what}: expected exactly one anchor, found {n}")
    return text.replace(old, new, 1)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Stage Aurora's PCE-CD menu soft-pause into Beetle PCE Fast"
    )
    ap.add_argument("--source", required=True, help="beetle-pce-fast checkout")
    args = ap.parse_args()

    root = Path(args.source).resolve()
    path = root / "mednafen/cdstream.c"
    if not path.is_file():
        die(f"missing Beetle source: {path}")

    s = path.read_text(encoding="utf-8")

    have = [STATE_MARK in s, WORKER_MARK in s, API_MARK in s]
    if any(have) and not all(have):
        die("partial PCE soft-pause patch detected; refusing to guess")
    if all(have):
        print("[PCE prepare] soft menu pause already staged")
        return

    old_state = """/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901
 * Request != 0 gates new async work; Ack is written by the worker only
 * after an in-flight read has returned and its private RFILE is closed. */
static volatile uint32_t s_AuroraPceAsyncPauseRequest;
static volatile uint32_t s_AuroraPceAsyncPauseAck;
static uint32_t s_AuroraPceAsyncPauseCounter;
"""
    new_state = """/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901
 * Request != 0 gates new async work.  Aurora now distinguishes a menu pause
 * from teardown: menu pause waits for the current read and preserves the
 * private RFILE/cache; hard quiesce closes it before content destruction.
 * %s */
static volatile uint32_t s_AuroraPceAsyncPauseRequest;
static volatile uint32_t s_AuroraPceAsyncPauseAck;
static volatile int s_AuroraPceAsyncPauseCloseFile;
static uint32_t s_AuroraPceAsyncPauseCounter;
""" % STATE_MARK
    s = replace_exact(s, old_state, new_state, "PCE async pause state")

    old_worker = """      /* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901 */
      if (s_AuroraPceAsyncPauseRequest != 0)
      {
         uint32_t pause_token = s_AuroraPceAsyncPauseRequest;

         if (worker_file)
            filestream_close(worker_file);
         worker_file = NULL;
         open_path[0] = 0;
         active_path[0] = 0;
         active_seq = 0;
         worker_pos = 0;

         AURORA_PCE_EE_SYNC();
         s_AuroraPceAsyncPauseAck = pause_token;
         AURORA_PCE_EE_SYNC();
         continue;
      }
"""
    new_worker = """      /* %s
       * A menu pause is deliberately not a teardown.  Once any already
       * executing 8 KiB read returns, acknowledge without closing/reopening
       * the VFS handle and without discarding the ready window.  Hard
       * quiesce retains the old close/reset behaviour. */
      if (s_AuroraPceAsyncPauseRequest != 0)
      {
         uint32_t pause_token = s_AuroraPceAsyncPauseRequest;

         if (s_AuroraPceAsyncPauseCloseFile)
         {
            if (worker_file)
               filestream_close(worker_file);
            worker_file = NULL;
            open_path[0] = 0;
            active_path[0] = 0;
            active_seq = 0;
            worker_pos = 0;
         }

         AURORA_PCE_EE_SYNC();
         s_AuroraPceAsyncPauseAck = pause_token;
         AURORA_PCE_EE_SYNC();
         continue;
      }
""" % WORKER_MARK
    s = replace_exact(s, old_worker, new_worker, "PCE async worker pause branch")

    old_api = """/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901
 * Stop async CDDA filesystem traffic and wait for worker acknowledgement.
 * Logical cdstream positions are not rewound. */
int PCE_AuroraCdAsyncQuiesce(unsigned int timeout_ms)
{
   uint32_t token;
   unsigned int waited = 0;

   if (s_AuroraPceAsyncThreadId < 0 || s_AuroraPceAsyncSema < 0)
      return 1;

   token = ++s_AuroraPceAsyncPauseCounter;
   if (token == 0)
      token = ++s_AuroraPceAsyncPauseCounter;

   s_AuroraPceAsyncBufSerial = 0;
   s_AuroraPceAsyncBufReady = 0;
   s_AuroraPceAsyncReqSerial = 0;
   s_AuroraPceAsyncReqPath[0] = 0;
   s_AuroraPceAsyncPauseAck = 0;
   s_AuroraPceAsyncPauseRequest = token;
   AURORA_PCE_EE_SYNC();

   ++s_AuroraPceAsyncReqSeq;
   PCE_AuroraCdAsyncSignal();

   for (;;)
   {
      AURORA_PCE_EE_SYNC();
      if (s_AuroraPceAsyncPauseAck == token)
         return 1;
      if (waited >= timeout_ms)
         break;
      DelayThread(1000);
      ++waited;
   }

   AURORA_PCE_EE_SYNC();
   return s_AuroraPceAsyncPauseAck == token ? 1 : 0;
}

void PCE_AuroraCdAsyncResume(void)
{
   s_AuroraPceAsyncPauseRequest = 0;
   s_AuroraPceAsyncPauseAck = 0;
   AURORA_PCE_EE_SYNC();
}
"""
    new_api = """/* %s
 * Two barriers share the same token/ack path:
 *   close_file=0: UI/menu pause. Preserve RFILE and the ready prefetch window.
 *   close_file=1: teardown. Cancel staged work and close the private RFILE.
 *
 * In both cases ACK is produced only by the worker, after any in-flight read
 * has returned, so the caller never races an active filesystem operation. */
static int PCE_AuroraCdAsyncPauseImpl(unsigned int timeout_ms, int close_file)
{
   uint32_t token;
   unsigned int waited = 0;

   if (s_AuroraPceAsyncThreadId < 0 || s_AuroraPceAsyncSema < 0)
      return 1;

   token = ++s_AuroraPceAsyncPauseCounter;
   if (token == 0)
      token = ++s_AuroraPceAsyncPauseCounter;

   if (close_file)
   {
      /* Preserve the pre-existing hard-quiesce semantics for unload/swap. */
      s_AuroraPceAsyncBufSerial = 0;
      s_AuroraPceAsyncBufReady = 0;
      s_AuroraPceAsyncReqSerial = 0;
      s_AuroraPceAsyncReqPath[0] = 0;
      ++s_AuroraPceAsyncReqSeq;
   }

   s_AuroraPceAsyncPauseAck = 0;
   s_AuroraPceAsyncPauseCloseFile = close_file ? 1 : 0;
   s_AuroraPceAsyncPauseRequest = token;
   AURORA_PCE_EE_SYNC();
   PCE_AuroraCdAsyncSignal();

   for (;;)
   {
      AURORA_PCE_EE_SYNC();
      if (s_AuroraPceAsyncPauseAck == token)
         return 1;
      if (waited >= timeout_ms)
         break;
      DelayThread(1000);
      ++waited;
   }

   AURORA_PCE_EE_SYNC();
   return s_AuroraPceAsyncPauseAck == token ? 1 : 0;
}

int PCE_AuroraCdAsyncPause(unsigned int timeout_ms)
{
   return PCE_AuroraCdAsyncPauseImpl(timeout_ms, 0);
}

/* Hard barrier kept for game unload/content swap. */
int PCE_AuroraCdAsyncQuiesce(unsigned int timeout_ms)
{
   return PCE_AuroraCdAsyncPauseImpl(timeout_ms, 1);
}

void PCE_AuroraCdAsyncResume(void)
{
   s_AuroraPceAsyncPauseRequest = 0;
   s_AuroraPceAsyncPauseAck = 0;
   s_AuroraPceAsyncPauseCloseFile = 0;
   AURORA_PCE_EE_SYNC();
}
""" % API_MARK
    s = replace_exact(s, old_api, new_api, "PCE async pause API")

    path.write_text(s, encoding="utf-8", newline="\n")
    print(f"[PCE prepare] patched {path.relative_to(root)}")


if __name__ == "__main__":
    main()
