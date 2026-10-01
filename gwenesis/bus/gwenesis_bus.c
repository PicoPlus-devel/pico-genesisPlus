/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "m68k.h"

#include "ym2612.h"
#include "z80inst.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"
#include "gwenesis_port.h"
#include "gwsram.h"
#include "scd.h"
#if GENESIS_SEGACD
#include "gwpier.h"
#endif

#if GNW_TARGET_MARIO !=0 || GNW_TARGET_ZELDA!=0
  #pragma GCC optimize("Ofast")
#endif

#define BUS_DISABLE_LOGGING 1

#if !BUS_DISABLE_LOGGING
#include <stdarg.h>
void bus_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%04x hc:%04x hv:%04x ", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter(),gwenesis_vdp_hvcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define bus_log(...)  do {} while(0)
#endif

// Setup M68k memories ROM & RAM
#if GWENESIS_PICO != 0

/* ROM image lives in PSRAM or XIP flash, already byte-swapped by the loader;
   the core only keeps a pointer to it. rom_addr_mask (pow2ceil(size)-1)
   mirrors/bounds all ROM fetches — see FETCH*ROM in m68k.h.
   RAM buffers are allocated by the port at game start (port/buffers.c). */
const unsigned char *ROM_DATA;
unsigned int rom_addr_mask;
extern unsigned char *M68K_RAM;

#elif GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

#include "rom_manager.h"
unsigned char *M68K_RAM=(void *)(uint32_t)(0); // 68K RAM
#else

unsigned char ROM_DATA[MAX_ROM_SIZE]; // 68K Main Program (uncompressed)
unsigned char M68K_RAM[MAX_RAM_SIZE];    // 68K RAM
#endif


// Setup Z80 Memory
#if GWENESIS_PICO != 0
extern unsigned char *ZRAM; // Z80 RAM, allocated by the port (port/buffers.c)
#else
unsigned char ZRAM[MAX_Z80_RAM_SIZE]; // Z80 RAM
#endif
unsigned char TMSS[0x4];
extern unsigned short gwenesis_vdp_status;

// TMSS
int tmss_state = 0;
int tmss_count = 0;

/******************************************************************************
 *
 *   Load a Sega Genesis Cartridge into CPU Memory
 *
 ******************************************************************************/


#if GWENESIS_PICO != 0

/* The ROM image already resides in PSRAM or XIP flash, pre-byte-swapped by
   pico_shared's loader — no copy, no swap: just point at it. The address
   mask mirrors the image through the 8 MB cart window for power-of-two
   sizes and bounds stray fetches for the rest (the backing region must be
   readable up to the rounded size — true for the XIP/PSRAM windows; the
   host harness rounds its buffer up accordingly). */
void load_cartridge(const unsigned char *buffer, size_t size)
{
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    ROM_DATA = buffer;
    rom_addr_mask = 1;
    while (rom_addr_mask < size)
        rom_addr_mask <<= 1;
    rom_addr_mask -= 1;

    /* Page table behind FETCH*ROM, back at its power-on banks. Must precede
       set_region() below, which reads the header through it. */
    gwmapper_reset(buffer, size, rom_addr_mask);

    /* TMSS latch survives from the previous game when relaunching without
       a reboot (PSRAM boards) — reset it here. */
    tmss_state = 0;
    tmss_count = 0;

    // Set Z80 Memory as ZRAM
    z80_set_memory(ZRAM);
    z80_pulse_reset();

    set_region();
}

#elif GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

void load_cartridge()
{
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as Z80_RAM
    z80_set_memory(ZRAM);

    z80_pulse_reset();

    set_region();

}
#else

void load_cartridge(unsigned char *buffer, size_t size)
{
    // Clear all volatile memory
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    memset(ROM_DATA, 0, MAX_ROM_SIZE);

    // Set Z80 Memory as ZRAM
    z80_set_memory(ZRAM);
    z80_pulse_reset();

    // Copy file contents to CPU ROM memory
    memcpy(ROM_DATA, buffer, size);

    #ifdef ROM_SWAP
    bus_log(__FUNCTION__,"--ROM swap mode--");
    for (int i=0; i < size;i+=2 )
    {   
        char z = ROM_DATA[i];
        ROM_DATA[i]=ROM_DATA[i+1];
        ROM_DATA[i+1]=z;
    }
    #endif


    set_region();
}

#endif

/******************************************************************************
 *
 *   Power ON the CPU
 *   Initialize 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void power_on() {
  // Set M68K CPU as original MOTOROLA 68000
  //m68k_set_cpu_type(M68K_CPU_TYPE_68000);
#if GWENESIS_PICO != 0
  /* m68k_init() does not clear the whole context; stale state crashes a
     relaunch when a new game is started without a reboot (PSRAM boards). */
  memset(&m68k, 0, sizeof(m68k));
#endif
  // Initialize M68K CPU
  m68k_init();
  // Initialize Z80 CPU
  z80_start();
  // Initialize YM2612 chip
  YM2612Init();
  YM2612Config(9);
  // Initialize PSG SN76489 chip
  //CLOCK_NTSC      = 3579545,
  //CLOCK_PAL       = 3546895,
 // CLOCK_NTSC_SMS1 = 3579527

//  if (mode_pal) {
//     gwenesis_SN76489_Init(3546895, GWENESIS_AUDIO_BUFFER_LENGTH_PAL*50,AUDIO_FREQ_DIVISOR);
//   } else{
//     gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC*60,AUDIO_FREQ_DIVISOR);
//   }
  
  gwenesis_SN76489_Init(3579545, 888*60,AUDIO_FREQ_DIVISOR);

}

/******************************************************************************
 *
 *   Reset the CPU Emulation
 *   Send a pulse reset to 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void reset_emulation() {
  // Send a reset pulse to Z80 CPU
  z80_pulse_reset();
  // Send a reset pulse to Z80 M68K
  m68k_pulse_reset();
  // Send a reset pulse to YM2612 chip
  YM2612ResetChip();
  // Send a reset pulse to SEGA 315-5313 chip
  gwenesis_vdp_reset();
  gwenesis_SN76489_Reset();
  gwenesis_io_reset();
}

/******************************************************************************
 *
 *   Set Region
 *   Look at ROM to set console compatible region
 *
 ******************************************************************************/
void set_region()
{    
  /*
    old style : JUE characters
    J : Domestic 60Hz (Asia)
    U : Oversea  60Hz (USA) 
    E : Oversea  50Hz (Europe) 

    new style : 1st character
    bit 0 : +1 Domestic 60Hz (Asia)
    bit 1 : +2 Domestc  50Hz (Asia)
    bit 2:  +4 Oversea  60Hz (USA) 
    bit 3:  +4 Oversea  50Hz (Europe) 
  */

   // extern int mode_pal;

    int country = 0;

    char rom_str[3];

    printf("ROM game  : ");
    for (int j=0; j < 48;j++) printf("%c",(char)FETCH8ROM(0x150+j));
    printf("\n");

    rom_str[0]=FETCH8ROM(0x1F0);
    rom_str[1]=FETCH8ROM(0x1F1);
    rom_str[2]=FETCH8ROM(0x1F2);

    printf("ROM region:%c%c%c (0x%02x 0x%02x 0x%02x)\n", rom_str[0],rom_str[1],rom_str[2],rom_str[0],rom_str[1],rom_str[2]);

    /* from Gens */
    if (!memcmp(rom_str, "eur", 3)) country |= 8;
    else if (!memcmp(rom_str, "EUR", 3)) country |= 8;
    else if (!memcmp(rom_str, "Europe", 3)) country |= 8;
    else if (!memcmp(rom_str, "jap", 3)) country |= 1;
    else if (!memcmp(rom_str, "JAP", 3)) country |= 1;
    else if (!memcmp(rom_str, "usa", 3)) country |= 4;
    else if (!memcmp(rom_str, "USA", 3)) country |= 4;
    else
    {
      int i;
      unsigned char c;

      /* look for each characters */
      for(i = 0; i < 3; i++)
      {
        c = rom_str[i];

        if (c == 'U') country |= 4;
        else if (c == 'E' || c == 'e' ) country |= 8;
        else if (c == 'J' || c == 'j' ) country |= 1;
        else if (c == 'K' || c == 'k' ) country |= 1;
        else if (c < 16) country |= c;
        else if ((c >= '0') && (c <= '9')) country |= c - '0';
        else if ((c >= 'A') && (c <= 'F')) country |= c - 'A' + 10;
      }
    }
    printf("country code=%01x : ",country);
      /* set default console region (USA > EUROPE > JAPAN) */
      /*
      IO REG0	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
      MODE (R) 	0: Domestic Model
  	            1: Overseas Model
      VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	            1: PAL CPU clock 7.60 MHz
      */

    /* USA 60Hz*/
    if (country & 4){
      printf("Oversea-NTSC USA 60Hz\n");
      gwenesis_io_set_reg(0, 0x81);
      gwenesis_vdp_status &= ~1;
      return;
    }
    /* EUROPE 50Hz */
    if (country & 8){
      printf("Oversea-PAL Europe 50Hz\n");
      gwenesis_io_set_reg(0, 0xC1);
      /* VDP status bit 0 (VERSION_PAL) is the console region. The VDP's
         vcounter and vblank logic already reads it -- until now nothing ever
         set it, so every PAL branch in there was dead code. $A10001 bit 6,
         set above, is the other way a game can ask. Not to be confused with
         the VDP's mode_pal, which despite the name is the 240-line display
         bit (REG1_PAL) and stays driven by the game. */
      gwenesis_vdp_status |= 1;
      return;
    }
    /* set Asia 60HZ */
    if (country & 1){
      printf("Domestic-NTSC Asia 60Hz\n");
      gwenesis_io_set_reg(0, 0x1);
      gwenesis_vdp_status &= ~1;
      return;
    }
      printf("Oversea-NTSC USA 60Hz no detection>> default mode\n");
      gwenesis_io_set_reg(0, 0x81);
      gwenesis_vdp_status &= ~1;

}
/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *   68K Access to Z80 Memory
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_z80_address(unsigned int address) {

  unsigned int range = (address & 0xF000);
  switch (range) {
  case 0:
  case 0x1000:
    return Z80_RAM_ADDR;
  case 0x2000:
  case 0x3000:
    return Z80_RAM_ADDR1K;
  case 0x4000:
    return Z80_YM2612_ADDR;
  case 0x6000:
    return Z80_BANK_ADDR;
  case 0x7000:
    return Z80_SN76489_ADDR;
  default:
    bus_log(__FUNCTION__,"no map Z80 %x",address);
    assert(0);
    return NONE_;
  }
}

/******************************************************************************
 *
 *   IO memory address mapper
 *   Map all input/output region address for CPU program
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_io_address(unsigned int address)
{
  /* /TIME region $A13000-$A130FF, which carries the save RAM control register
     at $A130F1 and the ROM bank registers at $A130F3-$FF. Upstream's "address & 0x1000" test sent $A13xxx to the Z80
     control registers, where it matched neither BUSREQ nor RESET and did
     nothing but a z80_sync(); reads returned z80_read_ctrl()'s 0xFF default,
     which the TIME_CTRL read case reproduces. */
  if ((address & 0xF000) == 0x3000)
    return TIME_CTRL;
#if GENESIS_SEGACD
  /* Sega CD: $A12000-$A120FF is the gate array (scd/cd/memory.c). Without a
     CD session these addresses keep upstream's decode below. */
  if ((address & 0xFF00) == 0x2000 && (gwcd_bus_mode & GWCD_BUS_SCD))
    return MCD_CTRL;
#endif

  unsigned int range = (address & 0x1000) ;
  switch (range) {
  case 0:      return IO_CTRL;
  case 0x1000: return Z80_CTRL;
  default:
      // if (address >= 0xa14000 && address < 0xa11404)
      // return (tmss_state == 0) ? TMSS_CTRL : NONE_;
      bus_log(__FUNCTION__,"no map io %x",address);

    return NONE_;
  }
}

/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *
 ******************************************************************************/

static inline 
unsigned int gwenesis_bus_map_address(unsigned int address) {
  // Mask address page
  unsigned int range = (address & 0xFF0000) >> 16;

  // Check mask and select memory type
  if (range < 0x80) //        ROM ADDRESS 0x000000 - 0x3FFFFF
    /* Cartridge save RAM overlays part of this window on the carts that have
       it; gwsram_hit() is one compare when they do not (port/gwsram.h). */
    return gwsram_hit(address) ? SRAM_ADDR : ROM_ADDR;

  else if (range == 0xA0) // Z80 ADDRESS 0xA00000 - 0xA0FFFF
    return gwenesis_bus_map_z80_address(address);


  else if (range == 0xA1) //                  IO ADDRESS  0xA10000 - 0xA1FFFF
    return gwenesis_bus_map_io_address(address);

  else if (range == 0xC0) // VDP ADDRESS 0xC00000 - 0xDFFFFFF
    return VDP_ADDR;
  else if (range == 0xFF) // RAM ADDRESS 0xE00000 - 0xFFFFFFF
    return RAM_ADDR;
  // If not a valid address return 0
  bus_log(__FUNCTION__,"M68K > ?? unnmap address %x", address);
  //assert(0);
  return NONE_;
}
/******************************************************************************
 *
 *   Main read address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_read_memory_8(unsigned int address) {
 bus_log(__FUNCTION__,"read8  %x", address);

  switch (gwenesis_bus_map_address(address)) {
  
  case VDP_ADDR:
    return gwenesis_vdp_read_memory_8(address);

  case ROM_ADDR:
    return FETCH8ROM(address);

  case SRAM_ADDR:
    return gwsram_read8(address);

  case TIME_CTRL:
#if GENESIS_SEGACD
    /* Pier Solar's SPI EEPROM answers at $A1300B (port/gwpier.h). */
    if (gwpier_active)
      return gwpier_read8(address);
#endif
    return 0xFF;

#if GENESIS_SEGACD
  case MCD_CTRL:
    return gwcd_m68k_io_read8(address);
#endif

  case RAM_ADDR:
    return FETCH8RAM(address);

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
    return z80_read_ctrl(address & 0xFFFF);

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return ZRAM[address & 0x1FFF];

  case Z80_YM2612_ADDR:
    return gwsnd_ym_read(m68k_cycles_master());

  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  case TMSS_CTRL:
    bus_log(__FUNCTION__,"TMS");
    if (tmss_state == 0)
      return TMSS[address & 0x4];
    return 0xFF;

  default:
     bus_log(__FUNCTION__," default read 8 %x", address);
    return 0x00;
  }
  return 0x00;
}

static inline unsigned int gwenesis_bus_read_memory_16(unsigned int address) {
   bus_log(__FUNCTION__,"read16 %x", address);
   unsigned int ret_value;

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    return gwenesis_vdp_read_memory_16(address);

  case RAM_ADDR:
    return FETCH16RAM(address);

  case ROM_ADDR:
    return FETCH16ROM(address);

  case SRAM_ADDR:
    return gwsram_read16(address);

#if GENESIS_SEGACD
  case MCD_CTRL:
    return gwcd_m68k_io_read16(address);
#endif

  case IO_CTRL:
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
  //  ret_value = z80_read_ctrl(address & 0xFFFF); 
   // return ret_value | ret_value << 8;
    address &=0xFFFF;
        return (z80_read_ctrl(address) << 8) | z80_read_ctrl(address | 1);


  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    return ZRAM[address & 0X1FFF] | (ZRAM[address & 0X1FFF] << 8);

  case Z80_YM2612_ADDR:
    ret_value = gwsnd_ym_read(m68k_cycles_master());
    return ret_value | ret_value << 8;


  case Z80_SN76489_ADDR:
    return 0xff;

  case Z80_BANK_ADDR:
    return 0xff;

  default:
    bus_log(__FUNCTION__,"read mem 16 default %x", address);
    return (gwenesis_bus_read_memory_8(address) << 8) |
           gwenesis_bus_read_memory_8(address + 1);
  }
  return 0x00;
}

/******************************************************************************
 *
 *   Main write address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline void gwenesis_bus_write_memory_8(unsigned int address,
                                              unsigned int value) {
  bus_log(__FUNCTION__,"write8  @%x:%x", address,value);

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    gwenesis_vdp_write_memory_16(address & ~1, (value << 8) | value);
    return;

  case RAM_ADDR:
    WRITE8RAM(address, value);
    return;

  case SRAM_ADDR:
    gwsram_write8(address, value);
    return;

  case TIME_CTRL:
    gwsram_time_write(address, value);
    return;

#if GENESIS_SEGACD
  /* Writes below $800000: upstream dropped them all as ROM writes. A Sega
     CD session has PRG-RAM and Word-RAM there, an MD+ session the MegaSD
     registers; a plain cartridge still drops them. */
  case ROM_ADDR:
    if (gwcd_bus_mode != GWCD_BUS_CART)
      gwcd_m68k_write8(address, value);
    return;

  case MCD_CTRL:
    gwcd_m68k_io_write8(address, value);
    return;
#endif

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    z80_write_ctrl(address & 0x1FFF, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ZRAM[address & 0x1FFF] = value;
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CPUZ80PSG8 ,m68kclk= %d", m68k_cycles_master());
    gwsnd_ym_write(address & 0x3, value & 0Xff,m68k_cycles_master());
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CPUZ80FM8  ,m68kclk= %d", m68k_cycles_master());
    gwsnd_psg_write( value & 0Xff, m68k_cycles_master());
    return;

  case Z80_BANK_ADDR:
  //TODO
    return;

  case TMSS_CTRL:

    if (tmss_state == 0) {
      TMSS[address & 0x4] = value;
      tmss_count++;
      if (tmss_count == 4)
        tmss_state = 1;
    }
    return;



  default:
    //printf("write(%x, %x)\n", address, value);
    return;
  }
  return;
}

static inline void gwenesis_bus_write_memory_16(unsigned int address,
                                               unsigned int value) {
  bus_log(__FUNCTION__,"write16  @%x:%x", address,value);

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    gwenesis_vdp_write_memory_16(address, value);
    return;

  case RAM_ADDR:
    WRITE16RAM(address, value);
    return;

  case SRAM_ADDR:
    gwsram_write16(address, value);
    return;

  case TIME_CTRL:
    /* Byte register on an odd address; a word write puts it in the low half. */
    gwsram_time_write(address | 1, value & 0xFF);
    return;

#if GENESIS_SEGACD
  case ROM_ADDR:
    if (gwcd_bus_mode != GWCD_BUS_CART)
      gwcd_m68k_write16(address, value);
    return;

  case MCD_CTRL:
    gwcd_m68k_io_write16(address, value);
    return;
#endif

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    ZRAM[address & 0X1FFF]= value >> 8;
    return;

  case IO_CTRL:
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    z80_write_ctrl(address & 0xFFFF, value >> 8) ;
    return;

  case Z80_YM2612_ADDR:
    bus_log(__FUNCTION__,"CZYM16 ,mclk=%d",  m68k_cycles_master());
    gwsnd_ym_write(address & 0x3, value >> 8,m68k_cycles_master() );
    return;

  case Z80_SN76489_ADDR:
    bus_log(__FUNCTION__,"CZSN16 ,mclk=%d", m68k_cycles_master());
    gwsnd_psg_write(value >> 8,m68k_cycles_master() );
    return;

  default:
    bus_log(__FUNCTION__,"write mem 16 default %x ", address);
    gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
    gwenesis_bus_write_memory_8(address + 1, (value)&0xff);

    return;
  }
  return;
}

/******************************************************************************
 *
 *   68K CPU read address R8
 *   Read an address from memory mapped and return value as byte
 *
 ******************************************************************************/
unsigned int GW_SRAM_FUNC(m68k_read_memory_8)(unsigned int address)
{
      //  if ((address &  0xFF0000 ) == 0xFF0000) return FETCH8RAM(address);
    return gwenesis_bus_read_memory_8(address);
}

/******************************************************************************
 *
 *   68K CPU read address R16
 *   Read an address from memory mapped and return value as word
 *
 ******************************************************************************/
 unsigned int GW_SRAM_FUNC(m68k_read_memory_16)(unsigned int address)
{
     //   if ((address &  0xFF0000 ) == 0xFF0000) return FETCH16RAM(address);
    return gwenesis_bus_read_memory_16(address);
}

/******************************************************************************
 *
 *   68K CPU read address R32
 *   Read an address from memory mapped and return value as long
 *
 ******************************************************************************/
 unsigned int GW_SRAM_FUNC(m68k_read_memory_32)(unsigned int address)
{
  //  if ((address &  0xFF0000 ) == 0xFF0000) return FETCH32RAM(address);
    return (gwenesis_bus_read_memory_16(address) << 16) | gwenesis_bus_read_memory_16(address + 2);
}

/******************************************************************************
 *
 *   68K CPU write address W8
 *   Write an value as byte to memory mapped on specified address
 *
 ******************************************************************************/
void GW_SRAM_FUNC(m68k_write_memory_8)(unsigned int address, unsigned int value) {
  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE8RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_8(address, value);
  return;
}

/******************************************************************************
 *
 *   68K CPU write address W16
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void GW_SRAM_FUNC(m68k_write_memory_16)(unsigned int address, unsigned int value) {
  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE16RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_16(address, value);
  return;
}
/******************************************************************************
 *
 *   68K CPU write address W32
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void GW_SRAM_FUNC(m68k_write_memory_32)(unsigned int address, unsigned int value) {

  // if ((address & 0xFF0000) == 0xFF0000) {
  //   WRITE32RAM(address, value);
  //   return;
  // }
  gwenesis_bus_write_memory_16(address, (value >> 16) & 0xffff);
  gwenesis_bus_write_memory_16(address + 2, (value)&0xffff);

  return;
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return m68k_read_memory_16(address);
}
unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_memory_32(address);
}

void gwenesis_bus_save_state() {
  SaveState* state;
  state = saveGwenesisStateOpenForWrite("bus");
  saveGwenesisStateSetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
  saveGwenesisStateSet(state, "tmss_state", tmss_state);
  saveGwenesisStateSet(state, "tmss_count", tmss_count);
}

void gwenesis_bus_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("bus");
    saveGwenesisStateGetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
    tmss_state = saveGwenesisStateGet(state, "tmss_state");
    tmss_count = saveGwenesisStateGet(state, "tmss_count");
}