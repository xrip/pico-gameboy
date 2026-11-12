#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Hardware AY-3-8910 pair (HWAY / TurboSound board, as in pico-wonderswan,
 * pico-gamate and murm386): the registers go over two cascaded 74HC595. */
#define HWAY_AY_CLOCK 1750000u /* Hz, the AY master clock (as in PICO-BK) */

void hway_init(void);
/* core 0: queue a register write (the bus itself belongs to core 1) */
void hway_write_register(unsigned chip, uint8_t reg, uint8_t value);
/* core 0, every emulated frame: the sound is alive */
void hway_heartbeat(void);
/* core 0: true once after core 1 silenced the chips (no heartbeat), the
 * caller must write all its registers again */
int hway_take_silenced(void);
/* core 1, as often as possible */
void hway_poll(void);
#ifdef __cplusplus
}
#endif
