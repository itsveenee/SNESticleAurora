#!/usr/bin/env python3
from __future__ import annotations
import argparse
from pathlib import Path
MARK='AURORA_SSF2_PCE_MENU_FIX_V2_20260913_SSF2_CORE'
def die(s): raise SystemExit('ERROR: '+s)
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--source',required=True); a=ap.parse_args()
    p=Path(a.source).resolve()/'pico/cart.c'
    if not p.is_file(): die(f'missing PicoDrive source: {p}')
    s=p.read_text(encoding='utf-8')
    if MARK in s:
        print('[PicoDrive prepare] compact SSF2 backing already staged'); return
    old='''  return alloc_size;\n}\n\nvoid *PicoCartAlloc(int filesize, int is_sms)\n'''
    new='''  return alloc_size;\n}\n\n/* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_SSF2_CORE\n * Exact PS2 borrowed-buffer exception for the real 5 MiB SSF2 cart.\n * carthw.cfg identifies it by this same title at 0x150; its mapper selects\n * only existing 512 KiB banks, so generic 5 MiB -> 8 MiB pow2 backing is\n * unnecessary. 0x40 tail bytes cover PicoCartInsert's 4-byte guard. */\nstatic int PicoCartPs2CompactSsf2(const unsigned char *rom,\n  unsigned int romsize, int is_sms)\n{\n  static const char title[] =\n    "SUPER STREET FIGHTER2 The New Challengers";\n  if (is_sms || rom == NULL || romsize != 0x500000U) return 0;\n  return memcmp(rom + 0x150, title, sizeof(title) - 1) == 0;\n}\n\nvoid *PicoCartAlloc(int filesize, int is_sms)\n'''
    if s.count(old)!=1: die(f'SSF2 helper anchor count={s.count(old)}')
    s=s.replace(old,new,1)
    old='''  borrowed_alloc_size = PicoCartCalcAllocSize(size, is_sms);\n  ps2_rom_borrowed = 0;\n'''
    new='''  /* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_SSF2_CORE */\n  borrowed_alloc_size = PicoCartPs2CompactSsf2(rom, romsize, is_sms)\n    ? 0x500040 : PicoCartCalcAllocSize(size, is_sms);\n  ps2_rom_borrowed = 0;\n'''
    if s.count(old)!=1: die(f'SSF2 borrow anchor count={s.count(old)}')
    s=s.replace(old,new,1); p.write_text(s,encoding='utf-8',newline='\n')
    print('[PicoDrive prepare] patched pico/cart.c')
if __name__=='__main__': main()
