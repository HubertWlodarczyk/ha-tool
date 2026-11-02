/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Measures the tone quality of the LC3 frame table (ha_tool/src/lc3_frames.c), decoded the way
 * the firmware sends it: the frames of a tone in a loop, the level set by rewriting the global
 * gain field. For comparison every tone is also encoded the usual way (a continuous sine through
 * a fresh encoder), which shows what the codec itself gives at that bitrate.
 *
 * Levels above LC3_TAB_LEVEL_MAX are measured too: they show the clipping the limit avoids.
 *
 * Per tone and level: SNR (least squares fit of the tone, residual = noise + distortion), SFDR
 * (tone vs. the largest other spectral line, Blackman-Harris window), the offset of that line from
 * the tone, and the envelope ripple (amplitude fitted over short windows).
 *
 *   lc3probe report <csv>                      every tone, summary on stdout
 *   lc3probe wav <out.wav> <cfg> <Hz> <dBFS> [clean|loss|phase|ref]
 *       loss: one frame per second lost (decoder concealment), phase: one frame per second
 *       skipped (phase jump), ref: continuous encoding instead of the table
 *
 * Build and run: scripts/gen_lc3_frames.sh --probe
 */

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lc3.h>

#include "lc3_frames.h"

#define NFFT      16384
#define MAX_NS    480
#define MAX_BYTES 400
#define SKIP      4 /* frames: decoder start-up */
#define WAV_SEC   4

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef double complex cplx;

static const int levels[] = {0, -1, -2, -3, -6, -12, -20, -40};

struct metrics {
	double snr, sfdr, spur_off, env, peak;
};

/* Same rounding as audio_tx.c level_to_delta() */
static int level_to_delta(int level_db)
{
	const int x = level_db * 14;

	return x >= 0 ? (x + 5) / 10 : (x - 5) / 10;
}

static void patch_gain(uint8_t *frame, int len, int bit, int delta)
{
	int v = 0, g;

	for (int i = 0; i < 8; i++) {
		const int b = bit + i;

		v |= ((frame[len - 1 - b / 8] >> (b % 8)) & 1) << i;
	}

	g = v + delta;
	g = g < 0 ? 0 : g > 255 ? 255 : g;

	for (int i = 0; i < 8; i++) {
		const int b = bit + i;
		uint8_t *p = &frame[len - 1 - b / 8];

		*p = (*p & ~(1 << (b % 8))) | (((g >> i) & 1) << (b % 8));
	}
}

static int coded_sr(const struct lc3_tab_cfg *c)
{
	return c->sr_hz == 44100 ? 48000 : (int)c->sr_hz;
}

/* Coding-domain frequency of tone m */
static double tone_hz(const struct lc3_tab_cfg *c, int m)
{
	return (double)m * coded_sr(c) / ((double)c->cycle * c->samples);
}

static void fft(cplx *x, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			const cplx t = x[i];

			x[i] = x[j];
			x[j] = t;
		}
	}

	for (int len = 2; len <= n; len <<= 1) {
		const cplx w = cexp(-2 * I * M_PI / len);

		for (int i = 0; i < n; i += len) {
			cplx wk = 1;

			for (int k = 0; k < len / 2; k++, wk *= w) {
				const cplx u = x[i + k], v = x[i + k + len / 2] * wk;

				x[i + k] = u + v;
				x[i + k + len / 2] = u - v;
			}
		}
	}
}

/* Amplitude of a*sin + b*cos at f fitted to x[0..n) (t0: sample index of x[0]) */
static double fit(const float *x, int n, long t0, double f, int sr, double *res_energy)
{
	double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0, e = 0;

	for (int i = 0; i < n; i++) {
		const double w = 2 * M_PI * f * (double)(t0 + i) / sr;
		const double s = sin(w), co = cos(w);

		ss += s * s;
		cc += co * co;
		sc += s * co;
		xs += x[i] * s;
		xc += x[i] * co;
		e += (double)x[i] * x[i];
	}

	const double det = ss * cc - sc * sc;
	const double a = (xs * cc - xc * sc) / det;
	const double b = (xc * ss - xs * sc) / det;

	if (res_energy != NULL) {
		*res_energy = e - (a * a * ss + b * b * cc + 2 * a * b * sc);
	}

	return sqrt(a * a + b * b);
}

static struct metrics analyze(const float *x, int n, double f, int sr)
{
	static cplx buf[NFFT];
	struct metrics m;
	double res, amp = fit(x, n, 0, f, sr, &res);
	const double sig = amp * amp / 2 * n;
	const int tone_bin = (int)lround(f * NFFT / sr);
	double tone_pk = 0, spur_pk = 0;
	int spur_bin = 0;

	m.snr = 10 * log10(sig / (res > 1e-20 ? res : 1e-20));
	m.peak = 0;
	for (int i = 0; i < n; i++) {
		m.peak = fmax(m.peak, fabs(x[i]));
	}

	for (int i = 0; i < NFFT; i++) {
		/* 4-term Blackman-Harris: sidelobes below -92 dB */
		const double p = 2 * M_PI * i / NFFT;
		const double w =
			0.35875 - 0.48829 * cos(p) + 0.14128 * cos(2 * p) - 0.01168 * cos(3 * p);

		buf[i] = i < n ? x[i] * w : 0;
	}
	fft(buf, NFFT);

	for (int i = 0; i <= NFFT / 2; i++) {
		const double p = cabs(buf[i]);

		if (abs(i - tone_bin) <= 6) {
			tone_pk = p > tone_pk ? p : tone_pk;
		} else if (p > spur_pk) {
			spur_pk = p;
			spur_bin = i;
		}
	}

	m.sfdr = 20 * log10(tone_pk / (spur_pk > 1e-12 ? spur_pk : 1e-12));
	m.spur_off = (double)spur_bin * sr / NFFT - f;

	/* Envelope: amplitude fitted over windows of max(2 ms, 2 periods), half overlapping */
	const int win = (int)fmax(0.002 * sr, 2.0 * sr / f);
	double amin = 1e9, amax = 0;

	for (int i = 0; i + win <= n; i += win / 2) {
		const double a = fit(x + i, win, i, f, sr, NULL);

		amin = a < amin ? a : amin;
		amax = a > amax ? a : amax;
	}
	m.env = 20 * log10(amax / amin);

	return m;
}

/* Frames of tone m as the firmware loops them */
static const uint8_t *tone_frames(const struct lc3_tab_cfg *c, int m, int *k)
{
	const int i = m - c->m_min;

	*k = (int)(c->offs[i + 1] - c->offs[i]) / c->octets;
	return &c->blob[c->offs[i]];
}

enum scenario {
	SC_CLEAN,
	SC_LOSS,
	SC_PHASE,
	SC_REF
};

/* Decodes `nframes` of tone m at level_db into pcm (float, coding domain) */
static void render(const struct lc3_tab_cfg *c, int m, int level_db, enum scenario sc, int nframes,
		   float *pcm)
{
	char dmem[lc3_decoder_size(10000, 48000)];
	char emem[lc3_encoder_size(10000, 48000)];
	const int sr = coded_sr(c), ns = c->samples;
	const int per_sec = (int)lround(1e6 / c->frame_us);
	lc3_decoder_t dec = lc3_setup_decoder(c->frame_us, sr, 0, dmem);
	lc3_encoder_t enc = lc3_setup_encoder(c->frame_us, sr, 0, emem);
	const double f = tone_hz(c, m);
	const double amp = pow(10, level_db / 20.0);
	int k;
	const uint8_t *frames = tone_frames(c, m, &k);
	long src = 0;
	float in[MAX_NS];

	for (int i = 0; i < nframes; i++) {
		uint8_t frame[MAX_BYTES];
		bool lost = false;

		if (sc == SC_REF) {
			for (int j = 0; j < ns; j++) {
				in[j] = (float)(amp *
						sin(2 * M_PI * f * (double)(i * ns + j) / sr));
			}
			lc3_encode(enc, LC3_PCM_FORMAT_FLOAT, in, 1, c->octets, frame);
		} else {
			if (sc == SC_PHASE && i > 0 && i % per_sec == 0) {
				src++; /* the firmware skipped a frame */
			}
			lost = sc == SC_LOSS && i > 0 && i % per_sec == 0;
			memcpy(frame, frames + (src % k) * c->octets, c->octets);
			patch_gain(frame, c->octets, c->gidx_bit, level_to_delta(level_db));
			src++;
		}

		lc3_decode(dec, lost ? NULL : frame, c->octets, LC3_PCM_FORMAT_FLOAT, pcm + i * ns,
			   1);
	}
}

static const struct lc3_tab_cfg *find_cfg(const char *name)
{
	for (int i = 0; i < LC3_TAB_COUNT; i++) {
		if (strcmp(lc3_tab[i].name, name) == 0) {
			return &lc3_tab[i];
		}
	}

	return NULL;
}

static int cmp(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static int report(const char *csv_path)
{
	static float pcm[(NFFT / 80 + SKIP + 2) * MAX_NS + NFFT];
	FILE *csv = fopen(csv_path, "w");

	if (csv == NULL) {
		perror(csv_path);
		return 1;
	}

	fprintf(csv, "cfg,m,freq_hz,level_db,frames,snr,sfdr,spur_off_hz,env_db,peak,"
		     "ref_snr,ref_sfdr,ref_spur_off_hz,ref_env_db\n");
	printf("SNR at the highest level (%d dBFS); SFDR and envelope over the levels up to it\n",
	       LC3_TAB_LEVEL_MAX);
	printf("%-6s %5s | %-24s | %-24s | %s\n", "cfg", "tones", "table: SNR min/med  SFDR",
	       "continuous: SNR min/med", "env max dB, worst SNR tone");

	for (int ci = 0; ci < LC3_TAB_COUNT; ci++) {
		const struct lc3_tab_cfg *c = &lc3_tab[ci];
		const int sr = coded_sr(c), ns = c->samples;
		const int nframes = (NFFT + ns - 1) / ns + SKIP;
		const int ntones = c->m_max - c->m_min + 1;
		double *snr = calloc(ntones, sizeof(double)),
		       *rsnr = calloc(ntones, sizeof(double));
		double min_sfdr = 1e9, max_env = 0, worst_f = 0;

		for (int m = c->m_min; m <= c->m_max; m++) {
			const double f = tone_hz(c, m);
			const double f_real = f * c->sr_hz / sr;
			struct metrics ref;
			int k;

			(void)tone_frames(c, m, &k);

			for (size_t li = 0; li < ARRAY_SIZE(levels); li++) {
				struct metrics t;

				/* Reference: the same tone and level, encoded continuously */
				render(c, m, levels[li], SC_REF, nframes, pcm);
				ref = analyze(pcm + SKIP * ns, NFFT, f, sr);
				render(c, m, levels[li], SC_CLEAN, nframes, pcm);
				t = analyze(pcm + SKIP * ns, NFFT, f, sr);
				fprintf(csv,
					"%s,%d,%.2f,%d,%d,%.1f,%.1f,%.1f,%.2f,%.4f,%.1f,%.1f,%.1f,%"
					".2f\n",
					c->name, m, f_real, levels[li], k, t.snr, t.sfdr,
					t.spur_off, t.env, t.peak, ref.snr, ref.sfdr, ref.spur_off,
					ref.env);
				if (levels[li] == LC3_TAB_LEVEL_MAX) {
					snr[m - c->m_min] = t.snr;
					rsnr[m - c->m_min] = ref.snr;
				}
				if (levels[li] <= LC3_TAB_LEVEL_MAX) {
					min_sfdr = fmin(t.sfdr, min_sfdr);
					max_env = fmax(t.env, max_env);
				}
			}
		}

		double worst = 1e9;

		for (int i = 0; i < ntones; i++) {
			if (snr[i] < worst) {
				worst = snr[i];
				worst_f = tone_hz(c, c->m_min + i) * c->sr_hz / sr;
			}
		}

		qsort(snr, ntones, sizeof(double), cmp);
		qsort(rsnr, ntones, sizeof(double), cmp);
		printf("%-6s %5d | %5.1f %5.1f   %6.1f    | %5.1f %5.1f              | %5.2f, %.0f "
		       "Hz\n",
		       c->name, ntones, snr[0], snr[ntones / 2], min_sfdr, rsnr[0],
		       rsnr[ntones / 2], max_env, worst_f);
		fflush(stdout);
		free(snr);
		free(rsnr);
	}

	fclose(csv);
	return 0;
}

static void put_le(FILE *f, uint32_t v, int n)
{
	for (int i = 0; i < n; i++) {
		fputc((v >> (8 * i)) & 0xff, f);
	}
}

static int wav(int argc, char **argv)
{
	static float pcm[(WAV_SEC * 1000000 / 7500 + 2) * MAX_NS];
	const struct lc3_tab_cfg *c = find_cfg(argv[3]);
	enum scenario sc = SC_CLEAN;

	if (c == NULL) {
		fprintf(stderr, "unknown cfg %s\n", argv[3]);
		return 1;
	}

	if (argc > 6) {
		sc = strcmp(argv[6], "loss") == 0    ? SC_LOSS
		     : strcmp(argv[6], "phase") == 0 ? SC_PHASE
		     : strcmp(argv[6], "ref") == 0   ? SC_REF
						     : SC_CLEAN;
	}

	const int sr = coded_sr(c), ns = c->samples;
	const double f_req = atof(argv[4]) * sr / c->sr_hz;
	const int m = (int)lround(f_req * c->cycle * ns / sr);
	const int nframes = (int)(WAV_SEC * 1000000LL / c->frame_us);
	const int n = nframes * ns;
	FILE *out;

	if (m < c->m_min || m > c->m_max) {
		fprintf(stderr, "tone out of range\n");
		return 1;
	}

	render(c, m, atoi(argv[5]), sc, nframes, pcm);

	out = fopen(argv[2], "wb");
	if (out == NULL) {
		perror(argv[2]);
		return 1;
	}

	fwrite("RIFF", 1, 4, out);
	put_le(out, 36 + n * 2, 4);
	fwrite("WAVEfmt ", 1, 8, out);
	put_le(out, 16, 4);
	put_le(out, 1, 2);
	put_le(out, 1, 2);
	put_le(out, c->sr_hz, 4);
	put_le(out, c->sr_hz * 2, 4);
	put_le(out, 2, 2);
	put_le(out, 16, 2);
	fwrite("data", 1, 4, out);
	put_le(out, n * 2, 4);
	for (int i = 0; i < n; i++) {
		const float s = pcm[i] > 1 ? 1 : pcm[i] < -1 ? -1 : pcm[i];

		put_le(out, (uint16_t)(int16_t)lrintf(s * 32767), 2);
	}
	fclose(out);

	printf("%s: %s tone %.2f Hz, %s dBFS, %s\n", argv[2], c->name,
	       tone_hz(c, m) * c->sr_hz / sr, argv[5], argc > 6 ? argv[6] : "clean");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "report") == 0) {
		return report(argv[2]);
	}

	if (argc >= 6 && strcmp(argv[1], "wav") == 0) {
		return wav(argc, argv);
	}

	fprintf(stderr, "usage: lc3probe report <csv> | wav <out.wav> <cfg> <Hz> <dBFS> "
			"[clean|loss|phase|ref]\n");
	return 1;
}
