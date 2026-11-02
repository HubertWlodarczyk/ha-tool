/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Precomputed LC3 frames of sine tones (generated: lc3_frames.c, see tools/lc3gen). The firmware
 * has no LC3 encoder: it loops the frames of a tone and sets the level by rewriting the global
 * gain field of each frame.
 */

#ifndef HA_TOOL_LC3_FRAMES_H_
#define HA_TOOL_LC3_FRAMES_H_

#include <stdint.h>

#define LC3_TAB_COUNT 16

/* Largest frame of the table (48_6); lc3_frames.c checks it, audio_tx.c against the ISO MTU */
#define LC3_TAB_MAX_OCTETS 155

/* The frames are stored at 0 dBFS and the gain field only lowers them. At the lower bitrates the
 * decoder overshoots a full scale sine by up to 0.7 dB and clips it (3rd harmonic at -30 dBc),
 * so the highest level keeps headroom; lc3gen checks that no tone gets near full scale there.
 */
#define LC3_TAB_LEVEL_MAX (-2)
#define LC3_TAB_LEVEL_MIN (-40)

struct lc3_tab_cfg {
	const char *name;  /* BAP configuration: 8_1 .. 48_6 */
	uint32_t sr_hz;    /* nominal sampling rate; 44100 is coded like 48 kHz */
	uint16_t frame_us; /* nominal frame duration: 7500 / 10000 */
	uint16_t octets;
	uint16_t samples; /* per frame */
	uint8_t cycle;    /* tone m repeats after cycle/gcd(m, cycle) frames */
	uint8_t gidx_bit; /* global gain: bit offset from the end of the frame */
	uint8_t gidx_min; /* lowest stored global gain */
	uint16_t m_min, m_max;
	const uint8_t *silence; /* one frame */
	/* Frames of tone m: blob[offs[m - m_min]] .. blob[offs[m - m_min + 1]] */
	const uint32_t *offs;
	const uint8_t *blob;
};

extern const struct lc3_tab_cfg lc3_tab[LC3_TAB_COUNT];

#endif /* HA_TOOL_LC3_FRAMES_H_ */
