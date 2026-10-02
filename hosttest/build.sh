#!/bin/bash
# Build the host harness (gen_host) and the LUT generator (lutgen).
#
#   ./hosttest/build.sh          build lutgen, (re)generate LUT headers if
#                                missing, then build gen_host
#   ./hosttest/build.sh lutgen   build only lutgen
#   ./hosttest/build.sh check    lutgen --check against committed headers
#
# The harness compiles the same vendored core sources as the firmware, with
# GWENESIS_HOST=1 (no pico headers) and RGB565 output, plus AddressSanitizer.
set -e
cd "$(dirname "$0")/.."

CORE_DEFS="-DGWENESIS_PICO=1 -DGWENESIS_HOST=1 -DGWENESIS_PIXEL_FMT=565 \
 -DTABLES_FULL=1 -DGNW_TARGET_MARIO=0 -DGNW_TARGET_ZELDA=0 -DGENESIS_SEGACD=1"

INCS="-Igwenesis -Igwenesis/bus -Igwenesis/cpus/M68K -Igwenesis/cpus/Z80 \
 -Igwenesis/io -Igwenesis/savestate -Igwenesis/sound -Igwenesis/vdp \
 -Iport -Ihosttest -Iscd -Ihosttest/shim"

CORE_SRCS="gwenesis/bus/gwenesis_bus.c gwenesis/cpus/M68K/m68kcpu.c \
 gwenesis/cpus/Z80/Z80.c gwenesis/io/gwenesis_io.c \
 gwenesis/savestate/gwenesis_savestate.c gwenesis/sound/ym2612.c \
 gwenesis/sound/gwenesis_sn76489.c gwenesis/sound/z80inst.c \
 gwenesis/vdp/gwenesis_vdp_mem.c gwenesis/vdp/gwenesis_vdp_gfx.c"

PORT_SRCS="port/buffers.c port/gwsnd_core0.c port/gwsnd_resample.c \
 port/gwsnd_shadow.c port/savestate_stubs.c port/gwsram.c port/gwmapper.c"

# Sega CD / MD+: the sub 68000, the vendored PicoDrive CD hardware (scd/),
# its port layer, and FatFs on POSIX for the disc and BIOS code.
SCD_SRCS="gwenesis/cpus/M68K/s68kcpu.c \
 scd/cd/mcd.c scd/cd/memory.c scd/cd/cdc.c scd/cd/cdd.c scd/cd/gfx.c \
 scd/cd/pcm.c scd/cd/misc.c scd/cd/megasd.c \
 port/scd.c port/scd_disc.c port/scd_audio.c port/scd_bios.c port/scd_msd.c \
 port/gwpier.c port/carthw/eeprom_spi.c \
 hosttest/fatfs_host.c"

build_lutgen() {
    echo "== building hosttest/lutgen"
    gcc -O2 -g $CORE_DEFS $INCS \
        hosttest/lutgen.c port/savestate_stubs.c \
        -o hosttest/lutgen -lm
}

case "${1:-all}" in
lutgen)
    build_lutgen
    ;;
check)
    build_lutgen
    ./hosttest/lutgen gwenesis/sound/luts --check
    ;;
all)
    # libchdr (Sega CD .chd images), built on its own: its malloc/free are
    # renamed by a force-included header (external/libchdr_alloc.h), which
    # must not reach the rest of the harness. Same sources and defines as
    # external/libchdr_pico.cmake.
    CHDR=external/libchdr
    CHD_OBJ=hosttest/obj/chd
    mkdir -p "$CHD_OBJ"
    CHD_FLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer \
      -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=0 -DVERIFY_BLOCK_CRC=0 \
      -DMINIZ_NO_STDIO=1 -DMINIZ_NO_TIME=1 -DMINIZ_NO_ARCHIVE_APIS=1 \
      -DMINIZ_NO_ARCHIVE_WRITING_APIS=1 \
      -I$CHDR/include -I$CHDR/src -I$CHDR/deps/miniz-3.1.1 \
      -I$CHDR/deps/lzma-25.01/include -I$CHDR/deps/zstd-1.5.7 \
      -include external/libchdr_alloc.h -w"
    CHD_OBJS=""
    for src in $CHDR/src/libchdr_bitstream.c $CHDR/src/libchdr_cdrom.c \
               $CHDR/src/libchdr_chd.c $CHDR/src/libchdr_flac.c \
               $CHDR/src/libchdr_huffman.c $CHDR/src/libchdr_codec_cdfl.c \
               $CHDR/src/libchdr_codec_cdlz.c $CHDR/src/libchdr_codec_cdzl.c \
               $CHDR/src/libchdr_codec_cdzs.c $CHDR/src/libchdr_codec_flac.c \
               $CHDR/src/libchdr_codec_huff.c $CHDR/src/libchdr_codec_lzma.c \
               $CHDR/src/libchdr_codec_zlib.c $CHDR/src/libchdr_codec_zstd.c \
               $CHDR/deps/miniz-3.1.1/miniz.c $CHDR/deps/lzma-25.01/src/LzmaDec.c \
               $CHDR/deps/zstd-1.5.7/zstddeclib.c hosttest/chd_alloc_host.c; do
        obj="$CHD_OBJ/$(basename "$src" .c).o"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            gcc -c $CHD_FLAGS "$src" -o "$obj"
        fi
        CHD_OBJS="$CHD_OBJS $obj"
    done
    if [ ! -f gwenesis/sound/luts/tl_tab.h ]; then
        build_lutgen
        ./hosttest/lutgen gwenesis/sound/luts
    fi
    echo "== building hosttest/gen_host"
    # EXTRA_CFLAGS: A/B switches, e.g. EXTRA_CFLAGS=-DGENESIS_ROM_MAPPER=0
    gcc -O1 -g -fsanitize=address -fno-omit-frame-pointer \
        $CORE_DEFS ${EXTRA_CFLAGS:-} $INCS \
        -DGWCD_ENABLE_CHD=1 -Iexternal/libchdr/include \
        $CORE_SRCS $PORT_SRCS $SCD_SRCS port/scd_chd.c $CHD_OBJS hosttest/host_main.c \
        -o hosttest/gen_host -lm
    echo "ok: hosttest/gen_host"
    ;;
*)
    echo "usage: $0 [all|lutgen|check]" >&2
    exit 2
    ;;
esac
