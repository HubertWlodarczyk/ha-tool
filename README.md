# leabench: an LE Audio test bench on the nRF5340

**leabench** is firmware for the **nRF5340 DK** that plays the other side of a Bluetooth LE Audio
product under test: earbuds, headphones, speakers or any other LE Audio sink. It acts as a phone
(unicast audio, calls, media, volume), as many phones at once (identities and bonds) or as a set
of broadcast transmitters, and it is driven through a CLI over USB (VCOM), with **Python and
Java** libraries for automation.

| Feature | Description |
|---|---|
| **Identities and bonds** (`bonding`) | Every virtual identity has its own BLE identity (address, IRK, keys) and connects with a changing RPA like a real phone. Everything lives in RAM only (**the tool never writes to flash**), so a board reset clears the list. Identities connect to and bond with the device under test one after another: filling its bond list, checking which bond it evicts, reconnections, "the phone forgot the device". Just Works, passkey or numeric comparison pairing; connection parameters (interval, latency, timeout, PHY, data length). Connect by address or by Bluetooth name. |
| **LE Audio source** (`le-source`, e.g. a phone) | LE Audio unicast with the Media, Ringtone, Conversational and Alerts contexts. Calls through TBS/CCP (incoming, outgoing, answered on the device, hold, end), music through MCS/MCP (play/pause also from the device), volume through VCP, and, like a companion app, presets (HAS), microphone (MICP/AICS) and balance (VOCS). Losses of the device's microphone stream are counted from sequence numbers. |
| **Broadcast** (`broadcast`, Auracast™-style) | A broadcast builder: the tester composes the BIG, its subgroups (preset, language, context) and BISes (channel, sampling rate, octets, tone). Up to 3 BIGs at once, up to 8 BIS and 4 subgroups per BIG, up to 11 BIS in total without loss (e.g. 8 + 3). Encryption (each BIG with its own code), PBP (SQ/HQ), PHY, RTN, interval, a common ISO interval for 7.5 + 10 ms BIGs, TX power; a shared airtime budget across all BIGs (`broadcast budget`) checked against the controller's own limits, and its real schedule (`radio schedule`); operations on every BIG (`--all`) with a result per BIG; tone, level, power and metadata changed live. |
| **Audio** | Ready-made LC3 frames from a table (no encoder on the board): all 16 BAP configurations (8–48 kHz including 44.1, 7.5 and 10 ms frames), about 2070 tones up to 8 kHz, level from −40 to −2 dBFS. A separate tone per BIS / stream, ringtone and beep patterns. |

Until a board is available, everything runs in **simulation** (Zephyr `nrf5340bsim` +
BabbleSim). The tests use a simulated device under test, **`dut_sim`**, an LE Audio sink that
decodes the audio and reports the tone it hears, so the tests check the audio content and not
only the signalling.

## Layout

```
leabench/        the tool firmware (Zephyr application, nRF5340 app core)
  src/cli_table.c   command table (generated from protocol/leabench.yaml); sys_cmds.c: modes,
                    status, stop, reset
  src/dev*.c        'bonding' identities (dev.c: identities and callbacks, dev_cmds.c /
                    dev_link_cmds.c: commands, name scan)
  src/phone.c       LE Audio source: state machine (one thread owns the state)
  src/phone_cmds.c  'le-source' commands (validation and request queueing only)
  src/unicast.c     CAP/BAP unicast streams on one link
  src/call.c        TBS (calls), src/media.c MCS (music), src/volume.c VCP (volume)
  src/bcast/        Auracast builder: model.c (drafts), budget.c (airtime),
                    source.c (Zephyr broadcast source), args.c / validate.c,
                    cmds.c / run_cmds.c (commands)
  src/remotectl.c   device presets (HAS) and microphone (MICP), like a companion app
  src/human_evt.c   events described in words for 'proto human'
  src/*.h           module APIs (mode.h, dev.h, audio_tx.h, phone.h, bcast.h, version.h)
  src/presets.c     shared LC3 preset table (all 16 BAP configurations)
  src/audio_tx.c    tone frames from the table + ISO transmission
  src/lc3_frames.c  GENERATED LC3 frame table (scripts/gen_lc3_frames.sh)
  src/board.c       DK only: app core at 128 MHz
  netcore.conf      own, trimmed controller configuration (net core)
  Kconfig           limits (LEA_BCAST_MAX_BIGS / _SUBGROUPS / _BIS / _BIS_TOTAL)
dut_sim/        simulated device (simulation/tests only): main.c (connections),
                pairing.c (bonds, IO), ctrl.c (CCP/MCP buttons), audio_services.c (VCP, HAS,
                MICP), unicast.c / bsink.c / audio_rx.c (audio reception and measurement)
common/         CLI: cli_out.c (text/json/human), cli_evt.c (events), cli_parse.c
                (arguments, command keys), cli_tree.c (dispatcher, help, hints);
                hci_util.c (RSSI, power); dut_sim controller
tools/lc3gen/   LC3 frame table generator (host, liblc3)
protocol/       leabench.yaml: CLI protocol specification (single source of truth)
tools/protogen/ generator: firmware command table, Python/Java clients and docs/protocol.md
                from the specification (spec.py, writer.py, emit_c.py, emit_python.py,
                emit_java.py, emit_markdown.py)
clients/python/ Python library (package leabench)
clients/java/   Java library (Gradle, io.leabench)
docs/           protocol.md: generated command and event reference; broadcast-ux.md: CLI design
west.yml        west manifest (Zephyr v4.5.0 + modules)
zephyr/         module.yml, patches.yml and the Zephyr patches (west patch)
tests/          pytest tests in BabbleSim; helpers.py (shared helpers), simlib/ (sim.py:
                BabbleSim processes, device.py: text CLI client, bridge.py: TCP bridge)
scripts/        build.sh, test.sh, lint.sh, analyze.sh, sim.py (interactive simulation),
                cli.py, gen_lc3_frames.sh, apply_patches.sh, airtime.py
LICENSE         MPL-2.0; CONTRIBUTING.md: style, lint, how to add a command
```

## Getting started

For the shortest path through setup, simulation, hardware, and pull-request
checks, see the [quick guide](docs/quick-guide.md). The detailed setup below
also documents the pinned Zephyr and BabbleSim dependencies.

Runnable client examples are in [examples/](examples/), including a Python
example for the simulator and a physical nRF5340 DK.

Command references:

- [CLI commands](docs/cli.md)
- [Python client commands](docs/python.md)
- [Java client commands](docs/java.md)

The repository is a `west` manifest: it pins Zephyr **v4.5.0**, fetches only the modules it
needs and applies the patches. Requirements: Python 3, git, gcc with 32-bit libraries
(`lib32-glibc`, `lib32-gcc-libs`; the bsim boards build as 32-bit), `dtc`.

```
mkdir ha-ws && cd ha-ws
git clone https://github.com/HubertWlodarczyk/leabench.git
python3 -m venv .venv && . .venv/bin/activate
pip install west cmake ninja
west init -l leabench
west update --narrow -o=--depth=1
west patch apply                       # patches from leabench/zephyr/patches.yml
pip install -r zephyr/scripts/requirements-base.txt -r zephyr/scripts/requirements-build-test.txt
export ZEPHYR_BASE=$PWD/zephyr BSIM_OUT_PATH=$PWD/tools/bsim BSIM_COMPONENTS_PATH=$PWD/tools/bsim/components
(cd tools/bsim && make everything -j$(nproc))
leabench/scripts/build.sh && leabench/scripts/test.sh
```

`ZEPHYR_BASE` and `BSIM_*` must point at **this** workspace. If they point at another one, the
build and BabbleSim silently use that one.

Zephyr patches (re-apply them after every `west update`: `west patch apply`, or
`scripts/apply_patches.sh`):

| Patch | Why |
|---|---|
| `0001-central-connect-from-identity` | `bt_conn_le_create_set_id()`: without it Zephyr always connects as central from the default identity, so virtual devices could not have their own addresses and bonds. |
| `0002-controller-bis-handle-range` | **Controller bug**: the BIS handle range was computed from the per-BIG limit instead of the pool of all BIGs; the 5th BIS of two BIGs hit an assertion. |
| `0003-controller-adv-iso-pto-kconfig` | The controller added "pre-transmission" subevents to every BIG event on its own (almost 2× longer events). Now a Kconfig option; disabled in `leabench`, repetitions are set by the tester with `rtn=`. |
| `0004-controller-big-create-prepare-per-big` | **Controller bug**: the "BIG create" switch was shared by all BIGs, so an event of a running BIG "ate" the creation of a new one and the 3rd BIG never started. |
| `0005-pbp-announcement-no-null-memcpy` | `bt_pbp_get_announcement()` without metadata called `memcpy(dst, NULL, 0)`: undefined behaviour in C (found by UBSan in the `--sanitize` build). |
| `0006-controller-adv-anchor-free-of-periodic` | **Controller bug**: a new BIG, PA train and AUX set were placed only after the *nearest* event of each role, so they could permanently land on a later event of a running role with a multiple interval: BIG on BIG (both lost every other event, 50% of frames), 60 ms PA on a 10 ms BIG (~17%), 70 ms AUX on a BIG (AUX never went out, the broadcast could not be found). Now every periodic role gets its own phase: a PA is accounted the way a synchronized receiver reserves it, PA trains and AUX sets with the same interval share one phase, and among free phases the one leaving the largest gap is chosen. |
| `0007-shell-refuse-line-with-filtered-characters` | The shell silently dropped non-ASCII bytes and ran the rest of the line (`name=Zażółć` gave the name "Za", and a line of only such characters got no reply). Now the line is refused with a shell error. |
| `0008-shell-builtin-help-optional` | The Kconfig option `SHELL_HELP_CMD` disables the shell's built-in `help`, so that `help` can come from the protocol specification (`common/src/cli_tree.c`). |
| `0009-shell-one-error-for-unknown-command` | A line with an unknown first word and more than `CONFIG_SHELL_ARGC_MAX` words got two errors ("command not found" and "too many arguments"), which shifted replies for clients. Now it gets one. |
| `0010-vocs-client-drop-conn-on-disconnect` | **Host bug**: the volume controller reset itself on a disconnection, but the VOCS (balance) clients it discovered kept their reference to the connection. The connection object was never freed, so after a session with volume control `bonding disconnect X` + `bonding connect X` failed with `-EINVAL` until reboot. VOCS now drops it on disconnection, like AICS. |
| `0011-vcp-vol-ctlr-retry-with-read-counter` | **Host bug**: after an "Invalid Change Counter" error the volume controller re-read the state but retried with the stale counter, so the retry failed too. It learns the counter only from notifications, so every volume command failed once the device's counter had moved: after any reconnection, or on a device whose volume had been changed before (the usual case with real devices). |
| `0012-controller-big-create-release-streams-on-error` | **Controller bug**: LE Create BIG reserved the BIS streams before its last check (the event must fit the ISO interval next to the advertising slots) and did not return them when that check refused the BIG. Two refused 8-BIS BIGs used up the pool of 16, and every later BIG failed with Memory Capacity Exceeded until reset. |
| `0013-controller-vs-read-schedule` | **New vendor command** `Read Schedule`: every running role as the controller's ticker has it (next event, period, reserved slot). `radio schedule` shows the real phases (proto human: a 60 ms timeline) and counts overlapping reservations, instead of the tool only modelling them. |

All of them are suitable for upstream submission.

## Build and test

```
scripts/build.sh              # leabench + dut_sim for nrf5340bsim (host gcc, no Zephyr SDK)
scripts/test.sh               # 122 tests in simulation, about 10 min (including the library tests)
scripts/test.sh -m slow       # 11 long ones: fuzzing, soak, cycles, event storm, budget limit (about 12 min)
scripts/test.sh -k phone      # selected tests
LEA_SIM_SPEED=2 scripts/test.sh   # simulation 2x faster than real time
```

Airtime analysis (who transmitted when, why ISO events are lost):

```
LEA_PHY_ARGS=-dump_imm scripts/test.sh -k <test>   # full radio dumps in $BSIM_OUT_PATH/results/<sim_id>/
scripts/airtime.py <sim_id> --from-ms 5000         # streams, intervals, occupancy, causes of CIS losses
```

Stacks (the simulation cannot show a stack overflow: in BabbleSim every thread runs on a host
stack, so only the board's MPU guard catches it):

```
scripts/stack_report.py --build  # DK images with -fstack-usage: each thread's stack next to the
                                 # deepest chain of direct calls, largest frames, deepest chains
scripts/build.sh dk --diag       # on the board: every thread's used / size on RTT every 10 s,
                                 # stack canaries, 'kernel thread stacks'
```

Limits and memory (the numbers in "How many BIGs and BISes at once" and "On hardware"):

```
scripts/capacity.py accept       # which BIGs the controller itself creates, next to the model
scripts/capacity.py broadcast    # each preset / RTN / PHY / encryption at the budget's edge and
                                 # one BIS past it (--force), received by the device
scripts/capacity.py multi        # several BIGs at their combined edge
scripts/capacity.py unicast      # CIS loss next to 1-2 other links of 30-100 ms
scripts/mem_report.sh <name> [hci_ipc_CONFIG_X=v ...]   # DK RAM / flash of a config variant
```

Logs of every test go to `tests/out/<test>/`:
- `<dev>.log` holds the Zephyr logs,
- `<dev>.cli.log` holds the full CLI conversation.

Playing around in the simulation by hand:

```
scripts/sim.py                # prints the ptys of both CLIs, e.g. /dev/pts/7
screen /dev/pts/7             # or any serial terminal / your own script
```

## Python and Java libraries

Automation uses the libraries: `clients/python` (package `leabench`, pyserial) and
`clients/java` (Java 17, jSerialComm). Details are in their READMEs. Both look the same:

```python
with LeaBench.open("/dev/ttyACM0") as tool:  # LeaBench.find(), LeaBench.tcp(...)
    d = tool.bonding.create("Phone of José")
    tool.bonding.connect(d.id, address=addr)
    tool.wait_event("bonding_connected", id=d.id, timeout=20)
    tool.broadcast.bis.tone(1, 2, freq=880)  # broadcast 1 bis tone 2 freq=880
```

```java
try (LeaBench tool = LeaBench.openSerial("/dev/ttyACM0")) {
    Reply d = tool.bonding().create(new BondingCreateOptions().name("Phone of José"));
    tool.bonding().connect("Phone of José", new BondingConnectOptions().address(addr));
    tool.waitEvent("bonding_connected", Duration.ofSeconds(20), "id", d.getInt("id"));
}
```

- **Single source of truth:** `protocol/leabench.yaml` describes every command (line form,
  arguments, keys, flags, types, ranges, reply fields) and every event. `tools/protogen/gen.py`
  generates from it the firmware command table (`leabench/src/cli_table.c`), the methods of both
  libraries and `docs/protocol.md`. A command without a firmware handler does not link, and
  `gen.py --check` makes sure the generated files are up to date. To change a command: edit the
  YAML and run `tools/protogen/gen.py`.
- **Library protocol:** JSON Lines (`proto json`), a common choice for devices driven over a
  serial line. The library enables it on connect and checks the protocol version.
- **Without a board:** `scripts/sim.py --tcp 7000` exposes the simulated `leabench` on TCP 7000
  (and the device on 7001), so Java and Python tests can be written and run without a DK.
- **Several boards:** `LeaBench.find(chip_id)` scans the SEGGER USB ports and recognizes the board
  by the `id` command.
- **No local Java:** a JDK 17 can be unpacked without admin rights (e.g. Temurin into
  `~/.local/opt`). Gradle downloads itself through `./gradlew`.

## CLI protocol

The interface design, with its rationale, is in [docs/broadcast-ux.md](docs/broadcast-ux.md),
and the full (generated) command and event reference is in [docs/protocol.md](docs/protocol.md).
`help`, `help bonding` and `help broadcast 1 bis add` print help from the same specification.

Every command ends with **exactly one** `OK ...` or `ERR <errno> <description>` line.
Asynchronous events look like `EVT t=<ms> <name> k=v ...`. Data lines (`IDENTITY`, `BIG`, `BIS`,
`RESULT`, ...) come before `OK`. Events go through a queue and a separate thread, so they never
block the Bluetooth stack. When the queue overflows, `EVT dropped n=<count>` appears. Logs use a
separate channel: RTT on hardware, stdout in simulation.

- **An error says what to do next:** `ERR -16 cannot change BIG 1 while it is active
  hint="broadcast 1 stop"`. In `proto json` it is the `hint` field, in `proto human` separate
  `Try:` lines.
- **Typos:** an unknown key is an error, not a silent default (`ERR -22 unknown key key=frq
  cmd="broadcast {big} bis tone" allowed=freq,exact,level,pattern`). An unknown command gets
  the closest valid ones as a `hint` (`le-source volume increase` → `le-source volume up;
  le-source volume down; le-source volume set <volume>`).
- **Flags** look like `--name` (`broadcast start --all`, `status --verbose`, `reset --yes`),
  parameters like `key=value`. Addresses: `AA:BB:CC:DD:EE:FF/random` (or `/public`).
- **Spaces and non-ASCII characters:** values with spaces are given in quotes (`name="Hall A"`)
  and printed the same way. The shell accepts ASCII only, so non-ASCII characters go as UTF-8
  `\xHH` escapes inside quotes (`name="Phone of Jos\xc3\xa9"`). The libraries do this for you.

> An unterminated quote, more than 24 words, an unknown first word and a line with non-ASCII
> characters are reported by the Zephyr shell itself (`not terminated: "`, `Too many arguments
> in the command.`, `... command not found`, `line ignored: N non-ASCII character(s)`), always
> as one line (patches 0007 and 0009). `tests/simlib` and the libraries map them to `ERR -22`.

| Command | Description |
|---|---|
| `proto [text\|json\|human] [--verbose]` | Output format: `text` (default, `k=v`), `json` (JSON Lines for the libraries), `human` (echo, `lea> ` prompt, tables, BIG tree, hints on separate lines, layered `status` and `le-source status`, events as sentences with low-level ones such as MTU or per-direction stream events hidden; for terminal work). `--verbose` in `human` also prints the raw `EVT` lines. |
| `status [--verbose]` | Layered state: `mode`, attached identity, device, `connection`, `security`, `bond`, source state, number of active and configured BIGs; `--verbose` adds counters. |
| `mode [unicast\|broadcast]` | Radio mode, see below. |
| `stop` | Stops all radio activity of the current mode (connections or BIGs); the configuration stays. |
| `reset [--yes]` | Everything from scratch: no mode, no identities, bonds or broadcasts. Requires `--yes` in `proto human`. |
| `help [<words>]` | Commands with usage and description (everything, a group or one command). |
| `radio schedule` | The controller's running roles (BIG, PA, AUX, ADV, ACL, CIG, scan) with next event, period and reserved slot (`SLOT` lines), and how many reservations of scheduled roles overlap within 60 ms; proto human: a timeline (Zephyr patch 0013). |
| `limits`, `id`, `time`, `version` | Limits of this firmware build, chip ID, uptime, version. |
| `evt off <name>...`, `evt on [<name>...]`, `evt list` | Event filter: filtered events are neither printed nor queued. `reset` clears the filter. |
| `cli spec`, `cli stats`, `cli burst [n=]` | Command table; truncated lines and dropped events; event queue diagnostics. |

### Modes: unicast or broadcast

The tool works in one radio mode at a time: **`unicast`** (the `bonding` and `le-source` areas)
or **`broadcast`** (Auracast). Reason: a unicast stream next to a broadcast loses up to about 30%
of its frames in the controller (measurements in "Found in simulation").

- After boot and after `reset` the mode is `none`. The first `bonding`/`le-source` command enters
  `unicast` by itself, and the first `broadcast` command enters `broadcast` (`EVT mode from= to=`).
  A command that fails while doing so leaves `none`.
- Switching between `unicast` and `broadcast` takes an explicit `mode` and **clears the whole
  context** of the previous mode: identities, bonds, connections and the source, or all BIGs. A
  command from the other area gets `ERR -16 ... hint="mode broadcast"`.
- Read-only commands (`bonding list`, `le-source status`, `broadcast list`, ...) always work.

### bonding: identities, connections, bonds

An identity is referred to by name or by ID (`bonding pair Phone`, `bonding pair 1`). Names are
unique and cannot consist of digits only.

| Command | Description |
|---|---|
| `bonding create [<name>] [addr=<static random>] [io=none\|display\|yesno\|keyboard\|kbd_display]` | New identity (address, IRK, keys; RAM only). Max. 8. |
| `bonding list`, `bonding show <id> [--verbose]` | State layers: `connection` (disconnected/connecting/connected), `device` and `device_name`, `security` (unencrypted/encrypted), `bond` (absent/present), `source`. `--verbose`: RSSI, interval, latency, supervision timeout, PHY. |
| `bonding scan [timeout=ms]` | Devices advertising a name (`DEVICE addr= name= rssi=`). |
| `bonding connect <id> address=<addr>\|bluetooth-name=<name> [timeout=] [interval=] [latency=] [sto=] [scan=]` | Connect as this identity. By name exactly one device must answer; if several do, they are listed and the connection does not start (`hint` with the address). Result in `EVT bonding_connected` / `bonding_connect_failed`. Connecting does not pair. |
| `bonding pair <id> [sec=2\|3\|4]` | Pair on an existing connection. Result: `bonding_security`, `bonding_paired` / `bonding_pair_failed`. |
| `bonding passkey <id> <n>`, `bonding confirm <id> yes\|no` | The phone user enters the passkey (after `bonding_passkey_request`) or confirms numeric comparison (after `bonding_confirm`). |
| `bonding disconnect <id>`, `bonding unpair <id>`, `bonding delete <id>` | Disconnect (the bond stays); delete the keys **locally only**; delete the identity. |
| `bonding bonds <id>`, `bonding io <id> <cap>` | Bonds of the identity; IO capabilities from the next connection on. |
| `bonding param`, `bonding phy`, `bonding dle`, `bonding txpower`, `bonding policy` | Link parameters, TX power (distance simulation), answer to the device's parameter requests. |

> **A bond is ready only after `EVT bonding_paired`.** `bonding_security` only means encryption
> is on; key distribution takes a moment longer, and disconnecting in that window aborts bonding.

### le-source: LE Audio source

`le-source attach <id>` selects the identity the source uses (one at a time; `le-source detach`
stops audio and calls and releases it). Attaching does not connect: `bonding connect` does.
Commands only queue a request (`OK queued=1`); the result always comes as `EVT source_*`.

| Command | Description |
|---|---|
| `le-source music start [preset=48_2] [freq=440]` / `music stop` | Music (Media context). Frequencies 0 or 100–8000 Hz; the nearest table tone is sent (440 Hz at 48_2 is 450 Hz). |
| `le-source call incoming [uri=] [name=] [ring_freq=] [call_freq=] [call_preset=]` | Rings; if music is playing, the stream switches to Ringtone. |
| `le-source call accept`, `outgoing`, `remote_answer`, `end`, `hold`, `retrieve` | A call (bidirectional Conversational stream); `remote_answer` simulates the remote party answering. Music resumes after the call. |
| `le-source alert [ms=1000] [freq=1500]` | Notification sound (Alerts). |
| `le-source volume set <0..255>\|up\|down`, `le-source mute on\|off` | Device volume control (VCP). Result: `EVT source_volume`. |
| `le-source preset list\|next\|prev\|set <n>` | Device presets (HAS). |
| `le-source mic mute on\|off`, `le-source mic gain <n>`, `le-source balance set <n>` | Microphone (MICP/AICS) and volume offset (VOCS). |
| `le-source stop`, `le-source status` | Stop audio and calls; state with identity, device, connection, stream and reception statistics. |

Source events: `source_state from= to= cause=` (every state machine transition: `idle`, `music`,
`ringing`, `dialing`, `in_call`, `held`, `alert`), `source_discovered`,
`source_stream_open/updated/closed`, `source_call`, `source_music`, `source_volume`,
`source_error op= err= state=` (also a request not allowed in the current state, `err=-1`),
`source_link_lost`. Music requested during a call is deferred and starts after it.

### broadcast: BIG builder

The BIG number (1–3) is always explicit: `broadcast 2 start`, with no selection command. BIG
states: `draft` (incomplete), `ready` (at least one subgroup and one BIS), `active`, `stopped`
(was active, configuration kept), `error` (the last start failed).

| Level | Parameters |
|---|---|
| **BIG** (shared by all BISes) | name, Broadcast ID, encryption (`code`), PBP, PHY, **interval** (7.5/10 ms), RTN, latency, presentation delay, framing, packing, periodic advertising interval (`pa=`), TX power |
| **Subgroup** | LC3 preset (base codec configuration), language, context, program |
| **BIS** | subgroup, channel (`location`), sampling rate (`rate`), octets per frame, tone, level, pattern |

| Command | Description |
|---|---|
| `broadcast <n> create [name=] [code=] [pbp=] [id=] [phy=] [interval=7.5\|10] [rtn=] [latency=] [pd=] [pa=] [iso=0\|15\|20\|30\|40] [packing=] [framing=] [preset=] [bis=N] [lang=] [ctx=] [program=]` | New BIG (draft). With `preset=` or `bis=`, one subgroup and N BISes right away (mono, L/R, L/R/C ...), tones 500, 1000, 1500 ... Hz. Existing BIG: `ERR -17 hint="broadcast <n> delete"`. |
| `broadcast <n> set ...`, `broadcast <n> delete` | Change BIG parameters (not while transmitting); delete. |
| `broadcast <n> subgroup add\|set <sg>\|delete <sg>\|list` | Subgroups. `set` on an active BIG changes only `lang`, `ctx`, `program` (one subgroup: a Zephyr API limitation). `delete` only without BISes; numbers stay contiguous. |
| `broadcast <n> bis add\|set <bis>\|delete <bis>\|list` | BISes: `subgroup=`, `location=mono\|left\|right\|front_center\|back_left\|back_right\|side_left\|side_right`, `rate=`, `octets=`, `freq=`, `exact=1`, `level=-40..-2`, `pattern=cont\|ring\|beep`. The reply gives `cfg`, `freq_actual` and `level_actual`; a combination outside the LC3 table is `ERR -95` with a list. |
| `broadcast <n> bis tone <bis> [freq=] [level=] [pattern=] [exact=]` | Tone, also live. |
| `broadcast <n> start [--force]`, `stop`, `restart` | Validation (subgroups with BISes, frame duration, table, **airtime budget of all BIGs**), then start; the error says what is missing. A BIG with a different interval than the running ones: `ERR -33`; `--force` starts it anyway (`EVT broadcast_warning reason=interval_mix`). |
| `broadcast start\|stop\|restart --all` | On every configured BIG: one `RESULT big= name= result= errno= reason=` line each; the reply counts the BIGs the operation was performed on (`stopped=1` if one was running). If any failed, the reply is `ERR` naming all of them ("1 of 3 BIGs started, BIGs 2, 3 failed"). |
| `broadcast <n> show`, `broadcast list` | Configuration, state and airtime of a BIG (a tree of subgroups and BISes in `proto human`); all BIGs (a table in `proto human`). |
| `broadcast budget [preset=16_2] [rtn=N] [phy=1m\|2m] [enc=1]` | Cost of each active BIG (also in %), free space and `next_big_bis`: how many BISes of the given kind one more BIG can take. |
| `broadcast <n> txpower <dBm>` | Advertising, PA and BIG power (−40..8), also live. |

Events: `broadcast_started big= broadcast_id= bis= subgroups= encrypted= airtime_us=`,
`broadcast_stopped big=`, `broadcast_warning`, `broadcast_error`.

#### LC3 frame table (instead of an encoder)

There is no LC3 encoder or DSP on the board: `tools/lc3gen` encodes the tones on the PC (liblc3,
the same library as in `dut_sim`), and the firmware only copies ready frames.

- **Configurations:** all 16 from BAP (8, 16, 24, 32, 44.1, 48 kHz; 7.5 and 10 ms frames; all
  octet variants). 44.1 kHz is encoded like 48 kHz (the same 360/480-sample frame, lasting
  8163/10884 µs; a framed BIG).
- **Tones:** a frame sent in a loop must contain a whole number of periods, so the grid follows
  the configuration: a 1–2 frame cycle, a step of **50 Hz at 10 ms, 66.7 Hz at 7.5 ms, 45.9 Hz at
  44.1 kHz**, from about 100 Hz to min(8 kHz, 0.45·fs). 2058 tones in total, 257 KB of flash
  (read-only).
- **Level without extra frames:** the LC3 decoder scales the whole spectrum by `10^(g_idx/28)`,
  and the 8-bit `g_idx` (global gain) sits at a fixed position in the frame's side information.
  The firmware rewrites that field while copying: any level from −40 to −2 dBFS in 0.71 dB steps.
- **Headroom:** at the lower bitrates (8_x … 32_x, 48_1, 48_2) the decoder overshoots a full
  scale sine by up to 0.7 dB and clips it: a 3rd harmonic at about −30 dBc, heard as distortion.
  The highest level is therefore −2 dBFS (decoded peak at most −1.3 dBFS over the whole table);
  0 dBFS is refused.
- **Generator self-test** (generation fails if it does not pass): every tone decoded in a ×50
  loop at −2, −20 and −40 dBFS gives the right frequency, a flat envelope across cycle boundaries
  (≤ 1 dB), an SNR ≥ 28 dB, a peak below −0.45 dBFS, and the level after rewriting `g_idx`
  within 0.3 dB.
- **Tone quality** (`scripts/gen_lc3_frames.sh --probe`: every tone and level next to a
  continuous encoding of the same sine, in `build/lc3probe/report.csv`; `--probe wav ...` writes
  listening samples, including a lost frame and a phase jump). The looped table is as clean as a
  continuous encoding; what remains is the codec at that bitrate. SNR (noise + distortion) at
  −12 dBFS, worst / median tone:

  | Config | 8_2 | 16_2 | 24_2 | 32_2 | 48_2 | 48_4 | 48_6 |
  |---|---|---|---|---|---|---|---|
  | SNR dB | 30 / 39 | 32 / 50 | 40 / 59 | 57 / 65 | 43 / 69 | 63 / 74 | 71 / 80 |

  At 8_x and 16_x the coding noise sits in lines ±100–200 Hz around the tone (the frame rate) and
  can be heard on a pure tone as roughness; prefer 24_2 or above for listening tests.
- Regeneration: `scripts/gen_lc3_frames.sh` (deterministic; `--max-cycle 4` gives a 2× denser
  grid, about 1.4 MB, i.e. external QSPI flash territory).

#### How many BIGs and BISes at once

Controller memory limits (`leabench/netcore.conf`): 3 BIGs, 8 BIS per BIG, 16 in total. In
practice **airtime** decides. `broadcast <n> start` checks two things, in this order.

**1. The controller's own event limit (one BIG).** LE Create BIG lays the whole BIG event out in
one ISO interval next to slots for the BIG's own AUX and PA advertising. If it does not fit, the
controller refuses the BIG, or, with RTN > 0, **silently lowers RTN** until it fits. The tool
checks this itself and refuses with the largest RTN that fits (`hint="broadcast 1 set rtn=1"`):

`airtime(rtn) + 2830 µs + 4 µs × (AUX_ADV_IND data + AUX_SYNC_IND data) ≤ ISO interval`

- `airtime_us` = number of BISes × (RTN+1) × ((largest PDU of the BIG + headers, +4 B MIC when
  encrypted, +5 B when framed) × byte time + 150 µs);
- 2830 µs = 3 × event start-up and end margin, the control subevent and the PDU headers;
- the data lengths come from the BIG: name, PBP, and the BASE (8 B per BIS, more with language or
  program, +24 B of BIGInfo when encrypted); advertising uses the 2M secondary PHY;
- the ISO interval is a whole number of 1.25 ms (44.1 kHz framed: 10 ms).

The constant was fitted to `scripts/capacity.py accept` (10 presets × RTN 0/1/2/4 × 1M/2M ×
plain/encrypted × 1–8 BIS, 727 BIGs): every BIG the tool accepts, the controller creates with
exactly the requested layout (0 refusals, 0 lowered RTNs). After a start the tool also reads the
real layout back (`bt_iso_chan_get_info`) and reports `broadcast_warning reason=layout` if it
ever differs. `--force` cannot bypass this check, because the controller would refuse anyway.

**2. The shared budget (several BIGs):**

`Σ over transmitting BIGs (airtime_us + 1000) · 10 ms / interval + reserve ≤ 10000 µs`,
reserve = 1000 µs with one BIG, 3800 µs with two or more

- 1000 µs per BIG is the controller's reservation around the event: control subevent, radio
  ramp-up (`EVENT_OVERHEAD_START_US` = 733 µs with scanning and coded PHY enabled), end and
  scheduler margin;
- the reserve with several BIGs = one phase for all PA trains (patch 0006 places them in the
  same phase, offset by whole BIG intervals), accounted the way a receiver synchronized to the
  PA reserves it (1.9 ms on 2M), + one phase for the AUX sets (about 1 ms), + about 0.7 ms of
  free gap, so that primary advertising goes out and the broadcasts can be found;
- with one BIG the controller keeps its advertising slots in every event anyway, so its event
  limit (1.) is the real one. Measured lossless right up to it.
- `--force` starts a BIG past this budget (with `broadcast_warning reason=over_budget`), for
  loss measurements.

`broadcast budget` shows `next_big_bis`, i.e. how many BISes one more BIG can take: both checks,
plus the controller's free BIGs and BISes.

**Measured capacity of one BIG** (`scripts/capacity.py broadcast`: the budget's number of BISes
is started and received for 4 s by the simulated device, which stays synchronized to the PA;
one more BIS is started with `--force`). Every configuration the tool accepts was received
without loss and found (190 cases, 0 losses). The table shows BISes for 2M / 1M PHY; where
encryption costs one BIS, the encrypted number is in parentheses:

| Preset | rtn=0 | rtn=2 | rtn=4 |
|---|---|---|---|
| 16_2 (10 ms, 40 B) | 8 / 8 | 6 / 4 (3) | 3 / 2 |
| 24_2 (10 ms, 60 B) | 8 / 8 | 5 (4) / 3 | 3 / 1 |
| 48_2 (10 ms, 100 B) | 8 / 6 | 3 / 2 | 2 / 1 |
| 48_4 (10 ms, 120 B) | 8 / 5 | 3 / 1 | 2 (1) / 1 |
| 48_6 (10 ms, 155 B) | 8 (7) / 4 | 2 / 1 | 1 / 0 |
| 441_2 (44.1 kHz framed, 130 B) | 8 / 5 | 3 / 1 | 1 / 1 |
| 16_1 (7.5 ms, 30 B) | 8 / 8 | 4 / 3 (2) | 2 / 1 |
| 48_1 (7.5 ms, 75 B) | 8 / 5 (4) | 2 / 1 | 1 / 1 |
| 48_5 (7.5 ms, 117 B) | 6 / 3 | 2 / 1 | 1 / 0 |

**Several BIGs** (`scripts/capacity.py multi`, `scripts/test.sh -m slow test_capacity.py`):

| Measurement (BIG 1 plain, the others encrypted) | result |
|---|---|
| 16_2 rtn=0: 8 + 3, 7 + 4, 6 + 5 (**11 BIS**, the limit for 2 BIGs) | lossless (test) |
| 16_2 rtn=0: 3 + 3 + 2, 4 + 3 + 1, 2 + 3 + 3 (8 BIS, the limit for 3 BIGs) | lossless (test) |
| 48_4 rtn=0 2 + 2 | lossless |
| 48_2 rtn=2 2 + 16_2 rtn=2 2, 24_2 rtn=1 3 + 2 + 16_2 2 | rejected by the budget |
| 8 + 4, 3 + 3 + 3 (16_2) | rejected by the budget (test) |
| before the patch 0006 fix: 8 + 3, 7 + 3, 7 + 2, 8 + 2 | 8–17% loss (PA on a BIG at the transmitter or at the receiver) |
| before the patch 0006 fix: 4 + 4 + 3, 8 + 3 + 1 | AUX or primary advertising did not go out: the broadcast could not be found |

**Start order and phases.** The edge cases started small BIG first (`scripts/capacity.py
multi-reversed`) were lossless too: patch 0006 places every new periodic role in the largest free
phase, so the order of `start` does not matter and `start --all` keeps the BIG numbers' order. All
PA trains already share one phase, offset by whole BIG intervals. The AUX sets cannot join it,
because their interval is the advertising interval plus the 10 ms random-delay range (70 ms next
to 60 ms PA), so they drift against it.

**Coded PHY costs capacity.** It raises the controller's event start-up from 428 to 733 µs
(`EVENT_OVERHEAD_START_US`). Measured without it (`scripts/capacity.py accept`), the controller's
reserve per BIG event drops by about 0.8 ms (2830 → 2030 µs), usually one more BIS per BIG on 1M
PHY and with 7.5 ms frames (48_6/1M 4 → 5, 48_5/2M 6 → 7). It stays enabled because `bonding phy
<id> coded` tests a device's long-range link; a broadcast-only build can drop it
(`CONFIG_BT_CTLR_PHY_CODED=n` in `leabench/netcore.conf`, then refit `CTLR_RESERVE_US`).

**Unicast** (`scripts/capacity.py unicast`): the music stream (48_2) to one device lost no
frame next to 1 or 2 other connections (to a second device) with intervals of 30, 50, 60 or
100 ms. Losses come only with a broadcast running next to the CIS (see "Found in simulation"),
which is why the tool works in modes.

Encryption costs only airtime (4 B MIC per PDU) and BIGInfo length: the radio's hardware CCM
encrypts, and the keys are computed once when the BIG starts. 16_2 with 4 BIS, rtn=2 on 1M fits
in plain text, but not encrypted (test).

**BIGs with different intervals** (7.5 and 10 ms) meet every few events. They could avoid it only
if their events together fit into the GCD of the intervals (2.5 ms), while the overhead alone is
about 1 ms per BIG plus its airtime. Measured losses: 25–40% of one of them. That is why
`broadcast <n> start` refuses by default, with the way out as a hint.

**A common ISO interval** (`iso=30`) puts them on one period. The controller sends several SDUs
per BIG event (BN = latency / SDU interval − 1, LE Create BIG), so the tool sets the latency that
gives 4 SDUs of 7.5 ms or 3 SDUs of 10 ms per 30 ms event. Both BIGs then repeat every 30 ms and
never meet: measured without loss, and `radio schedule` shows both at 30 ms (test). The cost is
latency, about 20 ms more. `show` reports `bn` and `iso_interval_us`. The airtime, budget and
controller checks count BN × (RTN+1) subevents per ISO interval. The controller has to hold BN
SDUs per BIS (one ISO TX buffer each, 17 in total), so `start` refuses a combination that needs
more. `--force` still starts BIGs of different ISO intervals side by side, as a deliberate
loss-tolerance test.

Example: a bilingual broadcast in which one BIS has a different sampling rate and frame size.

```
broadcast 1 create name=Conference
broadcast 1 subgroup add preset=24_2 lang=pol          -> sg=1
broadcast 1 subgroup add preset=16_2 lang=eng ctx=live -> sg=2
broadcast 1 bis add subgroup=1 location=left freq=500
broadcast 1 bis add subgroup=1 location=right freq=1000 rate=48 octets=100   -> cfg=48_2 freq_actual=1000.0
broadcast 1 bis add subgroup=2 freq=1500
broadcast 1 show                                       -> ... sdu=100 airtime_us=.. fits=1
broadcast 1 start
```

### dut_sim (simulated device under test)

| Command | Description |
|---|---|
| `version`, `addr`, `status` | `status` gives `conns`, `bonds`, `max_bonds` (4), `overwrite_oldest`, `streams`, `bcast`. |
| `bonds`, `unpair all\|<addr>` | Bond list; "forget the phone". |
| `adv on\|off`, `disconnect` | Advertising; disconnect everything. |
| `io <cap>`, `passkey <n>`, `confirm yes\|no` | Pairing on the device side (`EVT dut_passkey`, `dut_passkey_request`, `dut_confirm`). |
| `conn param [interval=ms] [latency=n] [sto=ms]` | The device requests other link parameters. |
| `call accept\|end\|hold\|retrieve [idx=]`, `call list` | Call "buttons" (CCP client). |
| `media play\|pause\|next` | Media "buttons" (MCP client). |
| `bcast scan [name=..] [code=..] [bis=1,3]`, `bcast stop`, `bcast status` | Auracast reception; `bis=` selects BISes, like a device choosing a channel or language. Up to 8 BIS, every sampling rate and both frame durations. |
| `rssi` | RSSI of every link (`LINK peer= rssi=`). |

`dut_sim` also has HAS (3 presets), MICP with AICS and VOCS servers (`EVT dut_preset`, `dut_mic`,
`dut_mic_input`, `dut_balance`) and remembers bonds across restarts.

Key events:
- `dut_stream state=enabled|metadata|started|... dir= ctx=`,
- `dut_audio src=unicast|bcast_bis1|bcast_bis2 freq=<Hz> level=<dBFS, 0.1 dB> active=<%> lost=
  clip=` (`clip`: decoded samples at full scale),
- `dut_call idx= state=`,
- `dut_media state=`,
- `dut_volume volume= mute=`,
- `dut_bonded`, `dut_bond_deleted`,
- `dut_bcast_found ... pbp_encrypted= pbp_sq= pbp_hq= rssi=`,
- `dut_bcast_stream state=started bis= sg= sample_rate= octets= lang=`,
- `dut_bcast_error reason=code_required|...`,
- `dut_bcast_stopped`.

The device's bond list has 4 slots, and on overflow the oldest bond is dropped. A build with
`-DCONFIG_BT_KEYS_OVERWRITE_OLDEST=n` makes it reject new bonds instead. Re-pairing a device the
device already knows is refused (Zephyr's default behaviour).

### Example (a full scenario from a test)

```
> mode unicast
OK mode=unicast
> bonding create Phone
OK id=1 name=Phone addr=FB:3A:AA:FA:FB:43/random io=none
> bonding connect Phone bluetooth-name=DUT-LEA
OK id=1 state=connecting device=FC:79:2F:8A:62:3D/random device_name=DUT-LEA
EVT t=3125 bonding_connected id=1 peer=FC:79:2F:8A:62:3D/random bonded=0
> bonding pair Phone
EVT t=4038 bonding_security id=1 level=2
EVT t=4038 bonding_paired id=1 bonded=1
> le-source attach Phone
OK id=1 name=Phone
> le-source music start preset=48_2 freq=440
OK queued=1
EVT ... source_discovered id=1 sink=1 source=1 vcp=1
EVT ... source_stream_open id=1 ctx=media preset=48_2 bidir=0 tone=440
> le-source call incoming uri=tel:5551234 name=Mum
EVT ... source_call idx=1 state=incoming by=local
EVT ... source_stream_updated id=1 ctx=ringtone tone=800
     (device: dut call accept)
EVT ... source_call idx=1 state=active by=peer
EVT ... source_stream_open id=1 ctx=conversational preset=16_2 bidir=1 tone=300
```

## Tests (tests/)

| File | What it checks |
|---|---|
| `test_basic.py` | CLI, identities (slot reuse), broadcast lifecycle on the tool alone, unicast/broadcast modes (automatic entry, refusal, context cleared on switch). |
| `test_persistence.py` | Restart of both boards: the tool starts empty (nothing was in flash), the device keeps an orphaned bond; the old identity without keys is rejected, a new one bonds normally. |
| `test_privacy.py` | RPA on connection, resolution to the identity after bonding, a fresh RPA on every connection, two devices bonded with the same device are not confused. |
| `test_pairing.py` | Passkey (correct, wrong), numeric comparison (yes, rejected on the device), level 3 requested from a device without IO. |
| `test_connparams.py` | Parameters on connection, interval change, coded/2M PHY, data length, rejected and accepted parameter requests from the device. |
| `test_remotectl.py` | Preset list and change, microphone mute and gain, balance. |
| `test_bonding.py` | Bond; filling the list, evicting the oldest (`key_missing`) and reconnecting the newest without pairing; "the phone forgot" (re-pairing refused until `unpair` on the device); parallel connections; fast connect–pair–disconnect cycles. |
| `test_phone.py` | Music (48 kHz, 440 Hz at the device); a ringtone interrupting music, answering on the device, a bidirectional call and music resuming; rejecting on the device; an outgoing call and hold; pause/play from the device; an alert during music; volume and mute; volume after the device changed it and after a reconnection (patches 0010, 0011); music requested during an alert; state machine (transitions, refusing a disallowed request at once, deferred music). |
| `test_broadcast.py` | Stereo L/R and a live tone change; encryption (no code, wrong code, right code); 48 kHz HQ; name filter; builder: 2 subgroups with languages and 3 BISes with their own parameters (the device checks the tone, sampling rate, octets and language of every BIS), selecting one BIS; 2 broadcasts at once (plain and encrypted); airtime limit and the `rtn=1` workaround; the budget covers all BIGs; builder validation. |
| `test_lc3_table.py` | Each of the 16 BAP configurations (low and high tone and a live change, lossless); nearest tone selection, `exact=1`, ranges; level through `g_idx` (−2…−40 dBFS measured at the device ±1 dB); the highest level not clipped (8_1, 16_2, 48_2), 0 dBFS refused; a 7.5 ms BIG next to a 10 ms one (refusal, `--force`, warning). |
| `test_capacity.py` | 11 BISes at once (8 plain + 3 encrypted) lossless and refusing one more BIG; 3 plain/encrypted BIGs with different codes and rejecting another BIG's code (MIC); encryption cost in the budget; budget limit (8 + 4 and 3 + 3 + 3 rejected); (slow) every configuration at the budget limit lossless at a receiver holding the PA. |
| `test_proto.py` | Firmware keys = specification, unknown keys refused, JSON Lines (values with quotes and non-ASCII characters), `human` mode, `limits`/`id`/`time`, event filter, help, hints, a single shell error. |
| `test_clients.py` | Python library over pty and TCP (device, pairing, errors, events, a broadcast heard by the device); Java library: `./gradlew test` with a test against the simulation over TCP (skipped without a JDK). |
| `test_txpower.py` | `bonding txpower` lowers the RSSI at the device by ~20 dB; `broadcast <n> txpower` (before start and live), broadcast RSSI at the device. |
| `test_output_limits.py` | The longest real lines fit without truncation; a line that is too long is cut at a word boundary and marked (`truncated=1`, in JSON `"truncated":true`, always valid JSON); counters in `cli stats`. |
| `test_config.py` | Consistency between the application and the controller image (separate builds, so `BUILD_ASSERT` is not enough): broadcast limits, ISO buffers, SDU size. No simulation. |
| `test_fuzz.py` (slow) | CLI fuzzing from the `protocol/leabench.yaml` grammar: exactly one reply per line, every JSON line parses, no assertions and no `fatal`, a consistent `status` at the end and zero truncated lines. |
| `test_soak.py` (slow) | Soak of 2 BIGs (9 BIS) with tone changes every second (zero loss, steady `sent` rate); 200× broadcast start/stop and 100× connect–pair–disconnect–delete without leaks; event storms with exact loss accounting. |
| `test_ux.py` | Acceptance criteria from `docs/broadcast-ux.md`: identities by name, connecting by Bluetooth name (also two devices with the same name), the unicast session from the document, layered `status`, `stop`, errors with hints, `reset --yes`; two independent BIGs without a selection command, BIG states, changing and deleting subgroups / BISes, `--all` with a result per BIG (counts, every failure named), `human` views, empty lists, status and events as sentences; hints for a swapped word order, `help` without the BIG number, authenticated pairing without IO, unique default names. |

## On hardware (nRF5340 DK)

> **Built, but not yet run on a board.** The DK build passes (Zephyr SDK 1.0.1,
> `west sdk install -t arm-zephyr-eabi`); the first run is waiting for a board.

Memory usage (`scripts/build.sh dk`):

| Core | Flash | RAM |
|---|---|---|
| app (`leabench`) | 585 KB / 1 MB (57%), including 257 KB of frame table | 114 KB / 448 KB (25%) |
| net (`hci_ipc` controller) | 164 KB / 256 KB (64%) | 49 KB / 64 KB (76%) |

Hardware optimizations:
- **Zero flash writes:** no flash driver, settings or NVS in the image (`nm` shows no
  `nvs_`/`settings_save`/`flash_write`). Automated test loops cannot wear out the flash.
- **No LC3 encoder and no FPU:** −50 KB RAM (encoder memory) and a constant, minimal CPU cost
  per SDU.
- **Own controller** (`leabench/netcore.conf` instead of the sample configuration with every
  role): no peripheral, BIG reception, PA sync, PAST, DTM or large scan buffers. Net core RAM went
  from 98% to 92% with 3 BIGs / 16 BIS instead of 2 / 6, then to 76% (table below).
- **App core at 128 MHz** (`src/board.c`; the MDK boots at 64 MHz).
- **CLI over UARTE with DMA at 1 Mbaud** (J-Link VCOM): an `EVT` line takes about 1 ms instead
  of 9 ms.
- BIG encryption: the radio's hardware CCM; upstream Zephyr uses the CryptoCell-312 only as an
  entropy source (accelerating ECDH for mass pairing would require NCS).

Net core RAM went from 59 KB (92%) to 49 KB (76%) without giving up a limit
(`scripts/mem_report.sh` variants, each one run through the whole test suite):

| Change in `leabench/netcore.conf` | RAM saved | Why it is enough |
|---|---|---|
| advertising / AUX / periodic sets 4 → 3 | 2.4 KB | one set per BIG; a BIG's set is deleted before it starts again |
| ISO TX buffers 20 → 17 (+ host count on both cores) | 1.8 KB | 16 BIS with 1 SDU in flight each + spare; `audio_tx.c` gives 2 in flight only while they fit |
| HCI event buffers 8 → 6 | 1.1 KB | also sizes the HCI command pool of `hci_ipc` |
| heap 2 KB → 1 KB | 1.0 KB | only the IPC virtqueues (boot) and AES-CMAC of encrypted BIGs allocate |
| controller RX buffers 4 → 3, advertising data buffers 3 → 2 | 0.6 KB | one connection-heavy and one update per BIG at a time |

Price list for raising a limit again: about 0.8 KB per BIG (its three advertising sets), 0.6 KB
per ISO TX buffer (one per BIS), 0.6 KB per HCI event buffer. Coded PHY stays (0.4 KB) because
`bonding phy` offers it. Every increase of the limits in `leabench/netcore.conf` must be checked
with a DK build (the simulation does not check net core RAM).

1. Zephyr SDK (ARM toolchain), `nrfutil` / J-Link.
2. `scripts/build.sh dk`. Builds `leabench` for `nrf5340dk/nrf5340/cpuapp` together with the
   controller image (`hci_ipc`) for the net core.
3. `west flash -d build/leabench_dk`.
4. CLI on the DK's first VCOM (**1000000** baud), logs over RTT.
5. Single commands: `scripts/cli.py --port /dev/ttyACM0 version "bonding list"`.
   Tests of the tool alone on the board: `scripts/test.sh --hw-port /dev/ttyACM0` (tests that
   need the simulated device are skipped).

Notes:
- **The same code** runs in simulation and on the board. In simulation the controller is the
  open-source Zephyr LL, and on the board too (upstream Zephyr). Moving to NCS with the
  SoftDevice Controller is possible (same host API), but then the patch has to be applied to
  `sdk-zephyr`.
- Limits: 3 simultaneous connections (controller), 8 virtual devices, 1 unicast stream
  (+ microphone), 3 BIGs with 8 BIS and 4 subgroups each, 16 BIS in total in memory; in practice
  the airtime budget (Auracast section).
- The simulation does not model range or interference; the airtime budget is worth confirming on
  the board (same controller code, so it should carry over).
- Identities, IRKs, bonds and the names and IO of virtual devices are **RAM only**. After a board
  reset the list is empty and the device has orphaned bonds (the realistic "phone restored
  to factory settings" case).
- Privacy (RPA) is global. Fixed addresses (e.g. for a sniffer): build with
  `-DCONFIG_BT_PRIVACY=n`. Legacy-only pairing is just a build option in Zephyr
  (`CONFIG_BT_SMP_LEGACY_PAIR_ONLY`), with no CLI switch.

## Found in simulation (worth knowing with real devices)

- Stopping and restarting a running advertising set right after a disconnection hung the
  simulated controller. The workaround is in `dut_sim`.
- Zephyr's MCC client handles one connection at a time. Parallel CCP/MCP discovery on several
  links ends with `-ENOMEM`, so `dut_sim` does them sequentially.
- An incoming call that started before the CCP subscription has to be read after discovery. A
  good test case for real devices.
- **A sink's own advertising vs. PA sync:** in the Zephyr controller the AUX event of a sink's own
  connectable advertising preempts the receive window of a periodic advertising sync being
  established. With BAP default intervals (advertising 20–30 ms, PA 60 ms) the phase can stay
  bad and the sync fails every time (`0x3e`). Worth checking on real devices that
  advertise while looking for a broadcast.
- **Several subgroups in the BASE:** a broadcast sink in Zephyr's default configuration
  (1 subgroup) **silently ignores** a BASE with more subgroups: BIGInfo arrives, the BASE does
  not. Worth checking how real devices react to a multilingual broadcast.
- **CIS next to a broadcast in `leabench`** (Zephyr LL controller, measured on radio dumps with
  `scripts/airtime.py`):
  - The BIG itself does not collide with the CIS. The CIS loses events when the **slot
    reservation** of another periodic activity overlaps the CIS reservation. The controller
    schedules by worst-case reservations, not by actual on-air time, and the CIS loses.
  - The culprits are the broadcast's periodic advertising (carries the BASE and BIGInfo, every
    60 ms by default) and ACL events of the same link (every 50–70 ms). Each collision costs
    about 1 CIS event.
  - Model: `loss ≈ Σ (ISO interval / activity period)` over activities whose phase hits the CIS.
    Whether it hits depends on where the controller placed them. The worst case with default
    settings is 10/60 + 10/70 ≈ 31%; the best is 0%.

    | Measurement (3 BIS 24_2, order ACL → BIG → CIS) | CIS loss | from PA | from ACL |
    |---|---|---|---|
    | `pa=60` (BAP default) | 29% | 14% | 14% |
    | `pa=150` | 20% | 7% | 13% |
    | `pa=155` (not a multiple of 10 ms, the phase drifts) | 17% | 3% | 14% |
    | `pa=1000` | 15% | 1% | 14% |
    | 1 BIS, `pa=60` | 17% | 17% | 0% |

    The number of BISes does not decide; the phase does. An earlier conclusion that the loss grows
    with the number of BISes was wrong.
  - A BIG started during a call collides even more, and with a running 3-BIS BIG `leabench` does
    not establish new connections (the initiator gets no slots for the device's AUX
    advertising channel).
  - **That is why the tool works in modes** (one function at a time). The measurements stay as
    the rationale and a reference point, should functions ever need to be combined, e.g. on SDC.
  - On a board with upstream Zephyr the same controller code runs, so these results should carry
    over. With the SoftDevice Controller (NCS) the scheduler is different and this has to be
    measured again.
- **Receiving CIS and BIG at once** does not work in `dut_sim` either (controller). Real
  devices switch between sources anyway.
- **Several BIGs at once** (Zephyr LL controller): two controller bugs (patches 0002, 0004) and
  the start order. `leabench` started a BIG before its periodic advertising, so one broadcast's PA
  landed on another BIG's events and took one event per PA interval from it (60 ms: 17% loss).
  Now the PA starts first, and the new BIG is placed after it.
- **With 16 streams** the host kept 2 SDUs in flight per stream, more than the controller has
  buffers for. With many streams there is now 1 SDU in flight per stream.

## Test campaign (November 2026)

Besides the functional tests, the code went through tools that look for what scenario tests
cannot see. Every defect found has a regression test and its own fix.

| Tool | How to run | Result |
|---|---|---|
| ASan + UBSan in simulation | `scripts/build.sh sim --sanitize`, `LEA_VARIANT=asan scripts/test.sh -m "slow or not slow"` | 130 tests (including the long ones): zero ASan/UBSan reports, all pass |
| Static analysis | `scripts/analyze.sh` (`gcc -fanalyzer`, `clang-tidy`, `.clang-tidy`) | 36 files, clean |
| CLI fuzzing | `scripts/test.sh -m slow test_fuzz.py` (`LEA_FUZZ_SEED`, `LEA_FUZZ_LINES`) | 3 seeds × 2500 lines (text and JSON alternating) + a run on ASan: no invariant violations |
| Specification conformance | automatically in every run (`tests/out/conformance.txt`) | every OK reply and every `leabench` event matches `protocol/leabench.yaml` |
| Coverage | `scripts/build.sh sim --coverage`, `LEA_VARIANT=cov scripts/test.sh -m "slow or not slow"`, `gcovr` → `build/coverage/` | `leabench` + `common`: 90.1% lines, 97.3% functions, 74.6% branches; 131 of 131 tests pass |
| Soak, cycles, event storm | `scripts/test.sh -m slow test_soak.py` (`LEA_SOAK_S`, `LEA_CYCLES`) | 300 s soak (9 BIS, a tone every second): zero loss; 200 + 100 cycles without leaks; storms of 16–1000 events counted exactly |
| Libraries without a board | `pytest clients/python/tests`, `./gradlew test` (in `scripts/lint.sh`) | 19 + 16 tests against a fake board |

Found and fixed:

| Defect | Found by | Effect |
|---|---|---|
| The controller placed a new BIG on a running one (patch 0006) | ASan changed the timing and hit the bad phase | both BIGs lost 50% of frames, depending on the start moment (also on the DK) |
| The controller placed a PA train and AUX set on a running BIG, and a receiver holding the PA could not keep up (patch 0006, extended) | final ASan run: `test_started_big_never_overlaps_running_one` and `test_most_bises`; radio dumps (`scripts/airtime.py`) | a BIG lost 8–17% of frames or the broadcast could not be found, depending on the start moment; the old budget only masked it |
| The simulated device's own advertising preempted its PA sync (`dut_sim`) | a flaky `test_broadcast_txpower` (1 failure in the coverage run, PA sync retries in 2 of 10 runs); radio dumps: the sink synced to every PA packet and aborted it 45 µs later, each time its own AUX_ADV_IND followed 846 µs after the PA | its advertising interval (30 ms) divides the PA interval (60 ms), so on an unlucky start the phase never moved and the sync failed (`0x3e`); test double only: it now pauses advertising while establishing the sync, 0 retries in 30 runs |
| The simulated device leaked its broadcast sink on `bcast stop` (`dut_sim`) | a warning while debugging the above (`Failed to add sync as Receive State: -12`) | stopping a synced BIG is asynchronous, so the immediate delete failed and the BASS receive state stayed taken; now deleted after the `stopped` callback |
| Ringtone / beep pattern after 71 minutes of streaming | review | `frame_count * frame_us` overflowed 32 bits and the on/off timing jumped |
| 48_6 (155 B SDUs) went out as 2 PDUs per SDU | `scripts/capacity.py accept`: 48_6 stopped at 4 BISes | the controller's BIS PDU is limited by `ISO_TX_BUFFER_SIZE` − 8 B timestamp header = 152 B, so every SDU was split (BN=2): twice the air time. Buffers are now 163 B |
| The controller silently lowered RTN; past its event limit it refused the BIG, which the budget did not know | acceptance matrix: BIGs "accepted" with 12–33 ms of air in a 10 ms interval; `ERR -22` for 48_6 with 7 BIS the budget allowed | the tool now models the controller's limit, refuses with the largest RTN that fits, and reads the real layout back (`broadcast_warning reason=layout`) |
| Two refused BIGs left broadcast unusable until reset (patch 0012) | acceptance matrix: after two refusals every BIG got `ERR -5` (Memory Capacity Exceeded) | BIS streams of a refused LE Create BIG were never returned |
| The simulated device sometimes never synchronized to a PA (`dut_sim`) | capacity matrix: one 7.5 ms encrypted BIG "not found" every run | it stopped scanning before the PA sync was established; it now scans until then |
| `broadcast budget` promised one BIS more than `start` accepted | capacity matrix | its probe BIG had no name or PBP, so the controller limit was computed with too little advertising data |
| Stack overflows on the board (first run on hardware) | the board's MPU guard; `scripts/stack_report.py` (the simulation runs threads on host stacks and could not show it) | `cli_evt` formatted each event into ~700 B of buffers on the caller's stack, deep inside Bluetooth callbacks of the 2 KB RX thread; the event thread had 1.8 KB of 2 KB in direct calls; the net core's HCI command thread (LE Create BIG with the phase search of patch 0006) and main thread had little headroom. Event buffers are static now (thread paths use 44 B), and the BT RX, event, HCI command and net core main / work queue stacks were raised |
| Reconnecting the same identity failed with `ERR -22 connect failed` until reboot (patch 0010) | UX walk on the simulation (`mode broadcast`, `reset`, then the same flow again); narrowed down to any disconnection after volume control, visible when the identity id is reused | a tester could not reconnect a device after a session with volume control |
| Volume control failed after any reconnection (patch 0011) | the new reconnection test; `source_error op=vol_set err=128` (Invalid Change Counter) once VCP write errors were reported at all | `le-source volume` silently did nothing on devices whose volume had changed; VCP write errors are now reported as `source_error` |
| A music start during an alert was dropped | code review | `le-source music start` while an alert played from idle answered `deferred`, then never played |
| The unicast group and the broadcast source were forgotten when their delete failed | code review | after a slow stop (link loss, busy controller) every later stream or BIG start failed until reboot |
| The BIG start timeout and shell commands ran `big_stop()` at the same time | code review | a double delete of the same source and advertising set when a timeout met `broadcast stop` / `delete` / `reset` |
| A call dialled by the device during an alert or another call stayed alerting | code review | GTBS created it, the phone refused it, nobody ended it |
| `bonding pair sec=3/4` with `io=none` said only "set security failed" | UX walk | now: authenticated pairing needs an IO capability, with the commands to run |
| `broadcast stop --all` counted inactive BIGs as stopped; a partial failure named only the first failed BIG | UX walk | `stopped=3` for one running BIG; "BIG 2 failed" when 2 and 3 failed |
| Two shell errors for one line (patch 0009) | fuzzer after the CLI change (`"bcast " * 60` stopped being a known command) | client replies shifted by one command |
| `le-source status` printed `preset=(null)` when idle | fuzzer | a value outside the specification |
| `next_big_bis` ignored the limit of 3 BIGs and 16 BIS | the new budget (with 3 BIGs airtime was left over) | `broadcast budget` promised BISes that could not be started |
| The device (`dut_sim`) reported CCP discovery before it stored the server | final run (`test_call_from_device`, 1 in 3) | `call dial` right after `dut_ccp_discovered` gave `ERR -107`; test double only |
| Deleting the newest device blocked creating new ones | connection cycles | after one deletion no device could be created |
| Device PHY command: a pointer to a compound literal past its scope | ASan | undefined behaviour (worked by accident on the DK) |
| The shell silently dropped non-ASCII characters (patch 0007) | fuzzer | `name=Zażółć` gave "Za"; a line of only such characters got no reply |
| The clients did not know 2 shell error messages | fuzzer | waiting until timeout on an unpaired `"` and on more than 16 words |
| A duplicated key silently took the first value | fuzzer (reply review) | `preset=16_2 preset=48_2` → 16_2 |
| A device data line had `vdev_selected` instead of `selected` | specification conformance | refactoring regression; typed libraries lost the field |
| Specification: wrong types (`proto`, `id`), missing conditional fields | specification conformance | the Java client with an `int` type would have crashed on `proto=text` |
| Bad arguments switched the board into a mode and returned EBUSY/ENOTCONN | regression test after typing `phone_msg` | a failing command changed the state |
| `uri`/`name` longer than the field silently truncated | review while typing `phone_msg` | a different number than given |
| Output line truncation and unterminated JSON | review (reported) | now `truncated=1`, `dropped_fields`, `cli stats` |
| `memcpy(NULL, 0)` in Zephyr's PBP (patch 0005) and in `dut_sim` | UBSan | undefined behaviour |
| Reply buffers in both libraries grew without bound | review during unit testing | memory grew throughout a long run |
| `snprintf` in the JSON writer without a result check | `clang-tidy` | an out-of-bounds write on a formatting error |

Final result: 3 × the full suite (120 tests) green, ASan with the long tests (130) green,
coverage with the long tests (131) green, `scripts/analyze.sh` and `scripts/lint.sh` (with the
Java client) clean, DK build without warnings: application 584 KB flash / 114 KB static RAM, net
core 164 KB / 59 KB. The former flaky `test_broadcast_txpower` (the simulated device's own
advertising, see the table) passed in every run.

Optimization: the event queue was the largest object in RAM (11.9 KB: 32 slots sized for the
longest line). A ring buffer with variable-length records takes 4 KB and holds more typical
events. Thread stacks were not shrunk blindly: in BabbleSim threads run on host stacks, so real
usage has to be measured on the DK (`scripts/build.sh dk --diag`, then `kernel thread stacks`
after a full scenario).

**Capacity (resolved):** the old budget was calibrated empirically and did not guarantee zero
loss. The 11 BISes of `test_most_bises` lost 34% in ASan and coverage. Radio dumps showed that the
interval had enough room, and the losses came from how the controller placed the roles: PA and
AUX landed on BIG events, and a PA right next to a BIG disturbed a receiver holding the PA. After
extending patch 0006 the budget is based on the controller's reservations (section "How many
BIGs and BISes at once"). Result: 11 BIS on two BIGs and 8 on three, lossless in every build
variant.

## Next steps

- First run on the DK and with real devices; confirming the airtime budget on a real radio.
- A denser frame table grid on the external QSPI flash (8 MB on the DK, written only when
  programming).
- Submitting the Zephyr patches upstream.
- Broadcast Assistant (BASS): the DK tells the device to join an Auracast.
- ASHA (Android) as a second phone simulation variant.
- From the architecture review, deferred:
  - a request id (`req=`) in replies (today replies come strictly in order, so the libraries
    correlate them by order),
  - `bonding delete` in the background instead of blocking the shell thread.
- `dut_sim` on the same JSON protocol (today the tests talk to it in text through simlib).

## License

The project code (the `leabench` firmware, `dut_sim`, `common`, the client libraries, tools,
scripts and tests) is licensed under the [Mozilla Public License 2.0](LICENSE).

Exceptions: the Zephyr patches in `zephyr/patches/` modify Zephyr code and stay under its
Apache-2.0 license, as do files taken from other projects (`.clang-format` from Zephyr,
`clients/java/gradlew*` and the Gradle wrapper).
