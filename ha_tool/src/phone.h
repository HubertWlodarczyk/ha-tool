/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Phone simulation, split in layers:
 *   phone_cmds.c  shell commands: validate, build a request, queue it
 *   phone.c       scenario state machine (one worker thread owns all phone state)
 *   unicast.c     CAP/BAP unicast streams on one link (blocking, worker thread only)
 *   call.c        Telephone Bearer Service server (GTBS)
 *   media.c       Media Control Service server (local media player)
 *   volume.c      Volume Control (VCP volume controller)
 * Profile adapters report what the peer did with phone_post_event().
 */

#ifndef HA_TOOL_PHONE_H_
#define HA_TOOL_PHONE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/conn.h>

#include "audio_tx.h"

#define PHONE_STR_LEN    32
#define PHONE_PRESET_LEN 10

/* --- Requests (shell) and events (peer / timers) handled by the state machine --- */
enum phone_ev {
	/* Requests */
	PH_REQ_MUSIC_START,
	PH_REQ_MUSIC_STOP,
	PH_REQ_CALL_INCOMING,
	PH_REQ_CALL_OUTGOING,
	PH_REQ_CALL_ACCEPT,
	PH_REQ_CALL_REMOTE_ANSWER,
	PH_REQ_CALL_END,
	PH_REQ_CALL_HOLD,
	PH_REQ_CALL_RETRIEVE,
	PH_REQ_ALERT,
	PH_REQ_VOLUME,
	PH_REQ_MUTE,
	PH_REQ_STOP,
	PH_REQ_PRESET,
	PH_REQ_MIC_MUTE,
	PH_REQ_MIC_GAIN,
	PH_REQ_BALANCE, /* VOCS volume offset */
	/* Events */
	PH_EV_PEER_ACCEPTED,   /* hearing aid answered (CCP) */
	PH_EV_PEER_TERMINATED, /* hearing aid hung up / rejected */
	PH_EV_PEER_ORIGINATED, /* hearing aid dialled */
	PH_EV_PEER_HELD,
	PH_EV_PEER_RETRIEVED,
	PH_EV_PEER_PLAY, /* hearing aid pressed play (MCP) */
	PH_EV_PEER_PAUSE,
	PH_EV_ALERT_TIMEOUT,
	PH_EV_LINK_LOST,
	PH_EV_COUNT,
};

/* Relative volume (VCP volume up / down) as the value of a PH_REQ_VOLUME request */
#define PHONE_VOLUME_UP   1000
#define PHONE_VOLUME_DOWN (-1000)

/* Hearing aid preset operations (HAS, hacontrol.c) */
enum phone_preset_op {
	PRESET_OP_LIST,
	PRESET_OP_SET,
	PRESET_OP_NEXT,
	PRESET_OP_PREV,
};

/* Which member of phone_msg's union a request / event carries */
enum phone_payload {
	PHONE_PAYLOAD_NONE,
	PHONE_PAYLOAD_MUSIC,   /* PH_REQ_MUSIC_START */
	PHONE_PAYLOAD_CALL,    /* PH_REQ_CALL_INCOMING, PH_REQ_CALL_OUTGOING */
	PHONE_PAYLOAD_ALERT,   /* PH_REQ_ALERT */
	PHONE_PAYLOAD_CONTROL, /* PH_REQ_VOLUME, _MUTE, _MIC_MUTE, _MIC_GAIN, _BALANCE */
	PHONE_PAYLOAD_PRESET,  /* PH_REQ_PRESET */
	PHONE_PAYLOAD_PEER,    /* PH_EV_PEER_* about a call, from the call control server */
};

struct phone_msg {
	enum phone_ev ev;
	enum phone_payload kind; /* checked against the event in phone.c */
	union {
		struct {
			uint16_t freq;
			char preset[PHONE_PRESET_LEN]; /* "": keep */
		} music;
		struct {
			char uri[PHONE_STR_LEN];
			char name[PHONE_STR_LEN]; /* caller name (incoming) */
			uint16_t ring_freq;
			uint16_t call_freq;
			char preset[PHONE_PRESET_LEN]; /* of the conversation, "": keep */
		} call;
		struct {
			uint16_t freq;
			uint16_t ms;
		} alert;
		struct {
			int32_t value; /* volume 0..255 or a step, mute 0/1, gain, balance */
		} control;
		struct {
			enum phone_preset_op op;
			uint8_t index; /* PRESET_OP_SET */
		} preset;
		struct {
			int32_t call_index;
		} peer;
	};
};

int phone_request(const struct phone_msg *msg);
/* An event from Bluetooth callbacks or timers; call_index for the PH_EV_PEER_* call events */
void phone_post_event(enum phone_ev ev, int32_t call_index);

/* Snapshot for 'phone status' (consistent, safe from any thread) */
struct phone_status {
	const char *state;
	uint8_t dev_id;
	bool discovered;
	bool streaming;
	enum bt_audio_context ctx;
	const char *preset;
	bool bidir;
	int call_index;
	const char *call_state;
	bool music;
	uint32_t tx;
	uint32_t rx;
	uint32_t rx_lost;
	bool vcp;
};

void phone_get_status(struct phone_status *st);

/* --- unicast.c --- */
struct unicast_cfg {
	const char *preset; /* canonical name from presets.c */
	enum bt_audio_context ctx;
	bool bidir;
	struct tone tone;
};

int unicast_init(void);
/* Binds to the link of a virtual device (forgets the previous one) */
void unicast_attach(struct bt_conn *conn, uint8_t dev_id);
struct bt_conn *unicast_conn(void);
uint8_t unicast_dev_id(void);
/* Encryption + CAS/ASE discovery on the attached link (blocking) */
int unicast_ensure_profile(void);
/* Opens streams in the configuration, or only switches the context of matching streams */
int unicast_open(const struct unicast_cfg *cfg);
int unicast_close(void);
void unicast_link_lost(void);
bool unicast_streaming(void);
bool unicast_discovered(void);
bool unicast_has_sink(void);
bool unicast_has_source(void);
const struct unicast_cfg *unicast_current(void);
uint32_t unicast_tx_count(void);
uint32_t unicast_rx_count(void);
uint32_t unicast_rx_lost(void);

/* --- call.c --- */
int call_init(void);
int call_incoming(const char *uri, const char *name); /* returns call index */
int call_originate(const char *uri);                  /* returns call index */
int call_accept(uint8_t idx);
int call_remote_answer(uint8_t idx);
int call_terminate(uint8_t idx);
int call_hold(uint8_t idx);
int call_retrieve(uint8_t idx);

/* --- hacontrol.c --- */
int hacontrol_init(void);
void hacontrol_discover(struct bt_conn *conn); /* blocking, all services optional */
void hacontrol_forget(void);
bool hacontrol_has_presets(void);
bool hacontrol_has_mic(void);
int hacontrol_preset(enum phone_preset_op op, uint8_t index);
int hacontrol_mic_mute(bool mute);
int hacontrol_mic_gain(int8_t gain);

/* --- media.c --- */
int media_init(void);
void media_play(void);
void media_pause(void);

/* --- volume.c --- */
int volume_init(void);
int volume_discover(struct bt_conn *conn); /* blocking */
void volume_forget(void);
bool volume_available(void);
int volume_set(uint8_t volume);
int volume_step(bool up);
int volume_mute(bool mute);
bool volume_has_balance(void);
int volume_balance(int16_t offset); /* VOCS volume offset */

/* --- Life cycle (phone.c) --- */
int phone_init(void);
void phone_reset(void);
const char *phone_state_str(void);

#endif /* HA_TOOL_PHONE_H_ */
