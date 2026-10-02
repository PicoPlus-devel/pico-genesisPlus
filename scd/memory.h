/*
scd/memory.h — stands in for PicoDrive's pico/memory.h for the vendored Sega
CD sources. Only the map-building API they call is kept; it is implemented
in port/scd.c on top of the two page maps this port uses:

  is_sub = 1  the sub 68000's read/write maps (port/scd_s68k_mem.h)
  is_sub = 0  the main 68000: reads through the gwenesis ROM page table
              (gw_rom, 128 KB slots, so FETCH*ROM stays the fast path),
              writes through a small CD write map consulted from the bus

PicoDrive maps in 64 KB units; this port uses 128 KB, which every range the
CD code maps below $FE0000 already is. The sub CPU's $FE0000 (backup RAM)
and $FF0000 (PCM, gate array) share one slot and one dispatching handler,
see PicoMemSetupCD() in scd/cd/memory.c.
*/
#ifndef SCD_MEMORY_H
#define SCD_MEMORY_H

#include "pico_int.h"

typedef u32  (cpu68k_read_f)(u32 a);
typedef void (cpu68k_write_f)(u32 a, u32 d);

void cpu68k_map_all_ram(u32 start_addr, u32 end_addr, void *ptr, int is_sub);
void cpu68k_map_all_funcs(u32 start_addr, u32 end_addr,
    u32 (*r8)(u32), u32 (*r16)(u32), void (*w8)(u32, u32), void (*w16)(u32, u32),
    int is_sub);
void cpu68k_map_read_mem(u32 start_addr, u32 end_addr, void *ptr, int is_sub);
void m68k_map_unmap(u32 start_addr, u32 end_addr);

#endif /* SCD_MEMORY_H */
