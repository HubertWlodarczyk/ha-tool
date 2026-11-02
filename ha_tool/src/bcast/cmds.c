/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * 'broadcast <n>' configuration commands: create, set, delete, subgroups and BISes. The BIG
 * number is always given (argv[1]); structure changes need an inactive BIG, tones and the
 * metadata of a single subgroup change live.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/shell/shell.h>

#include "audio_tx.h"
#include "cli.h"
#include "cli_cmds.h"
#include "lc3_frames.h"
#include "mode.h"
#include "presets.h"
#include "bcast/internal.h"

/* An existing BIG that may change (mode entered, not active) */
static int changeable(const struct shell *sh, const char *arg, struct big **big)
{
	if (bcast_target(sh, arg, true, true, big) != 0) {
		return -EINVAL;
	}

	return need_inactive(sh, *big);
}

/* Subgroup / BIS number argument within 1..count; ERR with the list command otherwise */
static int item_number(const struct shell *sh, const struct big *big, const char *arg,
		       const char *what, int count, int *out)
{
	char hint[40];
	uint8_t n;

	*out = 0;
	if (cli_parse_u8(arg, &n) != 0 || n < 1 || n > count) {
		snprintf(hint, sizeof(hint), "broadcast %d %s list", big_number(big), what);
		cli_err_hint(sh, -ENOENT, hint, "BIG %d has no %s %s (%d configured)",
			     big_number(big), what, arg, count);
		return -ENOENT;
	}

	*out = n;

	return 0;
}

/* Shortcut of 'create': one subgroup with N BISes (mono / L,R / L,R,C / L,R,BL,BR ...) */
static int add_quick_bises(const struct shell *sh, struct big *big, long n)
{
	static const uint32_t layout[] = {
		BT_AUDIO_LOCATION_FRONT_LEFT,   BT_AUDIO_LOCATION_FRONT_RIGHT,
		BT_AUDIO_LOCATION_FRONT_CENTER, BT_AUDIO_LOCATION_BACK_LEFT,
		BT_AUDIO_LOCATION_BACK_RIGHT,   BT_AUDIO_LOCATION_SIDE_LEFT,
		BT_AUDIO_LOCATION_SIDE_RIGHT,   BT_AUDIO_LOCATION_FRONT_LEFT_OF_CENTER,
	};
	static const uint32_t tones[] = {500, 1000, 1500, 2000, 2500, 3000, 3500, 4000};
	char *bis_argv[2] = {"add", NULL};

	for (int i = 0; i < n; i++) {
		uint32_t min_hz, max_hz, step_mhz;
		struct bis *b = &big->bis[i];

		if (add_bis(sh, big, 1, bis_argv,
			    n == 1 ? BT_AUDIO_LOCATION_MONO_AUDIO : layout[i]) < 0) {
			return -EINVAL;
		}

		/* Tones of a low sampling rate (8 kHz: up to 3.6 kHz) are capped */
		audio_tone_range(bis_cfg(big, b), &min_hz, &max_hz, &step_mhz);
		b->tone.freq_hz = MIN(tones[i], max_hz);
		(void)audio_tone_resolve(bis_cfg(big, b), b->tone.freq_hz, false, &b->freq_mhz);
	}

	return 0;
}

/* broadcast <n> create [BIG keys] [preset= bis= lang= ctx= program=]: a new draft; with preset=
 * or bis= also one subgroup and that many BISes
 */
int cmd_broadcast_create(const struct shell *sh, size_t argc, char **argv)
{
	const bool quick =
		cli_kv(argc, argv, "preset") != NULL || cli_kv(argc, argv, "bis") != NULL;
	struct big *big;
	char hint[32];
	int err = 0;
	long n;

	if (bcast_target(sh, argv[1], true, false, &big) != 0) {
		return -EINVAL;
	}

	if (big->state != BIG_NONE) {
		snprintf(hint, sizeof(hint), "broadcast %d delete", big_number(big));
		return cli_err_hint(sh, -EEXIST, hint, "BIG %d exists", big_number(big));
	}

	n = cli_kv_long(argc, argv, "bis", 1, &err);
	if (err != 0 || n < 1 || n > MAX_BIS) {
		return cli_err(sh, -EINVAL, "bis must be 1..%d", MAX_BIS);
	}

	big_init_draft(big, big_number(big));
	if (parse_big_args(sh, big, argc, argv) != 0 ||
	    (quick &&
	     (add_subgroup(sh, big, argc, argv) < 0 || add_quick_bises(sh, big, n) != 0))) {
		big->state = BIG_NONE;
		return -EINVAL;
	}

	cli_ok(sh,
	       "big=%d state=%s broadcast_id=0x%06X name=%s subgroups=%u bis=%u encrypted=%d "
	       "pbp=%s",
	       big_number(big), big_state_str(big), big->broadcast_id, CLI_Q(big->name),
	       big->sg_count, big->bis_count, big->encrypted,
	       !big->pbp_enabled ? "off" : (big->pbp_hq ? "hq" : "sq"));

	return 0;
}

int cmd_broadcast_set(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;

	if (changeable(sh, argv[1], &big) != 0 || parse_big_args(sh, big, argc, argv) != 0) {
		return -EINVAL;
	}

	big_changed(big);
	cli_ok(sh, "big=%d state=%s", big_number(big), big_state_str(big));

	return 0;
}

int cmd_broadcast_delete(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;

	ARG_UNUSED(argc);

	if (bcast_target(sh, argv[1], true, true, &big) != 0) {
		return -EINVAL;
	}

	big_stop(big);
	big->state = BIG_NONE;
	cli_ok(sh, "big=%d", big_number(big));

	return 0;
}

/* --- Subgroups --- */

int cmd_broadcast_subgroup_add(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	int sg;

	if (changeable(sh, argv[1], &big) != 0) {
		return -EINVAL;
	}

	sg = add_subgroup(sh, big, argc, argv);
	if (sg < 0) {
		return sg;
	}

	big_changed(big);
	cli_ok(sh, "big=%d sg=%d preset=%s", big_number(big), sg, big->sg[sg - 1].preset_name);

	return 0;
}

/* Applies new metadata of a subgroup, live if the BIG streams (Zephyr updates all subgroups at
 * once, so live only with a single subgroup)
 */
static int apply_metadata(const struct shell *sh, struct big *big, struct subgroup *sg)
{
	const struct bt_audio_codec_cfg *cfg;
	int err;

	if (build_metadata(sg) != 0) {
		return cli_err(sh, -ENOMEM, "metadata too long");
	}

	if (big->state != BIG_STREAMING) {
		return 0;
	}

	cfg = &big->sg[0].preset.codec_cfg;
	err = bt_cap_initiator_broadcast_audio_update(big->source, cfg->meta, cfg->meta_len);
	err = err ?: update_adv_data(big);

	return err != 0 ? cli_err(sh, err, "metadata update failed") : 0;
}

/* broadcast <n> subgroup set <sg> [preset= lang= ctx= program=]: the preset needs an inactive
 * BIG; lang / ctx / program change live (metadata) on a BIG of one subgroup
 */
int cmd_broadcast_subgroup_set(const struct shell *sh, size_t argc, char **argv)
{
	const char *preset = cli_kv(argc, argv, "preset");
	struct subgroup before;
	struct big *big;
	char hint[32];
	int s;

	if (bcast_target(sh, argv[1], true, true, &big) != 0 ||
	    item_number(sh, big, argv[2], "subgroup", big->sg_count, &s) != 0) {
		return -EINVAL;
	}

	if (big->state == BIG_STREAMING && (preset != NULL || big->sg_count > 1)) {
		snprintf(hint, sizeof(hint), "broadcast %d stop", big_number(big));
		return cli_err_hint(sh, -EBUSY, hint,
				    "BIG %d is active: only lang, ctx and program of a single "
				    "subgroup change live",
				    big_number(big));
	}

	before = big->sg[s - 1];
	if (parse_subgroup(sh, argc, argv, &big->sg[s - 1]) != 0) {
		big->sg[s - 1] = before;
		return -EINVAL;
	}

	/* Every BIS of the subgroup must still have frames in the new configuration */
	for (int i = 0; i < big->bis_count; i++) {
		if (big->bis[i].sg == s - 1 && bis_cfg(big, &big->bis[i]) == NULL) {
			big->sg[s - 1] = before;
			return cli_err(sh, -ENOTSUP, "BIS %d has no frames with preset %s", i + 1,
				       preset);
		}
	}

	if (apply_metadata(sh, big, &big->sg[s - 1]) != 0) {
		big->sg[s - 1] = before;
		return -EIO;
	}

	big_changed(big);
	{
		const struct subgroup *sg = &big->sg[s - 1];

		cli_ok(sh, "big=%d sg=%d preset=%s lang=%s ctx=%s program=%s", big_number(big), s,
		       sg->preset_name, sg->lang[0] != '\0' ? sg->lang : "none",
		       audio_ctx_str(sg->ctx), CLI_Q(sg->program));
	}

	return 0;
}

/* broadcast <n> subgroup delete <sg>: only a subgroup without BISes; later ones move down */
int cmd_broadcast_subgroup_delete(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	char hint[40];
	int s;

	ARG_UNUSED(argc);

	if (changeable(sh, argv[1], &big) != 0 ||
	    item_number(sh, big, argv[2], "subgroup", big->sg_count, &s) != 0) {
		return -EINVAL;
	}

	for (int i = 0; i < big->bis_count; i++) {
		if (big->bis[i].sg == s - 1) {
			snprintf(hint, sizeof(hint), "broadcast %d bis delete %d", big_number(big),
				 i + 1);
			return cli_err_hint(sh, -EBUSY, hint, "subgroup %d still has BIS %d", s,
					    i + 1);
		}
	}

	memmove(&big->sg[s - 1], &big->sg[s], (big->sg_count - s) * sizeof(big->sg[0]));
	big->sg_count--;
	for (int i = 0; i < big->bis_count; i++) {
		if (big->bis[i].sg > s - 1) {
			big->bis[i].sg--;
		}
	}

	big_changed(big);
	cli_ok(sh, "big=%d subgroups=%u", big_number(big), big->sg_count);

	return 0;
}

static void subgroup_line(const struct big *big, int s, const struct shell *sh)
{
	const struct subgroup *sg = &big->sg[s];
	int bises = 0;

	for (int i = 0; i < big->bis_count; i++) {
		bises += big->bis[i].sg == s;
	}

	cli_line(sh,
		 "SUBGROUP sg=%d preset=%s rate=%u frame_us=%u octets=%u lang=%s ctx=%s program=%s "
		 "bis=%d",
		 s + 1, sg->preset_name, sg_rate_hz(sg), sg_frame_us(sg), sg_octets(sg),
		 sg->lang[0] != '\0' ? sg->lang : "none", audio_ctx_str(sg->ctx),
		 CLI_Q(sg->program), bises);
}

int cmd_broadcast_subgroup_list(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;

	ARG_UNUSED(argc);

	if (bcast_target(sh, argv[1], false, true, &big) != 0) {
		return -EINVAL;
	}

	for (int s = 0; s < big->sg_count; s++) {
		subgroup_line(big, s, sh);
	}

	cli_ok(sh, "big=%d count=%u", big_number(big), big->sg_count);

	return 0;
}

/* --- BISes --- */

static void bis_reply(const struct shell *sh, const struct big *big, int bis)
{
	const struct bis *b = &big->bis[bis - 1];

	cli_ok(sh,
	       "big=%d bis=%d sg=%u location=%s cfg=%s freq=%u freq_actual=%s level=%d "
	       "level_actual=%s",
	       big_number(big), bis, b->sg + 1, loc_str(b->loc), bis_cfg(big, b)->name,
	       b->tone.freq_hz, CLI_FIXED(b->freq_mhz, 1000), b->tone.level_db,
	       CLI_FIXED(audio_level_actual_cdb(b->tone.level_db), 100));
}

int cmd_broadcast_bis_add(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	int bis;

	if (changeable(sh, argv[1], &big) != 0) {
		return -EINVAL;
	}

	bis = add_bis(sh, big, argc, argv, BT_AUDIO_LOCATION_MONO_AUDIO);
	if (bis < 0) {
		return bis;
	}

	big_changed(big);
	bis_reply(sh, big, bis);

	return 0;
}

/* Tone keys of a BIS (freq= level= pattern= exact=), checked against its frame table */
static int apply_tone(const struct shell *sh, struct big *big, struct bis *b, size_t argc,
		      char **argv)
{
	const char *pattern = cli_kv(argc, argv, "pattern");
	enum tone_pattern pat = b->tone.pattern;
	int err = 0;
	long freq = cli_kv_long(argc, argv, "freq", b->tone.freq_hz, &err);
	long level = cli_kv_long(argc, argv, "level", b->tone.level_db, &err);
	const bool exact = cli_kv_long(argc, argv, "exact", 0, &err) != 0;
	uint32_t actual;

	if (err != 0 || freq < 0 || (pattern != NULL && parse_pattern(pattern, &pat) != 0)) {
		return cli_err(sh, -EINVAL, "freq=Hz level=dBFS pattern=cont|ring|beep exact=0|1");
	}

	err = check_tone(sh, bis_cfg(big, b), freq, level, exact, &actual);
	if (err != 0) {
		return err;
	}

	b->tone.freq_hz = freq;
	b->tone.level_db = level;
	b->tone.pattern = pat;
	b->freq_mhz = actual;
	audio_tx_set_tone(&b->stream, &b->tone);

	return 0;
}

/* broadcast <n> bis tone <bis> [freq= level= pattern= exact=]: the tone, also live */
int cmd_broadcast_bis_tone(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	int bis;

	if (bcast_target(sh, argv[1], true, true, &big) != 0 ||
	    item_number(sh, big, argv[2], "bis", big->bis_count, &bis) != 0 ||
	    apply_tone(sh, big, &big->bis[bis - 1], argc, argv) != 0) {
		return -EINVAL;
	}

	bis_reply(sh, big, bis);

	return 0;
}

/* broadcast <n> bis set <bis> [subgroup= location= rate= octets= freq= level= pattern= exact=]:
 * the codec and the channel need an inactive BIG, the tone changes live
 */
int cmd_broadcast_bis_set(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const structural[] = {"subgroup", "location", "rate", "octets"};
	struct big *big;
	struct bis *b;
	struct bis_cfg_part {
		uint8_t raw[offsetof(struct bis, data)];
	} before;
	int bis;

	if (bcast_target(sh, argv[1], true, true, &big) != 0 ||
	    item_number(sh, big, argv[2], "bis", big->bis_count, &bis) != 0) {
		return -EINVAL;
	}

	for (size_t k = 0; k < ARRAY_SIZE(structural); k++) {
		if (cli_kv(argc, argv, structural[k]) != NULL && need_inactive(sh, big) != 0) {
			return -EBUSY;
		}
	}

	/* The configuration is the part of struct bis before its buffers and stream */
	b = &big->bis[bis - 1];
	memcpy(&before, b, sizeof(before));
	if (parse_bis(sh, big, argc, argv, b) != 0) {
		memcpy(b, &before, sizeof(before));
		return -EINVAL;
	}

	audio_tx_set_tone(&b->stream, &b->tone);
	big_changed(big);
	bis_reply(sh, big, bis);

	return 0;
}

/* broadcast <n> bis delete <bis>: later BISes move down (BIS indexes stay contiguous) */
int cmd_broadcast_bis_delete(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	int bis;

	ARG_UNUSED(argc);

	if (changeable(sh, argv[1], &big) != 0 ||
	    item_number(sh, big, argv[2], "bis", big->bis_count, &bis) != 0) {
		return -EINVAL;
	}

	/* configuration only: the stream objects stay with their slots */
	for (int i = bis; i < big->bis_count; i++) {
		memcpy(&big->bis[i - 1], &big->bis[i], offsetof(struct bis, data));
	}

	big->bis_count--;
	big_changed(big);
	cli_ok(sh, "big=%d bis=%u", big_number(big), big->bis_count);

	return 0;
}

void bis_line(const struct shell *sh, struct big *big, int i)
{
	struct bis *b = &big->bis[i];
	const struct lc3_tab_cfg *cfg = bis_cfg(big, b);

	cli_line(sh,
		 "BIS bis=%d sg=%u location=%s cfg=%s rate=%u octets=%u tone=%u freq_actual=%s "
		 "level=%d level_actual=%s sent=%u",
		 i + 1, b->sg + 1, loc_str(b->loc), cfg != NULL ? cfg->name : "none",
		 bis_rate_hz(big, b), bis_octets(big, b), b->tone.freq_hz,
		 CLI_FIXED(b->freq_mhz, 1000), b->tone.level_db,
		 CLI_FIXED(audio_level_actual_cdb(b->tone.level_db), 100),
		 audio_tx_sent_count(&b->stream));
}

void subgroup_lines(const struct shell *sh, const struct big *big)
{
	for (int s = 0; s < big->sg_count; s++) {
		subgroup_line(big, s, sh);
	}
}

int cmd_broadcast_bis_list(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;

	ARG_UNUSED(argc);

	if (bcast_target(sh, argv[1], false, true, &big) != 0) {
		return -EINVAL;
	}

	for (int i = 0; i < big->bis_count; i++) {
		bis_line(sh, big, i);
	}

	cli_ok(sh, "big=%d count=%u", big_number(big), big->bis_count);

	return 0;
}
