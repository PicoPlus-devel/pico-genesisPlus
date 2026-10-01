/*
 * PicoDrive
 * (C) irixxxx, 2024
 *
 * MEGASD enhancement support as "documented" in "MegaSD DEV Manual Rev.2"
 */

struct megasd {
  // modifiable fields visible through the interface
  /* PORT: points into the overlay's shadow page (port/scd_msd.c), where the
     68000 reads it with its ordinary cartridge fetches. Same layout as
     PicoDrive's array: native 16-bit words, bytes at MEM_BE2. */
  u16 *data;
  u16 command;
  u16 result;

  // internal state
  s8 state;                             // CD drive has been initialized

  s8 loop;                              // playback should loop?
  s16 index;                            // >= 0 if playing audio
  s32 startlba, endlba, looplba;        // lba's for playback

  s32 readlba;                          // >= 0 if reading data

  s32 currentlba;                       // lba currently playing

  s32 pad[7];
};

#define MSD_ST_INIT     1
#define MSD_ST_PLAY     2
#define MSD_ST_PAUSE    4

extern struct megasd Pico_msd;


extern void msd_update(void);		// 75Hz update, like CDD irq
extern void msd_reset(void);		// reset state
extern void msd_load(void);		// state loaded

extern void msd_write8(u32 a, u32 d);	// interface
extern void msd_write16(u32 a, u32 d);

/* PORT: port/scd_msd.c */
void gwcd_msd_overlay(int on);  /* swap the $020000-$03FFFF page */
void gwcd_msd_sync_regs(void);  /* result/command words into that page */
