#include "ay_hway.h"
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"

/* The HWAY/TurboSound serial bus of pico-wonderswan (pico2), pico-gamate and
 * murm386: two cascaded 74HC595s, latch = AUDIO_DATA_PIN, clock =
 * AUDIO_CLOCK_PIN, serial data = AUDIO_CLOCK_PIN + 1; AY clock on GP21. */
#define HWAY_LATCH_PIN AUDIO_DATA_PIN
#define HWAY_CLOCK_PIN AUDIO_CLOCK_PIN
#define HWAY_DATA_PIN  (AUDIO_CLOCK_PIN + 1)
#define CLK_AY_PIN     21

#define AY_CS_SAA1099 (1u << 15)
#define AY_ENABLE     (1u << 14)
#define AY_SAVE       (1u << 13)
#define AY_BEEPER     (1u << 12)
#define AY_CS1        (1u << 11)
#define AY_CS0        (1u << 10)
#define AY_BDIR       (1u << 9)
#define AY_BC1        (1u << 8)

static uint16_t control_bits;

#define HWAY_REG_QUEUE_SIZE 256u
#define HWAY_REG_QUEUE_MASK (HWAY_REG_QUEUE_SIZE - 1u)
typedef struct { uint8_t chip, reg, value; } hway_reg_cmd_t;
static hway_reg_cmd_t reg_queue[HWAY_REG_QUEUE_SIZE];
static volatile uint32_t reg_write_pos;
static volatile uint32_t reg_read_pos;

/* no heartbeat for this long (menu, file browser, a pause): silence */
#define HWAY_SILENCE_US 100000u
static volatile uint32_t heartbeat_us;
static volatile uint8_t silenced; // core 1 sets, core 0 takes

static inline void hway_wait_to_adjust(uint32_t wait_nops) {
    for (uint32_t i = 0; i < wait_nops; ++i)
        __asm volatile("nop");
}

static void __not_in_flash_func(hway_shift16)(uint16_t data) {
    /* the murm386 74HC595 timing: about 30 MHz maximum shift clock, with
     * setup/hold time around every edge */
    static uint32_t wait_nops;
    if (wait_nops == 0)
        wait_nops = clock_get_hz(clk_sys) / (30000000u * 5u);

    gpio_put(HWAY_CLOCK_PIN, 0);
    hway_wait_to_adjust(wait_nops);

    for (int i = 0; i < 16; ++i) {
        gpio_put(HWAY_DATA_PIN, (data & 0x8000u) != 0);
        data <<= 1;

        gpio_put(HWAY_CLOCK_PIN, 1);
        hway_wait_to_adjust(wait_nops);
        gpio_put(HWAY_CLOCK_PIN, 0);
        hway_wait_to_adjust(wait_nops);
    }

    gpio_put(HWAY_LATCH_PIN, 1);
    hway_wait_to_adjust(wait_nops);
    gpio_put(HWAY_LATCH_PIN, 0);
}

static inline void control_high(uint16_t mask) { control_bits |= mask; }
static inline void control_low(uint16_t mask) { control_bits &= (uint16_t)~mask; }

static void __not_in_flash_func(select_chip)(unsigned chip) {
    if (chip == 0) {
        control_low(AY_CS1);
        control_high(AY_CS0);
    } else {
        control_high(AY_CS1);
        control_low(AY_CS0);
    }
}

static void __not_in_flash_func(select_register)(uint8_t reg) {
    control_high(AY_BDIR | AY_BC1);
    hway_shift16(control_bits | reg);
    control_low(AY_BDIR | AY_BC1);
    hway_shift16(control_bits | reg);
}

static void __not_in_flash_func(write_data)(uint8_t value) {
    control_low(AY_BDIR);
    hway_shift16(control_bits | value);
    control_high(AY_BDIR);
    hway_shift16(control_bits | value);
    control_low(AY_BDIR);
    hway_shift16(control_bits | value);
}

static void __not_in_flash_func(bus_write)(unsigned chip, uint8_t reg, uint8_t value) {
    select_chip(chip);
    select_register(reg);
    write_data(value);
}

void hway_write_register(unsigned chip, uint8_t reg, uint8_t value) {
    const uint32_t write = __atomic_load_n(&reg_write_pos, __ATOMIC_RELAXED);
    const uint32_t read = __atomic_load_n(&reg_read_pos, __ATOMIC_ACQUIRE);
    if (write - read >= HWAY_REG_QUEUE_SIZE) return;
    reg_queue[write & HWAY_REG_QUEUE_MASK] = (hway_reg_cmd_t){ (uint8_t)(chip & 1u), (uint8_t)(reg & 0x0fu), value };
    __atomic_store_n(&reg_write_pos, write + 1u, __ATOMIC_RELEASE);
}

void hway_heartbeat(void) {
    heartbeat_us = time_us_32();
}

int hway_take_silenced(void) {
    if (!silenced) return 0;
    silenced = 0;
    return 1;
}

void hway_init(void) {
    gpio_init(HWAY_LATCH_PIN);
    gpio_init(HWAY_CLOCK_PIN);
    gpio_init(HWAY_DATA_PIN);
    gpio_set_dir(HWAY_LATCH_PIN, GPIO_OUT);
    gpio_set_dir(HWAY_CLOCK_PIN, GPIO_OUT);
    gpio_set_dir(HWAY_DATA_PIN, GPIO_OUT);
    gpio_put(HWAY_LATCH_PIN, 0);
    gpio_put(HWAY_CLOCK_PIN, 0);
    gpio_put(HWAY_DATA_PIN, 0);

    /* the reset / idle state of the Gamate / murm386 / WonderSwan code */
    control_bits = 0;
    control_low(AY_ENABLE);
    hway_shift16(control_bits);
    control_bits = AY_CS_SAA1099 | AY_ENABLE | AY_SAVE |
                   AY_CS1 | AY_CS0 | AY_BDIR | AY_BC1;
    hway_shift16(control_bits);

    /* the AY master clock: PWM, 4 counts per period, as in PICO-BK */
    gpio_set_function(CLK_AY_PIN, GPIO_FUNC_PWM);
    pwm_config ay_clock = pwm_get_default_config();
    pwm_config_set_wrap(&ay_clock, 3);
    pwm_config_set_clkdiv(&ay_clock, (float)clock_get_hz(clk_sys) / (4.0f * HWAY_AY_CLOCK));
    pwm_init(pwm_gpio_to_slice_num(CLK_AY_PIN), &ay_clock, true);
    pwm_set_gpio_level(CLK_AY_PIN, 2);

    /* both chips quiet */
    for (unsigned chip = 0; chip < 2; ++chip) {
        for (uint8_t reg = 8; reg <= 10; ++reg) bus_write(chip, reg, 0);
        bus_write(chip, 7, 0xbf);
    }
    __atomic_store_n(&reg_write_pos, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&reg_read_pos, 0, __ATOMIC_RELAXED);
    heartbeat_us = time_us_32();
    silenced = 0;
}

void __not_in_flash_func(hway_poll)(void) {
    for (;;) {
        const uint32_t read = __atomic_load_n(&reg_read_pos, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&reg_write_pos, __ATOMIC_ACQUIRE);
        if (read == write) break;
        const hway_reg_cmd_t cmd = reg_queue[read & HWAY_REG_QUEUE_MASK];
        __atomic_store_n(&reg_read_pos, read + 1u, __ATOMIC_RELEASE);
        bus_write(cmd.chip, cmd.reg, cmd.value);
    }

    static bool quiet;
    if (time_us_32() - heartbeat_us > HWAY_SILENCE_US) {
        if (!quiet) {
            quiet = true;
            for (unsigned chip = 0; chip < 2; ++chip)
                for (uint8_t reg = 8; reg <= 10; ++reg) bus_write(chip, reg, 0);
            silenced = 1;
        }
    }
    else {
        quiet = false;
    }
}
