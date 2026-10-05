cmake_minimum_required(VERSION 3.13)
set(projectname picogenesisPlus)
#set(PICO_DEOPTIMIZED_DEBUG on)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

IF(NOT CMAKE_BUILD_TYPE)
   SET(CMAKE_BUILD_TYPE Debug
       CACHE STRING "Choose the type of build : None Debug Release RelWithDebInfo MinSizeRel."
       FORCE)
ENDIF(NOT CMAKE_BUILD_TYPE)
message("* Current build type is : ${CMAKE_BUILD_TYPE}")

if (NOT HW_CONFIG)
    set(HW_CONFIG 15 CACHE STRING "Select the hardware configuration for your board")      
endif()
if (NOT USE_HSTX)
    set(USE_HSTX 1 CACHE STRING "Use HSTX for display output when possible")
endif()
include("pico_shared/BoardConfigs.cmake")

if (NOT PICO_BOARD )
    set(PICO_BOARD pico2 CACHE STRING "Board type")
    message("PICO_BOARD not set, using default: ${PICO_BOARD}")
endif()
# ---------------------------------------------------------------------------
# TinyUSB family support + Pico SDK init
# Include family_support.cmake BEFORE pico_sdk_init() so that TinyUSB picks up
# the board-specific configuration (PIO USB VBUSEN pin, DP pin, etc.) from the
# BOARD variable set by BoardConfigs.cmake.  The Pico SDK's own TinyUSB
# integration forcibly sets BOARD="pico_sdk" which loses these board-specific
# defines; including family_support.cmake first (with its include_guard) prevents
# the SDK from overriding the board configuration.
# ---------------------------------------------------------------------------
set(FAMILY rp2040)
include($ENV{PICO_SDK_PATH}/lib/tinyusb/hw/bsp/family_support.cmake)
# Some boards (e.g. adafruit_metro_rp2350) force CFG_TUH_RPI_PIO_USB=1 in their
# board.cmake, which TinyUSB's rp2040/family.cmake then bakes into
# tinyusb_common_base. When PIO USB is disabled for this build, strip that
# interface define so it doesn't fight our -DCFG_TUH_RPI_PIO_USB=0 and drag
# hcd_pio_usb.c / pio_usb.h into the compile.
if(ENABLE_PIO_USB EQUAL 0 AND TARGET tinyusb_common_base)
    get_target_property(_tu_defs tinyusb_common_base INTERFACE_COMPILE_DEFINITIONS)
    if(_tu_defs)
        list(REMOVE_ITEM _tu_defs "CFG_TUH_RPI_PIO_USB=1")
        set_target_properties(tinyusb_common_base PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${_tu_defs}")
    endif()
endif()
project(${projectname} C CXX ASM)
pico_sdk_init()
# check_and_add_pico_pio_usb_support() needs pico_generate_pio_header which is
# only available after pico_sdk_init().  The first call inside family.cmake
# (before SDK init) was a no-op; this second call actually creates the
# tinyusb_pico_pio_usb target AND appends hcd_pio_usb.c / dcd_pio_usb.c to
# tinyusb_host_base / tinyusb_device_base. Skip it when PIO USB is disabled,
# otherwise those files get compiled into the project and fail looking for
# pio_usb.h (its include dir lives on tinyusb_pico_pio_usb, which we don't
# link in that case).
if(ENABLE_PIO_USB EQUAL 1)
    message("Checking for PIO USB support...")
    check_and_add_pico_pio_usb_support()
endif()
message("PIO USB support: ${ENABLE_PIO_USB}, using PIO: ${PIO_USB_USE_PIO}")

# ROMs too large for PSRAM run from a region in the top 8 MB of a 16 MB flash,
# with any excess read into PSRAM (romflash.cpp, HSTX builds only -- see the
# HSTX block below). The progress bar shown while
# such a ROM is written only ever draws between flash operations, after
# romflash.cpp has put QMI M0 back, so it need not be SRAM resident -- and
# SRAM is what this emulator is short of. Saves ~1.7 KB. Directory scoped so
# it reaches the pico_shared build.
add_definitions(-DPROGRESS_BAR_IN_SRAM=0)
# Testing aid: pretend the flash region never already holds the ROM, so every
# launch of an oversized ROM rewrites it (about a minute). Never ship this on.
option(ROMFLASH_FORCE_REWRITE "Always rewrite a flash-resident ROM (progress-bar testing)" OFF)
if(ROMFLASH_FORCE_REWRITE)
    add_definitions(-DROMFLASH_FORCE_REWRITE=1)
    message(WARNING "ROMFLASH_FORCE_REWRITE=1 - every oversized ROM launch rewrites flash. Testing only.")
endif()

add_executable(${projectname}
    main.cpp
    splash.cpp
    DefaultSS444.c
    DefaultSS555.c
    port/buffers.c
    port/gwsnd_core0.c
    port/gwsnd_resample.c
    port/gwsnd_shadow.c
    port/savestate_stubs.c
    port/gwsram.c
    port/gwmapper.c
)

pico_set_program_name(${projectname} "${projectname}")
pico_set_program_version(${projectname} "0.1")

# disable uart output for Pimoronis Pico DV Demo Base, as gpio 1 is used for NES controller
# so debug messages will not be visible on the serial console.
if ( HW_CONFIG EQUAL 1 )
    message("Disabling UART output for Pimoroni Pico DV Demo Base")
    pico_enable_stdio_uart(${projectname} 0)
else()
    pico_enable_stdio_uart(${projectname} 1)
endif()
pico_enable_stdio_usb(${projectname} 0)

# ---------------------------------------------------------------------------
# What family_configure_target() used to do for us, inlined:
#   * RP2040-E5 USB device enumeration fix
#   * Silence harmless warnings from TinyUSB / pico-pio-usb sources
# ---------------------------------------------------------------------------
# Cartridge save RAM (battery-backed SRAM -> /SAVES/<rom>.srm). On by default.
# Building with -DGENESIS_CART_SRAM=0 compiles the save-RAM test out of the
# 68000 read paths and the bus mapper, so the emulator can be measured with and
# without it on the same board and scene when attributing a frame-rate or
# audio-underrun change. Saves do not work in such a build.
if (NOT DEFINED GENESIS_CART_SRAM)
    set(GENESIS_CART_SRAM 1 CACHE STRING "Emulate cartridge save RAM (1) or compile it out for perf A/B (0)")
endif()
message("Cartridge save RAM: ${GENESIS_CART_SRAM}")

# Cartridge ROM bank switching (carts over 4 MB, e.g. Super Street Fighter II;
# port/gwmapper.h). On by default. -DGENESIS_ROM_MAPPER=0 restores the old
# single-pointer ROM fetch for a frame-rate A/B; such carts do not work then.
if (NOT DEFINED GENESIS_ROM_MAPPER)
    set(GENESIS_ROM_MAPPER 1 CACHE STRING "Emulate ROM bank switching (1) or compile it out for perf A/B (0)")
endif()
message("ROM bank switching: ${GENESIS_ROM_MAPPER}")

target_compile_definitions(${projectname} PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)

target_compile_definitions(${projectname} PUBLIC
    GENESIS_CART_SRAM=${GENESIS_CART_SRAM}
    GENESIS_ROM_MAPPER=${GENESIS_ROM_MAPPER}
)

target_compile_options(${projectname} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-cast-function-type
    -Wno-maybe-uninitialized
)

target_compile_definitions(${projectname} PUBLIC 
   DVICONFIG=${DVICONFIG}
    SDCARD_PIN_CS=${SD_CS}
    SDCARD_PIN_SCK=${SD_SCK}
    SDCARD_PIN_MOSI=${SD_MOSI}
    SDCARD_PIN_MISO=${SD_MISO}
    SDCARD_PIO=pio1        # Fallback when SPI initialization fails
    SDCARD_SPI=${SD_SPI}  
    NES_PIN_CLK=${NES_CLK}
    NES_PIN_DATA=${NES_DATA}
    NES_PIN_LAT=${NES_LAT}
    NES_PIO=${NES_PIO}
    NES_PIN_CLK_1=${NES_CLK_1}
    NES_PIN_DATA_1=${NES_DATA_1}
    NES_PIN_LAT_1=${NES_LAT_1}
    NES_PIO_1=${NES_PIO_1}
    WII_PIN_SDA=${WII_SDA}
    WII_PIN_SCL=${WII_SCL}
    WIIPAD_DELAYED_START=${WIIPAD_DELAYED_START}
    LED_GPIO_PIN=${LED_GPIO_PIN}
    HW_CONFIG=${HW_CONFIG}
    WIIPAD_I2C=${WIIPAD_I2C}
    USE_I2S_AUDIO=${USE_I2S_AUDIO}
    PICO_AUDIO_I2S_DATA_PIN=${PICO_AUDIO_I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${PICO_AUDIO_I2S_CLOCK_PIN_BASE}
    PICO_AUDIO_I2S_PIO=${PICO_AUDIO_I2S_PIO}
    PICO_AUDIO_I2S_DMA_IRQ=1
    PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED=${PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED}
    PICO_AUDIO_I2S_RESET_PIN=${PICO_AUDIO_I2S_RESET_PIN} # Reset pin for TLV320 codec
    PICO_AUDIO_I2S_INTERRUPT_PIN=${PICO_AUDIO_I2S_INTERRUPT_PIN}
    PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON=${PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON}
    CFG_TUH_RPI_PIO_USB=${ENABLE_PIO_USB} # Enable PIO USB support
    PIO_USB_USE_PIO=${PIO_USB_USE_PIO} # Use PIO for USB
    PSRAM_CS_PIN=${PSRAM_CS_PIN} # Select the GPIO pin for PSRAM chip select
    GPIOHSTXD0=${GPIOHSTXD0} # HSTX D0 pin positive
    GPIOHSTXD1=${GPIOHSTXD1} # HSTX D1 pin positive
    GPIOHSTXD2=${GPIOHSTXD2} # HSTX D2 pin positive
    GPIOHSTXCK=${GPIOHSTXCK} # HSTX clock pin positive
    GPIOHSTXINVERTED=${GPIOHSTXINVERTED} # Set to 1 if HSTX pins are inverted: D- = D+ -1
    ENABLE_VU_METER=${ENABLE_VU_METER} # Enable VU meter (fruitjam NeoPixel leds)
    ENABLEDVI=1
    # Route clk_hstx from PLL_USB at 378 MHz overclock so PLL_SYS jitter no longer
    # closes the TMDS eye-pattern (dots/dotted lines on strict HDMI receivers).
    # FrensHelpers::setClocksAndStartStdio only honors this when PIO-USB is in use
    # (PLL_USB must stay at 48 MHz for native TinyUSB).
    GENESIS_OVERCLOCK_HSTX_FIX=1
    PICO_FLASH_SIZE_BYTES=16777216  # Real size determined at runtime.
)

# ---------------------------------------------------------------------------
# USB drive mode: the "USB drive mode" entry in the settings menu shows the SD
# card on a PC as a mass storage device. The device stack is started when that
# screen opens and stopped when it closes, so nothing outside it changes; when
# this is 0 no tinyusb device code is linked at all.
#
# RP2040 included: those boards take the no-PIO-USB route, so the USB host has
# to give the native port back - gamepads stop while the card is mounted and the
# console reboots on the way out.
# ---------------------------------------------------------------------------
option(ENABLE_USB_MSC "Show the SD card on a PC over USB from the settings menu" ON)
if(ENABLE_USB_MSC)
    set(USB_MSC_ENABLED 1)
else()
    set(USB_MSC_ENABLED 0)
endif()
message(STATUS "USB drive mode (mass storage): ${USB_MSC_ENABLED}")
target_compile_definitions(${projectname} PRIVATE FRENS_USB_MSC=${USB_MSC_ENABLED})
if(USB_MSC_ENABLED)
    target_link_libraries(${projectname} PRIVATE tinyusb_device pico_unique_id)
    # Widen the pool of claimable hardware spinlocks from 8 to 12.
    #
    # spin_lock_claim_unused() only hands out PICO_SPINLOCK_ID_CLAIM_FREE_FIRST
    # ..LAST, which is ids 24..31 by default - eight in total. On PicoDVI builds
    # dvi::DVI alone takes four of them (validTMDSQueue_, freeTMDSQueue_,
    # validLineQueue_, freeLineQueue_ - util::Queue holds a util::SpinLock), and
    # the TinyUSB host stack takes most of the rest, so bringing the device
    # stack up for USB drive mode ran the pool dry and panicked with "No
    # spinlocks are available" on the very first visit. HSTX builds have no
    # PicoDVI and so never hit it.
    #
    # The striped range gives up the four ids instead. Striped locks are handed
    # out round-robin and are shared by design, so a smaller range costs a
    # little more contention and nothing else.
    target_compile_definitions(${projectname} PRIVATE
        PICO_SPINLOCK_ID_STRIPED_FIRST=16
        PICO_SPINLOCK_ID_STRIPED_LAST=19
        PICO_SPINLOCK_ID_CLAIM_FREE_FIRST=20
        PICO_SPINLOCK_ID_CLAIM_FREE_LAST=31
    )
endif()
# The gwenesis core renders directly into the display framebuffer, so its
# CRAM->pixel conversion must match the video driver's format:
# HSTX (pico_hdmi) = RGB555, PicoDVI = RGB444. See gwenesis/gwenesis_port.h.
# Same condition as the HSTX macro in pico_shared/FrensHelpers.h: all four
# HSTX pins configured (BoardConfigs only sets them for HSTX-capable boards
# when USE_HSTX=1 and FORCE_DVI=0; HSTX pins are never GPIO 0).
if(GPIOHSTXD0 AND GPIOHSTXD1 AND GPIOHSTXD2 AND GPIOHSTXCK)
    target_compile_definitions(${projectname} PUBLIC GWENESIS_PIXEL_FMT=555)
    # HSTX: sound synthesis is offloaded to core1 (the pico_hdmi background
    # task); core0 keeps a shadow of the YM2612 timers for status reads.
    target_compile_definitions(${projectname} PUBLIC GWSND_OFFLOAD=1)
    # The audio backlog is held at DI_TARGET_PACKETS (160) so a run of
    # heavy frames cannot walk the data-island ring empty and splice in
    # silence packets. The driver's default drop watermark of 200 sits too
    # close to that: peaks would be discarded instead, trading underruns
    # for dropped samples. Raise it; the ring is 256 entries and this is a
    # threshold only, so it costs no memory.
    target_compile_definitions(${projectname} PUBLIC HSTX_AUDIO_DI_HIGH_WATERMARK=240)
    target_sources(${projectname} PRIVATE port/gwsnd_core1.c)
    # ROMs too large for PSRAM, run from flash (+ PSRAM). HSTX only: the flash
    # write takes XIP away, which HSTX scan-out survives and PicoDVI does not.
    target_sources(${projectname} PRIVATE romflash.cpp)
    # Sega CD / Mega-CD and MD+ (port/scd.h): a second 68000 (s68kcpu.c),
    # PicoDrive's Mega-CD hardware (scd/, see scd/PORTING.md) and the disc,
    # BIOS and CD audio layers. HSTX builds only, like the core1 sound engine
    # the CD audio rides on; at run time also PSRAM only (main.cpp), since
    # every CD buffer lives there. -DGENESIS_SEGACD=0 leaves it out.
    if (NOT DEFINED GENESIS_SEGACD)
        set(GENESIS_SEGACD 1 CACHE STRING "Sega CD and MD+ support (HSTX builds)")
    endif()
    if (GENESIS_SEGACD)
        target_compile_definitions(${projectname} PUBLIC GENESIS_SEGACD=1)
        target_sources(${projectname} PRIVATE
            gwenesis/cpus/M68K/s68kcpu.c
            scd/cd/mcd.c
            scd/cd/memory.c
            scd/cd/cdc.c
            scd/cd/cdd.c
            scd/cd/gfx.c
            scd/cd/pcm.c
            scd/cd/misc.c
            scd/cd/megasd.c
            port/scd.c
            port/scd_disc.c
            port/scd_audio.c
            port/scd_bios.c
            port/scd_msd.c
            port/gwpier.c
            port/carthw/eeprom_spi.c
        )
        target_include_directories(${projectname} PRIVATE scd)
        # Wait-loop skipping in both 68000s (gwenesis/PORTING.md); building
        # with -DGWCD_IDLE_SKIP=0 turns it off, for a frame-rate A/B.
        if (NOT DEFINED GWCD_IDLE_SKIP)
            set(GWCD_IDLE_SKIP 1 CACHE STRING "Skip the 68000s' wait loops in Sega CD sessions (1) or not (0)")
        endif()
        message("Sega CD wait-loop skipping: ${GWCD_IDLE_SKIP}")
        target_compile_definitions(${projectname} PUBLIC GWCD_IDLE_SKIP=${GWCD_IDLE_SKIP})
        # Same treatment as the main 68000 (gwenesis/CMakeLists.txt).
        set_source_files_properties(gwenesis/cpus/M68K/s68kcpu.c
            PROPERTIES COMPILE_OPTIONS "-O2")
        # CHD disc images through libchdr (submodule external/libchdr, as in
        # pico-pcePlus; external/libchdr_pico.cmake). Off by default (cue/bin
        # only); -DGENESIS_SEGACD_CHD=1 adds them.
        if (NOT DEFINED GENESIS_SEGACD_CHD)
            set(GENESIS_SEGACD_CHD 0 CACHE STRING "CHD disc images for Sega CD / MD+")
        endif()
        if (GENESIS_SEGACD_CHD)
            include(${CMAKE_CURRENT_SOURCE_DIR}/external/libchdr_pico.cmake)
            target_link_libraries(${projectname} PRIVATE libchdr_pico)
            target_sources(${projectname} PRIVATE port/scd_chd.c)
            target_compile_definitions(${projectname} PUBLIC GWCD_ENABLE_CHD=1)
        endif()
        message("Sega CD CHD images: ${GENESIS_SEGACD_CHD}")
    endif()
    message("Sega CD / MD+ support: ${GENESIS_SEGACD}")
else()
    target_compile_definitions(${projectname} PUBLIC GWENESIS_PIXEL_FMT=444)
endif()
if(NOT PICO_PLATFORM STREQUAL "rp2040")
    message(STATUS "Using PICO_PIO_USE_GPIO_BASE=1 for non-rp2040 platforms.")
    target_compile_definitions(${projectname} PRIVATE
        PICO_PIO_USE_GPIO_BASE=1 # Needed for PIO USB support and DVI with gpio pins > 32
        PICO_STACK_SIZE=3072
    )
endif()
if (DEFINED PICO_DEFAULT_UART_TX_PIN)
    message(STATUS "Using PICO_DEFAULT_UART_TX_PIN: ${PICO_DEFAULT_UART_TX_PIN}")
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_TX_PIN=${PICO_DEFAULT_UART_TX_PIN}
        PICO_DEFAULT_UART=0
    )
endif()
if (DEFINED PICO_DEFAULT_UART_RX_PIN)
    message(STATUS "Using PICO_DEFAULT_UART_RX_PIN: ${PICO_DEFAULT_UART_RX_PIN}")
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_RX_PIN=${PICO_DEFAULT_UART_RX_PIN}
    )
endif()
if (DEFINED PICO_DEFAULT_UART)
    message(STATUS "Using PICO_DEFAULT_UART: ${PICO_DEFAULT_UART}")
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART=${PICO_DEFAULT_UART}
    )
endif()
# ---------------------------------------------------------------------------
# PIO USB (optional)
# tinyusb_pico_pio_usb is a target the pico-sdk exposes when TinyUSB is present.
# ---------------------------------------------------------------------------
if(ENABLE_PIO_USB EQUAL 1)
    if(NOT PIO_DP_PLUS_PIN EQUAL -1)
        message(STATUS "Using PIO USB DP pin: ${PIO_DP_PLUS_PIN}")
        target_compile_definitions(${projectname} PRIVATE
            PICO_DEFAULT_PIO_USB_DP_PIN=${PIO_DP_PLUS_PIN}
        )
    endif()
    message("Adding target link libraries for PIO USB support.")
    target_link_libraries(${projectname} PRIVATE tinyusb_pico_pio_usb)
endif()
target_link_libraries(${projectname}
PRIVATE
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_i2c
    hardware_interp
    hardware_timer
    hardware_clocks
    hardware_pwm
    hardware_flash
    dvi
    util
    tinyusb_host 
    tinyusb_board
    pico_shared  
    xinput_host 
    gwenesis
)
if(PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    # Attention pico2_w in arm-s or risv-s mode causes the led blinking to generate red screen flicker and
    # multiple "[CYW43] do_ioctl(2, 263, 16): timeout" messages to the console.
    # pico_w does not have this issue.
    target_link_libraries(${projectname} PRIVATE pico_cyw43_arch_none)
endif()

target_include_directories(${projectname}
PRIVATE
    pico_lib
    tusb_xinput
    pico_shared
    port
    .
)
add_subdirectory(pico_lib/dvi)
add_subdirectory(pico_lib/util)
add_subdirectory(pico_shared)
add_subdirectory(gwenesis)
add_subdirectory(tusb_xinput xinput_host)
target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections)

# Optional: relink this emulator to run from the bootloader's application
# partition (for use with pico_emuLoader). Off by default so standalone builds
# are unchanged. Enable with: cmake -DBUILD_FOR_BOOTLOADER=ON ..
if(BUILD_FOR_BOOTLOADER)
    include("pico_shared/BootPartition.cmake")
    if(DEFINED BUILD_FOR_BOOTLOADER_SLOT)
        frens_link_to_pinned_slot(${projectname} ${BUILD_FOR_BOOTLOADER_SLOT})
    else()
        frens_offset_for_bootloader(${projectname})
    endif()
endif()

pico_add_extra_outputs(${projectname})



