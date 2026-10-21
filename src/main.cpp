/**
 * Copyright (C) 2023 by Ilya Maslennikov <xrip@xrip.ru>
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
// Peanut-GB emulator settings
#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define ENABLE_SDCARD 1
#define USE_PS2_KBD 1
#define USE_NESPAD 1


/* C Headers */
#include <cstdio>
#include <cstring>
#include <strings.h>

/* RP2040 Headers */
#include "pico/runtime.h"
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#if PICO_RP2350
#include <hardware/structs/qmi.h>
#endif
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/critical_section.h>
#include <sys/unistd.h>
#include <hardware/watchdog.h>
#include <hardware/clocks.h>

#include "audio.h"
#include "minigb_apu.h"

/* Project headers */
#include "hedley.h"
#include "peanut_gb.h"
#include "gbcolors.h"
#include "mono_palettes.h"

/* Murmulator board */
#include "graphics.h"
#include "f_util.h"
#include "ff.h"


#include "nespad.h"

#include "ps2kbd_mrmltr.h"
#include "psram.h"


/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define HOME_DIR (char*)"\\GB"
extern char __flash_binary_end;
#define FLASH_TARGET_OFFSET (((((uintptr_t)&__flash_binary_end - XIP_BASE) / FLASH_SECTOR_SIZE) + 4) * FLASH_SECTOR_SIZE)
#define FLASH_ROM ((const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET))
/* the cartridge being played: in flash, or in QSPI PSRAM when there is one */
static const uint8_t* rom = FLASH_ROM;

/* A ROM loaded into PSRAM survives a watchdog reset (SELECT in the browser
 * runs it again: PSRAM keeps its contents while powered).  After power-up the
 * uninitialized RAM is random, hence the magic; then the flash copy is used. */
#define ROM_INFO_MAGIC 0x31524F47u
static uint32_t __uninitialized_ram(rom_magic);
static uint32_t __uninitialized_ram(rom_in_psram);
static uint32_t __uninitialized_ram(rom_psram_size);

/* Flash is an external chip, its size does not depend on the board header.
 * Read it from JEDEC ID (0x9F) like murmulator_os2: the third ID byte is
 * log2(capacity).  Called first thing in main(), before overclocking. */
static size_t flash_bytes = PICO_FLASH_SIZE_BYTES;

static void __no_inline_not_in_flash_func(detect_flash_size)() {
    uint8_t tx[4] = { 0x9f, 0, 0, 0 };
    uint8_t rx[4] = { 0 };
    const uint32_t ints = save_and_disable_interrupts();
#if PICO_RP2350
    /* the SDK re-enters XIP with the boot-time QMI window-0 setup */
    const uint32_t timing = qmi_hw->m[0].timing;
    const uint32_t rfmt = qmi_hw->m[0].rfmt;
    const uint32_t rcmd = qmi_hw->m[0].rcmd;
#endif
    flash_do_cmd(tx, rx, sizeof tx);
#if PICO_RP2350
    qmi_hw->m[0].timing = timing;
    qmi_hw->m[0].rfmt = rfmt;
    qmi_hw->m[0].rcmd = rcmd;
    __asm volatile ("dsb; isb" ::: "memory");
#endif
    restore_interrupts(ints);
    /* 16 MiB is the XIP window of one chip select */
    if (rx[3] >= 20 && rx[3] <= 24)
        flash_bytes = (size_t)1 << rx[3];
}

/* The ROM goes right after the firmware; the top 512 KB of the flash stay
 * untouched (pico-launcher lives in the last 260 KB). */
static size_t flash_rom_capacity() {
    const size_t end = flash_bytes - (512u << 10);
    return end > FLASH_TARGET_OFFSET ? end - FLASH_TARGET_OFFSET : 0;
}

static bool rom_fits(const size_t size) {
    return size != 0 && (size <= flash_rom_capacity() ||
                         (gb_psram_available() && size <= gb_psram_size()));
}

/* the cartridge "Run previous" (SELECT) starts */
static const uint8_t* previous_rom() {
    if (rom_magic == ROM_INFO_MAGIC && rom_in_psram &&
        gb_psram_available() && rom_psram_size <= gb_psram_size())
        return (const uint8_t *)GB_PSRAM_BASE;
    return FLASH_ROM;
}

/* All watchdog resets (Ctrl+Alt+Del) take the same route: release QMI CS1 so
 * the PSRAM does not stay in QPI mode across reset, then drop clk_sys before
 * VREG, so the boot ROM and the next firmware start at a safe clock. */
extern "C" void __no_inline_not_in_flash_func(gb_safe_reset)(void) {
    watchdog_enable(100, true); // failsafe in case QMI/clock transition stalls
    save_and_disable_interrupts();
#if PICO_RP2350
    gb_psram_prepare_reset();
    set_sys_clock_khz(150000, false);
    vreg_set_voltage(VREG_VOLTAGE_1_10);
    sleep_ms(10);
#else
    set_sys_clock_khz(125000, false);
#endif
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

static uint8_t ram[32768];

semaphore vga_start_semaphore;
// set by the render core once text/graphics buffers are attached to the video driver
static volatile bool render_ready = false;

gb_s gb;

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];
static FATFS fs;

uint16_t stream[AUDIO_BUFFER_SIZE_BYTES];

typedef uint32_t palette222_t[3][4];
static palette222_t palette;
static palette_t palette16; // Colour palette

/* Colours of Game Boy (not Color) games, as in pico-wonderswan: each of the
 * three palettes (background, sprites 0, sprites 1 - 4 shades each) has its
 * own selector: AUTO / the GBC boot palettes (the colours this palette gets
 * there), the pico-gamate four-colour palettes, Random or Custom.  The palette
 * editor shows all 12 colours; editing one turns its palette into Custom.
 * One selector above them sets all three; they are chosen one by one only
 * when it is CUSTOM. */
enum palette_layer_e : uint8_t { LAYER_BG = 0, LAYER_OBJ0, LAYER_OBJ1, LAYER_COUNT };
/* the core (LCD_PALETTE_ALL) and gbcolors.h number them OBJ0, OBJ1, BG */
static const uint8_t layer_hw[LAYER_COUNT] = { 2, 0, 1 };
static constexpr uint8_t PALETTE_AUTO = 0;                               // by the game (GBC boot ROM)
static constexpr uint8_t PALETTE_MONO_FIRST = NUMBER_OF_MANUAL_PALETTES; // pico-gamate 4-colour palettes
static constexpr uint8_t PALETTE_RANDOM = PALETTE_MONO_FIRST + MONO_PALETTES_NUMBER;
static constexpr uint8_t PALETTE_CUSTOM = PALETTE_RANDOM + 1;
/* the selector for all three palettes; with CUSTOM each has its own one */
static uint8_t palette_all = PALETTE_AUTO;
static uint8_t palette_index[LAYER_COUNT] = { PALETTE_AUTO, PALETTE_AUTO, PALETTE_AUTO };
static uint32_t pal_rgb[LAYER_COUNT][4];    // the colours in use, RGB888
static uint32_t random_rgb[LAYER_COUNT][4]; // what Random drew
static uint32_t custom_rgb[LAYER_COUNT][4] = {
    {0xCCFFFF, 0xFFB266, 0xCC0066, 0x663300},
    {0xCCFFFF, 0xFFB266, 0xCC0066, 0x663300},
    {0xCCFFFF, 0xFFB266, 0xCC0066, 0x663300},
};
static bool game_ini_linked = false;
static char rom_file[80] = "";      // the ROM file name, for the game palette

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

static uint8_t swap_ab = 0;
static input_bits_t keyboard = { false, false, false, false, false, false, false, false }; //Keyboard
static input_bits_t gamepad_bits = { false, false, false, false, false, false, false, false }; //Joypad
//-----------------------------------------------------------------------------

void nespad_tick() {
    nespad_read();

    if (swap_ab) {
        gamepad_bits.b = keyboard.a || (nespad_state & DPAD_A) != 0;
        gamepad_bits.a = keyboard.b || (nespad_state & DPAD_B) != 0;
    } else {
        gamepad_bits.a = keyboard.a || (nespad_state & DPAD_A) != 0;
        gamepad_bits.b = keyboard.b || (nespad_state & DPAD_B) != 0;
    }
    gamepad_bits.select = keyboard.select || (nespad_state & DPAD_SELECT) != 0;
    gamepad_bits.start = keyboard.start || (nespad_state & DPAD_START) != 0;
    gamepad_bits.up = keyboard.up || (nespad_state & DPAD_UP) != 0;
    gamepad_bits.down = keyboard.down || (nespad_state & DPAD_DOWN) != 0;
    gamepad_bits.left = keyboard.left || (nespad_state & DPAD_LEFT) != 0;
    gamepad_bits.right = keyboard.right || (nespad_state & DPAD_RIGHT) != 0;
}

//-----------------------------------------------------------------------------


static bool isInReport(hid_keyboard_report_t const* report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

static volatile bool altPressed = false;
static volatile bool ctrlPressed = false;
static volatile uint8_t fxPressedV = 0;
static volatile bool kbd_page_up = false;
static volatile bool kbd_page_down = false;
static volatile bool kbd_home = false;
static volatile bool kbd_end = false;

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /*    printf("HID key report modifiers %2.2X report ", report->modifier);
        for (unsigned char i: report->keycode)
            printf("%2.2X", i);
        printf("\r\n");*/
    keyboard.start = isInReport(report, HID_KEY_ENTER) || isInReport(report, HID_KEY_KEYPAD_ENTER);
    keyboard.select = isInReport(report, HID_KEY_BACKSPACE) || isInReport(report, HID_KEY_ESCAPE) || isInReport(report, HID_KEY_KEYPAD_ADD);

    keyboard.a = isInReport(report, HID_KEY_Z) || isInReport(report, HID_KEY_O) || isInReport(report, HID_KEY_KEYPAD_0);
    keyboard.b = isInReport(report, HID_KEY_X) || isInReport(report, HID_KEY_P) || isInReport(report, HID_KEY_KEYPAD_DECIMAL);

    bool b7 = isInReport(report, HID_KEY_KEYPAD_7);
    bool b9 = isInReport(report, HID_KEY_KEYPAD_9);
    bool b1 = isInReport(report, HID_KEY_KEYPAD_1);
    bool b3 = isInReport(report, HID_KEY_KEYPAD_3);

    const bool wasd = !kbd_hex_mode;
    keyboard.up = b7 || b9 || isInReport(report, HID_KEY_ARROW_UP) || (wasd && isInReport(report, HID_KEY_W)) || isInReport(report, HID_KEY_KEYPAD_8);
    keyboard.down = b1 || b3 || isInReport(report, HID_KEY_ARROW_DOWN) || (wasd && isInReport(report, HID_KEY_S)) || isInReport(report, HID_KEY_KEYPAD_2) || isInReport(report, HID_KEY_KEYPAD_5);
    keyboard.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) || (wasd && isInReport(report, HID_KEY_A)) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT)  || (wasd && isInReport(report, HID_KEY_D)) || isInReport(report, HID_KEY_KEYPAD_6);

    // hex digits for the palette editor: 0-9 of the main block, A-F
    if (kbd_hex_mode) {
        static const uint8_t hex_keys[16] = {
            HID_KEY_0, HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4, HID_KEY_5, HID_KEY_6, HID_KEY_7,
            HID_KEY_8, HID_KEY_9, HID_KEY_A, HID_KEY_B, HID_KEY_C, HID_KEY_D, HID_KEY_E, HID_KEY_F
        };
        for (int8_t h = 0; h < 16; h++)
            if (isInReport(report, hex_keys[h]) && !isInReport(prev_report, hex_keys[h]))
                kbd_hex_key = h;
    }

    kbd_page_up = isInReport(report, HID_KEY_PAGE_UP);
    kbd_page_down = isInReport(report, HID_KEY_PAGE_DOWN);
    kbd_home = isInReport(report, HID_KEY_HOME);
    kbd_end = isInReport(report, HID_KEY_END);

    kbd_page_up = isInReport(report, HID_KEY_PAGE_UP);
    kbd_page_down = isInReport(report, HID_KEY_PAGE_DOWN);
    kbd_home = isInReport(report, HID_KEY_HOME);
    kbd_end = isInReport(report, HID_KEY_END);

    altPressed = isInReport(report, HID_KEY_ALT_LEFT) || isInReport(report, HID_KEY_ALT_RIGHT);
    ctrlPressed = isInReport(report, HID_KEY_CONTROL_LEFT) || isInReport(report, HID_KEY_CONTROL_RIGHT);

    if (altPressed && ctrlPressed && isInReport(report, HID_KEY_DELETE)) {
        gb_safe_reset();
    }
    if (ctrlPressed || altPressed) {
        uint8_t fxPressed = 0;
        if (isInReport(report, HID_KEY_F1)) fxPressed = 1;
        else if (isInReport(report, HID_KEY_F2)) fxPressed = 2;
        else if (isInReport(report, HID_KEY_F3)) fxPressed = 3;
        else if (isInReport(report, HID_KEY_F4)) fxPressed = 4;
        else if (isInReport(report, HID_KEY_F5)) fxPressed = 5;
        else if (isInReport(report, HID_KEY_F6)) fxPressed = 6;
        else if (isInReport(report, HID_KEY_F7)) fxPressed = 7;
        else if (isInReport(report, HID_KEY_F8)) fxPressed = 8;
        fxPressedV = fxPressed;
    }
    else {
        /* palette keys (as in pico-wonderswan): F1..F3 - the next palette of
         * BG / OBJ0 / OBJ1 (with Shift - the previous one), F6 - shuffle,
         * F7 - shuffle and save, F8 - save the colours for this game,
         * F9 - all AUTO, F12 - the palette editor */
        const bool shift = isInReport(report, HID_KEY_SHIFT_LEFT) || isInReport(report, HID_KEY_SHIFT_RIGHT);
        static const uint8_t layer_keys[LAYER_COUNT] = { HID_KEY_F1, HID_KEY_F2, HID_KEY_F3 };
        for (int8_t layer = 0; layer < LAYER_COUNT; layer++)
            if (isInReport(report, layer_keys[layer]) && !isInReport(prev_report, layer_keys[layer]))
                palette_layer_cycle_requested = shift ? -(layer + 1) : layer + 1;
        if (isInReport(report, HID_KEY_F6) && !isInReport(prev_report, HID_KEY_F6))
            palette_shuffle_requested_no_save = true;
        if (isInReport(report, HID_KEY_F7) && !isInReport(prev_report, HID_KEY_F7))
            palette_shuffle_requested = true;
        if (isInReport(report, HID_KEY_F8) && !isInReport(prev_report, HID_KEY_F8))
            palette_save_requested = true;
        if (isInReport(report, HID_KEY_F9) && !isInReport(prev_report, HID_KEY_F9))
            palette_default_requested = true;
        if (isInReport(report, HID_KEY_F12) && !isInReport(prev_report, HID_KEY_F12))
            palette_editor_requested = true;
    }
}

Ps2Kbd_Mrmltr ps2kbd(
    pio1,
    PS2KBD_GPIO_FIRST,
    process_kbd_report
);

enum : uint8_t {
    BTN_UP     = 1u << 0,
    BTN_DOWN   = 1u << 1,
    BTN_LEFT   = 1u << 2,
    BTN_RIGHT  = 1u << 3,
    BTN_A      = 1u << 4,
    BTN_B      = 1u << 5,
    BTN_START  = 1u << 6,
    BTN_SELECT = 1u << 7,
};

/* keyboard and NES pad together (nespad_tick() merges them) */
static uint8_t input_held() {
    uint8_t b = 0;
    if (gamepad_bits.up) b |= BTN_UP;
    if (gamepad_bits.down) b |= BTN_DOWN;
    if (gamepad_bits.left) b |= BTN_LEFT;
    if (gamepad_bits.right) b |= BTN_RIGHT;
    if (gamepad_bits.a) b |= BTN_A;
    if (gamepad_bits.b) b |= BTN_B;
    if (gamepad_bits.start) b |= BTN_START;
    if (gamepad_bits.select) b |= BTN_SELECT;
    return b;
}

/* Buttons that are already down when the mode changes (the Enter that started
 * a ROM, the Start+Select that opened the menu, Enter on "Return to game",
 * Enter on a directory...) are ignored until they are released. */
struct input_gate_t {
    uint8_t mask = 0;

    void arm() { mask = input_held(); }

    uint8_t read() {
        const uint8_t held = input_held();
        mask &= held;
        return held & (uint8_t)~mask;
    }
};

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t __not_in_flash_func(gb_rom_read)(struct gb_s* gb, const uint_fast32_t addr) {
    return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t __not_in_flash_func(gb_cart_ram_read)(struct gb_s* gb, const uint_fast32_t addr) {
    return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void __not_in_flash_func(gb_cart_ram_write)(struct gb_s* gb, const uint_fast32_t addr, const uint8_t val) {
    ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s* gb, const enum gb_error_e gb_err, const uint16_t addr) {
    const char* gb_err_str[4] = {
        "UNKNOWN",
        "INVALID OPCODE",
        "INVALID READ",
        "INVALID WRITE"
    };
    printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
}

/* The picture is triple buffered.  The emulator draws a whole frame in a few
 * milliseconds and then waits for the sound, while the video driver shows the
 * buffer at its own rate: with a single buffer the screen showed a mix of the
 * old and the new frame (sprites of the new frame over the background of the
 * old one, or the other way round).  Now the emulator draws into the back
 * buffer, a complete frame becomes pending, and the render core shows it in
 * the vertical blank of the display together with its palette.  A frame still
 * pending when the next one is complete is dropped, so the emulator never
 * waits for the display.  SCREEN is buffer 0: the text mode and the flash
 * loader use it as before. */
static uint8_t SCREEN_2[2][LCD_HEIGHT][LCD_WIDTH];
static uint8_t (*const frames[3])[LCD_WIDTH] = {SCREEN, SCREEN_2[0], SCREEN_2[1]};

/* Game Boy Color palettes often change in the middle of a frame (the status
 * bar under the play field gets its own colours).  The buffers keep 8-bit
 * colour indices, and the video driver turns them into colours while it shows
 * the frame: the lines drawn before a mid-frame change keep their set, and the
 * following lines use the next one: up to 3 sets of 64 colours, palette slots
 * 0..191.  Every frame keeps its own copy of the sets; they go to the video
 * driver when the frame is shown, so the frame on the screen never changes its
 * colours.
 * "Hi-colour" pictures change the palettes on almost every line (thousands of
 * colours in a frame).  A frame that needs more than 3 sets is drawn in a
 * fixed 6x6x5 colour cube instead (slots 0..179, 2x2 ordered dither), with the
 * colours each line had when it was drawn. */
#define CGB_PAL_SETS 3
#define CGB_PAL_SIZE (CGB_PAL_SETS * 64)
#define CGB_CUBE 0xff // frame_sets[]: the frame uses the colour cube
#define CGB_CUBE_SIZE (6 * 6 * 5)
static uint32_t frame_pal[3][CGB_PAL_SIZE];
static uint8_t frame_sets[3]; // sets the frame uses, 0 for a Game Boy frame
static uint32_t cube_pal[CGB_CUBE_SIZE];

static void cube_pal_init() {
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 5; b++)
                cube_pal[(r * 6 + g) * 5 + b] = RGB888(r * 255 / 5, g * 255 / 5, b * 255 / 4);
}

static critical_section_t frame_lock;
static volatile int8_t frame_front = 0;    // on the screen (render core)
static volatile int8_t frame_pending = -1; // complete, to be shown
static int8_t frame_back = 1;              // being drawn (emulator core)

static uint32_t hw_pal[CGB_PAL_SIZE];      // what the video driver holds (render core)
static volatile bool hw_pal_invalid = true;

/* emulator core: the back buffer holds a complete frame */
static void frame_complete() {
    critical_section_enter_blocking(&frame_lock);
    const int8_t done = frame_back;
    const int8_t dropped = frame_pending;
    frame_pending = done;
    frame_back = dropped >= 0 ? dropped : 3 - frame_front - done;
    critical_section_exit(&frame_lock);
}

/* render core, in the vertical blank: show the pending frame */
static void __time_critical_func(frame_show)() {
    critical_section_enter_blocking(&frame_lock);
    const int8_t p = frame_pending;
    if (p >= 0) {
        frame_front = p;
        frame_pending = -1;
    }
    critical_section_exit(&frame_lock);
    if (p < 0) return;
    // the emulator core writes neither this buffer nor its palette now
    if (frame_sets[p]) {
        if (hw_pal_invalid) {
            hw_pal_invalid = false;
            memset(hw_pal, 0xff, sizeof hw_pal); // no colour has all bits set
        }
        const bool cube = frame_sets[p] == CGB_CUBE;
        const uint32_t* pal = cube ? cube_pal : frame_pal[p];
        const unsigned int n = cube ? CGB_CUBE_SIZE : frame_sets[p] * 64u;
        for (unsigned int i = 0; i < n; i++) {
            if (hw_pal[i] != pal[i]) {
                hw_pal[i] = pal[i];
                graphics_set_palette(i, pal[i]);
            }
        }
    }
    graphics_set_buffer((uint8_t *)frames[p], LCD_WIDTH, LCD_HEIGHT);
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    multicore_lockout_victim_init();
#ifdef KBDUSB
    tuh_init(BOARD_TUH_RHPORT);
#endif
    graphics_init();

    const auto buffer = (uint8_t *)SCREEN;
    graphics_set_buffer(buffer, LCD_WIDTH, LCD_HEIGHT);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);

#if VGA
    graphics_set_offset(60, 6);
#endif
#if HDMI | TV
    graphics_set_offset(80, 48);
#endif
#if SOFTTV
    graphics_set_offset(0, 48); // the TV driver centres the picture itself, with square pixels
#endif

    graphics_set_flashmode(false, false);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    // clrScr(1);

    render_ready = true;
    sem_acquire_blocking(&vga_start_semaphore);

    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
#ifdef TFT
    uint64_t last_renderer_tick = tick;
#endif
    uint64_t last_input_tick = tick;
#ifndef TFT
    uint32_t frame_count = graphics_frame_count;
#endif
    while (true) {
#ifdef TFT
        if (tick >= last_renderer_tick + frame_tick) {
            frame_show();
            refresh_lcd();
            last_renderer_tick = tick;
        }
#else
        if (frame_count != graphics_frame_count) {
            frame_count = graphics_frame_count; // the vertical blank of the display
            frame_show();
        }
#endif
        if (tick >= last_input_tick + frame_tick * 1) {
            ps2kbd.tick();
            nespad_tick();
            last_input_tick = tick;
        }
        tick = time_us_64();


#ifdef KBDUSB
        tuh_task();
#endif
        tight_loop_contents();
    }

    __unreachable();
}


/**
 * Draws scanline into framebuffer.
 */
/* A game rewrites a palette in a burst of writes that may span up to 4 lines;
 * the whole burst goes into one set (the lines drawn during it show its final
 * colours).  Writes in the vertical blank need nothing: every frame starts
 * with the current colours in set 0. */
static uint8_t cgb_set = 0;                       // the set for the next lines
static uint8_t cgb_need = 1;                      // sets the frame would need
static bool cgb_set_used = false;                 // a line was drawn with it
static uint8_t cgb_last_y = LCD_HEIGHT - 1;
static int16_t cgb_burst_y = -16;                 // line where the last burst began
static bool frame_started = false;                // line 0 is in the back buffer
static bool cgb_cube = false;                     // the frame is drawn in the cube
static bool cgb_cube_next = false;                // so is the next one
static uint16_t cgb_raw[CGB_PAL_SETS][64];        // the sets as GBC colours
static uint8_t cube_q[4][64];                     // cube colour per dither position

static uint16_t cgb_raw_color(const uint8_t i) {
    const uint8_t* const p = i < 32 ? &gb.cgb.BGPalette[i * 2] : &gb.cgb.OAMPalette[(i - 32) * 2];
    return p[0] | p[1] << 8; // bits 0-4 red, 5-9 green, 10-14 blue
}

/* 2x2 ordered dither: position (x & 1) + (y & 1) * 2 */
static inline uint8_t cube_index(const uint16_t c, const unsigned int pos) {
    static const uint8_t bayer[4] = {0, 2, 3, 1};
    const unsigned int k = 2 * bayer[pos] + 1; // threshold (k / 8) of a step
    const unsigned int r = (c & 31) * 40 + k * 31;
    const unsigned int g = (c >> 5 & 31) * 40 + k * 31;
    const unsigned int b = (c >> 10 & 31) * 32 + k * 31;
    return (r / 248 * 6 + g / 248) * 5 + b / 248;
}

static void cube_set_color(const uint8_t i) {
    const uint16_t c = cgb_raw_color(i);
    for (unsigned int pos = 0; pos < 4; pos++)
        cube_q[pos][i] = cube_index(c, pos);
}

static void cgb_load_set(const uint8_t set) {
    memcpy(&frame_pal[frame_back][set * 64], gb.cgb.fixPalette, sizeof gb.cgb.fixPalette);
    for (uint8_t i = 0; i < 64; i++)
        cgb_raw[set][i] = cgb_raw_color(i);
}

/* a 4th set is needed: the lines drawn so far go into the cube too */
static void cgb_enter_cube() {
    static uint8_t q[4][CGB_PAL_SIZE];
    for (unsigned int v = 0; v < (cgb_set + 1u) * 64; v++)
        for (unsigned int pos = 0; pos < 4; pos++)
            q[pos][v] = cube_index(cgb_raw[v >> 6][v & 63], pos);
    for (unsigned int y = 0; y <= cgb_last_y; y++) {
        uint8_t* const line = frames[frame_back][y];
        const uint8_t* const q0 = q[(y & 1) * 2];
        const uint8_t* const q1 = q[(y & 1) * 2 + 1];
        for (unsigned int x = 0; x < LCD_WIDTH; x += 2) {
            line[x] = q0[line[x]];
            line[x + 1] = q1[line[x + 1]];
        }
    }
    for (uint8_t i = 0; i < 64; i++)
        cube_set_color(i);
    cgb_cube = true;
}

/* a new game, a loaded state, the menu: the video driver gets all colours again */
static void cgb_palette_reset() {
    cgb_set = 0;
    cgb_need = 1;
    cgb_set_used = false;
    cgb_last_y = LCD_HEIGHT - 1;
    cgb_burst_y = -16;
    cgb_cube = cgb_cube_next = false;
    hw_pal_invalid = true;
}

void gb_cgb_palette_changed(const uint8_t index) {
    if (cgb_last_y >= LCD_HEIGHT - 1)
        return; // vertical blank (or LCD off after a full frame)
    if (cgb_last_y > cgb_burst_y + 3 && cgb_set_used) { // a new burst
        cgb_burst_y = cgb_last_y;
        cgb_set_used = false;
        if (cgb_need < 255) cgb_need++;
        if (!cgb_cube) {
            if (cgb_set < CGB_PAL_SETS - 1) {
                cgb_set++; // keep the lines already drawn in their colours
                cgb_load_set(cgb_set);
                return;
            }
            cgb_enter_cube();
            return;
        }
    }
    if (cgb_cube)
        cube_set_color(index);
    else {
        frame_pal[frame_back][cgb_set * 64 + index] = gb.cgb.fixPalette[index];
        cgb_raw[cgb_set][index] = cgb_raw_color(index);
    }
}

void __always_inline lcd_draw_line(struct gb_s* gb, const uint8_t pixels[160], const uint_fast8_t y) {
    uint8_t* const line = frames[frame_back][y];
    if (y == 0) frame_started = true;
    if (gb->cgb.cgbMode) {
        if (y == 0) {
            cgb_burst_y = -16;
            cgb_set = 0;
            cgb_need = 1;
            cgb_cube = cgb_cube_next;
            if (cgb_cube) {
                for (uint8_t i = 0; i < 64; i++)
                    cube_set_color(i);
            }
            else
                cgb_load_set(0);
        }
        cgb_last_y = y;
        cgb_set_used = true;
        if (cgb_cube) {
            const uint8_t* const q0 = cube_q[(y & 1) * 2];
            const uint8_t* const q1 = cube_q[(y & 1) * 2 + 1];
            for (unsigned int x = 0; x < LCD_WIDTH; x += 2) {
                line[x] = q0[pixels[x]];
                line[x + 1] = q1[pixels[x + 1]];
            }
        }
        else if (cgb_set == 0) {
            memcpy((uint32_t *)line, (uint32_t *)pixels, 160);
        }
        else {
            const uint8_t base = cgb_set * 64;
            for (unsigned int x = 0; x < LCD_WIDTH; x++)
                line[x] = pixels[x] + base;
        }
    }
    else {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
            line[x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    }
    if (y == LCD_HEIGHT - 1 && frame_started) {
        frame_started = false;
        if (gb->cgb.cgbMode) {
            frame_sets[frame_back] = cgb_cube ? CGB_CUBE : cgb_set + 1;
            cgb_cube_next = cgb_need > CGB_PAL_SETS; // the next frame is likely the same
        }
        else
            frame_sets[frame_back] = 0;
        frame_complete();
    }
}

/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s* gb) {
    char filename[24];
    uint_fast32_t save_size;
    UINT br;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {
        FIL fil;
        FRESULT fr = f_open(&fil, filename, FA_READ);
        if (fr == FR_OK) {
            f_read(&fil, ram, f_size(&fil), &br);
        }
        else {
            printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK) {
            printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
    }
    printf("I read_cart_ram_file(%s) COMPLETE (%u bytes)\n", filename, save_size);
}

/**
 * Write a save file to the SD card
 */
void write_cart_ram_file(struct gb_s* gb) {
    char filename[16];
    uint_fast32_t save_size;
    UINT bw;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {
        FIL fil;
        FRESULT fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) {
            f_write(&fil, ram, save_size, &bw);
        }
        else {
            printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK) {
            printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
    }
    printf("I write_cart_ram_file(%s) COMPLETE (%u bytes)\n", filename, save_size);
}


typedef struct __attribute__((__packed__)) {
    bool is_directory;
    bool is_executable;
    size_t size;
    char filename[79];
} file_item_t;

constexpr int max_files = 500;
static file_item_t fileItems[max_files];

int compareFileItems(const void* a, const void* b) {
    const auto* itemA = (file_item_t *)a;
    const auto* itemB = (file_item_t *)b;
    // Directories come first
    if (itemA->is_directory && !itemB->is_directory)
        return -1;
    if (!itemA->is_directory && itemB->is_directory)
        return 1;
    // Sort files alphabetically
    return strcmp(itemA->filename, itemB->filename);
}

bool isExecutable(const char pathname[255],const char *extensions) {
    char *pathCopy = strdup(pathname);
    const char* token = strrchr(pathCopy, '.');

    if (token == nullptr) {
        return false;
    }

    token++;

    while (token != NULL) {
        if (strstr(extensions, token) != NULL) {
            free(pathCopy);
            return true;
        }
        token = strtok(NULL, ",");
    }
    free(pathCopy);
    return false;
}

#if PICO_RP2350
/* flash_range_erase/program leave XIP through the boot-time XIP setup, which
 * restores the QMI window-0 timing chosen for the boot clock.  overclock()
 * runs the flash with its own timing at 378+ MHz, so the boot value would
 * clock the flash far too fast afterwards and every XIP fetch would return
 * garbage.  Keep the overclock setup; nothing runs from flash in between. */
static void __no_inline_not_in_flash_func(flash_write_sector)(const uint32_t offset, const uint8_t* data) {
    const uint32_t qmi_timing = qmi_hw->m[0].timing;
    const uint32_t qmi_rfmt = qmi_hw->m[0].rfmt;
    const uint32_t qmi_rcmd = qmi_hw->m[0].rcmd;
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    qmi_hw->m[0].timing = qmi_timing;
    qmi_hw->m[0].rfmt = qmi_rfmt;
    qmi_hw->m[0].rcmd = qmi_rcmd;
    __asm volatile ("dsb; isb" ::: "memory");
}
#else
static void __no_inline_not_in_flash_func(flash_write_sector)(const uint32_t offset, const uint8_t* data) {
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
}
#endif

/* QSPI PSRAM, when found, takes the ROM directly (no flash wear, no erase);
 * otherwise it goes to flash sector by sector: a sector that already holds
 * the same data is not erased again, and every written sector is verified. */
bool filebrowser_loadfile(const char pathname[256]) {
    FIL file;
    FILINFO fileinfo;

    const int window_w = (int)TEXTMODE_COLS < 43 ? (int)TEXTMODE_COLS : 43; // TV NTSC has only 40 columns
    const int window_y = (TEXTMODE_ROWS - 5) / 2;
    const int window_x = ((int)TEXTMODE_COLS - window_w) / 2;

    auto fail = [&](const char* message) {
        draw_text(message, window_x + 1, window_y + 2, 13, 1);
        sleep_ms(3000);
        return false;
    };

    draw_window("Loading ROM", window_x, window_y, window_w, 5);

    if (FR_OK != f_stat(pathname, &fileinfo) || fileinfo.fsize == 0)
        return fail("ERROR: Can't read the ROM file!   ");

    const size_t load_size = fileinfo.fsize;
    const bool use_psram = gb_psram_available() && load_size <= gb_psram_size();
    if (!use_psram && load_size > flash_rom_capacity())
        return fail("ERROR: ROM too large! Canceled!!  ");

    if (FR_OK != f_open(&file, pathname, FA_READ))
        return fail("ERROR: Can't open the ROM file!   ");

    draw_text(use_psram ? "Loading into QSPI PSRAM..." : "Loading into Flash...",
              window_x + 1, window_y + 2, 10, 1);

    size_t total_read = 0;
    UINT bytes_read = 0;
    bool verify_failed = false;

    if (use_psram) {
        rom_magic = 0; // the PSRAM copy is being overwritten
        uint8_t* dst = (uint8_t *)GB_PSRAM_BASE;
        while (total_read < load_size) {
            const UINT chunk = load_size - total_read > 4096 ? 4096 : (UINT)(load_size - total_read);
            if (FR_OK != f_read(&file, dst + total_read, chunk, &bytes_read) || bytes_read != chunk)
                break;
            total_read += bytes_read;
        }
    }
    else {
        /* the sector buffer lives in the tail of SCREEN, behind the text
         * mode: the game redraws the whole picture anyway */
        uint8_t* const buffer = &SCREEN[0][0] + sizeof(SCREEN) - FLASH_SECTOR_SIZE;
#ifdef TEXTMODE_COLS_MAX
        static_assert(TEXTMODE_COLS_MAX * TEXTMODE_ROWS * 2 + FLASH_SECTOR_SIZE <= sizeof(SCREEN),
                      "no room for the flash sector buffer");
#else
        static_assert(TEXTMODE_COLS * TEXTMODE_ROWS * 2 + FLASH_SECTOR_SIZE <= sizeof(SCREEN),
                      "no room for the flash sector buffer");
#endif
        static_assert(sizeof(SCREEN) % 4 == 0, "flash sector buffer must stay word aligned");
        uint32_t target = FLASH_TARGET_OFFSET;
        while (total_read < load_size) {
            const UINT chunk = load_size - total_read > FLASH_SECTOR_SIZE
                                   ? FLASH_SECTOR_SIZE : (UINT)(load_size - total_read);
            memset(buffer, 0xff, FLASH_SECTOR_SIZE);
            if (FR_OK != f_read(&file, buffer, chunk, &bytes_read) || bytes_read != chunk)
                break;
            total_read += bytes_read;

            const auto* mapped = (const uint8_t *)(XIP_BASE + target);
            if (memcmp(mapped, buffer, FLASH_SECTOR_SIZE) != 0) {
                multicore_lockout_start_blocking();
                const uint32_t ints = save_and_disable_interrupts();
                flash_write_sector(target, buffer);
                restore_interrupts(ints);
                multicore_lockout_end_blocking();
                if (memcmp(mapped, buffer, FLASH_SECTOR_SIZE) != 0) {
                    verify_failed = true;
                    break;
                }
            }
            target += FLASH_SECTOR_SIZE;
            gpio_put(PICO_DEFAULT_LED_PIN, target >> 13 & 1);
        }
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        memset(buffer, 0, FLASH_SECTOR_SIZE);
    }
    f_close(&file);

    if (verify_failed)
        return fail("ERROR: Flash verify failed!       ");
    if (total_read != load_size)
        return fail("ERROR: ROM load failed!           ");

    if (use_psram) {
        rom_psram_size = load_size;
        rom_in_psram = 1;
        rom_magic = ROM_INFO_MAGIC;
        rom = (const uint8_t *)GB_PSRAM_BASE;
    }
    else {
        rom_in_psram = 0; // a PSRAM copy, if any, is no longer the latest ROM
        rom = FLASH_ROM;
    }
    return true;
}

/* Where the browser was left: re-entering it opens the same directory with
 * the same entry under the cursor (e.g. the ROM that was just played). */
static char fb_dir[256] = "\\GB";
static char fb_focus[80] = "";

static void fb_remember(const char* dir, const char* name) {
    strncpy(fb_dir, dir, sizeof fb_dir - 1);
    fb_dir[sizeof fb_dir - 1] = '\0';
    strncpy(fb_focus, name, sizeof fb_focus - 1);
    fb_focus[sizeof fb_focus - 1] = '\0';
}

/* Returns when a ROM is ready to run: a newly loaded one (START) or the
 * previous one (SELECT). */
void filebrowser(const char executables[11]) {
    input_gate_t gate;
    char basepath[256];
    char focus[80];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, fb_dir);
    strcpy(focus, fb_focus);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    DIR dir;
    FILINFO fileInfo;

    while (true) {
        gate.arm();
        memset(fileItems, 0, sizeof(file_item_t) * max_files);
        int total_files = 0;

        if (FR_OK != f_opendir(&dir, basepath)) {
            if (strcmp(basepath, HOME_DIR) != 0) {
                // the remembered directory is gone (another SD card?)
                strcpy(basepath, HOME_DIR);
                focus[0] = '\0';
                continue;
            }
            draw_text("Failed to open directory", 1, 1, 4, 0);
            while (true);
        }

        snprintf(tmp, TEXTMODE_COLS, "SD:\\%s", basepath);
        draw_window(tmp, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS - 1);
        draw_text(gb_psram_available() ? "QSPI PSRAM" : "FLASH", TEXTMODE_COLS - 11, 0, 10, 1);
        memset(tmp, ' ', TEXTMODE_COLS);


        draw_text(tmp, 0, 29, 0, 0);
        auto off = 0;
        draw_text("START", off, 29, 7, 0);
        off += 5;
        draw_text(" Run at cursor ", off, 29, 0, 3);
        off += 16;
        draw_text("SELECT", off, 29, 7, 0);
        off += 6;
        draw_text(" Run previous  ", off, 29, 0, 3);
#ifndef TFT
        off += 16;
        draw_text("ARROWS", off, 29, 7, 0);
        off += 6;
        draw_text(" Navigation    ", off, 29, 0, 3);
        off += 16;
        draw_text("A/F10", off, 29, 7, 0);
        off += 5;
        draw_text(" USB DRV ", off, 29, 0, 3);
#endif

        if (strlen(basepath) > 0) {
            strcpy(fileItems[total_files].filename, "..\0");
            fileItems[total_files].is_directory = true;
            fileItems[total_files].size = 0;
            total_files++;
        }

        while (f_readdir(&dir, &fileInfo) == FR_OK &&
               fileInfo.fname[0] != '\0' &&
               total_files < max_files
        ) {
            // Set the file item properties
            fileItems[total_files].is_directory = fileInfo.fattrib & AM_DIR;
            fileItems[total_files].size = fileInfo.fsize;
            fileItems[total_files].is_executable = !(fileInfo.fattrib & AM_DIR) &&
                                                   rom_fits(fileInfo.fsize) &&
                                                   isExecutable(fileInfo.fname, executables);
            strncpy(fileItems[total_files].filename, fileInfo.fname, 78);
            total_files++;
        }
        f_closedir(&dir);

        qsort(fileItems, total_files, sizeof(file_item_t), compareFileItems);

        if (total_files > max_files) {
            draw_text(" Too many files!! ", TEXTMODE_COLS - 17, 0, 12, 3);
        }

        int offset = 0;
        int current_item = 0;

        /* Scroll the remembered entry into view, near the middle of the page
         * when possible and clamped at the beginning/end of the list. */
        if (focus[0]) {
            for (int i = 0; i < total_files; ++i) {
                if (!strcmp(fileItems[i].filename, focus)) {
                    constexpr int preferred_row = per_page / 2;
                    const int max_offset = total_files > per_page ? total_files - per_page : 0;
                    offset = i > preferred_row ? i - preferred_row : 0;
                    if (offset > max_offset) offset = max_offset;
                    current_item = i - offset;
                    break;
                }
            }
            focus[0] = '\0';
        }

        while (true) {
            sleep_ms(100);

            const uint8_t keys = gate.read();

            // SELECT: run the previous cartridge
            if (keys & BTN_SELECT) {
                fb_remember(basepath, fileItems[offset + current_item].filename);
                rom = previous_rom();
                return;
            }

            if (keys & BTN_DOWN) {
                if (offset + (current_item + 1) < total_files) {
                    if (current_item + 1 < per_page) {
                        current_item++;
                    }
                    else {
                        offset++;
                    }
                }
            }

            if (keys & BTN_UP) {
                if (current_item > 0) {
                    current_item--;
                }
                else if (offset > 0) {
                    offset--;
                }
            }

            if (keys & BTN_RIGHT) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);
                }
            }

            if (keys & BTN_LEFT) {
                if (offset > per_page) {
                    offset -= per_page;
                }
                else {
                    offset = 0;
                    current_item = 0;
                }
            }

            if (total_files > 0 && (kbd_page_down || kbd_page_up || kbd_home || kbd_end)) {
                const int last = total_files - 1;
                const int per_page_half = per_page >> 1;
                const int max_offset = total_files > per_page_half ? total_files - per_page_half : 0;
                int pos = offset + current_item;
                if (kbd_page_down) {
                    pos += per_page_half;
                    offset += per_page_half;
                }
                else if (kbd_page_up) {
                    pos -= per_page_half;
                    offset -= per_page_half;
                }
                else if (kbd_home) {
                    pos = 0;
                    offset = 0;
                }
                else {
                    pos = last;
                    offset = max_offset;
                }
                if (pos > last) pos = last;
                if (pos < 0) pos = 0;
                if (offset > max_offset) offset = max_offset;
                if (offset < 0) offset = 0;
                if (pos < offset) offset = pos;
                if (pos >= offset + per_page_half) offset = pos - per_page_half + 1;
                current_item = pos - offset;
            }

            if (keys & BTN_START) {
                const auto& file_at_cursor = fileItems[offset + current_item];

                if (file_at_cursor.is_directory) {
                    if (strcmp(file_at_cursor.filename, "..") == 0) {
                        const char* lastBackslash = strrchr(basepath, '\\');
                        if (lastBackslash != nullptr) {
                            // come back to the directory we have just left
                            strncpy(focus, lastBackslash + 1, sizeof focus - 1);
                            focus[sizeof focus - 1] = '\0';
                            basepath[lastBackslash - basepath] = '\0';
                        }
                    }
                    else {
                        char subdir[256];
                        if (snprintf(subdir, sizeof subdir, "%s\\%s", basepath, file_at_cursor.filename) <
                            (int)sizeof subdir)
                            strcpy(basepath, subdir);
                    }
                    break;
                }

                if (file_at_cursor.is_executable) {
                    char fullpath[256];
                    if (snprintf(fullpath, sizeof fullpath, "%s\\%s", basepath, file_at_cursor.filename) <
                        (int)sizeof fullpath && filebrowser_loadfile(fullpath)) {
                        fb_remember(basepath, file_at_cursor.filename);
                        return;
                    }
                    // the error window covered the list: redraw it, same entry
                    strcpy(focus, file_at_cursor.filename);
                    break;
                }
            }

            for (int i = 0; i < per_page; i++) {
                uint8_t color = 11;
                uint8_t bg_color = 1;

                if (offset + i < max_files) {
                    const auto item = fileItems[offset + i];


                    if (i == current_item) {
                        color = 0;
                        bg_color = 3;
                        memset(tmp, 0xCD, TEXTMODE_COLS - 2);
                        tmp[TEXTMODE_COLS - 2] = '\0';
                        draw_text(tmp, 1, per_page + 1, 11, 1);
                        snprintf(tmp, TEXTMODE_COLS - 2, " Size: %iKb, File %lu of %i ", item.size / 1024,
                                 offset + i + 1,
                                 total_files);
                        draw_text(tmp, 2, per_page + 1, 14, 3);
                    }

                    const auto len = strlen(item.filename);
                    color = item.is_directory ? 15 : color;
                    color = item.is_executable ? 10 : color;
                    //color = strstr((char *)rom_filename, item.filename) != nullptr ? 13 : color;

                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                    tmp[TEXTMODE_COLS - 2] = '\0';
                    memcpy(&tmp, item.filename, len < TEXTMODE_COLS - 2 ? len : TEXTMODE_COLS - 2);
                }
                else {
                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                }
                draw_text(tmp, 1, i + 1, color, bg_color);
            }
        }
    }
}


bool restart = false;

enum menu_type_e {
    NONE,
    INT,
    TEXT,
    ARRAY,

    SAVE,
    LOAD,
    ROM_SELECT,
    GAME_INI,
    PALETTE_EDITOR,
    RETURN,
};

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char* text;
    menu_type_e type;
    const void* value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[49][18];
} MenuItem;

static int save_slot = 0;
#ifndef CPU_FREQ
#define CPU_FREQ 378
#endif
#define FREQ_STR_(x) #x
#define FREQ_STR(x) FREQ_STR_(x)
static uint16_t frequencies[] = { CPU_FREQ, 396, 404, 408, 412, 416, 420, 424, 433 };
static uint8_t frequency_index = 0;

bool overclock() {
#if PICO_RP2350
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(frequencies[frequency_index] * KHZ, false);
    *qmi_m0_timing = 0x60007303;
    gb_psram_reclock(); // QMI CS1 timing depends on clk_sys (no-op without PSRAM)
    return true;
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);
    return set_sys_clock_khz(frequencies[frequency_index] * KHZ, true);
#endif
}

/* The configuration, the game palettes and the saved states live in
 * /.config/gbc, apart from the games (as in pico-gamate); the settings are
 * kept per board and video output. */
#define CONFIG_DIR "/.config/gbc"

static const char* config_platform_name() {
#ifdef PICO_RP2350
    if (strcmp(PICO_BOARD, "murmulator2") == 0) return "m2p2";
    if (strcmp(PICO_BOARD, "olimex-pico-pc") == 0) return "pcp2";
    if (strcmp(PICO_BOARD, "waveshare_rp2350_pizero") == 0) return "z0p2";
    return "m1p2";
#else
    if (strcmp(PICO_BOARD, "murmulator2") == 0) return "m2p1";
    if (strcmp(PICO_BOARD, "olimex-pico-pc") == 0) return "pcp1";
    if (strcmp(PICO_BOARD, "waveshare_rp2040_pizero") == 0) return "z0p1";
    return "m1p1";
#endif
}

static const char* config_video_name() {
#if TFT
    return "tft";
#elif HDMI
    return "hdmi";
#elif TV
    return "tv";
#elif SOFTTV
    return "softtv";
#else
    return "vga";
#endif
}

static void config_mkdirs() {
    char path[64];
    f_mkdir("/.config"); // nothing happens if it is already there
    f_mkdir(CONFIG_DIR);
    snprintf(path, sizeof path, CONFIG_DIR "/%s", config_platform_name());
    f_mkdir(path);
    snprintf(path, sizeof path, CONFIG_DIR "/%s/%s", config_platform_name(), config_video_name());
    f_mkdir(path);
}

static void state_path(char* pathname, size_t size, const char* dir) {
    char filename[24];
    gb_get_rom_name(&gb, filename);
    if (save_slot)
        snprintf(pathname, size, "%s/%s_%d.save", dir, filename, save_slot);
    else
        snprintf(pathname, size, "%s/%s.save", dir, filename);
}

static bool save() {
    char pathname[255];
    config_mkdirs();
    state_path(pathname, sizeof pathname, CONFIG_DIR);

    static FIL fd; // static: core 0 has a small stack
    if (FR_OK != f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE))
        return true;
    UINT bw;
    f_write(&fd, &gb, sizeof(gb), &bw);
    f_write(&fd, ram, sizeof(ram), &bw);
    f_close(&fd);

    return true;
}

static bool load() {
    char pathname[255];
    static FIL fd; // static: core 0 has a small stack
    state_path(pathname, sizeof pathname, CONFIG_DIR);
    if (FR_OK != f_open(&fd, pathname, FA_READ)) {
        state_path(pathname, sizeof pathname, HOME_DIR); // saved by an older version
        if (FR_OK != f_open(&fd, pathname, FA_READ))
            return true;
    }
    UINT br;
    f_read(&fd, &gb, sizeof(gb), &br);
    f_read(&fd, ram, sizeof(ram), &br);
    f_close(&fd);
    cgb_palette_reset(); // the loaded GBC palette is not in the video driver yet
    return true;
}
/* TV-SOFT: a TV standard is a coherent set (lines, colour subcarrier,
 * sync), so the menu only selects the system; the game is always in colour. */
static uint8_t tv_system = 0; // 0 - PAL, 1 - NTSC
/* HDMI / VGA / TV-SOFT: picture size (enum graphics_scale_t), by default as
 * the drivers showed it before: HDMI and TV 2x, VGA 3x */
#if VGA
static uint8_t screen_scale = 1;
#else
static uint8_t screen_scale = 0;
#endif
#ifdef TEXTMODE_COLS_MAX // text columns change at run time (TV-SOFT)
#define TEXT_BUFFER_COLS TEXTMODE_COLS_MAX
#else
#define TEXT_BUFFER_COLS TEXTMODE_COLS
#endif
#ifdef TEXTMODE_COLS_MAX // text columns change at run time (TV-SOFT)
#define TEXT_BUFFER_COLS TEXTMODE_COLS_MAX
#else
#define TEXT_BUFFER_COLS TEXTMODE_COLS
#endif

static const uint32_t preset_rgb0[] = {
    0xD4FFFD, 0xD4FFF3, 0xFFE9FA, 0xE9E9FF, 0xE9FAFF, 0xE9FFF4,
    0xF5FFE9, 0xFFF8E9, 0xFFF2F0, 0xD4FFDA, 0xEDFFD4
};
static const uint32_t preset_rgb1[] = {
    // Cold (B4:G4), then warm (H4:N4).
    0x92F937, 0x37F992, 0x37F9D9, 0x37D9F9, 0x62BAC7, 0xC0CBD5,
    0xE29BE2, 0xBDC762, 0xF79036, 0xFF8000, 0xD2F937, 0xE29BB7,
    0xE0A3F1
};
static const uint32_t preset_rgb2[] = {
    // Cold (B5:G5), then warm (H5:N5).
    0x139566, 0x349BC0, 0x009999, 0x319413, 0x9039D2, 0x137294,
    0xCD3476, 0xCD34B8, 0x909413, 0xD58B41, 0xD8C835, 0xC339A5,
    0xE07100
};
static const uint32_t preset_rgb3[] = {
    0x6C6800, 0x6B0400, 0x366C00, 0x006C48, 0x00686C, 0x00326C,
    0x04006C, 0x44006C, 0x6C0044, 0x6C0004, 0x000000
};
static constexpr uint8_t RANDOM_RGB1_COLD_COUNT = 6; // B4:G4
static constexpr uint8_t RANDOM_RGB2_COLD_COUNT = 6; // B5:G5

static uint32_t rgb565_to_rgb888(const uint16_t c) {
    const uint32_t r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

/* the colours in use go to the video driver (slots 0..11); a Game Boy Color
 * game keeps its own palette in slots 0..191 */
static void palette_apply() {
    if (gb.cgb.cgbMode) return;
    for (int i = 0; i < LAYER_COUNT; i++)
        for (int j = 0; j < 4; j++) {
            const uint32_t c = pal_rgb[i][j];
            // RGB888() does not bracket its arguments: plain values only
            const uint32_t r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
            const uint8_t slot = layer_hw[i] * 4 + j;
            graphics_set_palette(slot, RGB888(r, g, b));
            palette[layer_hw[i]][j] = slot;
        }
}

static uint32_t palette_random_state = 0;

static uint32_t palette_random_next() {
    if (!palette_random_state)
        palette_random_state = (uint32_t)time_us_64() ^ 0x9E3779B9u;
    palette_random_state ^= palette_random_state << 13;
    palette_random_state ^= palette_random_state >> 17;
    palette_random_state ^= palette_random_state << 5;
    return palette_random_state;
}

/* Random as in pico-gamate: shades from the light / middle / dark lists, the
 * second middle one of the opposite temperature */
static void randomize_layer(const unsigned layer) {
    uint32_t* const c = random_rgb[layer];
    c[0] = preset_rgb0[palette_random_next() % count_of(preset_rgb0)];
    const uint8_t i1 = palette_random_next() % count_of(preset_rgb1);
    c[1] = preset_rgb1[i1];
    if (i1 < RANDOM_RGB1_COLD_COUNT) {
        const uint8_t warm_count = count_of(preset_rgb2) - RANDOM_RGB2_COLD_COUNT;
        c[2] = preset_rgb2[RANDOM_RGB2_COLD_COUNT + palette_random_next() % warm_count];
    }
    else {
        c[2] = preset_rgb2[palette_random_next() % RANDOM_RGB2_COLD_COUNT];
    }
    c[3] = preset_rgb3[palette_random_next() % count_of(preset_rgb3)];
}

/* the selector a palette follows */
static uint8_t layer_selector(const unsigned layer) {
    return palette_all == PALETTE_CUSTOM ? palette_index[layer] : palette_all;
}

/* the palettes get their own selectors (keeping the colours shown) */
static void palette_split() {
    if (palette_all == PALETTE_CUSTOM) return;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++) palette_index[layer] = palette_all;
    palette_all = PALETTE_CUSTOM;
}

static void randomize_random_layers() {
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
        if (layer_selector(layer) == PALETTE_RANDOM) randomize_layer(layer);
}

/* the colours of every palette by its selector */
static void update_palette() {
    bool gbc_palette_ready = false;
    uint8_t gbc_palette_of = 0xff;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++) {
        const uint8_t sel = layer_selector(layer);
        uint32_t* const out = pal_rgb[layer];
        if (sel < PALETTE_MONO_FIRST) {
            // AUTO / a GBC boot palette: the colours this palette gets there
            if (!gbc_palette_ready || gbc_palette_of != sel) {
                if (sel == PALETTE_AUTO) {
                    char rom_title[16];
                    auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
                }
                else {
                    manual_assign_palette(palette16, sel);
                }
                gbc_palette_ready = true;
                gbc_palette_of = sel;
            }
            for (int j = 0; j < 4; j++) out[j] = rgb565_to_rgb888(palette16[layer_hw[layer]][j]);
        }
        else if (sel < PALETTE_RANDOM) {
            const uint8_t* m = mono_palettes[sel - PALETTE_MONO_FIRST];
            for (int j = 0; j < 4; j++) out[j] = m[j * 3] << 16 | m[j * 3 + 1] << 8 | m[j * 3 + 2];
        }
        else if (sel == PALETTE_RANDOM) {
            memcpy(out, random_rgb[layer], sizeof random_rgb[layer]);
        }
        else {
            memcpy(out, custom_rgb[layer], sizeof custom_rgb[layer]);
        }
    }
    palette_apply();
}

/* the colours saved for a game: /.config/gbc/<ROM file name>.ini */
static bool game_palette_ini_path(char* pathname, const size_t size) {
    if (!rom_file[0]) return false;
    char basename[80];
    snprintf(basename, sizeof basename, "%s", rom_file);
    char* dot = strrchr(basename, '.');
    if (dot && dot != basename) *dot = '\0';
    return snprintf(pathname, size, CONFIG_DIR "/%s.ini", basename) > 0;
}

static bool game_palette_ini_exists() {
    char pathname[128];
    FILINFO info;
    return game_palette_ini_path(pathname, sizeof pathname) && FR_OK == f_stat(pathname, &info);
}

static const char* const ini_layer_keys[LAYER_COUNT] = { "bg=", "obj0=", "obj1=" };

/* [palette]: the Custom colours, [layers]: the selectors; a Random palette is
 * kept with the colours it drew (as Custom) */
static bool write_game_palette_ini() {
    char pathname[128];
    if (!game_palette_ini_path(pathname, sizeof pathname)) return false;
    config_mkdirs();

    static char data[256];
    int len = snprintf(data, sizeof data, "[palette]\r\n");
    for (unsigned i = 0; i < LAYER_COUNT && len > 0 && len < (int)sizeof data; i++) {
        const uint32_t* c = layer_selector(i) == PALETTE_RANDOM ? pal_rgb[i] : custom_rgb[i];
        len += snprintf(data + len, sizeof data - len, "%s%06lX,%06lX,%06lX,%06lX\r\n", ini_layer_keys[i],
                        (unsigned long)c[0], (unsigned long)c[1], (unsigned long)c[2], (unsigned long)c[3]);
    }
    const bool all = palette_all != PALETTE_CUSTOM && palette_all != PALETTE_RANDOM;
    if (len > 0 && len < (int)sizeof data)
        len += snprintf(data + len, sizeof data - len, "[layers]\r\nall=%u\r\n",
                        all ? (unsigned)palette_all : (unsigned)PALETTE_CUSTOM);
    for (unsigned i = 0; i < LAYER_COUNT && len > 0 && len < (int)sizeof data; i++) {
        const unsigned sel = all ? palette_index[i]
                           : layer_selector(i) == PALETTE_RANDOM ? PALETTE_CUSTOM : layer_selector(i);
        len += snprintf(data + len, sizeof data - len, "%s%u\r\n", ini_layer_keys[i], sel);
    }
    if (len <= 0 || len >= (int)sizeof data) return false;

    static FIL file; // static: core 0 has a small stack
    if (FR_OK != f_open(&file, pathname, FA_CREATE_ALWAYS | FA_WRITE)) return false;
    UINT written = 0;
    const FRESULT fr = f_write(&file, data, (UINT)len, &written);
    const FRESULT close_fr = f_close(&file);
    return fr == FR_OK && written == (UINT)len && close_fr == FR_OK;
}

static bool parse_rgb4(const char* text, uint32_t rgb[4]) {
    unsigned long c[4];
    if (!text || 4 != sscanf(text, "%lx,%lx,%lx,%lx", &c[0], &c[1], &c[2], &c[3])) return false;
    for (int j = 0; j < 4; j++) {
        if (c[j] > 0xFFFFFFul) return false;
        rgb[j] = (uint32_t)c[j];
    }
    return true;
}

static bool load_game_palette_ini() {
    char pathname[128];
    if (!game_palette_ini_path(pathname, sizeof pathname)) return false;

    static FIL file; // static: core 0 has a small stack
    if (FR_OK != f_open(&file, pathname, FA_READ)) return false;
    static char data[320];
    UINT bytes_read = 0;
    const FRESULT fr = f_read(&file, data, sizeof data - 1, &bytes_read);
    f_close(&file);
    if (fr != FR_OK || bytes_read == 0) return false;
    data[bytes_read] = '\0';

    uint32_t rgb[LAYER_COUNT][4];
    char* const layers = strstr(data, "[layers]");
    if (layers) *layers = '\0'; // the colours are searched in [palette] only
    for (unsigned i = 0; i < LAYER_COUNT; i++) {
        const char* line = strstr(data, ini_layer_keys[i]);
        if (!line || !parse_rgb4(line + strlen(ini_layer_keys[i]), rgb[i])) return false;
    }
    memcpy(custom_rgb, rgb, sizeof custom_rgb);
    for (unsigned i = 0; i < LAYER_COUNT; i++) {
        // no [layers] (a file of the previous version): every palette is Custom
        const char* line = layers ? strstr(layers + 1, ini_layer_keys[i]) : nullptr;
        const unsigned long sel = line ? strtoul(line + strlen(ini_layer_keys[i]), nullptr, 10) : PALETTE_CUSTOM;
        palette_index[i] = sel <= PALETTE_CUSTOM ? (uint8_t)sel : PALETTE_CUSTOM;
    }
    // no "all=" (an older file): every palette by its own selector
    const char* all = layers ? strstr(layers + 1, "all=") : nullptr;
    const unsigned long sel_all = all ? strtoul(all + 4, nullptr, 10) : PALETTE_CUSTOM;
    palette_all = sel_all <= PALETTE_CUSTOM ? (uint8_t)sel_all : PALETTE_CUSTOM;
    randomize_random_layers();
    update_palette();
    return true;
}

static bool game_ini_action() {
    if (gb.cgb.cgbMode) return false;
    char pathname[128];
    if (!game_palette_ini_path(pathname, sizeof pathname)) return false;
    if (game_ini_linked) {
        const FRESULT fr = f_unlink(pathname);
        if (fr == FR_OK || fr == FR_NO_FILE) game_ini_linked = false;
    }
    else if (write_game_palette_ini()) {
        game_ini_linked = true;
    }
    return false;
}

static void apply_game_palette_on_load() {
    game_ini_linked = game_palette_ini_exists();
    if (!load_game_palette_ini()) {
        randomize_random_layers(); // new colours for every game
        update_palette();
    }
}

static void f_save_conf(void);

/* ---- the palette editor: the 12 colours as 3 rows (BG, OBJ0, OBJ1) of 4 ---- */
static constexpr int PE_W = LCD_WIDTH / 4;  // 40
static constexpr int PE_H = LCD_HEIGHT / 3; // 48

/* 3x5 glyphs: 0-9, A-F, G, J, O, space */
static void pe_char(uint8_t* buf, const int x, const int y, const char ch, const uint8_t color) {
    static const char chars[] = "0123456789ABCDEFGJO ";
    static const uint8_t glyphs[][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
        {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
        {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {7, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
        {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 7, 4, 7}, {7, 4, 7, 4, 4},
        {7, 4, 5, 5, 7}, {1, 1, 1, 5, 7}, {2, 5, 5, 5, 2}, {0, 0, 0, 0, 0},
    };
    const char* p = strchr(chars, ch);
    if (!p || !ch) return;
    const uint8_t* g = glyphs[p - chars];
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if (g[row] & (4u >> col))
                buf[(y + row) * LCD_WIDTH + x + col] = color;
}

static void pe_text(uint8_t* buf, int x, const int y, const char* s, const uint8_t color) {
    for (; *s; s++, x += 4) pe_char(buf, x, y, *s, color);
}

/* the most distant of the 12 colours keeps the text readable */
static uint8_t pe_contrast(const unsigned slot) {
    const uint32_t bg = pal_rgb[slot >> 2][slot & 3];
    uint8_t best = 0;
    unsigned best_distance = 0;
    for (unsigned i = 0; i < LAYER_COUNT * 4; i++) {
        const uint32_t fg = pal_rgb[i >> 2][i & 3];
        const int dr = (int)(bg >> 16 & 0xFF) - (int)(fg >> 16 & 0xFF);
        const int dg = (int)(bg >> 8 & 0xFF) - (int)(fg >> 8 & 0xFF);
        const int db = (int)(bg & 0xFF) - (int)(fg & 0xFF);
        const unsigned distance = dr * dr + dg * dg + db * db;
        if (distance > best_distance) {
            best_distance = distance;
            best = layer_hw[i >> 2] * 4 + (i & 3); // the video palette slot
        }
    }
    return best;
}

static void pe_draw(uint8_t* buf, const unsigned selected, const int edit_channel) {
    static const char* const names[LAYER_COUNT] = { "BG", "OBJ0", "OBJ1" };
    for (unsigned slot = 0; slot < LAYER_COUNT * 4; slot++) {
        const int x0 = (int)(slot & 3) * PE_W;
        const int y0 = (int)(slot >> 2) * PE_H;
        for (int y = y0; y < y0 + PE_H; y++)
            memset(buf + y * LCD_WIDTH + x0, layer_hw[slot >> 2] * 4 + (slot & 3), PE_W);

        const uint8_t text = pe_contrast(slot);
        char label[8];
        snprintf(label, sizeof label, "%s %u", names[slot >> 2], slot & 3);
        pe_text(buf, x0 + (PE_W - (int)strlen(label) * 4 + 1) / 2, y0 + 15, label, text);
        char rgb[8];
        snprintf(rgb, sizeof rgb, "%06lX", (unsigned long)pal_rgb[slot >> 2][slot & 3]);
        pe_text(buf, x0 + 8, y0 + 26, rgb, text);

        if (slot == selected) { // the focus frame
            for (int x = x0 + 1; x < x0 + PE_W - 1; x++) {
                buf[(y0 + 1) * LCD_WIDTH + x] = text;
                buf[(y0 + PE_H - 2) * LCD_WIDTH + x] = text;
            }
            for (int y = y0 + 1; y < y0 + PE_H - 1; y++) {
                buf[y * LCD_WIDTH + x0 + 1] = text;
                buf[y * LCD_WIDTH + x0 + PE_W - 2] = text;
            }
            if (edit_channel >= 0) { // the edited byte: RR, GG or BB
                const int dx = x0 + 8 + edit_channel * 8;
                for (int x = dx; x < dx + 7; x++)
                    buf[(y0 + 32) * LCD_WIDTH + x] = text;
            }
        }
    }
}

typedef struct {
    bool active;
    uint64_t next_repeat;
} pe_repeat_t;

static bool pe_repeat(const bool level, pe_repeat_t* state) {
    const uint64_t now = time_us_64();
    if (!level) {
        state->active = false;
        return false;
    }
    if (!state->active) {
        state->active = true;
        state->next_repeat = now + 300000; // initial key-repeat delay
        return true;
    }
    if (now >= state->next_repeat) {
        state->next_repeat = now + 80000;
        return true;
    }
    return false;
}

/* The first edit of a colour turns its palette into Custom with the colours
 * shown, the other two palettes keep their colours (with their own selectors). */
static void pe_ensure_custom(const unsigned layer) {
    if (layer_selector(layer) == PALETTE_CUSTOM) return;
    palette_split();
    memcpy(custom_rgb[layer], pal_rgb[layer], sizeof custom_rgb[layer]);
    palette_index[layer] = PALETTE_CUSTOM;
}

/* Arrows - a colour, Enter / A - edit it (left / right: R, G, B; up / down:
 * the value), hex digits type RRGGBB, Esc / B - back, gamepad START or F12 -
 * close.  The picture is drawn in the back buffer (the game is paused). */
static void palette_editor(const bool from_menu) {
    if (gb.cgb.cgbMode) return; // a Game Boy Color game has its own colours
    for (int i = 0; i < 100 && frame_pending >= 0; i++) sleep_ms(1); // the render core takes it
    uint8_t* const buf = (uint8_t *)frames[frame_back];
    unsigned selected = 0;
    int edit_channel = -1;
    int hex_digit = -1;

    kbd_hex_key = -1;
    kbd_hex_mode = true;
    palette_editor_requested = false;
    if (from_menu) graphics_set_mode(GRAPHICSMODE_DEFAULT);
    graphics_set_buffer(buf, LCD_WIDTH, LCD_HEIGHT);
    palette_apply();
    pe_draw(buf, selected, edit_channel);

    auto accept_level = [] { return keyboard.start || keyboard.a || (nespad_state & DPAD_A); };
    auto back_level = [] { return keyboard.select || keyboard.b || (nespad_state & DPAD_B); };
    auto close_level = [] { return (nespad_state & DPAD_START) != 0; };
    bool accept_prev = accept_level(), back_prev = back_level(), close_prev = close_level();
    pe_repeat_t rep_left = {}, rep_right = {}, rep_up = {}, rep_down = {};

    while (true) {
        const bool accept_now = accept_level(), back_now = back_level(), close_now = close_level();
        const bool accept = accept_now && !accept_prev;
        const bool back = back_now && !back_prev;
        const bool close = (close_now && !close_prev) || palette_editor_requested;
        accept_prev = accept_now;
        back_prev = back_now;
        close_prev = close_now;
        palette_editor_requested = false;
        const bool left = pe_repeat(gamepad_bits.left, &rep_left);
        const bool right = pe_repeat(gamepad_bits.right, &rep_right);
        const bool up = pe_repeat(gamepad_bits.up, &rep_up);
        const bool down = pe_repeat(gamepad_bits.down, &rep_down);
        const unsigned layer = selected >> 2;
        uint32_t* const value = &pal_rgb[layer][selected & 3];
        bool redraw = false, changed = false;

        const int8_t typed = kbd_hex_key;
        kbd_hex_key = -1;
        if (close) {
            break;
        }
        if (typed >= 0) {
            if (hex_digit < 0) hex_digit = 0;
            const unsigned shift = (5 - hex_digit) * 4u;
            pe_ensure_custom(layer);
            *value = (*value & ~(0xFu << shift)) | (uint32_t)typed << shift;
            changed = true;
            if (++hex_digit >= 6) {
                hex_digit = -1;
                edit_channel = -1;
            }
            else {
                edit_channel = hex_digit >> 1;
            }
        }
        else if (edit_channel < 0) {
            if (left) selected = (selected & ~3u) | ((selected - 1) & 3u);
            else if (right) selected = (selected & ~3u) | ((selected + 1) & 3u);
            else if (up) selected = (selected + LAYER_COUNT * 4 - 4) % (LAYER_COUNT * 4);
            else if (down) selected = (selected + 4) % (LAYER_COUNT * 4);
            else if (accept) edit_channel = 0;
            else if (back) break;
            redraw = left || right || up || down || accept;
        }
        else {
            if (left) edit_channel = (edit_channel + 2) % 3;
            else if (right) edit_channel = (edit_channel + 1) % 3;
            else if (up || down) {
                const unsigned shift = (2 - edit_channel) * 8u;
                unsigned v = *value >> shift & 0xFF;
                if (up && v < 255) v++;
                if (down && v > 0) v--;
                pe_ensure_custom(layer);
                *value = (*value & ~(0xFFu << shift)) | v << shift;
                changed = true;
            }
            else if (accept || back) {
                edit_channel = -1;
                hex_digit = -1;
            }
            redraw = true;
        }
        if (changed) {
            custom_rgb[layer][selected & 3] = *value;
            palette_apply();
            redraw = true;
        }
        if (redraw) pe_draw(buf, selected, edit_channel);
        sleep_ms(10);
    }

    kbd_hex_mode = false;
    kbd_hex_key = -1;
    f_save_conf();
    graphics_set_buffer((uint8_t *)frames[frame_front], LCD_WIDTH, LCD_HEIGHT);
    if (from_menu) graphics_set_mode(TEXTMODE_DEFAULT);
}

/* keys pressed in the ROM browser or in the menu are not for the game */
static void palette_hotkeys_clear() {
    palette_layer_cycle_requested = 0;
    palette_shuffle_requested = palette_shuffle_requested_no_save = false;
    palette_save_requested = palette_default_requested = palette_editor_requested = false;
}

/* F1..F3 (+Shift), F6, F7, F8, F9, F12 - see process_kbd_report();
 * returns true after the palette editor (keys may still be held) */
static bool handle_palette_hotkeys() {
    const int8_t cycle = palette_layer_cycle_requested;
    const bool shuffle = palette_shuffle_requested;
    const bool shuffle_no_save = palette_shuffle_requested_no_save;
    const bool save_game = palette_save_requested;
    const bool to_default = palette_default_requested;
    const bool editor = palette_editor_requested;
    palette_layer_cycle_requested = 0;
    palette_shuffle_requested = palette_shuffle_requested_no_save = false;
    palette_save_requested = palette_default_requested = palette_editor_requested = false;
    if (gb.cgb.cgbMode) return false; // a Game Boy Color game has its own colours

    if (cycle) {
        const unsigned layer = (cycle < 0 ? -cycle : cycle) - 1;
        if (layer < LAYER_COUNT) {
            palette_split();
            uint8_t& sel = palette_index[layer];
            sel = cycle < 0 ? (sel + PALETTE_CUSTOM) % (PALETTE_CUSTOM + 1) : (sel + 1) % (PALETTE_CUSTOM + 1);
            if (sel == PALETTE_RANDOM) randomize_layer(layer);
            update_palette();
            f_save_conf();
        }
    }
    if (shuffle || shuffle_no_save) {
        palette_all = PALETTE_CUSTOM;
        for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
            palette_index[layer] = palette_random_next() % (PALETTE_CUSTOM + 1);
        randomize_random_layers();
        update_palette();
        if (shuffle) f_save_conf();
    }
    if (save_game && write_game_palette_ini())
        game_ini_linked = true;
    if (to_default) {
        palette_all = PALETTE_AUTO;
        update_palette();
        f_save_conf();
    }
    if (editor) {
        palette_editor(false);
        return true;
    }
    return false;
}

const MenuItem menu_items[] = {
    //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    { "Swap AB <> BA: %s", ARRAY, &swap_ab,  nullptr, 1, {"NO ", "YES"}},
    { "Palette:      %s", ARRAY, &palette_all, nullptr, PALETTE_CUSTOM,
        {
            "AUTO (by game)   ",
            "1 - yellow-red   ",
            "2 - orange       ",
            "3 - negative     ",
            "4 - dark green   ",
            "5 - red          ",
            "6 - pink         ",
            "7 - green        ",
            "8 - dark blue    ",
            "9 - pastel       ",
            "10 - blue        ",
            "11 - yellow      ",
            "12 - DMG         ",
            "GAMATE DEFAULT   ",
            "BLACK & WHITE    ",
            "BGB              ",
            "AUTUMN FOREST    ",
            "OCEAN SAND       ",
            "MINT SAND        ",
            "AMBER            ",
            "GREEN            ",
            "BLUE             ",
            "WATAROO          ",
            "GB_DMG           ",
            "GB_POCKET        ",
            "GB_LIGHT         ",
            "BLOSSOM_PINK     ",
            "BUBBLES_BLUE     ",
            "BUTTERCUP_GREEN  ",
            "DIGIVICE         ",
            "GAME_COM         ",
            "GAMEKING         ",
            "GAME_MASTER      ",
            "GOLDEN_WILD      ",
            "GREENSCALE       ",
            "HOKAGE_ORANGE    ",
            "LABO_FAWN        ",
            "SUPER_SAIYAN     ",
            "MICROVISION      ",
            "MILLION_LIVE_GOLD",
            "ODYSSEY_GOLD     ",
            "SHINY_SKY_BLUE   ",
            "SLIME_BLUE       ",
            "TI_83            ",
            "TRAVEL_WOOD      ",
            "VIRTUAL_BOY      ",
            "TV-LINK          ",
            "RANDOM           ",
            "CUSTOM           "
        }
    },
    { "  BG:         %s", ARRAY, &palette_index[LAYER_BG], nullptr, PALETTE_CUSTOM,
        {
            "AUTO (by game)   ",
            "1 - yellow-red   ",
            "2 - orange       ",
            "3 - negative     ",
            "4 - dark green   ",
            "5 - red          ",
            "6 - pink         ",
            "7 - green        ",
            "8 - dark blue    ",
            "9 - pastel       ",
            "10 - blue        ",
            "11 - yellow      ",
            "12 - DMG         ",
            "GAMATE DEFAULT   ",
            "BLACK & WHITE    ",
            "BGB              ",
            "AUTUMN FOREST    ",
            "OCEAN SAND       ",
            "MINT SAND        ",
            "AMBER            ",
            "GREEN            ",
            "BLUE             ",
            "WATAROO          ",
            "GB_DMG           ",
            "GB_POCKET        ",
            "GB_LIGHT         ",
            "BLOSSOM_PINK     ",
            "BUBBLES_BLUE     ",
            "BUTTERCUP_GREEN  ",
            "DIGIVICE         ",
            "GAME_COM         ",
            "GAMEKING         ",
            "GAME_MASTER      ",
            "GOLDEN_WILD      ",
            "GREENSCALE       ",
            "HOKAGE_ORANGE    ",
            "LABO_FAWN        ",
            "SUPER_SAIYAN     ",
            "MICROVISION      ",
            "MILLION_LIVE_GOLD",
            "ODYSSEY_GOLD     ",
            "SHINY_SKY_BLUE   ",
            "SLIME_BLUE       ",
            "TI_83            ",
            "TRAVEL_WOOD      ",
            "VIRTUAL_BOY      ",
            "TV-LINK          ",
            "RANDOM           ",
            "CUSTOM           "
        }
    },
    { "  OBJ0:       %s", ARRAY, &palette_index[LAYER_OBJ0], nullptr, PALETTE_CUSTOM,
        {
            "AUTO (by game)   ",
            "1 - yellow-red   ",
            "2 - orange       ",
            "3 - negative     ",
            "4 - dark green   ",
            "5 - red          ",
            "6 - pink         ",
            "7 - green        ",
            "8 - dark blue    ",
            "9 - pastel       ",
            "10 - blue        ",
            "11 - yellow      ",
            "12 - DMG         ",
            "GAMATE DEFAULT   ",
            "BLACK & WHITE    ",
            "BGB              ",
            "AUTUMN FOREST    ",
            "OCEAN SAND       ",
            "MINT SAND        ",
            "AMBER            ",
            "GREEN            ",
            "BLUE             ",
            "WATAROO          ",
            "GB_DMG           ",
            "GB_POCKET        ",
            "GB_LIGHT         ",
            "BLOSSOM_PINK     ",
            "BUBBLES_BLUE     ",
            "BUTTERCUP_GREEN  ",
            "DIGIVICE         ",
            "GAME_COM         ",
            "GAMEKING         ",
            "GAME_MASTER      ",
            "GOLDEN_WILD      ",
            "GREENSCALE       ",
            "HOKAGE_ORANGE    ",
            "LABO_FAWN        ",
            "SUPER_SAIYAN     ",
            "MICROVISION      ",
            "MILLION_LIVE_GOLD",
            "ODYSSEY_GOLD     ",
            "SHINY_SKY_BLUE   ",
            "SLIME_BLUE       ",
            "TI_83            ",
            "TRAVEL_WOOD      ",
            "VIRTUAL_BOY      ",
            "TV-LINK          ",
            "RANDOM           ",
            "CUSTOM           "
        }
    },
    { "  OBJ1:       %s", ARRAY, &palette_index[LAYER_OBJ1], nullptr, PALETTE_CUSTOM,
        {
            "AUTO (by game)   ",
            "1 - yellow-red   ",
            "2 - orange       ",
            "3 - negative     ",
            "4 - dark green   ",
            "5 - red          ",
            "6 - pink         ",
            "7 - green        ",
            "8 - dark blue    ",
            "9 - pastel       ",
            "10 - blue        ",
            "11 - yellow      ",
            "12 - DMG         ",
            "GAMATE DEFAULT   ",
            "BLACK & WHITE    ",
            "BGB              ",
            "AUTUMN FOREST    ",
            "OCEAN SAND       ",
            "MINT SAND        ",
            "AMBER            ",
            "GREEN            ",
            "BLUE             ",
            "WATAROO          ",
            "GB_DMG           ",
            "GB_POCKET        ",
            "GB_LIGHT         ",
            "BLOSSOM_PINK     ",
            "BUBBLES_BLUE     ",
            "BUTTERCUP_GREEN  ",
            "DIGIVICE         ",
            "GAME_COM         ",
            "GAMEKING         ",
            "GAME_MASTER      ",
            "GOLDEN_WILD      ",
            "GREENSCALE       ",
            "HOKAGE_ORANGE    ",
            "LABO_FAWN        ",
            "SUPER_SAIYAN     ",
            "MICROVISION      ",
            "MILLION_LIVE_GOLD",
            "ODYSSEY_GOLD     ",
            "SHINY_SKY_BLUE   ",
            "SLIME_BLUE       ",
            "TI_83            ",
            "TRAVEL_WOOD      ",
            "VIRTUAL_BOY      ",
            "TV-LINK          ",
            "RANDOM           ",
            "CUSTOM           "
        }
    },
    { "Palette editor", PALETTE_EDITOR },
    { "", GAME_INI, nullptr, &game_ini_action },
    {},
    { "Save state: %i", INT, &save_slot, &save, 8 },
    { "Load state: %i", INT, &save_slot, &load, 8 },
#if SOFTTV
    { "" },
    { "TV system %s", ARRAY, &tv_system, nullptr, 1, { "PAL ", "NTSC" } },
#endif
#if VGA || HDMI || SOFTTV
    { "" },
    { "Screen: %s", ARRAY, &screen_scale, nullptr, 2, { "2x  ", "3x  ", "Full" } },
#endif
    {},
{
    "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
    { FREQ_STR(CPU_FREQ), "396", "404", "408", "412", "416", "420", "424", "432" }
},
{ "Press START / Enter to apply", NONE },
    { "Reset to ROM select", ROM_SELECT },
    { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

/* key=value text: a new key never invalidates an existing file */
static void config_path(char* pathname, const size_t size) {
    snprintf(pathname, size, CONFIG_DIR "/%s/%s/config.ini", config_platform_name(), config_video_name());
}

static void f_load_conf(void) {
    static FIL f; // static: core 0 has a small stack
    char pathname[96];
    config_path(pathname, sizeof pathname);
    if (f_open(&f, pathname, FA_READ) == FR_OK) {
        static char text[512];
        UINT br = 0;
        if (f_read(&f, text, sizeof text - 1, &br) != FR_OK) br = 0;
        f_close(&f);
        text[br] = '\0';
        for (char* line = strtok(text, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
            char* val = strchr(line, '=');
            if (!val) continue;
            *val++ = '\0';
            const unsigned long d = strtoul(val, nullptr, 10);
            if (!strcmp(line, "swap_ab")) swap_ab = d ? 1 : 0;
            else if (!strcmp(line, "tv_system")) tv_system = d <= 1 ? (uint8_t)d : 0;
#if BACKPLANE
            else if (!strcmp(line, "backplane")) backplane = d ? 1 : 0;
#endif
            else if (!strcmp(line, "palette_all")) palette_all = d <= PALETTE_CUSTOM ? (uint8_t)d : PALETTE_AUTO;
            else if (!strcmp(line, "screen")) screen_scale = d <= GRAPHICS_SCALE_STRETCH ? (uint8_t)d : screen_scale;
            else if (!strncmp(line, "palette", 7) || !strncmp(line, "custom", 6)) {
                const bool custom = line[0] == 'c';
                const unsigned layer = strtoul(line + (custom ? 6 : 7), nullptr, 10);
                if (layer >= LAYER_COUNT) continue;
                if (custom) parse_rgb4(val, custom_rgb[layer]);
                else palette_index[layer] = d <= PALETTE_CUSTOM ? (uint8_t)d : PALETTE_AUTO;
            }
        }
    }
    else if (f_open(&f, "/GB/gb.conf", FA_READ) == FR_OK) {
        // the settings of an older version: swap AB, palette (0..12 as now), TV system, screen
        UINT br;
        uint8_t v = 0;
        f_read(&f, &swap_ab, 1, &br);
        f_read(&f, &manual_palette_selected, 1, &br);
        // the same layout for all builds; older configs end earlier
        uint8_t v;
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v <= 1) tv_system = v;
        if (f_read(&f, &v, 1, &br) == FR_OK && br == 1 && v <= GRAPHICS_SCALE_STRETCH) screen_scale = v;
        f_close(&f);
    }
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++) randomize_layer(layer);
}

static void f_save_conf(void) {
    f_mkdir("/GB"); // ничего не делает, если она уже есть
    FIL f;
    f_open(&f, "/GB/gb.conf", FA_CREATE_ALWAYS | FA_WRITE);
    UINT br;
    f_write(&f, &swap_ab, 1, &br);
    f_write(&f, &manual_palette_selected, 1, &br);
    f_write(&f, &tv_system, 1, &br);
    f_write(&f, &screen_scale, 1, &br);
    f_close(&f);
}

static void menu_frame() {
    char footer[TEXTMODE_COLS];
    snprintf(footer, TEXTMODE_COLS, ":: %s ::", PICO_PROGRAM_NAME);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, 0, 11, 1);
    snprintf(footer, TEXTMODE_COLS, ":: %s build %s %s ::", PICO_PROGRAM_VERSION_STRING, __DATE__,
             __TIME__);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, TEXTMODE_ROWS - 1, 11, 1);
}

/* the palettes are chosen one by one only under "Palette: CUSTOM" */
static bool menu_item_selectable(const unsigned i) {
    const MenuItem& item = menu_items[i];
    if (item.type == NONE) return false;
    for (unsigned layer = 0; layer < LAYER_COUNT; layer++)
        if (item.value == &palette_index[layer]) return palette_all == PALETTE_CUSTOM;
#if BACKPLANE
    if (item.value == &backplane) return screen_scale != GRAPHICS_SCALE_STRETCH; // Full has none
#endif
    return true;
}

void menu() {
    bool exit = false;
    graphics_set_mode(TEXTMODE_DEFAULT);
    menu_frame();
    uint current_item = 0;
    /* Only fresh presses count: the Start+Select that opened the menu must be
     * released first, so it can't change or activate the first item. */
    uint8_t prev_keys = input_held();

    while (!exit) {
        const uint8_t held_keys = input_held();
        const uint8_t pressed = held_keys & (uint8_t)~prev_keys;
        prev_keys = held_keys;
        const bool activate = pressed & BTN_START;

        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + (TEXTMODE_ROWS - MENU_ITEMS_NUMBER >> 1);
            uint8_t x = TEXTMODE_COLS / 2 - 10;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }
            const MenuItem* item = &menu_items[i];
            if (i == current_item) {
                switch (item->type) {
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto* value = (uint8_t *)item->value;
                            if ((pressed & BTN_RIGHT) && *value < item->max_value) {
                                (*value)++;
                            }
                            if ((pressed & BTN_LEFT) && *value > 0) {
                                (*value)--;
                            }
                            for (unsigned layer = 0; layer < LAYER_COUNT; layer++) {
                                if (item->value != &palette_index[layer] && item->value != &palette_all)
                                    continue;
                                // Start on RANDOM draws new colours
                                if (layer_selector(layer) == PALETTE_RANDOM && (old != *value || activate))
                                    randomize_layer(layer);
                                if (old != *value || activate)
                                    update_palette();
                            }
                        }
                        break;
                    case PALETTE_EDITOR:
                        if (activate && dmg) {
                            palette_editor(true);
                            menu_frame();
                            prev_keys = input_held(); // the key that closed the editor
                        }
                        break;
                    case RETURN:
                        if (activate)
                            exit = true;
                        break;

                    case ROM_SELECT:
                        if (activate) {
                            restart = true;
                            return;
                        }
                        break;
                    default:
                        break;
                }

                if (nullptr != item->callback && activate) {
                    exit = item->callback();
                }
            }
            static char result[TEXT_BUFFER_COLS];
            switch (item->type) {
                case INT:
                    snprintf(result, TEXTMODE_COLS, item->text, *(uint8_t *)item->value);
                    break;
                case ARRAY:
                    if (!menu_item_selectable(i)) {
                        // a palette that follows "Palette:" shows its selector
                        const bool layer = item->value != &palette_all && item->max_value == PALETTE_CUSTOM;
                        snprintf(result, TEXTMODE_COLS, item->text,
                                 item->value_list[layer ? palette_all : *(uint8_t *)item->value]);
                        color = 6;
                        break;
                    }
                    snprintf(result, TEXTMODE_COLS, item->text, item->value_list[*(uint8_t *)item->value]);
                    break;
                case TEXT:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value);
                    break;
                case GAME_INI:
                    snprintf(result, TEXTMODE_COLS, "%s",
                             game_ini_linked ? "Unlink game color file   " : "Save colors for this game");
                    if (!dmg || !rom_file[0]) color = 6;
                    break;
                case PALETTE_EDITOR:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
                    if (!dmg) color = 6;
                    break;
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            draw_text(result, x, y, color, bg_color);
        }

        if (pressed & BTN_DOWN) {
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item++;
        }
        if (pressed & BTN_UP) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item--;
        }

        sleep_ms(25);
    }
    if (manual_palette_selected > 0) {
        manual_assign_palette(palette16, manual_palette_selected);
    } else {
        char rom_title[16];
        auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));
    }
    // a Game Boy Color game keeps its own palette in slots 0..63
    if (!gb.cgb.cgbMode)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                graphics_set_palette(i * 4 + j, RGB565_TO_RGB888(palette16[i][j]));
                palette[i][j] = i * 4 + j;
            }
    f_save_conf();
#if SOFTTV
    graphics_set_tv(tv_system == 0, true); // applied by graphics_set_mode()
#endif
#if VGA || HDMI || SOFTTV
    graphics_set_scale((enum graphics_scale_t)screen_scale);
#endif
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
}

int main() {
    detect_flash_size();
    overclock();
    /* QMI CS1 setup before core1 starts: the probe briefly blocks XIP */
    gb_psram_init();

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    sem_init(&vga_start_semaphore, 0, 1);
    critical_section_init(&frame_lock);
    cube_pal_init();
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);
    // Do not draw before the video driver has its text buffer:
    // draw_text() into a NULL text_buffer is lost.
    while (!render_ready) tight_loop_contents();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    // Initialize I2S sound driver
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);

    // Initialize audio emulation
    audio_init();

    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        /// TODO: error handling
        while(1);
    } else {
        f_mkdir(HOME_DIR);
        f_load_conf();
#if SOFTTV
        graphics_set_tv(tv_system == 0, true); // applied by the next graphics_set_mode()
#endif
#if VGA || HDMI || SOFTTV
        graphics_set_scale((enum graphics_scale_t)screen_scale);
#endif
    }

    while (true) {
        /* ROM File selector */
        if (FR_OK == fr) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            filebrowser("gbc,gb");
            graphics_set_mode(GRAPHICSMODE_DEFAULT);
        }

        /* Initialise GB context. */
        cgb_palette_reset();
        gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                      &gb_cart_ram_write, &gb_error, nullptr);

        if (ret != GB_INIT_NO_ERROR) {
            while (1) draw_text("error", 1, 1, 1, 2);
        }

        /* The colours of a Game Boy game: the palette saved for it, or the selected one */
        apply_game_palette_on_load();
        palette_hotkeys_clear();

        gb_init_lcd(&gb, &lcd_draw_line);
        /* Load Save File. */
        read_cart_ram_file(&gb);

        // the Enter/Start that launched the ROM must not reach the game
        input_gate_t game_gate;
        game_gate.arm();

        //=============================================================================
        while (!restart) {
            //------------------------------------------------------------------------------
            if (fxPressedV) {
                if (altPressed) {
                    save_slot = fxPressedV;
                    load();
                } else if (ctrlPressed) {
                    save_slot = fxPressedV;
                    save();
                }
                fxPressedV = 0; // once per key press, not on every frame
            }
            const uint8_t keys = game_gate.read();
            gb.direct.joypad_bits.up = !(keys & BTN_UP);
            gb.direct.joypad_bits.down = !(keys & BTN_DOWN);
            gb.direct.joypad_bits.left = !(keys & BTN_LEFT);
            gb.direct.joypad_bits.right = !(keys & BTN_RIGHT);
            gb.direct.joypad_bits.a = !(keys & BTN_A);
            gb.direct.joypad_bits.b = !(keys & BTN_B);
            gb.direct.joypad_bits.select = !(keys & BTN_SELECT);
            gb.direct.joypad_bits.start = !(keys & BTN_START);

            //gb.direct.joypad = nespad_state;
            //------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            static int keydown_counter = 0;
            if (!(gb.direct.joypad & 0b00001100) || nespad_state & DPAD_X) {
                char romname[24];

                gb_get_rom_name(&gb, romname);

                if (nullptr != strstr(romname, "ZELDA")) {
                    // half a second (the game itself uses Start+Select)
                    if (keydown_counter++ > 30) {
                        menu();
                        cgb_palette_reset(); // the menu may have changed the video palette
                        keydown_counter = 0;
                        game_gate.arm();
                    }
                }

                else {
                    menu();
                    cgb_palette_reset(); // the menu may have changed the video palette
                    // keys still held on leaving the menu (Enter on "Return to game",
                    // the Start+Select combination) must not reach the game
                    game_gate.arm();
                }
            }
            else {
                keydown_counter = 0; // the half second counts one continuous hold
            }
            // TODO F2
            if ((nespad_state & DPAD_RT)) {
                // wait for release to prevent cycle
                while (nespad_state & DPAD_RT) {
                    sleep_ms(500);
                }
                load();
            }

            // TODO F3
            if ((nespad_state & DPAD_LT)) {
                // wait for release to prevent cycle
                while (nespad_state & DPAD_LT) {
                    sleep_ms(500);
                }
                save();
            }

            if (handle_palette_hotkeys())
                game_gate.arm(); // the keys that closed the palette editor

            //-----------------------------------------------------------------
            gb_run_frame(&gb);

            //gb.direct.interlace = 1;

            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
        }
        restart = false;
    }
}
