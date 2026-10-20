#pragma once

// Waveshare RP2350-PiZero, z0p2 (RP2350B, 16 MB flash).
// Do not include boards/pico2.h here: it makes the SDK treat the chip as
// RP2350A, and GPIO32..39 (HDMI) are then not usable.

#define ZERO 1
#define BOARD_Z0 1
#define CPU_FREQ 252

// QSPI PSRAM on QMI CS1 (src/psram.c): probed at start, used for ROMs if found
#define PSRAM_CS1_GPIO_RP2350A 8
#define PSRAM_CS1_GPIO_RP2350B 47

// 16 MB flash
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// SDCARD (hardware SPI1)
#define SDCARD_SPI_BUS spi1
#define SDCARD_PIN_SPI0_SCK 30
#define SDCARD_PIN_SPI0_MOSI 31
#define SDCARD_PIN_SPI0_MISO 40
#define SDCARD_PIN_SPI0_CS 43

// PS2KBD: CLK GP2, DATA GP3 (PIO1), as in MOS2, murm386 and pico-nes for z0p2
#define PS2KBD_GPIO_FIRST 2

// NES Gamepad: CLK GP4, LATCH GP5, DATA GP7 (PIO1), as in MOS2 for z0p2
#define NES_GPIO_CLK 4
#define NES_GPIO_LAT 5
#define NES_GPIO_DATA 7

// HDMI: data GP32..37, clock pair GP38/39 (PIO0 with GPIO base 16)
#define HDMI_BASE_PIN 32
#define VGA_BASE_PIN 32

// Sound: PCM5102A on I2S (PIO1): DIN GP10, BCK GP11, LRCK GP12
#define AUDIO_DATA_PIN 10
#define AUDIO_CLOCK_PIN 11
#define AUDIO_LCK_PIN 12
// drivers/audio switches to PWM whenever AUDIO_PWM_PIN is defined,
// so define it only for a PWM build (cmake -DI2S=OFF)
#if defined(AUDIO_PWM)
#define AUDIO_PWM_PIN 10
#endif
