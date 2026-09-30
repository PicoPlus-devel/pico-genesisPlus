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
 -DTABLES_FULL=1 -DGNW_TARGET_MARIO=0 -DGNW_TARGET_ZELDA=0"

INCS="-Igwenesis -Igwenesis/bus -Igwenesis/cpus/M68K -Igwenesis/cpus/Z80 \
 -Igwenesis/io -Igwenesis/savestate -Igwenesis/sound -Igwenesis/vdp \
 -Iport -Ihosttest"

CORE_SRCS="gwenesis/bus/gwenesis_bus.c gwenesis/cpus/M68K/m68kcpu.c \
 gwenesis/cpus/Z80/Z80.c gwenesis/io/gwenesis_io.c \
 gwenesis/savestate/gwenesis_savestate.c gwenesis/sound/ym2612.c \
 gwenesis/sound/gwenesis_sn76489.c gwenesis/sound/z80inst.c \
 gwenesis/vdp/gwenesis_vdp_mem.c gwenesis/vdp/gwenesis_vdp_gfx.c"

PORT_SRCS="port/buffers.c port/gwsnd_core0.c port/gwsnd_resample.c \
 port/gwsnd_shadow.c port/savestate_stubs.c port/gwsram.c port/gwmapper.c"

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
    if [ ! -f gwenesis/sound/luts/tl_tab.h ]; then
        build_lutgen
        ./hosttest/lutgen gwenesis/sound/luts
    fi
    echo "== building hosttest/gen_host"
    # EXTRA_CFLAGS: A/B switches, e.g. EXTRA_CFLAGS=-DGENESIS_ROM_MAPPER=0
    gcc -O1 -g -fsanitize=address -fno-omit-frame-pointer \
        $CORE_DEFS ${EXTRA_CFLAGS:-} $INCS \
        $CORE_SRCS $PORT_SRCS hosttest/host_main.c \
        -o hosttest/gen_host -lm
    echo "ok: hosttest/gen_host"
    ;;
*)
    echo "usage: $0 [all|lutgen|check]" >&2
    exit 2
    ;;
esac
