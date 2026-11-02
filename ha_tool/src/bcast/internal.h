/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Broadcast builder internals shared by bcast/model.c, budget.c, source.c and cmds.c.
 *
 * A tester composes each broadcast (BIG) freely:
 *   BIG       - name, Broadcast ID, encryption, PBP, PHY, ISO interval, RTN, latency,
 *               presentation delay, framing, packing (shared by all its BISes)
 *   subgroup  - LC3 preset (codec base configuration) and metadata: language, context, program
 *   BIS       - codec overrides (sampling rate, octets per frame, channel allocation) and its tone
 * Up to CONFIG_HA_BCAST_MAX_BIGS broadcasts can run at the same time; streams that must differ in
 * a BIG-level parameter go into separate BIGs.
 *
 * A broadcast is a draft until 'bcast start': the Zephyr broadcast source is created from the
 * draft on start and deleted on stop, so the draft can be edited and restarted.
 */

#ifndef HA_TOOL_BCAST_INTERNAL_H_
#define HA_TOOL_BCAST_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/shell/shell.h>

#include "audio_tx.h"

#define MAX_BIGS     CONFIG_HA_BCAST_MAX_BIGS
#define MAX_SG       CONFIG_HA_BCAST_MAX_SUBGROUPS
#define MAX_BIS      CONFIG_HA_BCAST_MAX_BIS
#define NAME_MAX_LEN 32
#define PROG_MAX_LEN 16
#define BIS_DATA_LEN 13 /* frequency (3) + frame length (4) + channel allocation (6) */

/* Every BIS of every BIG plus the two unicast CIS (phone) need an ISO channel */
BUILD_ASSERT(CONFIG_HA_BCAST_MAX_BIS <= CONFIG_HA_BCAST_MAX_BIS_TOTAL,
	     "a BIG cannot have more BISes than all broadcasts together");
BUILD_ASSERT(CONFIG_HA_BCAST_MAX_BIGS <= CONFIG_HA_BCAST_MAX_BIS_TOTAL,
	     "every BIG needs at least one BIS");
BUILD_ASSERT(CONFIG_HA_BCAST_MAX_BIS_TOTAL + 2 <= CONFIG_BT_ISO_MAX_CHAN,
	     "raise CONFIG_BT_ISO_MAX_CHAN");

enum big_state {
	BIG_NONE,
	BIG_DRAFT,
	BIG_STREAMING,
};

struct subgroup {
	const char *preset_name;
	struct bt_bap_lc3_preset preset;
	enum bt_audio_context ctx;
	char lang[BT_AUDIO_LANG_SIZE + 1];
	char program[PROG_MAX_LEN + 1];
};

struct bis {
	uint8_t sg;       /* subgroup index */
	uint32_t loc;     /* channel allocation */
	uint32_t rate_hz; /* 0: subgroup's */
	uint16_t octets;  /* 0: subgroup's */
	struct tone tone;
	uint32_t freq_mhz; /* tone actually sent (nearest of the frame table) */
	uint8_t data[BIS_DATA_LEN];
	struct bt_cap_stream stream;
};

/* Optional BIG-level overrides of the first subgroup's preset QoS (-1: keep) */
struct big_qos_override {
	int phy;
	int interval_us;
	int rtn;
	int latency_ms;
	int pd_us;
	int framing;
};

struct big {
	enum big_state state;
	char name[NAME_MAX_LEN + 1];
	uint32_t broadcast_id;
	bool encrypted;
	uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
	bool pbp_enabled;
	bool pbp_hq;
	uint8_t packing;
	uint16_t pa_ms; /* periodic advertising interval (carries BASE and BIGInfo) */
	int8_t txpower; /* dBm of advertising, PA and BIG; TXPOWER_DEFAULT: controller default */
	struct big_qos_override ovr;
	struct subgroup sg[MAX_SG];
	uint8_t sg_count;
	struct bis bis[MAX_BIS];
	uint8_t bis_count;

	/* Reported state (big_state_str): started at least once, outcome of the last start */
	bool was_started;
	int last_err;

	/* While streaming */
	struct bt_bap_qos_cfg qos;
	struct bt_cap_broadcast_source *source;
	struct bt_le_ext_adv *adv;
	uint8_t started;
};

#define TXPOWER_DEFAULT INT8_MAX

/* Budget all broadcasts share (budget.c) */
#define BUDGET_PERIOD_US     10000U
#define BIG_OVERHEAD_US      1000U
#define ADV_RESERVE_ONE_US   2500U /* one BIG streaming */
#define ADV_RESERVE_MULTI_US 3800U /* two or more */

extern struct big bcast_bigs[MAX_BIGS];
extern const struct tone default_tone;
extern struct bt_bap_stream_ops bcast_stream_ops;

/* --- model.c: drafts and effective configuration --- */

struct big *big_of_stream(const struct bt_bap_stream *bap_stream, int *bis_index);

int big_number(const struct big *big);

uint32_t sg_octets(const struct subgroup *sg);

uint32_t bis_octets(const struct big *big, const struct bis *bis);

uint32_t sg_frame_us(const struct subgroup *sg);

uint32_t sg_rate_hz(const struct subgroup *sg);

uint32_t bis_rate_hz(const struct big *big, const struct bis *bis);

const struct lc3_tab_cfg *bis_cfg(const struct big *big, const struct bis *bis);

uint32_t bis_sdu_interval_us(const struct big *big, const struct bis *bis);

void big_effective_qos(const struct big *big, struct bt_bap_qos_cfg *qos);

void big_init_draft(struct big *big, int number);

/* --- budget.c: air time --- */

/* Budget left for one more BIG next to the streaming ones */
uint32_t budget_free_us(void);

/* Budget of all BIGs once `big` (NULL: one more BIG) streams next to the others */
uint32_t budget_limit_us(const struct big *big);

/* BISes of a preset (with this QoS) that one more BIG could hold: free budget, BIGs and BISes */
int budget_fit_bis(const struct bt_bap_qos_cfg *qos, const struct bt_bap_lc3_preset *preset,
		   bool encrypted);

uint32_t big_airtime_us(const struct big *big, const struct bt_bap_qos_cfg *qos);

uint32_t big_cost_us(uint32_t air_us, uint32_t interval_us);

uint32_t budget_used_us(const struct big *except);

void warn_interval_mix(const struct big *big);

/* --- source.c: Zephyr broadcast source --- */

int build_metadata(struct subgroup *sg);

int update_adv_data(struct big *big);

int apply_txpower(struct big *big, int8_t *selected);

int big_start(struct big *big);

void big_stop(struct big *big);

/* --- args.c: arguments of the commands --- */

/* The broadcast a command names (argv[1], "broadcast 2 start"); the mode first if it changes
 * radio state. ERR printed (with the next step) if invalid, or if it does not exist and
 * must_exist.
 */
int bcast_target(const struct shell *sh, const char *arg, bool changes, bool must_exist,
		 struct big **big);

/* ERR (hint: broadcast <n> stop) if the broadcast is active */
int need_inactive(const struct shell *sh, const struct big *big);

/* The configuration changed: a failed start is no longer the state to report */
void big_changed(struct big *big);

/* draft | ready | active | stopped | error (docs/broadcast-ux.md) */
const char *big_state_str(const struct big *big);

/* BIG level key=value parameters (name, code, pbp, id, phy, interval, rtn, ...) */
int parse_big_args(const struct shell *sh, struct big *big, size_t argc, char **argv);

/* Applies the key=value parameters given over the values of a subgroup / BIS (ERR printed) */
int parse_subgroup(const struct shell *sh, size_t argc, char **argv, struct subgroup *sg);
int parse_bis(const struct shell *sh, const struct big *big, size_t argc, char **argv,
	      struct bis *bis);

/* Adds a subgroup / BIS from key=value parameters: its number (1..), or a negative error */
int add_subgroup(const struct shell *sh, struct big *big, size_t argc, char **argv);
int add_bis(const struct shell *sh, struct big *big, size_t argc, char **argv,
	    uint32_t default_loc);

/* ERR with the available range if the tone or level is not in the frame table of cfg */
int check_tone(const struct shell *sh, const struct lc3_tab_cfg *cfg, uint32_t freq_hz, long level,
	       bool exact, uint32_t *actual_mhz);

/* cont|ring|beep */
int parse_pattern(const char *str, enum tone_pattern *pattern);

/* Channel allocation name: mono, left, right, front_center, ... */
const char *loc_str(uint32_t loc);
int parse_loc(const char *str, uint32_t *loc);

/* --- cmds.c: data lines shared with run_cmds.c --- */
void subgroup_lines(const struct shell *sh, const struct big *big);
void bis_line(const struct shell *sh, struct big *big, int i);

/* --- validate.c --- */

/* All checks before 'bcast start' (ERR printed); force: start next to BIGs of another interval */
int bcast_validate(const struct shell *sh, const struct big *big, bool force);

#endif /* HA_TOOL_BCAST_INTERNAL_H_ */
