/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Phone scenario state machine. A single worker thread owns all phone state: shell requests and
 * peer events arrive through a message queue, an event allowed in the current state runs its
 * action, every state change is reported as
 *   EVT phone_state from=<state> to=<state> cause=<event>
 * Other threads only read a status snapshot.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "dev.h"
#include "phone.h"
#include "presets.h"

LOG_MODULE_REGISTER(phone, LOG_LEVEL_INF);

/* Level of notification beeps (dBFS) */
#define ALERT_LEVEL_DB (-6)

enum state {
	ST_IDLE,
	ST_MUSIC,
	ST_RINGING, /* incoming call, ringtone */
	ST_DIALING, /* outgoing call, waiting for the far end */
	ST_IN_CALL,
	ST_HELD,
	ST_ALERT, /* notification sound, returns to IDLE or MUSIC */
	ST_COUNT,
};

static const char *const state_names[] = {
	"idle", "music", "ringing", "dialing", "in_call", "held", "alert",
};
BUILD_ASSERT(ARRAY_SIZE(state_names) == ST_COUNT, "one entry per value");

static const char *const ev_names[] = {
	[PH_REQ_MUSIC_START] = "music_start",
	[PH_REQ_MUSIC_STOP] = "music_stop",
	[PH_REQ_CALL_INCOMING] = "call_incoming",
	[PH_REQ_CALL_OUTGOING] = "call_outgoing",
	[PH_REQ_CALL_ACCEPT] = "call_accept",
	[PH_REQ_CALL_REMOTE_ANSWER] = "call_remote_answer",
	[PH_REQ_CALL_END] = "call_end",
	[PH_REQ_CALL_HOLD] = "call_hold",
	[PH_REQ_CALL_RETRIEVE] = "call_retrieve",
	[PH_REQ_ALERT] = "alert",
	[PH_REQ_VOLUME] = "volume",
	[PH_REQ_MUTE] = "mute",
	[PH_REQ_STOP] = "stop",
	[PH_REQ_PRESET] = "preset",
	[PH_REQ_MIC_MUTE] = "mic_mute",
	[PH_REQ_MIC_GAIN] = "mic_gain",
	[PH_REQ_BALANCE] = "balance",
	[PH_EV_PEER_ACCEPTED] = "peer_accepted",
	[PH_EV_PEER_TERMINATED] = "peer_terminated",
	[PH_EV_PEER_ORIGINATED] = "peer_originated",
	[PH_EV_PEER_HELD] = "peer_held",
	[PH_EV_PEER_RETRIEVED] = "peer_retrieved",
	[PH_EV_PEER_PLAY] = "peer_play",
	[PH_EV_PEER_PAUSE] = "peer_pause",
	[PH_EV_ALERT_TIMEOUT] = "alert_timeout",
	[PH_EV_LINK_LOST] = "link_lost",
};
BUILD_ASSERT(ARRAY_SIZE(ev_names) == PH_EV_COUNT, "one entry per value");

#define ANY       (BIT(ST_COUNT) - 1)
#define IN_A_CALL (BIT(ST_RINGING) | BIT(ST_DIALING) | BIT(ST_IN_CALL) | BIT(ST_HELD))

/* States in which each request / event is accepted */
static const uint32_t allowed[] = {
	[PH_REQ_MUSIC_START] = ANY,
	[PH_REQ_MUSIC_STOP] = ANY,
	[PH_REQ_CALL_INCOMING] = BIT(ST_IDLE) | BIT(ST_MUSIC),
	[PH_REQ_CALL_OUTGOING] = BIT(ST_IDLE) | BIT(ST_MUSIC),
	[PH_REQ_CALL_ACCEPT] = BIT(ST_RINGING),
	[PH_REQ_CALL_REMOTE_ANSWER] = BIT(ST_DIALING),
	[PH_REQ_CALL_END] = IN_A_CALL,
	[PH_REQ_CALL_HOLD] = BIT(ST_IN_CALL),
	[PH_REQ_CALL_RETRIEVE] = BIT(ST_HELD),
	[PH_REQ_ALERT] = BIT(ST_IDLE) | BIT(ST_MUSIC),
	[PH_REQ_VOLUME] = ANY,
	[PH_REQ_MUTE] = ANY,
	[PH_REQ_STOP] = ANY,
	[PH_REQ_PRESET] = ANY,
	[PH_REQ_MIC_MUTE] = ANY,
	[PH_REQ_MIC_GAIN] = ANY,
	[PH_REQ_BALANCE] = ANY,
	[PH_EV_PEER_ACCEPTED] = BIT(ST_RINGING),
	[PH_EV_PEER_TERMINATED] = IN_A_CALL,
	[PH_EV_PEER_ORIGINATED] = BIT(ST_IDLE) | BIT(ST_MUSIC),
	[PH_EV_PEER_HELD] = BIT(ST_IN_CALL),
	[PH_EV_PEER_RETRIEVED] = BIT(ST_HELD),
	[PH_EV_PEER_PLAY] = ANY,
	[PH_EV_PEER_PAUSE] = ANY,
	[PH_EV_ALERT_TIMEOUT] = BIT(ST_ALERT),
	[PH_EV_LINK_LOST] = ANY,
};
BUILD_ASSERT(ARRAY_SIZE(allowed) == PH_EV_COUNT, "one entry per value");

static struct {
	enum state state;
	bool music_wanted; /* resume music after calls / alerts */
	struct unicast_cfg music;
	struct unicast_cfg ring;
	struct unicast_cfg call;
	int call_index;
} ph = {
	.music = {.preset = "48_2",
		  .ctx = BT_AUDIO_CONTEXT_TYPE_MEDIA,
		  .tone = {.freq_hz = 440, .level_db = -12, .pattern = TONE_CONTINUOUS}},
	.ring = {.ctx = BT_AUDIO_CONTEXT_TYPE_RINGTONE,
		 .tone = {.freq_hz = 800, .level_db = -6, .pattern = TONE_RING}},
	.call = {.preset = "16_2",
		 .ctx = BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL,
		 .bidir = true,
		 .tone = {.freq_hz = 300, .level_db = -12, .pattern = TONE_CONTINUOUS}},
	.call_index = -1,
};

static struct k_spinlock status_lock;
static struct phone_status status = {.state = "idle", .call_index = -1, .call_state = "idle"};

K_MSGQ_DEFINE(phone_q, sizeof(struct phone_msg), 8, 4);

static void alert_timeout(struct k_work *work)
{
	ARG_UNUSED(work);
	phone_post_event(PH_EV_ALERT_TIMEOUT, 0);
}

static K_WORK_DELAYABLE_DEFINE(alert_work, alert_timeout);

/* --- Helpers --- */

static void set_state(enum state to, enum phone_ev cause)
{
	if (to != ph.state) {
		cli_evt("source_state", "from=%s to=%s cause=%s", state_names[ph.state],
			state_names[to], ev_names[cause]);
		ph.state = to;
	}
}

static void report_error(enum phone_ev ev, int err)
{
	LOG_WRN("%s failed: %d", ev_names[ev], err);
	cli_evt("source_error", "op=%s err=%d state=%s", ev_names[ev], err, state_names[ph.state]);
}

static void call_event(const char *state, const char *by)
{
	cli_evt("source_call", "idx=%d state=%s by=%s", ph.call_index, state, by);
}

static const char *call_state_name(void)
{
	switch (ph.state) {
	case ST_RINGING:
		return "incoming";
	case ST_DIALING:
		return "alerting";
	case ST_IN_CALL:
		return "active";
	case ST_HELD:
		return "held";
	default:
		return "idle";
	}
}

/* Binds to the selected device's link and discovers its audio services once */
static int ensure_link(void)
{
	const uint8_t id = dev_selected();
	struct bt_conn *conn = dev_get_conn(id);
	int err;

	if (conn == NULL) {
		return -ENOTCONN;
	}

	if (conn != unicast_conn()) {
		volume_forget();
		hacontrol_forget();
		unicast_attach(conn, id);
	}

	if (unicast_discovered()) {
		return 0;
	}

	err = unicast_ensure_profile();
	if (err != 0) {
		return err;
	}

	/* Optional services */
	(void)volume_discover(conn);
	hacontrol_discover(conn);
	cli_evt("source_discovered", "id=%u sink=%d source=%d vcp=%d vocs=%d has=%d micp=%d", id,
		unicast_has_sink(), unicast_has_source(), volume_available(), volume_has_balance(),
		hacontrol_has_presets(), hacontrol_has_mic());

	return 0;
}

static int open_stream(const struct unicast_cfg *cfg)
{
	int err = ensure_link();

	return err ?: unicast_open(cfg);
}

/* After a call or an alert: back to music if it was playing, otherwise silence */
static void resume(enum phone_ev cause)
{
	int err;

	if (ph.music_wanted) {
		err = open_stream(&ph.music);
		if (err == 0) {
			set_state(ST_MUSIC, cause);
			return;
		}

		report_error(cause, err);
	}

	(void)unicast_close();
	set_state(ST_IDLE, cause);
}

static void end_call(const char *by, enum phone_ev cause)
{
	call_event("ended", by);
	ph.call_index = -1;
	resume(cause);
}

static void enter_call(const char *by, enum phone_ev cause)
{
	int err;

	call_event("active", by);
	set_state(ST_IN_CALL, cause);
	err = open_stream(&ph.call);
	if (err != 0) {
		report_error(cause, err);
	}
}

static void start_music(const char *by, enum phone_ev cause)
{
	int err;

	ph.music_wanted = true;
	if (ph.state != ST_IDLE && ph.state != ST_MUSIC) {
		/* Calls and alerts take precedence; music resumes afterwards */
		cli_evt("source_music", "state=deferred reason=%s", state_names[ph.state]);
		return;
	}

	err = open_stream(&ph.music);
	if (err != 0) {
		ph.music_wanted = false;
		report_error(cause, err);
		return;
	}

	set_state(ST_MUSIC, cause);
	cli_evt("source_music", "state=playing by=%s", by);
}

static void stop_music(const char *by, enum phone_ev cause)
{
	ph.music_wanted = false;
	if (ph.state == ST_MUSIC) {
		(void)unicast_close();
		set_state(ST_IDLE, cause);
	}

	cli_evt("source_music", "state=paused by=%s", by);
}

static void apply_tone(struct unicast_cfg *cfg, uint16_t freq, const char *preset)
{
	cfg->tone.freq_hz = freq;
	if (preset[0] != '\0') {
		(void)preset_find(PRESET_UNICAST, preset, &cfg->preset);
	}
}

/* --- Event handling --- */

/*
 * One handler per event; a non-zero return is reported as phone_error. The state check
 * (allowed[]) runs before, in handle().
 */

static int on_music_start(const struct phone_msg *m)
{
	apply_tone(&ph.music, m->music.freq, m->music.preset);
	start_music("local", m->ev);
	if (ph.music_wanted) {
		media_play();
	}

	return 0;
}

static int on_peer_play(const struct phone_msg *m)
{
	start_music("remote", m->ev);
	return 0;
}

static int on_music_stop(const struct phone_msg *m)
{
	stop_music("local", m->ev);
	media_pause();
	return 0;
}

static int on_peer_pause(const struct phone_msg *m)
{
	stop_music("remote", m->ev);
	return 0;
}

static int on_call_incoming(const struct phone_msg *m)
{
	int ret;

	ph.ring.tone.freq_hz = m->call.ring_freq;
	apply_tone(&ph.call, m->call.call_freq, m->call.preset);
	ret = call_incoming(m->call.uri, m->call.name);
	if (ret < 0) {
		return ret;
	}

	ph.call_index = ret;
	call_event("incoming", "local");
	set_state(ST_RINGING, m->ev);
	/* Ring on the running stream (pure context switch) or the music configuration */
	ph.ring.preset = unicast_streaming() ? unicast_current()->preset : ph.music.preset;

	return open_stream(&ph.ring);
}

static int on_call_outgoing(const struct phone_msg *m)
{
	int ret;

	apply_tone(&ph.call, m->call.call_freq, m->call.preset);
	ret = call_originate(m->call.uri);
	if (ret < 0) {
		return ret;
	}

	ph.call_index = ret;
	call_event("alerting", "local");
	set_state(ST_DIALING, m->ev);

	return 0;
}

static int on_peer_originated(const struct phone_msg *m)
{
	ph.call_index = m->peer.call_index;
	call_event("alerting", "peer");
	set_state(ST_DIALING, m->ev);
	return 0;
}

static int on_call_accept(const struct phone_msg *m)
{
	const int err = call_accept(ph.call_index);

	if (err == 0) {
		enter_call("local", m->ev);
	}

	return err;
}

static int on_peer_accepted(const struct phone_msg *m)
{
	enter_call("peer", m->ev);
	return 0;
}

static int on_call_remote_answer(const struct phone_msg *m)
{
	const int err = call_remote_answer(ph.call_index);

	if (err == 0) {
		enter_call("far_end", m->ev);
	}

	return err;
}

static int on_call_end(const struct phone_msg *m)
{
	const int err = call_terminate(ph.call_index);

	/* The call ends locally even if the hearing aid could not be told */
	if (err != 0) {
		report_error(m->ev, err);
	}

	end_call("local", m->ev);

	return 0;
}

static int on_peer_terminated(const struct phone_msg *m)
{
	end_call("peer", m->ev);
	return 0;
}

static int on_call_hold_retrieve(const struct phone_msg *m)
{
	const bool hold = m->ev == PH_REQ_CALL_HOLD;
	const int err = hold ? call_hold(ph.call_index) : call_retrieve(ph.call_index);

	if (err == 0) {
		call_event(hold ? "held" : "active", "local");
		set_state(hold ? ST_HELD : ST_IN_CALL, m->ev);
	}

	return err;
}

static int on_peer_held_retrieved(const struct phone_msg *m)
{
	const bool held = m->ev == PH_EV_PEER_HELD;

	call_event(held ? "held" : "active", "peer");
	set_state(held ? ST_HELD : ST_IN_CALL, m->ev);

	return 0;
}

static int on_alert(const struct phone_msg *m)
{
	struct unicast_cfg alert = {
		.preset = ph.music.preset,
		.ctx = BT_AUDIO_CONTEXT_TYPE_ALERTS,
		.tone = {.freq_hz = m->alert.freq,
			 .level_db = ALERT_LEVEL_DB,
			 .pattern = TONE_BEEP},
	};
	int err;

	/* Interrupts the running stream with its configuration */
	if (unicast_streaming()) {
		alert.preset = unicast_current()->preset;
		alert.bidir = unicast_current()->bidir;
	}

	err = open_stream(&alert);
	if (err != 0) {
		return err;
	}

	set_state(ST_ALERT, m->ev);
	k_work_reschedule(&alert_work, K_MSEC(m->alert.ms));

	return 0;
}

static int on_alert_timeout(const struct phone_msg *m)
{
	/* Alerts start only in IDLE or MUSIC: music_wanted already says what comes next (a music
	 * stop during the alert cleared it, a music start during the alert set it)
	 */
	resume(m->ev);
	return 0;
}

/* Controls of the hearing aid on the link: volume (VCP), presets (HAS), microphone, balance */
static int on_control(const struct phone_msg *m)
{
	const int err = ensure_link();

	if (err != 0) {
		return err;
	}

	switch (m->ev) {
	case PH_REQ_VOLUME:
		if (m->control.value == PHONE_VOLUME_UP || m->control.value == PHONE_VOLUME_DOWN) {
			return volume_step(m->control.value == PHONE_VOLUME_UP);
		}

		return volume_set(m->control.value);
	case PH_REQ_MUTE:
		return volume_mute(m->control.value != 0);
	case PH_REQ_PRESET:
		return hacontrol_preset(m->preset.op, m->preset.index);
	case PH_REQ_MIC_MUTE:
		return hacontrol_mic_mute(m->control.value != 0);
	case PH_REQ_MIC_GAIN:
		return hacontrol_mic_gain(m->control.value);
	case PH_REQ_BALANCE:
		return volume_balance(m->control.value);
	default:
		return -EINVAL;
	}
}

/* Ends a tracked call without asking (stop, link lost) */
static void drop_call(void)
{
	k_work_cancel_delayable(&alert_work);
	if (ph.call_index >= 0) {
		(void)call_terminate(ph.call_index);
		ph.call_index = -1;
	}
}

static int on_stop(const struct phone_msg *m)
{
	drop_call();
	ph.music_wanted = false;
	(void)unicast_close();
	media_pause();
	set_state(ST_IDLE, m->ev);
	cli_evt("source_stopped", NULL);

	return 0;
}

static int on_link_lost(const struct phone_msg *m)
{
	drop_call();
	cli_evt("source_link_lost", "id=%u", unicast_dev_id());
	unicast_link_lost();
	volume_forget();
	hacontrol_forget();
	set_state(ST_IDLE, m->ev);

	return 0;
}

typedef int (*event_handler_t)(const struct phone_msg *m);

static const event_handler_t handlers[] = {
	[PH_REQ_MUSIC_START] = on_music_start,
	[PH_REQ_MUSIC_STOP] = on_music_stop,
	[PH_REQ_CALL_INCOMING] = on_call_incoming,
	[PH_REQ_CALL_OUTGOING] = on_call_outgoing,
	[PH_REQ_CALL_ACCEPT] = on_call_accept,
	[PH_REQ_CALL_REMOTE_ANSWER] = on_call_remote_answer,
	[PH_REQ_CALL_END] = on_call_end,
	[PH_REQ_CALL_HOLD] = on_call_hold_retrieve,
	[PH_REQ_CALL_RETRIEVE] = on_call_hold_retrieve,
	[PH_REQ_ALERT] = on_alert,
	[PH_REQ_VOLUME] = on_control,
	[PH_REQ_MUTE] = on_control,
	[PH_REQ_PRESET] = on_control,
	[PH_REQ_MIC_MUTE] = on_control,
	[PH_REQ_MIC_GAIN] = on_control,
	[PH_REQ_BALANCE] = on_control,
	[PH_REQ_STOP] = on_stop,
	[PH_EV_PEER_PLAY] = on_peer_play,
	[PH_EV_PEER_PAUSE] = on_peer_pause,
	[PH_EV_PEER_ORIGINATED] = on_peer_originated,
	[PH_EV_PEER_ACCEPTED] = on_peer_accepted,
	[PH_EV_PEER_TERMINATED] = on_peer_terminated,
	[PH_EV_PEER_HELD] = on_peer_held_retrieved,
	[PH_EV_PEER_RETRIEVED] = on_peer_held_retrieved,
	[PH_EV_ALERT_TIMEOUT] = on_alert_timeout,
	[PH_EV_LINK_LOST] = on_link_lost,

};
BUILD_ASSERT(ARRAY_SIZE(handlers) == PH_EV_COUNT, "one entry per value");

/* Peer events about a call: only for the call the phone tracks */
static bool is_peer_call_event(enum phone_ev ev)
{
	return ev == PH_EV_PEER_ACCEPTED || ev == PH_EV_PEER_TERMINATED || ev == PH_EV_PEER_HELD ||
	       ev == PH_EV_PEER_RETRIEVED;
}

/* Payload each event carries (the rest carry none; events from Bluetooth carry a call index);
 * read only by the assertion below
 */
static const enum phone_payload payload_of[PH_EV_COUNT] __maybe_unused = {
	[PH_REQ_MUSIC_START] = PHONE_PAYLOAD_MUSIC,   [PH_REQ_CALL_INCOMING] = PHONE_PAYLOAD_CALL,
	[PH_REQ_CALL_OUTGOING] = PHONE_PAYLOAD_CALL,  [PH_REQ_ALERT] = PHONE_PAYLOAD_ALERT,
	[PH_REQ_VOLUME] = PHONE_PAYLOAD_CONTROL,      [PH_REQ_MUTE] = PHONE_PAYLOAD_CONTROL,
	[PH_REQ_PRESET] = PHONE_PAYLOAD_PRESET,       [PH_REQ_MIC_MUTE] = PHONE_PAYLOAD_CONTROL,
	[PH_REQ_MIC_GAIN] = PHONE_PAYLOAD_CONTROL,    [PH_REQ_BALANCE] = PHONE_PAYLOAD_CONTROL,
	[PH_EV_PEER_ACCEPTED] = PHONE_PAYLOAD_PEER,   [PH_EV_PEER_TERMINATED] = PHONE_PAYLOAD_PEER,
	[PH_EV_PEER_ORIGINATED] = PHONE_PAYLOAD_PEER, [PH_EV_PEER_HELD] = PHONE_PAYLOAD_PEER,
	[PH_EV_PEER_RETRIEVED] = PHONE_PAYLOAD_PEER,
};

static void handle(const struct phone_msg *m)
{
	int err;

	/* Events without payload may come through phone_post_event (PEER tag, no call) */
	__ASSERT(m->kind == payload_of[m->ev] ||
			 (payload_of[m->ev] == PHONE_PAYLOAD_NONE && m->kind == PHONE_PAYLOAD_PEER),
		 "%s with the wrong payload (%d)", ev_names[m->ev], m->kind);

	if ((allowed[m->ev] & BIT(ph.state)) == 0) {
		/* A call the hearing aid dialled as the state changed (call.c checks first): GTBS
		 * already created it, so end it rather than leave it alerting
		 */
		if (m->ev == PH_EV_PEER_ORIGINATED) {
			(void)call_terminate(m->peer.call_index);
		}
		report_error(m->ev, -EPERM);
		return;
	}

	if ((is_peer_call_event(m->ev) && m->peer.call_index != ph.call_index) ||
	    handlers[m->ev] == NULL) {
		return;
	}

	err = handlers[m->ev](m);
	if (err != 0) {
		report_error(m->ev, err);
	}
}

static void publish_status(void)
{
	const struct unicast_cfg *cur = unicast_current();
	struct phone_status st = {
		.state = state_names[ph.state],
		.dev_id = unicast_dev_id(),
		.discovered = unicast_discovered(),
		.streaming = cur != NULL,
		.ctx = cur != NULL ? cur->ctx : 0,
		.preset = cur != NULL ? cur->preset : "none",
		.bidir = cur != NULL && cur->bidir,
		.call_index = ph.call_index,
		.call_state = call_state_name(),
		.music = ph.music_wanted,
		.vcp = volume_available(),
	};

	K_SPINLOCK(&status_lock) {
		status = st;
	}
}

static void phone_thread(void *p1, void *p2, void *p3)
{
	struct phone_msg m;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_msgq_get(&phone_q, &m, K_FOREVER);
		handle(&m);
		publish_status();
	}
}

K_THREAD_DEFINE(phone_tid, 4096, phone_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, 0);

/* --- Public API --- */

int phone_request(const struct phone_msg *msg)
{
	return k_msgq_put(&phone_q, msg, K_NO_WAIT);
}

void phone_post_event(enum phone_ev ev, int32_t call_index)
{
	const struct phone_msg m = {
		.ev = ev,
		.kind = PHONE_PAYLOAD_PEER,
		.peer.call_index = call_index,
	};

	if (k_msgq_put(&phone_q, &m, K_NO_WAIT) != 0) {
		LOG_ERR("Phone queue full, %s lost", ev_names[ev]);
	}
}

void phone_get_status(struct phone_status *st)
{
	K_SPINLOCK(&status_lock) {
		*st = status;
	}

	st->tx = unicast_tx_count();
	st->rx = unicast_rx_count();
	st->rx_lost = unicast_rx_lost();
}

const char *phone_refused(enum phone_ev ev)
{
	const char *state = phone_state_str();

	for (int s = 0; s < ST_COUNT; s++) {
		if (state == state_names[s]) {
			return (allowed[ev] & BIT(s)) != 0 ? NULL : state;
		}
	}

	return NULL;
}

bool phone_takes_peer_call(void)
{
	const char *state = phone_state_str();

	return state == state_names[ST_IDLE] || state == state_names[ST_MUSIC];
}

const char *phone_state_str(void)
{
	struct phone_status st;

	phone_get_status(&st);

	return st.state;
}

void phone_reset(void)
{
	const struct phone_msg m = {.ev = PH_REQ_STOP};

	(void)phone_request(&m);
}

int phone_init(void)
{
	int err;

	err = unicast_init();
	err = err ?: volume_init();
	err = err ?: hacontrol_init();
	err = err ?: call_init();
	err = err ?: media_init();

	return err;
}
