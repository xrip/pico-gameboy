/**
 * minigb_apu is released under the terms listed within the LICENSE file.
 *
 * minigb_apu emulates the audio processing unit (APU) of the Game Boy. This
 * project is based on MiniGBS by Alex Baines: https://github.com/baines/MiniGBS
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "minigb_apu.h"

#define DMG_CLOCK_FREQ_U	((unsigned)DMG_CLOCK_FREQ)
#define AUDIO_NSAMPLES		(AUDIO_SAMPLES * 2u)

#define AUDIO_MEM_SIZE		(0xFF3F - 0xFF10 + 1)
#define AUDIO_ADDR_COMPENSATION	0xFF10

#define MAX(a, b)		( a > b ? a : b )
#define MIN(a, b)		( a <= b ? a : b )

#define VOL_INIT_MAX		(INT16_MAX/8)
#define VOL_INIT_MIN		(INT16_MIN/8)

/* Handles time keeping for sound generation.
 * FREQ_INC_REF must be equal to, or larger than AUDIO_SAMPLE_RATE in order
 * to avoid a division by zero error.
 * Using a square of 2 simplifies calculations. */
#define FREQ_INC_REF		(AUDIO_SAMPLE_RATE * 16)

#define MAX_CHAN_VOLUME		15

/**
 * Memory holding audio registers between 0xFF10 and 0xFF3F inclusive.
 */
static uint8_t audio_mem[AUDIO_MEM_SIZE];

struct chan_len_ctr {
	uint8_t load;
	unsigned enabled : 1;
	uint32_t counter;
	uint32_t inc;
};

struct chan_vol_env {
	uint8_t step;
	unsigned up : 1;
	uint32_t counter;
	uint32_t inc;
};

struct chan_freq_sweep {
	uint16_t freq;
	uint8_t rate;
	uint8_t shift;
	unsigned up : 1;
	uint32_t counter;
	uint32_t inc;
};

static struct chan {
	unsigned enabled : 1;
	unsigned powered : 1;
	unsigned on_left : 1;
	unsigned on_right : 1;
	unsigned muted : 1;

	uint8_t volume;
	uint8_t volume_init;

	uint16_t freq;
	uint32_t freq_counter;
	uint32_t freq_inc;

	int_fast16_t val;

	struct chan_len_ctr    len;
	struct chan_vol_env    env;
	struct chan_freq_sweep sweep;

	union {
		struct {
			uint8_t duty;
			uint8_t duty_counter;
		} square;
		struct {
			uint16_t lfsr_reg;
			uint8_t  lfsr_wide;
			uint8_t  lfsr_div;
		} noise;
		struct {
			uint8_t sample;
		} wave;
	};
} chans[4];

static int32_t vol_l, vol_r;

static void set_note_freq(struct chan *c, const uint32_t freq)
{
	/* Lowest expected value of freq is 64. */
	c->freq_inc = freq * (uint32_t)(FREQ_INC_REF / AUDIO_SAMPLE_RATE);
}

static void chan_enable(const uint_fast8_t i, const bool enable)
{
	uint8_t val;

	chans[i].enabled = enable;
	val = (audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] & 0x80) |
		(chans[3].enabled << 3) | (chans[2].enabled << 2) |
		(chans[1].enabled << 1) | (chans[0].enabled << 0);

	audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] = val;
	//audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] |= 0x80 | ((uint8_t)enable) << i;
}

static void update_env(struct chan *c)
{
	c->env.counter += c->env.inc;

	while (c->env.counter > FREQ_INC_REF) {
		if (c->env.step) {
			c->volume += c->env.up ? 1 : -1;
			if (c->volume == 0 || c->volume == MAX_CHAN_VOLUME) {
				c->env.inc = 0;
			}
			c->volume = MAX(0, MIN(MAX_CHAN_VOLUME, c->volume));
		}
		c->env.counter -= FREQ_INC_REF;
	}
}

static void update_len(struct chan *c)
{
	if (!c->len.enabled)
		return;

	c->len.counter += c->len.inc;
	if (c->len.counter > FREQ_INC_REF) {
		chan_enable(c - chans, 0);
		c->len.counter = 0;
	}
}

static bool update_freq(struct chan *c, uint32_t *pos)
{
	uint32_t inc = c->freq_inc - *pos;
	c->freq_counter += inc;

	if (c->freq_counter > FREQ_INC_REF) {
		*pos		= c->freq_inc - (c->freq_counter - FREQ_INC_REF);
		c->freq_counter = 0;
		return true;
	} else {
		*pos = c->freq_inc;
		return false;
	}
}

/* The part of an output sample between two steps of a channel is weighted
 * by its length (a box filter): acc += box_part(c, pos - prev_pos) * value,
 * the sample is acc / box_whole(c).  Before, the parts were divided by the
 * whole sample in integers, i.e. dropped: the output was the value at the
 * end of the sample, and a tone above the sample rate came out as a loud
 * alias.  The shift keeps acc in 32 bits (freq_inc is up to ~2^25). */
static unsigned box_shift(const struct chan *c)
{
	return c->freq_inc >= (1u << 22) ? 8u : c->freq_inc >= (1u << 16) ? 4u : 0u;
}

static void update_sweep(struct chan *c)
{
	c->sweep.counter += c->sweep.inc;

	while (c->sweep.counter > FREQ_INC_REF) {
		if (c->sweep.shift) {
			uint16_t inc = (c->sweep.freq >> c->sweep.shift);
			if (!c->sweep.up)
				inc *= -1;

			c->freq += inc;
			if (c->freq > 2047) {
				c->enabled = 0;
			} else {
				set_note_freq(c,
					DMG_CLOCK_FREQ_U / ((2048 - c->freq)<< 5));
				c->freq_inc *= 8;
			}
		} else if (c->sweep.rate) {
			c->enabled = 0;
		}
		c->sweep.counter -= FREQ_INC_REF;
	}
}

static void update_square(int16_t* samples, const bool ch2, const uint_fast16_t sample_count)
{
	uint32_t freq;
	struct chan* c = chans + ch2;

	if (!c->powered || !c->enabled)
		return;

	freq = DMG_CLOCK_FREQ_U / ((2048 - c->freq) << 5);
	set_note_freq(c, freq);
	c->freq_inc *= 8;

	for (uint_fast16_t i = 0; i < sample_count; i += 2) {
		update_len(c);

		if (!c->enabled)
			continue;

		update_env(c);
		if (!ch2)
			update_sweep(c);

		int32_t sample;

		if (2048 - c->freq < 6) {
			/* above 21 kHz (131072 / (2048 - x)): no tone on a Game Boy, only
			 * the mean level of the duty cycle.  Games play PCM with it by
			 * changing the volume (Gauntlet II: x = 2047, 131 kHz). */
			unsigned high = 0;
			for (unsigned b = 0; b < 8; b++)
				high += (c->square.duty >> b) & 1;
			sample = ((int32_t)high * (VOL_INIT_MAX / MAX_CHAN_VOLUME) +
				  (int32_t)(8 - high) * (VOL_INIT_MIN / MAX_CHAN_VOLUME)) / 8;
		} else {
			const unsigned sh = box_shift(c);
			uint32_t pos = 0;
			uint32_t prev_pos = 0;
			int32_t acc = 0;
			bool moved = false;

			while (update_freq(c, &pos)) {
				moved = true;
				c->square.duty_counter = (c->square.duty_counter + 1) & 7;
				acc += (int32_t)((pos - prev_pos) >> sh) * c->val;
				c->val = (c->square.duty & (1 << c->square.duty_counter)) ?
					VOL_INIT_MAX / MAX_CHAN_VOLUME :
					VOL_INIT_MIN / MAX_CHAN_VOLUME;
				prev_pos = pos;
			}
			/* No edge in this sample: the mean is the level itself, exactly
			 * acc / (freq_inc >> sh) with acc = (freq_inc >> sh) * val.  Most
			 * notes are far below the sample rate, so the division (a call
			 * on RP2040) is left for the samples with an edge. */
			if (moved) {
				acc += (int32_t)((c->freq_inc - prev_pos) >> sh) * c->val;
				sample = acc / (int32_t)(c->freq_inc >> sh);
			} else
				sample = c->val;
		}

		if (c->muted)
			continue;

		sample *= c->volume;
		sample /= 4;

		samples[i + 0] += (sample * c->on_left * vol_l) >> 1;
		samples[i + 1] += (sample * c->on_right * vol_r) >> 1;
	}
}

static uint8_t wave_sample(const unsigned int pos, const unsigned int volume)
{
	uint8_t sample;

	sample =  audio_mem[(0xFF30 + pos / 2) - AUDIO_ADDR_COMPENSATION];
	if (pos & 1) {
		sample &= 0xF;
	} else {
		sample >>= 4;
	}
	return volume ? (sample >> (volume - 1)) : 0;
}

static void update_wave(int16_t *samples, const uint_fast16_t sample_count)
{
	uint32_t freq;
	struct chan *c = chans + 2;

	if (!c->powered || !c->enabled)
		return;

	freq = (DMG_CLOCK_FREQ_U / 64) / (2048 - c->freq);
	set_note_freq(c, freq);

	c->freq_inc *= 32;

	for (uint_fast16_t i = 0; i < sample_count; i += 2) {
		update_len(c);

		if (!c->enabled)
			continue;

		const unsigned sh = box_shift(c);
		uint32_t pos      = 0;
		uint32_t prev_pos = 0;
		int32_t acc      = 0;
		int32_t sample;
		bool moved = false;

		c->wave.sample = wave_sample(c->val, c->volume);

		while (update_freq(c, &pos)) {
			moved = true;
			c->val = (c->val + 1) & 31;
			acc += (int32_t)((pos - prev_pos) >> sh) *
				((int)c->wave.sample - 8) * (INT16_MAX/64);
			c->wave.sample = wave_sample(c->val, c->volume);
			prev_pos  = pos;
		}

		if (moved) {
			acc += (int32_t)((c->freq_inc - prev_pos) >> sh) *
				((int)c->wave.sample - 8) * (INT16_MAX/64);
			sample = acc / (int32_t)(c->freq_inc >> sh);
		} else
			sample = ((int)c->wave.sample - 8) * (INT16_MAX/64); /* as for the square */

		if (c->volume == 0)
			continue;

		/* volume 1..3: 100 %, 50 %, 25 %; constant divisors, no division call */
		if (c->volume == 2)
			sample /= 2;
		else if (c->volume == 3)
			sample /= 4;

		if (c->muted)
			continue;

		sample /= 4;

		samples[i + 0] += (sample * c->on_left * vol_l) >> 1;
		samples[i + 1] += (sample * c->on_right * vol_r) >> 1;
	}
}

static void update_noise(int16_t *samples, const uint_fast16_t sample_count)
{
	struct chan *c = chans + 3;

	if (!c->powered)
		return;

	{
		const uint32_t lfsr_div_lut[] = {
			8, 16, 32, 48, 64, 80, 96, 112
		};
		uint32_t freq;

		freq = DMG_CLOCK_FREQ_U / (lfsr_div_lut[c->noise.lfsr_div] << c->freq);
		set_note_freq(c, freq);
	}

	if (c->freq >= 14)
		c->enabled = 0;

	for (uint_fast16_t i = 0; i < sample_count; i += 2) {
		update_len(c);

		if (!c->enabled)
			continue;

		update_env(c);

		const unsigned sh = box_shift(c);
		uint32_t pos      = 0;
		uint32_t prev_pos = 0;
		int32_t acc       = 0;
		int32_t sample;
		bool moved = false;

		while (update_freq(c, &pos)) {
			moved = true;
			acc += (int32_t)((pos - prev_pos) >> sh) * c->val;
			c->noise.lfsr_reg = (c->noise.lfsr_reg << 1) |
				(c->val >= VOL_INIT_MAX/MAX_CHAN_VOLUME);

			if (c->noise.lfsr_wide) {
				c->val = !(((c->noise.lfsr_reg >> 14) & 1) ^
						((c->noise.lfsr_reg >> 13) & 1)) ?
					VOL_INIT_MAX / MAX_CHAN_VOLUME :
					VOL_INIT_MIN / MAX_CHAN_VOLUME;
			} else {
				c->val = !(((c->noise.lfsr_reg >> 6) & 1) ^
						((c->noise.lfsr_reg >> 5) & 1)) ?
					VOL_INIT_MAX / MAX_CHAN_VOLUME :
					VOL_INIT_MIN / MAX_CHAN_VOLUME;
			}

			prev_pos = pos;
		}
		if (moved) {
			acc += (int32_t)((c->freq_inc - prev_pos) >> sh) * c->val;
			sample = acc / (int32_t)(c->freq_inc >> sh);
		} else
			sample = c->val; /* as for the square */

		if (c->muted)
			continue;

		sample *= c->volume;
		sample /= 4;

		samples[i + 0] += (sample * c->on_left * vol_l) >> 1;
		samples[i + 1] += (sample * c->on_right * vol_r) >> 1;
	}
}

/* Render any part of an audio buffer with the APU register values which
 * were effective at that point in *emulated CPU time*.  In particular,
 * streaming voice via FF30..FF3F must not see only the final wave table. */
static void audio_render_samples(int16_t *stream, const uint_fast16_t count)
{
	update_square(stream, 0, count);
	update_square(stream, 1, count);
	update_wave(stream, count);
	update_noise(stream, count);
}

/** SDL2-style full-buffer callback, retained for HWAY and other callers. */
void audio_callback(void *userdata, int16_t *stream, size_t len)
{
	(void)userdata;
	memset(stream, 0, len);
	audio_render_samples(stream, AUDIO_NSAMPLES);
}

/* Unlike a frame-end APU callback, this tracks register writes inside the
 * emulated frame.  No allocation, events array, or additional output buffer.
 * At most AUDIO_SAMPLES stereo frames are rendered per GB frame in total. */
static int16_t *audio_frame_stream;
static uint_fast16_t audio_frame_pos;

static void audio_frame_sync(const uint32_t cycles)
{
	if (!audio_frame_stream) return;
	const uint_fast16_t target = cycles >= 70224u ? AUDIO_SAMPLES :
		(uint_fast16_t)((cycles * (uint32_t)AUDIO_SAMPLES) / 70224u);
	if (target <= audio_frame_pos) return;
	const uint_fast16_t frames = target - audio_frame_pos;
	audio_render_samples(audio_frame_stream + audio_frame_pos * 2u, frames * 2u);
	audio_frame_pos = target;
}

void audio_frame_begin(int16_t *stream)
{
	memset(stream, 0, AUDIO_BUFFER_SIZE_BYTES);
	audio_frame_pos = 0;
	audio_frame_stream = stream;
}

void audio_frame_end(void)
{
	audio_frame_sync(70224u);
	audio_frame_stream = NULL;
}

#ifdef HWAY
#include "hway/ay_hway.h"

/* Game Boy -> AY-3-8910 pair, by the channel state once a frame (the
 * envelopes step at 64 Hz, the sweep at 128 Hz, so it is heard almost as is):
 *   CH1, CH2 (squares)  -> AY 0, tones A and B (the duty cycle is lost: 50%)
 *   CH3 (wave)          -> AY 0, tone C (a square of the wave's pitch)
 *   CH4 (noise)         -> AY 1, noise on channel A (the 7-bit mode is lost)
 * The AY period of a Game Boy one: f = 131072 / (2048 - x) for a square,
 * 65536 / (2048 - x) for the wave; AY: f = clock / (16 N). */
static uint8_t hway_shadow[2][16];
static bool hway_shadow_valid;

static void hway_set(const unsigned chip, const uint8_t reg, const uint8_t value)
{
	if (hway_shadow_valid && hway_shadow[chip][reg] == value)
		return;
	hway_shadow[chip][reg] = value;
	hway_write_register(chip, reg, value);
}

/* the Game Boy volume is linear, the AY one ~3 dB a step */
static const uint8_t hway_level[16] = {
	0, 7, 9, 10, 11, 12, 12, 13, 13, 14, 14, 14, 14, 15, 15, 15
};

static uint8_t hway_volume(const struct chan *c, unsigned linear)
{
	if (!c->powered || !c->enabled || c->muted || !(c->on_left || c->on_right))
		return 0;
	/* NR50: the louder side, 0..7 -> 1/8..8/8 */
	linear = linear * ((unsigned)MAX(vol_l, vol_r) + 1) / 8;
	return hway_level[linear > 15 ? 15 : linear];
}

static void hway_tone(const unsigned chip, const unsigned ay_chan, uint32_t gb_period, const unsigned mult)
{
	/* N = clock * mult * (2048 - x) / 2097152, rounded */
	uint32_t n = (uint32_t)(((uint64_t)HWAY_AY_CLOCK * mult * gb_period + 1048576u) / 2097152u);
	if (n < 1) n = 1;
	if (n > 4095) n = 4095;
	hway_set(chip, (uint8_t)(ay_chan * 2), (uint8_t)n);
	hway_set(chip, (uint8_t)(ay_chan * 2 + 1), (uint8_t)(n >> 8));
}

void audio_hway_update(void)
{
	static const uint8_t lfsr_div_lut[] = { 8, 16, 32, 48, 64, 80, 96, 112 };

	hway_heartbeat();
	if (hway_take_silenced())
		hway_shadow_valid = false; /* core 1 zeroed the volumes: all again */

	/* AY 0: three tones, no noise; AY 1: the noise on channel A only */
	hway_set(0, 7, 0x38);
	hway_set(1, 7, 0xb7);

	for (unsigned i = 0; i < 2; i++) {
		const struct chan *c = chans + i;
		hway_tone(0, i, 2048u - (c->freq & 0x7ff), 1);
		hway_set(0, (uint8_t)(8 + i), hway_volume(c, c->volume));
	}

	{
		const struct chan *c = chans + 2;
		static const uint8_t wave_linear[4] = { 0, 15, 8, 4 }; /* NR32: 0, 100, 50, 25% */
		hway_tone(0, 2, 2048u - (c->freq & 0x7ff), 2);
		hway_set(0, 10, hway_volume(c, wave_linear[c->volume & 3]));
	}

	{
		const struct chan *c = chans + 3;
		/* the LFSR clocks 4194304 / (div << s) times a second, the AY noise
		 * clock / (16 NP) */
		const uint32_t div = (uint32_t)lfsr_div_lut[c->noise.lfsr_div & 7] << (c->freq & 15);
		uint32_t np = (uint32_t)(((uint64_t)HWAY_AY_CLOCK * div + 33554432u) / 67108864u);
		if (np < 1) np = 1;
		if (np > 31) np = 31;
		hway_set(1, 6, (uint8_t)np);
		hway_set(1, 8, hway_volume(c, c->volume));
	}

	hway_shadow_valid = true;
}
#endif

static void chan_trigger(uint_fast8_t i)
{
	struct chan *c = chans + i;

	chan_enable(i, 1);
	c->volume = c->volume_init;

	// volume envelope
	{
		uint8_t val =
			audio_mem[(0xFF12 + (i * 5)) - AUDIO_ADDR_COMPENSATION];

		c->env.step = val & 0x07;
		c->env.up   = val & 0x08 ? 1 : 0;
		c->env.inc  = c->env.step ?
			(FREQ_INC_REF * 64ul) / ((uint32_t)c->env.step * AUDIO_SAMPLE_RATE) :
			(8ul * FREQ_INC_REF) / AUDIO_SAMPLE_RATE ;
		c->env.counter = 0;
	}

	// freq sweep
	if (i == 0) {
		uint8_t val = audio_mem[0xFF10 - AUDIO_ADDR_COMPENSATION];

		c->sweep.freq  = c->freq;
		c->sweep.rate  = (val >> 4) & 0x07;
		c->sweep.up    = !(val & 0x08);
		c->sweep.shift = (val & 0x07);
		c->sweep.inc   = c->sweep.rate ?
			((128 * FREQ_INC_REF) / (c->sweep.rate * AUDIO_SAMPLE_RATE)) : 0;
		c->sweep.counter = FREQ_INC_REF;
	}

	int len_max = 64;

	if (i == 2) { // wave
		len_max = 256;
		c->val = 0;
	} else if (i == 3) { // noise
		c->noise.lfsr_reg = 0xFFFF;
		c->val = VOL_INIT_MIN / MAX_CHAN_VOLUME;
	}

	c->len.inc = (256 * FREQ_INC_REF) / (AUDIO_SAMPLE_RATE * (len_max - c->len.load));
	c->len.counter = 0;
}

/**
 * Read audio register.
 * \param addr	Address of audio register. Must be 0xFF10 <= addr <= 0xFF3F.
 *				This is not checked in this function.
 * \return	Byte at address.
 */
uint8_t audio_read(const uint16_t addr)
{
	static const uint8_t ortab[] = {
		0x80, 0x3f, 0x00, 0xff, 0xbf,
		0xff, 0x3f, 0x00, 0xff, 0xbf,
		0x7f, 0xff, 0x9f, 0xff, 0xbf,
		0xff, 0xff, 0x00, 0x00, 0xbf,
		0x00, 0x00, 0x70,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	return audio_mem[addr - AUDIO_ADDR_COMPENSATION] |
		ortab[addr - AUDIO_ADDR_COMPENSATION];
}

/**
 * Write audio register.
 * \param addr	Address of audio register. Must be 0xFF10 <= addr <= 0xFF3F.
 *				This is not checked in this function.
 * \param val	Byte to write at address.
 */
void audio_write(const uint16_t addr, const uint8_t val)
{
	/* Finish samples up to this CPU write using the PREVIOUS register
	 * values.  That preserves the succession of CH3 wave-RAM tables for PCM. */
	audio_frame_sync(gb_audio_cycles);
	/* Find sound channel corresponding to register address. */
	uint_fast8_t i;

	if(addr == 0xFF26)
	{
		audio_mem[addr - AUDIO_ADDR_COMPENSATION] = val & 0x80;
		/* On APU power off, clear all registers apart from wave
		 * RAM. */
		if((val & 0x80) == 0)
		{
			memset(audio_mem, 0x00, 0xFF26 - AUDIO_ADDR_COMPENSATION);
			chans[0].enabled = false;
			chans[1].enabled = false;
			chans[2].enabled = false;
			chans[3].enabled = false;
		}

		return;
	}

	/* Ignore register writes if APU powered off. */
	if(audio_mem[0xFF26 - AUDIO_ADDR_COMPENSATION] == 0x00)
		return;

	audio_mem[addr - AUDIO_ADDR_COMPENSATION] = val;
	i = (addr - AUDIO_ADDR_COMPENSATION) / 5;

	switch (addr) {
	case 0xFF12:
	case 0xFF17:
	case 0xFF21: {
		chans[i].volume_init = val >> 4;
		chans[i].powered     = (val >> 3) != 0;

		// "zombie mode" stuff, needed for Prehistorik Man and probably
		// others
		if (chans[i].powered && chans[i].enabled) {
			if ((chans[i].env.step == 0 && chans[i].env.inc != 0)) {
				if (val & 0x08) {
					chans[i].volume++;
				} else {
					chans[i].volume += 2;
				}
			} else {
				chans[i].volume = 16 - chans[i].volume;
			}

			chans[i].volume &= 0x0F;
			chans[i].env.step = val & 0x07;
		}
	} break;

	case 0xFF1C:
		chans[i].volume = chans[i].volume_init = (val >> 5) & 0x03;
		break;

	case 0xFF11:
	case 0xFF16:
	case 0xFF20: {
		const uint8_t duty_lookup[] = { 0x10, 0x30, 0x3C, 0xCF };
		chans[i].len.load = val & 0x3f;
		chans[i].square.duty = duty_lookup[val >> 6];
		break;
	}

	case 0xFF1B:
		chans[i].len.load = val;
		break;

	case 0xFF13:
	case 0xFF18:
	case 0xFF1D:
		chans[i].freq &= 0xFF00;
		chans[i].freq |= val;
		break;

	case 0xFF1A:
		chans[i].powered = (val & 0x80) != 0;
		chan_enable(i, val & 0x80);
		break;

	case 0xFF14:
	case 0xFF19:
	case 0xFF1E:
		chans[i].freq &= 0x00FF;
		chans[i].freq |= ((val & 0x07) << 8);
		/* Intentional fall-through. */
	case 0xFF23:
		chans[i].len.enabled = val & 0x40 ? 1 : 0;
		if (val & 0x80)
			chan_trigger(i);

		break;

	case 0xFF22:
		chans[3].freq = val >> 4;
		chans[3].noise.lfsr_wide = !(val & 0x08);
		chans[3].noise.lfsr_div = val & 0x07;
		break;

	case 0xFF24:
	{
		vol_l = ((val >> 4) & 0x07);
		vol_r = (val & 0x07);
		break;
	}

	case 0xFF25:
		for (uint_fast8_t j = 0; j < 4; j++) {
			chans[j].on_left  = (val >> (4 + j)) & 1;
			chans[j].on_right = (val >> j) & 1;
		}
		break;
	}
}

void audio_init(void)
{
	/* Initialise channels and samples. */
	memset(chans, 0, sizeof(chans));
	chans[0].val = chans[1].val = -1;

	/* Initialise IO registers. */
	{
		const uint8_t regs_init[] = { 0x80, 0xBF, 0xF3, 0xFF, 0x3F,
					      0xFF, 0x3F, 0x00, 0xFF, 0x3F,
					      0x7F, 0xFF, 0x9F, 0xFF, 0x3F,
					      0xFF, 0xFF, 0x00, 0x00, 0x3F,
					      0x77, 0xF3, 0xF1 };

		for(uint_fast8_t i = 0; i < sizeof(regs_init); ++i)
			audio_write(0xFF10 + i, regs_init[i]);
	}

	/* Initialise Wave Pattern RAM. */
	{
		const uint8_t wave_init[] = { 0xac, 0xdd, 0xda, 0x48,
					      0x36, 0x02, 0xcf, 0x16,
					      0x2c, 0x04, 0xe5, 0x2c,
					      0xac, 0xdd, 0xda, 0x48 };

		for(uint_fast8_t i = 0; i < sizeof(wave_init); ++i)
			audio_write(0xFF30 + i, wave_init[i]);
	}
}
