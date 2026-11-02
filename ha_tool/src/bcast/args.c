/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Arguments of the 'broadcast' commands: broadcast number, BIG / subgroup / BIS parameters. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/shell/shell.h>

#include "audio_tx.h"
#include "cli.h"
#include "lc3_frames.h"
#include "mode.h"
#include "presets.h"
#include "bcast/internal.h"

static const struct {
	const char *name;
	uint32_t loc;
} locations[] = {
	{"mono", BT_AUDIO_LOCATION_MONO_AUDIO},
	{"left", BT_AUDIO_LOCATION_FRONT_LEFT},
	{"right", BT_AUDIO_LOCATION_FRONT_RIGHT},
	{"front_center", BT_AUDIO_LOCATION_FRONT_CENTER},
	{"back_left", BT_AUDIO_LOCATION_BACK_LEFT},
	{"back_right", BT_AUDIO_LOCATION_BACK_RIGHT},
	{"side_left", BT_AUDIO_LOCATION_SIDE_LEFT},
	{"side_right", BT_AUDIO_LOCATION_SIDE_RIGHT},
};

static const struct {
	const char *name;
	enum bt_audio_context ctx;
} contexts[] = {
	{"media", BT_AUDIO_CONTEXT_TYPE_MEDIA},
	{"live", BT_AUDIO_CONTEXT_TYPE_LIVE},
	{"conversational", BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL},
	{"instructional", BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL},
	{"sound_effects", BT_AUDIO_CONTEXT_TYPE_SOUND_EFFECTS},
	{"notifications", BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS},
	{"alerts", BT_AUDIO_CONTEXT_TYPE_ALERTS},
	{"emergency", BT_AUDIO_CONTEXT_TYPE_EMERGENCY_ALARM},
	{"unspecified", BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED},
};

const char *loc_str(uint32_t loc)
{
	for (size_t i = 0; i < ARRAY_SIZE(locations); i++) {
		if (locations[i].loc == loc) {
			return locations[i].name;
		}
	}

	return "other";
}

int parse_loc(const char *str, uint32_t *loc)
{
	for (size_t i = 0; i < ARRAY_SIZE(locations); i++) {
		if (strcmp(locations[i].name, str) == 0) {
			*loc = locations[i].loc;
			return 0;
		}
	}

	return -EINVAL;
}

static int parse_ctx(const char *str, enum bt_audio_context *ctx)
{
	for (size_t i = 0; i < ARRAY_SIZE(contexts); i++) {
		if (strcmp(contexts[i].name, str) == 0) {
			*ctx = contexts[i].ctx;
			return 0;
		}
	}

	return -EINVAL;
}

int parse_pattern(const char *str, enum tone_pattern *pattern)
{
	/* In the order of enum tone_pattern */
	static const char *const names[] = {"cont", "ring", "beep"};
	const int i = cli_parse_enum(str, names, ARRAY_SIZE(names));

	if (i < 0) {
		return i;
	}

	*pattern = (enum tone_pattern)i;

	return 0;
}

/* Prints ERR with the available range if the tone is not in the frame table of `cfg` */
int check_tone(const struct shell *sh, const struct lc3_tab_cfg *cfg, uint32_t freq_hz, long level,
	       bool exact, uint32_t *actual_mhz)
{
	uint32_t min_hz, max_hz, step_mhz;
	int m;

	if (level > LC3_TAB_LEVEL_MAX || level < LC3_TAB_LEVEL_MIN) {
		return cli_err(sh, -EINVAL, "level=%d..%d (dBFS)", LC3_TAB_LEVEL_MIN,
			       LC3_TAB_LEVEL_MAX);
	}

	m = audio_tone_resolve(cfg, freq_hz, exact, actual_mhz);
	if (m < 0) {
		audio_tone_range(cfg, &min_hz, &max_hz, &step_mhz);
		return cli_err(sh, m, "%s: freq=0 (silence) or %u..%u Hz in steps of %s Hz%s",
			       cfg->name, min_hz, max_hz, CLI_FIXED(step_mhz, 1000),
			       m == -EINVAL ? " (exact=1: no such tone)" : "");
	}

	return 0;
}

void big_changed(struct big *big)
{
	big->last_err = 0;
}

const char *big_state_str(const struct big *big)
{
	switch (big->state) {
	case BIG_NONE:
		return "none";
	case BIG_STREAMING:
		return "active";
	default:
		break;
	}

	if (big->last_err != 0) {
		return "error";
	}

	if (big->was_started) {
		return "stopped";
	}

	return big->sg_count > 0 && big->bis_count > 0 ? "ready" : "draft";
}

int bcast_target(const struct shell *sh, const char *arg, bool changes, bool must_exist,
		 struct big **big)
{
	char hint[32];
	uint8_t n;

	*big = NULL;
	if (cli_parse_u8(arg, &n) != 0 || n < 1 || n > MAX_BIGS) {
		cli_err_hint(sh, -EINVAL, "broadcast list", "broadcast number must be 1..%d",
			     MAX_BIGS);
		return -EINVAL;
	}

	if (must_exist && bcast_bigs[n - 1].state == BIG_NONE) {
		snprintf(hint, sizeof(hint), "broadcast %u create", n);
		cli_err_hint(sh, -ENOENT, hint, "BIG %u does not exist", n);
		return -ENOENT;
	}

	if (changes && mode_require(sh, MODE_BROADCAST) != 0) {
		return -EBUSY;
	}

	*big = &bcast_bigs[n - 1];

	return 0;
}

int need_inactive(const struct shell *sh, const struct big *big)
{
	char hint[32];

	if (big->state == BIG_STREAMING) {
		snprintf(hint, sizeof(hint), "broadcast %d stop", big_number(big));
		return cli_err_hint(sh, -EBUSY, hint, "cannot change BIG %d while it is active",
				    big_number(big));
	}

	return 0;
}

/* interval=7.5|10 (ms): the SDU interval of the BIG in us, or -1 if absent */
static int parse_interval(size_t argc, char **argv, int def, int *out)
{
	const char *v = cli_kv(argc, argv, "interval");

	if (v == NULL) {
		*out = def;
	} else if (strcmp(v, "7.5") == 0) {
		*out = 7500;
	} else if (strcmp(v, "10") == 0) {
		*out = 10000;
	} else {
		return -EINVAL;
	}

	return 0;
}

int parse_big_args(const struct shell *sh, struct big *big, size_t argc, char **argv)
{
	const char *name = cli_kv(argc, argv, "name");
	const char *code = cli_kv(argc, argv, "code");
	const char *pbp = cli_kv(argc, argv, "pbp");
	const char *phy = cli_kv(argc, argv, "phy");
	const char *packing = cli_kv(argc, argv, "packing");
	const char *framing = cli_kv(argc, argv, "framing");
	int err = 0;
	long id = cli_kv_long(argc, argv, "id", -1, &err);
	int interval;
	long rtn = cli_kv_long(argc, argv, "rtn", big->ovr.rtn, &err);
	long latency = cli_kv_long(argc, argv, "latency", big->ovr.latency_ms, &err);
	long pd = cli_kv_long(argc, argv, "pd", big->ovr.pd_us, &err);
	long pa = cli_kv_long(argc, argv, "pa", big->pa_ms, &err);

	if (parse_interval(argc, argv, big->ovr.interval_us, &interval) != 0 || err != 0 ||
	    id > 0xFFFFFF || rtn > 15 || latency > 4000 || pd > 0xFFFFFF || pa < 8 || pa > 10000 ||
	    (pa * 4) % 5 != 0) {
		return cli_err(sh, -EINVAL,
			       "bad number (id<=0xFFFFFF interval=7.5|10 ms rtn<=15 "
			       "pa=8..10000 ms in 1.25 ms steps)");
	}

	if ((name != NULL && (strlen(name) == 0 || strlen(name) > NAME_MAX_LEN)) ||
	    (code != NULL && (strlen(code) == 0 || strlen(code) > BT_ISO_BROADCAST_CODE_SIZE))) {
		return cli_err(sh, -EINVAL, "name 1..%d chars, code 1..16 chars", NAME_MAX_LEN);
	}

	if ((pbp != NULL && strcmp(pbp, "sq") != 0 && strcmp(pbp, "hq") != 0 &&
	     strcmp(pbp, "off") != 0) ||
	    (phy != NULL && strcmp(phy, "1m") != 0 && strcmp(phy, "2m") != 0) ||
	    (packing != NULL && strcmp(packing, "seq") != 0 && strcmp(packing, "int") != 0) ||
	    (framing != NULL && strcmp(framing, "unframed") != 0 &&
	     strcmp(framing, "framed") != 0)) {
		return cli_err(sh, -EINVAL,
			       "pbp=sq|hq|off phy=1m|2m packing=seq|int framing=unframed|framed");
	}

	if (name != NULL) {
		snprintf(big->name, sizeof(big->name), "%s", name);
	}

	if (code != NULL) {
		big->encrypted = true;
		memset(big->code, 0, sizeof(big->code));
		memcpy(big->code, code, strlen(code));
	}

	if (pbp != NULL) {
		big->pbp_enabled = strcmp(pbp, "off") != 0;
		big->pbp_hq = strcmp(pbp, "hq") == 0;
	}

	if (id >= 0) {
		big->broadcast_id = id;
	}

	if (phy != NULL) {
		big->ovr.phy = strcmp(phy, "1m") == 0 ? BT_BAP_QOS_CFG_1M : BT_BAP_QOS_CFG_2M;
	}

	if (packing != NULL) {
		big->packing = strcmp(packing, "int") == 0 ? BT_ISO_PACKING_INTERLEAVED
							   : BT_ISO_PACKING_SEQUENTIAL;
	}

	if (framing != NULL) {
		big->ovr.framing = strcmp(framing, "framed") == 0 ? BT_BAP_QOS_CFG_FRAMING_FRAMED
								  : BT_BAP_QOS_CFG_FRAMING_UNFRAMED;
	}

	big->pa_ms = pa;
	big->ovr.interval_us = interval;
	big->ovr.rtn = rtn;
	big->ovr.latency_ms = latency;
	big->ovr.pd_us = pd;

	return 0;
}

int parse_subgroup(const struct shell *sh, size_t argc, char **argv, struct subgroup *sg)
{
	const char *preset = cli_kv(argc, argv, "preset");
	const char *lang = cli_kv(argc, argv, "lang");
	const char *ctx = cli_kv(argc, argv, "ctx");
	const char *program = cli_kv(argc, argv, "program");

	if (preset != NULL) {
		const struct bt_bap_lc3_preset *p =
			preset_find(PRESET_BROADCAST, preset, &sg->preset_name);

		if (p == NULL) {
			return cli_err(sh, -EINVAL, "unknown preset (%s)", preset_names());
		}

		sg->preset = *p;
	}

	if (ctx != NULL && parse_ctx(ctx, &sg->ctx) != 0) {
		return cli_err(sh, -EINVAL,
			       "ctx: media live conversational instructional sound_effects "
			       "notifications alerts emergency unspecified");
	}

	if ((lang != NULL && strlen(lang) != BT_AUDIO_LANG_SIZE) ||
	    (program != NULL && strlen(program) > PROG_MAX_LEN)) {
		return cli_err(sh, -EINVAL, "lang=<3 letters, ISO 639-3> program=<max %d chars>",
			       PROG_MAX_LEN);
	}

	if (lang != NULL) {
		snprintf(sg->lang, sizeof(sg->lang), "%s", lang);
	}

	if (program != NULL) {
		snprintf(sg->program, sizeof(sg->program), "%s", program);
	}

	if (build_metadata(sg) != 0) {
		return cli_err(sh, -ENOMEM, "metadata too long");
	}

	return 0;
}

int add_subgroup(const struct shell *sh, struct big *big, size_t argc, char **argv)
{
	struct subgroup *sg;

	if (big->sg_count >= MAX_SG) {
		return cli_err(sh, -ENOMEM, "max %d subgroups", MAX_SG);
	}

	sg = &big->sg[big->sg_count];
	memset(sg, 0, sizeof(*sg));
	sg->preset = *preset_find(PRESET_BROADCAST, "16_2", &sg->preset_name);
	sg->ctx = BT_AUDIO_CONTEXT_TYPE_MEDIA;
	if (parse_subgroup(sh, argc, argv, sg) != 0) {
		return -EINVAL;
	}

	return ++big->sg_count;
}

int parse_bis(const struct shell *sh, const struct big *big, size_t argc, char **argv,
	      struct bis *bis)
{
	const char *loc = cli_kv(argc, argv, "location");
	const char *pattern = cli_kv(argc, argv, "pattern");
	const char *rate_str = cli_kv(argc, argv, "rate");
	int err = 0;
	long sg = cli_kv_long(argc, argv, "subgroup", bis->sg + 1, &err);
	const bool exact = cli_kv_long(argc, argv, "exact", 0, &err) != 0;
	long rate = bis->rate_hz;
	long octets = cli_kv_long(argc, argv, "octets", bis->octets, &err);
	long freq = cli_kv_long(argc, argv, "freq", bis->tone.freq_hz, &err);
	long level = cli_kv_long(argc, argv, "level", bis->tone.level_db, &err);

	if (err != 0 || sg < 1 || sg > big->sg_count) {
		char hint[40];

		snprintf(hint, sizeof(hint), "broadcast %d subgroup %s", big_number(big),
			 big->sg_count > 0 ? "list" : "add preset=16_2");
		return cli_err_hint(sh, -EINVAL, hint, "BIG %d has no subgroup %ld (subgroups: %u)",
				    big_number(big), sg, big->sg_count);
	}

	if (rate_str != NULL) {
		rate = strcmp(rate_str, "44.1") == 0 ? 44100 : strtol(rate_str, NULL, 10) * 1000;
	}

	if ((rate_str != NULL && rate != 8000 && rate != 16000 && rate != 24000 && rate != 32000 &&
	     rate != 44100 && rate != 48000) ||
	    (octets != 0 && (octets < 20 || octets > CONFIG_BT_ISO_TX_MTU)) || freq < 0) {
		return cli_err(sh, -EINVAL, "rate=8|16|24|32|44.1|48 (kHz) octets=20..%d freq=Hz",
			       CONFIG_BT_ISO_TX_MTU);
	}

	bis->sg = sg - 1;
	bis->rate_hz = rate;
	bis->octets = octets;
	bis->tone.freq_hz = freq;
	bis->tone.level_db = level;

	if (bis_cfg(big, bis) == NULL) {
		return cli_err(sh, -ENOTSUP,
			       "unsupported combination: no frames for rate=%u frame_us=%u "
			       "octets=%u (%s)",
			       bis_rate_hz(big, bis), sg_frame_us(&big->sg[bis->sg]),
			       bis_octets(big, bis), audio_cfg_names());
	}

	err = check_tone(sh, bis_cfg(big, bis), freq, level, exact, &bis->freq_mhz);
	if (err != 0) {
		return err;
	}

	if (loc != NULL && parse_loc(loc, &bis->loc) != 0) {
		return cli_err(sh, -EINVAL,
			       "location: mono left right front_center back_left "
			       "back_right side_left side_right");
	}

	if (pattern != NULL && parse_pattern(pattern, &bis->tone.pattern) != 0) {
		return cli_err(sh, -EINVAL, "pattern=cont|ring|beep");
	}

	return 0;
}

int add_bis(const struct shell *sh, struct big *big, size_t argc, char **argv, uint32_t default_loc)
{
	struct bis *bis;

	if (big->bis_count >= MAX_BIS) {
		return cli_err(sh, -ENOMEM, "max %d BISes per broadcast", MAX_BIS);
	}

	bis = &big->bis[big->bis_count];
	bis->sg = big->sg_count > 0 ? big->sg_count - 1 : 0;
	bis->loc = default_loc;
	bis->rate_hz = 0;
	bis->octets = 0;
	bis->tone = default_tone;
	if (big->sg_count == 0) {
		char hint[40];

		snprintf(hint, sizeof(hint), "broadcast %d subgroup add preset=16_2",
			 big_number(big));
		return cli_err_hint(sh, -EINVAL, hint, "cannot add BIS to BIG %d: no subgroup",
				    big_number(big));
	}

	if (parse_bis(sh, big, argc, argv, bis) != 0) {
		return -EINVAL;
	}

	return ++big->bis_count;
}
