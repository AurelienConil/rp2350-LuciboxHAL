/*
 * boards/lucibox.h — Lucibox PCB board definition (RP2350B, 80-pin QFN)
 *
 * Needed because the PCB uses GPIO 32-47 (lit buttons, ADC pots A0-A7), which
 * do not exist on the RP2350A of the stock "pico2" board.
 * Derived from the SDK's pico2.h; only the RP2350B flag differs.
 *
 * ⚠ PICO_FLASH_SIZE_BYTES is copied from pico2 (4 MB) — set it to the actual
 *   flash chip fitted on the PCB.
 */
#ifndef _BOARDS_LUCIBOX_H
#define _BOARDS_LUCIBOX_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define PICO_RP2350A 0

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
