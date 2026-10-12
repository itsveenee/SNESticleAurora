#!/usr/bin/env python3
from __future__ import annotations
import argparse
from pathlib import Path
MARK='AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_ACK_POLL_CORE'
OLD='AURORA_PS2_THREE_BUG_FIX_V1_20260912_PCE_SOFT_PAUSE_CORE_API'
def die(s): raise SystemExit('ERROR: '+s)
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--source',required=True); a=ap.parse_args()
    p=Path(a.source).resolve()/'mednafen/cdstream.c'
    if not p.is_file(): die(f'missing Beetle source: {p}')
    s=p.read_text(encoding='utf-8')
    if MARK in s:
        print('[PCE prepare v2] pause-ready API already staged'); return
    if OLD not in s: die('V1 soft-pause API is not staged before V2')
    old='''int PCE_AuroraCdAsyncPause(unsigned int timeout_ms)\n{\n   return PCE_AuroraCdAsyncPauseImpl(timeout_ms, 0);\n}\n\n/* Hard barrier kept for game unload/content swap. */\n'''
    new='''int PCE_AuroraCdAsyncPause(unsigned int timeout_ms)\n{\n   return PCE_AuroraCdAsyncPauseImpl(timeout_ms, 0);\n}\n\n/* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_ACK_POLL_CORE\n * Poll-only completion test: no wait and no storage access. */\nint PCE_AuroraCdAsyncPauseReady(void)\n{\n   uint32_t request, ack;\n   if (s_AuroraPceAsyncThreadId < 0 || s_AuroraPceAsyncSema < 0) return 1;\n   AURORA_PCE_EE_SYNC();\n   request = s_AuroraPceAsyncPauseRequest;\n   ack = s_AuroraPceAsyncPauseAck;\n   return request != 0 && ack == request;\n}\n\n/* Hard barrier kept for game unload/content swap. */\n'''
    if s.count(old)!=1: die(f'pause-ready anchor count={s.count(old)}')
    s=s.replace(old,new,1); p.write_text(s,encoding='utf-8',newline='\n')
    print('[PCE prepare v2] patched mednafen/cdstream.c')
if __name__=='__main__': main()
