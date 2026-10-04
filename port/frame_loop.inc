/*
port/frame_loop.inc — one emulated frame, shared VERBATIM between the pico
build (main.cpp) and the host harness (hosttest/host_main.c) so the two can
never drift apart. Modeled on the reference loop in
game-and-watch-retro-go/Core/Src/porting/gwenesis/main_gwenesis.c.

Include AFTER the core headers (gwenesis_bus.h, gwenesis_vdp.h, m68k.h,
z80inst.h, ym2612.h, gwenesis_sn76489.h) and port/gwsnd.h.
The including translation unit must be C, or wrap this in extern "C".

Contract:
  - gwenesis_frame_get_config() refreshes screen geometry and frame rate;
    call it, place the output buffer (320-wide rows, first visible line at
    the pointer given), then run gwenesis_frame_run().
  - gwenesis_frame_run() executes all scanlines with per-line 68000/Z80
    interleave, renders when draw_frame != 0, raises H/V interrupts, then
    tops up the sound chips via gwsnd_frame_end() and rebases the clocks.
*/

/* Sub-frame pacing hook, called after every completed scanline with the
   line count so far and the frame's total. The pico build uses it to
   spread the frame's wall-clock budget across the frame instead of
   emulating flat out and then idling: core1 may only synthesize up to the
   watermark core0 publishes per line, so an idle tail at the end of the
   frame is a hole in audio production that drains the sink dry. No-op
   everywhere else, so the host harness still runs at full speed and its
   output stays bit-identical. */
#ifndef GWENESIS_LINE_PACE
#define GWENESIS_LINE_PACE(line, lines_per_frame) ((void)0)
#endif

/* Sega CD (port/scd.h): the sub 68000 and the rest of the Mega-CD run
   alongside the main CPU line by line; an MD+ game needs its 75 Hz MegaSD
   tick. Without a CD session these are the plain cartridge calls. */
#if GENESIS_SEGACD
#include "scd.h"
#define GWENESIS_M68K_RUN(target) gwcd_run_line(target)
#else
#define GWENESIS_M68K_RUN(target) m68k_run(target)
#endif

/* Core globals not exported through headers. */
extern unsigned char gwenesis_vdp_regs[];
extern unsigned short gwenesis_vdp_status;
extern int screen_width;
extern int screen_height;
extern int hint_pending;
extern int scan_line;      /* port-owned (port/buffers.c) */
extern int frame_counter;  /* port-owned (port/buffers.c) */
extern int system_clock;   /* port-owned (port/buffers.c) */

static int gwenesis_frame_lines_per_frame = LINES_PER_FRAME_NTSC;
static int gwenesis_frame_is_pal = 0;

/* Refresh geometry from the VDP registers; returns nonzero when PAL.

   Frame rate and display height are two different things from two different
   places. VDP status bit 0 is the console (set_region, from the cartridge
   header) and decides 313 lines at 50 Hz vs 262 at 60. REG1_PAL is the VDP's
   240-line bit, which only decides how many of those lines carry picture --
   most PAL games leave it clear and display 224 lines at 50 Hz. Upstream drove
   all three from REG1_PAL, so those games ran at NTSC timing. */
static int gwenesis_frame_get_config(void)
{
    gwenesis_frame_is_pal = (gwenesis_vdp_status & 1) ? 1 : 0;
    screen_height = REG1_PAL ? 240 : 224;
    screen_width = REG12_MODE_H40 ? 320 : 256;
    gwenesis_frame_lines_per_frame =
        gwenesis_frame_is_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    return gwenesis_frame_is_pal;
}

static void gwenesis_frame_run(int draw_frame)
{
    int hint_counter = gwenesis_vdp_regs[10];

    gwenesis_vdp_render_config();

    /* Reset the per-frame clocks (chip clocks are reset by
       gwsnd_frame_end at the end of the previous frame). */
    system_clock = 0;
    zclk = 0;
    scan_line = 0;

#if GENESIS_SEGACD
    gwcd_frame_start();
#endif

    while (scan_line < gwenesis_frame_lines_per_frame) {

        /* CPUs, interleaved per scanline; the sound chips catch up on
           every register access inside these (GWENESIS_AUDIO_ACCURATE). */
        GWENESIS_M68K_RUN(system_clock + VDP_CYCLES_PER_LINE);
        z80_run(system_clock + VDP_CYCLES_PER_LINE);

        /* Both CPUs are done with this line: let the sound engine make
           progress up to its end (no-op in sync mode). */
        gwsnd_line_tick(system_clock + VDP_CYCLES_PER_LINE);

        /* Video. The clamp is not redundant: the host places the output
           buffer using screen_height sampled at frame start, but the core
           bounds its own writes with the LIVE REG1_PAL (240-line) bit. A
           game toggling that bit mid-frame would render more lines than
           the placement reserved and write past the end of the
           framebuffer. */
        if (draw_frame && scan_line < screen_height)
            gwenesis_vdp_render_line(scan_line);

        /* On these lines, the H-int line counter is reloaded */
        if ((scan_line == 0) || (scan_line > screen_height))
            hint_counter = REG10_LINE_COUNTER;

        /* interrupt line counter */
        if (--hint_counter < 0) {
            if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
            }
            hint_counter = REG10_LINE_COUNTER;
        }

        scan_line++;

        /* vblank begins at the end of the last rendered line */
        if (scan_line == screen_height) {
            if (REG1_VBLANK_INTERRUPT != 0) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            z80_irq_line(1);

            /* Interlace field flag (status bit 4): flips at every vblank
               while either interlace mode is on and reads 0 otherwise, as in
               PicoDrive. Only games see it -- the renderer always draws the
               even field. gwenesis_vdp_reset() clears it between games. */
            if (gwenesis_vdp_regs[12] & 2)
                gwenesis_vdp_status ^= STATUS_ODDFRAME;
            else
                gwenesis_vdp_status &= ~STATUS_ODDFRAME;
        }
        if (scan_line == (screen_height + 1))
            z80_irq_line(0);

        system_clock += VDP_CYCLES_PER_LINE;

        /* After gwsnd_line_tick published this line's watermark, so the
           sound engine has work to do while the pacer waits. */
        GWENESIS_LINE_PACE(scan_line, gwenesis_frame_lines_per_frame);
    }

#if GENESIS_SEGACD
    /* Before the sound engine's frame end, so the frame's PCM is queued
       when it mixes, and before the rebase below (the CD code keeps the
       main CPU's time on a monotonic scale). */
    gwcd_frame_end(system_clock, gwenesis_frame_is_pal);
#endif

    /* Top up both sound chips to the frame boundary, emit the frame's
       audio, and rebase the chip clocks. */
    gwsnd_frame_end(system_clock);

    /* Rebase the 68000 cycle counter to the start of the next frame. */
    m68k.cycles -= system_clock;

    frame_counter++;
}
