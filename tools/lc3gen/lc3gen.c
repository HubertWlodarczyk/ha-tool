/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Generates the LC3 frame table of leabench (leabench/src/lc3_frames.c): sine tones encoded once,
 * on the build host, so the firmware only copies frames.
 *
 * Every LC3 configuration of BAP (8..48 kHz incl. 44.1, 7.5 and 10 ms) gets the tones that repeat
 * after 1 or 2 frames (--max-cycle): f = m * fs / (2 * N), N = samples per frame. A frame holding
 * a whole number of periods can be sent in a loop without a click; the encoder is run on a steady
 * sine first and the cycle is stored without warmup frames.
 *
 * Level: frames are encoded at 0 dBFS. The decoder scales the whole spectrum by 10^(g_idx/28),
 * g_idx being the 8 bit global gain at a fixed position of the side information (after the
 * bandwidth and the nq field), so the firmware sets any level by rewriting that field.
 *
 * Headroom: at the lower bitrates the decoder overshoots a full scale sine by up to 0.7 dB and
 * clips it, so the firmware's highest level is LEVEL_MAX (-2 dBFS, lc3_frames.h), not 0 dBFS.
 *
 * Self test (generation fails otherwise): every entry, decoded in a loop at LEVEL_MAX, -20 and
 * -40 dBFS, has the right frequency, a flat envelope across the loop point, enough SNR, the
 * expected level after the gain field was rewritten, and peaks below MAX_PEAK (no clipping).
 *
 * Build and run: scripts/gen_lc3_frames.sh
 */

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lc3.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))
#define MAX_NS        480
#define MAX_BYTES     400
#define PREROLL       64 /* frames encoded before the stored cycle (bit allocation settles ~20) */
#define MAX_LOOP      4  /* longest stored cycle, frames (--max-cycle) */
#define VERIFY        64 /* frames after the preroll that must repeat the cycle */
#define CHECK_LOOPS   50
#define F_MIN         100.0
#define F_MAX         8000.0
#define LEVEL_MIN     -40           /* dBFS, lowest level the firmware may set */
#define LEVEL_MAX     -2            /* dBFS, highest level (LC3_TAB_LEVEL_MAX) */
#define GAIN_STEP     (20.0 / 28.0) /* dB per g_idx step */

/* Self test limits. The worst cases are the lowest bitrates (8_2: 30 octets = 24 kbit/s gives
 * about 30 dB, the same as a continuous encoding; scripts/gen_lc3_frames.sh --probe).
 */
#define MIN_SNR      28.0 /* dB, noise + distortion vs. the tone */
#define MAX_ENV      1.0  /* dB, frame RMS variation incl. across the loop point */
#define MAX_REF_ERR  1.0  /* dB, decoded level at LEVEL_MAX vs. nominal */
#define MAX_GAIN_ERR 0.3  /* dB, level after rewriting g_idx vs. reference + delta */
#define MAX_PEAK     0.95 /* decoded peak (full scale 1.0) at LEVEL_MAX: -0.45 dBFS */

struct cfg {
	const char *name;
	int sr;    /* nominal sampling rate (44100 is coded as 48 kHz) */
	int dt_us; /* 7500 / 10000 (nominal) */
	int octets;
};

static const struct cfg cfgs[] = {
	{"8_1", 8000, 7500, 26},      {"8_2", 8000, 10000, 30},    {"16_1", 16000, 7500, 30},
	{"16_2", 16000, 10000, 40},   {"24_1", 24000, 7500, 45},   {"24_2", 24000, 10000, 60},
	{"32_1", 32000, 7500, 60},    {"32_2", 32000, 10000, 80},  {"441_1", 44100, 7500, 97},
	{"441_2", 44100, 10000, 130}, {"48_1", 48000, 7500, 75},   {"48_2", 48000, 10000, 100},
	{"48_3", 48000, 7500, 90},    {"48_4", 48000, 10000, 120}, {"48_5", 48000, 7500, 117},
	{"48_6", 48000, 10000, 155},
};

/* Generator output */
static uint8_t *blob;
static size_t blob_len, blob_cap;

static int max_cycle = 2;
static double min_snr = 1e9, worst_env, worst_lvl, worst_peak;
static const char *worst_snr_at = "";

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "lc3gen: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static size_t blob_add(const uint8_t *data, size_t len)
{
	const size_t off = blob_len;

	if (blob_len + len > blob_cap) {
		blob_cap = (blob_cap + len) * 2;
		blob = realloc(blob, blob_cap);
		if (blob == NULL) {
			die("out of memory");
		}
	}

	memcpy(blob + blob_len, data, len);
	blob_len += len;

	return off;
}

static int enc_sr(const struct cfg *c)
{
	return c->sr == 44100 ? 48000 : c->sr;
}

/* Bit offset of g_idx in the backward (plain bits) stream, counted from the last byte's LSB */
static int gidx_bit(const struct cfg *c, int ns)
{
	const int sr = enc_sr(c);
	const int nbw = sr == 8000 ? 0 : sr == 16000 ? 1 : sr == 48000 ? 3 : 2;
	const int ne = sr == 48000 ? ns * 5 / 6 : ns;
	const int nnq = 4 + (ne > 32) + (ne > 64) + (ne > 128) + (ne > 256) + (ne > 512);

	return nbw + nnq + 1;
}

static int get_gidx(const uint8_t *frame, int len, int bit)
{
	int v = 0;

	for (int i = 0; i < 8; i++, bit++) {
		v |= ((frame[len - 1 - bit / 8] >> (bit % 8)) & 1) << i;
	}

	return v;
}

static void set_gidx(uint8_t *frame, int len, int bit, int v)
{
	for (int i = 0; i < 8; i++, bit++) {
		uint8_t *b = &frame[len - 1 - bit / 8];

		*b = (*b & ~(1 << (bit % 8))) | (((v >> i) & 1) << (bit % 8));
	}
}

/* Decodes `cycle` frames in a loop (with the gain field shifted by `delta`) and checks frequency,
 * level, envelope, SNR and peak against a sine of `f` Hz (in coding domain) at `level_db`; `ref`:
 * the reference measurement (looser level tolerance).
 */
static double check(const struct cfg *c, double f, const uint8_t *frames, int k, int delta,
		    double level_db, bool ref, const char *what)
{
	static float pcm[CHECK_LOOPS * MAX_LOOP * MAX_NS];
	uint8_t frame[MAX_BYTES];
	char mem[lc3_decoder_size(10000, 48000)];
	const int sr = enc_sr(c);
	const int ns = lc3_frame_samples(c->dt_us, sr);
	const int nframes = CHECK_LOOPS * k;
	lc3_decoder_t dec = lc3_setup_decoder(c->dt_us, sr, 0, mem);
	const int skip = 2 * ns; /* decoder start-up (overlap) */
	double sum_ss = 0, sum_cc = 0, sum_sc = 0, sum_xs = 0, sum_xc = 0, energy = 0;
	double fmin_rms = 1e9, fmax_rms = 0, peak = 0;
	int n = 0;

	for (int i = 0; i < nframes; i++) {
		memcpy(frame, frames + (i % k) * c->octets, c->octets);
		if (delta != 0) {
			const int bit = gidx_bit(c, ns);

			set_gidx(frame, c->octets, bit, get_gidx(frame, c->octets, bit) + delta);
		}

		if (lc3_decode(dec, frame, c->octets, LC3_PCM_FORMAT_FLOAT, pcm + i * ns, 1) != 0) {
			die("%s %.1f Hz %s: decode error", c->name, f, what);
		}
	}

	/* Least squares fit of a*sin + b*cos at f; residual = noise and distortion */
	for (int i = skip; i < nframes * ns; i++, n++) {
		const double w = 2 * M_PI * f * i / sr;
		const double s = sin(w), co = cos(w), x = pcm[i];

		sum_ss += s * s;
		sum_cc += co * co;
		sum_sc += s * co;
		sum_xs += x * s;
		sum_xc += x * co;
		energy += x * x;
		peak = fabs(x) > peak ? fabs(x) : peak;
	}

	const double det = sum_ss * sum_cc - sum_sc * sum_sc;
	const double a = (sum_xs * sum_cc - sum_xc * sum_sc) / det;
	const double b = (sum_xc * sum_ss - sum_xs * sum_sc) / det;
	const double sig = (a * a * sum_ss + b * b * sum_cc + 2 * a * b * sum_sc);
	const double res = energy - sig;
	const double snr = 10 * log10(sig / (res > 1e-20 ? res : 1e-20));
	const double amp_db = 20 * log10(sqrt(a * a + b * b));

	/* Envelope: RMS of every frame after start-up, incl. those across the loop point */
	for (int i = 2; i < nframes; i++) {
		double e = 0;

		for (int j = 0; j < ns; j++) {
			e += (double)pcm[i * ns + j] * pcm[i * ns + j];
		}

		e = sqrt(e / ns);
		fmin_rms = e < fmin_rms ? e : fmin_rms;
		fmax_rms = e > fmax_rms ? e : fmax_rms;
	}

	const double env_db = 20 * log10(fmax_rms / fmin_rms);
	const double lvl_err = fabs(amp_db - level_db);

	if (snr < min_snr) {
		min_snr = snr;
		worst_snr_at = c->name;
	}

	worst_env = env_db > worst_env ? env_db : worst_env;
	worst_lvl = lvl_err > worst_lvl ? lvl_err : worst_lvl;
	worst_peak = peak > worst_peak ? peak : worst_peak;

	/* A tone at another frequency fails the SNR of the fit at f */
	if (snr < MIN_SNR || env_db > MAX_ENV || peak > MAX_PEAK ||
	    lvl_err > (ref ? MAX_REF_ERR : MAX_GAIN_ERR)) {
		die("%s %.2f Hz %s: snr=%.1f dB envelope=%.2f dB level=%.2f dB (want %.2f) "
		    "peak=%.3f",
		    c->name, f, what, snr, env_db, amp_db, level_db, peak);
	}

	return amp_db;
}

/* Encodes a steady sine of f Hz; returns the cycle length (frames) and the frames in `out` */
static int encode_tone(const struct cfg *c, double f, int k0, uint8_t *out, bool *stable)
{
	static float pcm[MAX_NS];
	static uint8_t frames[(PREROLL + VERIFY) * MAX_BYTES];
	char mem[lc3_encoder_size(10000, 48000)];
	const int sr = enc_sr(c);
	const int ns = lc3_frame_samples(c->dt_us, sr);
	lc3_encoder_t enc = lc3_setup_encoder(c->dt_us, sr, 0, mem);
	long t = 0;

	for (int i = 0; i < PREROLL + VERIFY; i++) {
		for (int j = 0; j < ns; j++, t++) {
			/* t modulo the cycle keeps the phase exact over long runs */
			const long tm = t % ((long)ns * k0);

			pcm[j] = (float)sin(2 * M_PI * f * tm / sr);
		}

		if (lc3_encode(enc, LC3_PCM_FORMAT_FLOAT, pcm, 1, c->octets,
			       frames + i * c->octets) != 0) {
			die("%s: encode error", c->name);
		}
	}

	/* Usually the encoder output repeats exactly after k0 frames. Its bit allocation can also
	 * settle into a small oscillation of its own (period 5, 11, ... frames) or never settle
	 * exactly; looping k0 frames is fine then too (same tone, the frames differ only in the
	 * allocation of a few bits), which check() verifies by decoding.
	 */
	*stable = true;
	for (int i = PREROLL + k0; i < PREROLL + VERIFY; i++) {
		if (memcmp(frames + i * c->octets, frames + (i - k0) * c->octets, c->octets) != 0) {
			*stable = false;
			break;
		}
	}

	memcpy(out, frames + PREROLL * c->octets, (size_t)k0 * c->octets);
	return k0;
}

static int gcd(int a, int b)
{
	return b == 0 ? a : gcd(b, a % b);
}

/* What the table holds for one configuration */
struct cfg_table {
	int m_min, m_max; /* tone m has the frequency m * fs / (cycle * N) */
	int gidx_bit;     /* global gain position */
	int gidx_min;     /* lowest stored global gain */
	size_t silence;   /* blob offset of the silence frame */
	uint32_t *offs;   /* blob offsets of the tones, m_max - m_min + 2 entries */
};

static int total_tones;
static int long_loops;

static void parse_args(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--max-cycle") == 0 && i + 1 < argc) {
			max_cycle = atoi(argv[++i]);
		} else {
			die("usage: lc3gen [--max-cycle 1|2|4] > lc3_frames.c");
		}
	}

	if (max_cycle != 1 && max_cycle != 2 && max_cycle != 4) {
		die("--max-cycle must be 1, 2 or 4");
	}
}

/* One frame of silence (the encoder settled on zeros) */
static size_t encode_silence(const struct cfg *c)
{
	char mem[lc3_encoder_size(10000, 48000)];
	float zero[MAX_NS] = {0};
	uint8_t frame[MAX_BYTES];
	lc3_encoder_t enc = lc3_setup_encoder(c->dt_us, enc_sr(c), 0, mem);

	for (int i = 0; i < 4; i++) {
		lc3_encode(enc, LC3_PCM_FORMAT_FLOAT, zero, 1, c->octets, frame);
	}

	return blob_add(frame, c->octets);
}

/* Encodes and checks tone m; returns its blob offset */
static uint32_t add_tone(const struct cfg *c, struct cfg_table *t, int m, int ns)
{
	static uint8_t frames[MAX_LOOP * MAX_BYTES];
	/* Coding-domain frequency: periods per cycle = m, cycle = max_cycle frames */
	const double f = (double)m * enc_sr(c) / (max_cycle * ns);
	const int k0 = max_cycle / gcd(m, max_cycle);
	char what[32];
	bool stable;
	const int k = encode_tone(c, f, k0, frames, &stable);
	/* Same rounding as the firmware (audio_tx.c level_to_delta) */
	const int max_delta = (LEVEL_MAX * 14 - 5) / 10;
	double ref;

	for (int i = 0; i < k; i++) {
		const int g = get_gidx(frames + i * c->octets, c->octets, t->gidx_bit);

		t->gidx_min = MIN(g, t->gidx_min);
	}

	/* Reference at the highest level (the gain field rewritten as by the firmware), then
	 * -20 and -40 dB relative to it
	 */
	ref = check(c, f, frames, k, max_delta, max_delta * GAIN_STEP, true, "max");
	for (int d = -28; d >= (int)(LEVEL_MIN / GAIN_STEP); d -= 28) {
		snprintf(what, sizeof(what), "gain%+d", d);
		check(c, f, frames, k, d, ref + (d - max_delta) * GAIN_STEP, false, what);
	}

	total_tones++;
	long_loops += !stable;

	return blob_add(frames, (size_t)k * c->octets);
}

static void generate_config(const struct cfg *c, struct cfg_table *t)
{
	const int ns = lc3_frame_samples(c->dt_us, enc_sr(c));
	const double res_nom = (double)c->sr / (max_cycle * ns); /* Hz per m, nominal */
	const double fmax = MIN(F_MAX, 0.45 * c->sr);

	if (ns <= 0) {
		die("%s: unsupported by liblc3", c->name);
	}

	t->m_min = (int)ceil(F_MIN / res_nom);
	t->m_max = (int)floor(fmax / res_nom);
	t->gidx_bit = gidx_bit(c, ns);
	t->gidx_min = 255;
	t->offs = calloc(t->m_max - t->m_min + 2, sizeof(uint32_t));
	t->silence = encode_silence(c);

	for (int m = t->m_min; m <= t->m_max; m++) {
		t->offs[m - t->m_min] = add_tone(c, t, m, ns);
	}

	t->offs[t->m_max - t->m_min + 1] = blob_len;

	if (t->gidx_min + (int)(LEVEL_MIN / GAIN_STEP) < 0) {
		die("%s: g_idx %d too low for %d dBFS", c->name, t->gidx_min, LEVEL_MIN);
	}

	fprintf(stderr, "%-6s m=%3d..%3d res=%6.2f Hz  g_idx bit %2d (min %3d)\n", c->name,
		t->m_min, t->m_max, res_nom, t->gidx_bit, t->gidx_min);
}

static void print_summary(void)
{
	fprintf(stderr,
		"%d tones not byte periodic (bit allocation oscillation), checked by decoding\n",
		long_loops);
	fprintf(stderr,
		"%d tones, %zu bytes; min SNR %.1f dB (%s), envelope %.2f dB, level error %.2f "
		"dB, peak at %d dBFS %.2f dBFS\n",
		total_tones, blob_len, min_snr, worst_snr_at, worst_env, worst_lvl, LEVEL_MAX,
		20 * log10(worst_peak));
}

static int max_octets(void)
{
	int max = 0;

	for (size_t i = 0; i < ARRAY_SIZE(cfgs); i++) {
		max = MAX(max, cfgs[i].octets);
	}

	return max;
}

static void write_table(FILE *out, const struct cfg_table *tables)
{
	fprintf(out, "/*\n * Copyright (c) 2026 Hubert W\xc5\x82odarczyk\n *\n"
		     " * SPDX-License-Identifier: MPL-2.0\n */\n\n");
	fprintf(out, "/* Generated by tools/lc3gen (scripts/gen_lc3_frames.sh). Do not edit. */\n");
	fprintf(out, "/* %d tones, %zu bytes, max cycle %d frames */\n\n", total_tones, blob_len,
		max_cycle);
	fprintf(out, "#include <zephyr/toolchain.h>\n\n#include \"lc3_frames.h\"\n\n");

	for (size_t ci = 0; ci < ARRAY_SIZE(cfgs); ci++) {
		const struct cfg_table *t = &tables[ci];
		const int n = t->m_max - t->m_min + 2;

		fprintf(out, "static const uint32_t offs_%zu[%d] = {", ci, n);
		for (int i = 0; i < n; i++) {
			fprintf(out, "%s%u,", i % 10 == 0 ? "\n\t" : " ", t->offs[i]);
		}
		fprintf(out, "\n};\n\n");
	}

	fprintf(out, "static const uint8_t blob[%zu] = {", blob_len);
	for (size_t i = 0; i < blob_len; i++) {
		fprintf(out, "%s0x%02x,", i % 16 == 0 ? "\n\t" : " ", blob[i]);
	}
	fprintf(out, "\n};\n\n");

	fprintf(out, "const struct lc3_tab_cfg lc3_tab[LC3_TAB_COUNT] = {\n");
	for (size_t ci = 0; ci < ARRAY_SIZE(cfgs); ci++) {
		const struct cfg *c = &cfgs[ci];
		const struct cfg_table *t = &tables[ci];
		const int ns = lc3_frame_samples(c->dt_us, enc_sr(c));

		fprintf(out,
			"\t{.name = \"%s\", .sr_hz = %d, .frame_us = %d, .octets = %d, .samples = "
			"%d, "
			".cycle = %d, .gidx_bit = %d, .gidx_min = %d, .m_min = %d, .m_max = %d, "
			".silence = &blob[%zu], .offs = offs_%zu, .blob = blob},\n",
			c->name, c->sr, c->dt_us, c->octets, ns, max_cycle, t->gidx_bit,
			t->gidx_min, t->m_min, t->m_max, t->silence, ci);
	}
	fprintf(out, "};\n\n");
	fprintf(out, "BUILD_ASSERT(%d <= LC3_TAB_MAX_OCTETS, \"raise LC3_TAB_MAX_OCTETS\");\n",
		max_octets());
	fprintf(out, "BUILD_ASSERT(%zu == LC3_TAB_COUNT, \"LC3_TAB_COUNT\");\n", ARRAY_SIZE(cfgs));
}

int main(int argc, char **argv)
{
	static struct cfg_table tables[ARRAY_SIZE(cfgs)];

	parse_args(argc, argv);
	for (size_t ci = 0; ci < ARRAY_SIZE(cfgs); ci++) {
		generate_config(&cfgs[ci], &tables[ci]);
	}

	print_summary();
	write_table(stdout, tables);

	return 0;
}
