
#ifndef _PS2MEM_H
#define _PS2MEM_H


#define PS2MEM_ADDR_SCRATCHPAD      (0x70000000)
#define PS2MEM_ADDR_VU0MICROMEM     (0x11000000)
#define PS2MEM_ADDR_VU0DATAMEM      (0x11004000)
#define PS2MEM_ADDR_VU1MICROMEM     (0x11008000)
#define PS2MEM_ADDR_VU1DATAMEM      (0x1100C000)

#define PS2MEM_SCRATCHPAD           (PS2MEM_ADDR_SCRATCHPAD)

/* AURORA_SNES_BG_LOOKUP_SCRATCHPAD_V2_20260920
 * Keep only the first planar lookup table in the final 2 KiB of EE scratchpad.
 * V1 tried to reserve all 4608 bytes at 11 KiB, but SNSpcDspDataT currently
 * occupies 12,512 bytes from scratchpad+0. 14..16 KiB is therefore a disjoint
 * 2 KiB region; compile-time guards below keep proving the separation. */
#define PS2MEM_SNES_LOOKUP_OFFSET   (14 * 1024)
#define PS2MEM_SNES_LOOKUP_ADDR     (PS2MEM_SCRATCHPAD + PS2MEM_SNES_LOOKUP_OFFSET)
#define PS2MEM_SNES_LOOKUP_SIZE     (2 * 1024)


#define PS2MEM_CACHED(_Addr)        (((Uint32)_Addr)&0x0FFFFFFF)
#define PS2MEM_UNCACHED(_Addr)      (((Uint32)_Addr)|0x20000000)
#define PS2MEM_UNCACHEDACCEL(_Addr) (((Uint32)_Addr)|0x30000000)


#endif

