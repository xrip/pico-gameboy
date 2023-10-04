/**
 * minigb_apu is released under the terms listed within the LICENSE file.
 *
 * minigb_apu emulates the audio processing unit (APU) of the Game Boy. This
 * project is based on MiniGBS by Alex Baines: https://github.com/baines/MiniGBS
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef AUDIO_SAMPLE_RATE
# define AUDIO_SAMPLE_RATE	32768
#endif

#define DMG_CLOCK_FREQ		4194304.0
#define SCREEN_REFRESH_CYCLES	70224.0
#define VERTICAL_SYNC		(DMG_CLOCK_FREQ/SCREEN_REFRESH_CYCLES)

/* Number of audio samples in each channel. */
#define AUDIO_SAMPLES		((unsigned)(AUDIO_SAMPLE_RATE / VERTICAL_SYNC))
/* Number of audio channels. The audio output is in interleaved stereo format.*/
#define AUDIO_CHANNELS		2
/* Number of audio samples output in each audio_callback call. */
#define AUDIO_SAMPLES_TOTAL	(AUDIO_SAMPLES * 2)

/**
 * Fill allocated buffer "stream" with AUDIO_SAMPLES_TOTAL number of 16-bit
 * signed samples (native endian order) in stereo interleaved format.
 * "sz" must be equal to AUDIO_SAMPLES_TOTAL.
 * Each call corresponds to the the time taken for each VSYNC in the Game Boy.
 *
 * \param userdata Unused. Only kept for ease of use with SDL2.
 * \param stream Allocated pointer to store audio samples. Must be at least
 *		AUDIO_SAMPLES_TOTAL in size.
 * \param sz Size of stream. Must be AUDIO_SAMPLES_TOTAL.
 */
void audio_callback(void *userdata, void *stream, int sz);

/**
 * Read audio register at given address "addr".
 */
uint8_t audio_read(const uint16_t addr);

/**
 * Write "val" to audio register at given address "addr".
 */
void audio_write(const uint16_t addr, const uint8_t val);

/**
 * Initialise audio driver.
 */
void audio_init(void);

#ifdef __cplusplus
}
#endif
