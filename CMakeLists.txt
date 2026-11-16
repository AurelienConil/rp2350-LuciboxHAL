# CMakeLists.txt — rp2350-LuciboxHAL
# Compilation: "Raspberry Pi Pico" VS Code extension -> "Compile Project" only

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion       2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion  2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

# Custom RP2350B board (GPIO 32-47 needed) — see boards/lucibox.h
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD lucibox CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(rp2350-LuciboxHAL C CXX ASM)

pico_sdk_init()

# ── Sources ───────────────────────────────────────────────────────────────────
# Template: a single instrument (class Instrument, fixed name; the shipped
# content is a HAL proof-of-life check). To derive a project, rewrite
# instruments/instrument.hpp/.cpp — nothing else to change here.
add_executable(rp2350-LuciboxHAL
    main.cpp
    hal/audio.c
    hal/es8311.c
    hal/controls.c
    hal/leds.c
    hal/touch.c
    hal/i2c_bus.c
    hal/tpa6130a2.c
    instruments/instrument.cpp
)

# Generates i2s_duplex.pio.h from i2s_duplex.pio (TX + RX)
pico_generate_pio_header(rp2350-LuciboxHAL
    ${CMAKE_CURRENT_SOURCE_DIR}/i2s_duplex.pio
)

# Generates ws2812.pio.h (WS2812 strip, PIO1)
pico_generate_pio_header(rp2350-LuciboxHAL
    ${CMAKE_CURRENT_SOURCE_DIR}/ws2812.pio
)

# Firmware name and version
pico_set_program_name(rp2350-LuciboxHAL "rp2350-LuciboxHAL")
pico_set_program_version(rp2350-LuciboxHAL "0.1")

# Stdio via USB CDC only (UART disabled)
pico_enable_stdio_uart(rp2350-LuciboxHAL 0)
pico_enable_stdio_usb(rp2350-LuciboxHAL  1)

# ── SDK dependencies ─────────────────────────────────────────────────────────
target_link_libraries(rp2350-LuciboxHAL
    pico_stdlib
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_clocks
    hardware_pwm
    hardware_adc
    m           # libm for sinf(), cosf() in the sweep
)

# ── Include paths ─────────────────────────────────────────────────────────────
target_include_directories(rp2350-LuciboxHAL PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}       # access to hal/, instruments/
    ${CMAKE_CURRENT_BINARY_DIR}     # access to the generated i2s_duplex.pio.h
)

# ── Compile options ────────────────────────────────────────────────────────────
# ⚠ Do NOT add -mfloat-abi=hard or -mfpu=... : ABI mismatch with the softfp SDK
target_compile_options(rp2350-LuciboxHAL PRIVATE
    -O2
    -Wdouble-promotion          # warn when a float literal/expr silently becomes
                                 # a double (~40x cost on this FPU) — fix the 'f'
                                 # suffix wherever this fires
    -fno-math-errno             # lets sqrtf()/etc. lower to a single FPU instruction
                                 # instead of a libm call that sets errno
    -fsingle-precision-constant # unsuffixed float literals (0.5) stay float,
                                 # never silently promote to double
)

# Generates .uf2, .bin, .hex, .map, etc.
pico_add_extra_outputs(rp2350-LuciboxHAL)
