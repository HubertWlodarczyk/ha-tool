# UX design of the Bluetooth test tool

## Quick start

The tool has two radio modes:

- `unicast` — `bonding` and `le-source` working together,
- `broadcast` — Auracast transmissions; cannot run in parallel with unicast.

Three rules are enough to get started:

1. `connect` establishes a connection, and `pair` starts pairing.
2. `bonding` and `le-source` work together in `unicast` mode.
3. `broadcast` needs its own radio mode, and changing the mode clears the
   previous context.

Minimal unicast flow:

```text
mode unicast
bonding create Tablet
bonding connect Tablet address=AA:BB:CC:DD:EE:FF/random
bonding pair Tablet sec=4
le-source attach Tablet
le-source music start freq=440
le-source volume up
le-source music stop
```

Minimal Auracast flow:

```text
mode broadcast
broadcast 1 create name="Test"
broadcast 1 subgroup add preset=48_2
broadcast 1 bis add subgroup=1 location=mono
broadcast 1 start
```

## Purpose of this document

This document describes the target CLI of the Bluetooth LE Audio test tool.
The tool supports three main test goals:

1. creating and running several broadcasts with different configurations,
2. simulating an LE Audio source (`le-source`), for example a phone,
3. simulating many virtual devices to study bonds, their limits,
   reconnections, key removal and the receiver's behaviour with many
   identities.

The interface is meant for a Bluetooth tester who knows the concepts of BIG,
subgroup and BIS, but needs a readable, predictable syntax to build test
variants quickly.

The design does not hide the Bluetooth model behind a wizard or a menu. It
mainly simplifies:

- referring to a specific BIG,
- managing the resource hierarchy,
- reading configuration and state,
- operations on several BIGs,
- telling a working configuration apart from an active transmission.

## Test scope and radio modes

The two radio modes match the physical capabilities of the module:

| Radio mode | Work areas | Main resource |
|---|---|---|
| `unicast` | `bonding`, `le-source` | identities, connections and unicast sessions |
| `broadcast` | Auracast | BIG, subgroup, BIS |

`bonding` and `le-source` are work areas within `unicast` mode: `bonding`
prepares the identity and the connection, and `le-source` uses an explicitly
attached identity to generate source traffic and to send control to the
connected device.

`broadcast` (Auracast) is mutually exclusive with `unicast`. The radio module
has a physical limitation: it cannot handle an Auracast transmission and
unicast work at the same time. This follows from the limited radio time and
computing power of the module, so the interface must not promise these kinds
of work running in parallel.

`mode` only selects the radio mode:

```text
mode unicast
mode broadcast
```

In `unicast` mode, `bonding` and `le-source` commands can be run without
switching modes. `le-source` must have an identity explicitly attached through
`le-source attach`, and managing identities stays the domain of `bonding`.
Switching between `unicast` and `broadcast` always stops activity and clears
the whole context of the previous mode. A prior `stop` is not needed.

In `broadcast` mode several BIGs can be managed at once. The limitation is
about the kind of radio work, not the number of BIG configurations.

`mode unicast` and `mode broadcast` are explicit. Switching does not preserve
data: afterwards, the previous identities, bonds, connections, BIG
configurations, subgroups, BISes and the rest of the working state no longer
exist.

Modes do not describe the physical type of a device. `Phone`, `Tablet` or
`TV` can be test names of virtual devices, but the Bluetooth behaviour is
decided by the mode and the implemented service profile.

### Case: different broadcast configurations

The tester should be able to prepare several BIGs independently, for example:

```text
mode broadcast
broadcast 1 create name="Music" phy=2m interval=10 iso=30
broadcast 1 subgroup add preset=48_2 lang=pol ctx=media program="Music"
broadcast 1 bis add subgroup=1 location=left freq=440
broadcast 1 bis add subgroup=1 location=right freq=880

broadcast 2 create name="Announcements" phy=1m interval=7.5 iso=30
broadcast 2 subgroup add preset=16_1 lang=pol ctx=instructional program="Announcements"
broadcast 2 bis add subgroup=1 location=mono freq=1000

broadcast 1 start
broadcast 2 start
```

7.5 ms and 10 ms BIGs would meet every few events; `iso=30` gives both a 30 ms ISO interval (4
and 3 SDUs per event), so they never do, at about 20 ms more latency.

It matters that the user can set different BIG, subgroup and BIS parameters
and start them independently. `broadcast start --all` is only a bulk
operation, not the required way of working.

### Case: simulating an LE Audio source

The tester prepares an identity in `unicast` mode. The connection is part of
the unicast session, and the source must explicitly attach the chosen
identity:

```text
bonding create Tablet
bonding connect Tablet address=AA:BB:CC:DD:EE:FF/random
bonding pair Tablet sec=4
le-source attach Tablet
le-source music start freq=440
le-source call incoming name="Test"
le-source call accept
```

`le-source` does not mean a physical device type. `Tablet` can be the name of
a test identity, but the same role can represent a phone, a TV, a computer or
another LE Audio source. The remote device can be a hearing aid, earbuds, a
speaker or another LE Audio receiver.

### Case: many virtual devices and bonds

The tester performs these operations in `unicast` mode, using the `bonding`
area:

```text
mode unicast
bonding create Phone-1
bonding create Phone-2
bonding create Tablet-1
bonding create TV-1

bonding connect Phone-1 address=AA:BB:CC:DD:EE:FF/random
bonding pair Phone-1 sec=4
bonding disconnect Phone-1

bonding connect Phone-2 address=AA:BB:CC:DD:EE:FF/random
bonding pair Phone-2 sec=4
bonding disconnect Phone-2

bonding list
bonding bonds Phone-1
bonding bonds Phone-2
```

This makes it possible to study, among other things:

- the bond limit on the receiver side,
- the order in which the oldest bonds are evicted,
- reconnecting a known identity,
- a missing key on one of the devices,
- a local `unpair` without removing the bond on the connected device,
- behaviour after deleting and recreating an identity,
- differences between several local addresses and IO capabilities.

`bonding` does not start `le-source` functions automatically, but both areas
can work together in `unicast` mode. To change the identity used by the
source, the tester runs `le-source detach`, changes the state in `bonding`,
and then uses `le-source attach` for the new identity.

## Assumptions

### Audience

The main audience is a Bluetooth/LE Audio tester. We assume the user:

- understands the difference between BIG, subgroup and BIS,
- needs to set radio and audio parameters deliberately,
- often compares several transmissions,
- may run the same operations by hand and from a script.

We do not design the interface for a beginner consumer user.

### CLI first

The primary interface is short, imperative commands. We do not use:

- a main menu,
- mandatory interactive choices,
- a hidden "active BIG" as a precondition of a command,
- automatic switching of the active transmission without a message.

Every command should be possible to copy into a script and repeat without
depending on earlier terminal history.

### One public CLI

We do not maintain separate simple and expert modes. The public, canonical
interface is these commands:

```text
bonding ...
le-source ...
broadcast ...
status
mode
reset
```

The same syntax serves manual work, scripts and the Python/Java clients.
Advanced use is controlled through optional parameters, not through a second
command system:

```text
broadcast 1 create name="Test"
broadcast 1 create name="Test" phy=2m interval=10 rtn=3 latency=40 pd=40000
```

Every command has one implementation, one validation and one state model:

```text
public CLI
      │
      ├── bonding ...
      ├── le-source ...
      └── broadcast ...
              │
              └── shared handlers, model and validation
```

The short form and the form with full parameters must not lead to separate
functional paths. They differ only in how many arguments the user gave.

The former names `dev`, `phone` and `bcast` were implementation names of the
earlier firmware. The migration moves their commands into one target
specification in [protocol/hatool.yaml](../protocol/hatool.yaml). We do not
create two independent handlers or two validation models.

## Resource model

### Hierarchy

```text
BIG
├── shared transmission parameters
├── Subgroup
│   ├── program parameters and base codec configuration
│   └── BIS
│       └── parameters of a specific audio stream
└── ...
```

### BIG

A BIG is a separate broadcast transmission. It has its own:

- number in the tool's configuration,
- state,
- Broadcast ID,
- name,
- security settings,
- radio and timing parameters,
- set of subgroups and BISes.

The BIG number is explicit in commands:

```text
broadcast 1 show
broadcast 2 start
broadcast 3 bis list
```

A prior `broadcast use 1` must not be required. An explicit number reduces the
risk of modifying the wrong transmission and makes automation easier.

### Subgroup

A subgroup describes one program or program variant. It holds the parameters
shared by its BISes:

- `preset` — base LC3/BAP configuration,
- `lang` — ISO 639-3 language,
- `ctx` — audio context,
- `program` — program name or metadata description.

Example:

```text
Subgroup 1:
  preset=48_2
  lang=pol
  ctx=media
  program="Stereo music"
```

The subgroup number is local to the BIG.

### BIS

A BIS is a single audio stream. It holds:

- the subgroup number `sg`,
- the channel location `loc`,
- an optional sampling rate override `rate`,
- an optional octet count override `octets`,
- the test tone `freq`,
- the exact tone requirement `exact`,
- the level `level`,
- the tone pattern `pattern`.

The BIS number is also local to the BIG:

```text
BIG 1: BIS 1, BIS 2
BIG 2: BIS 1, BIS 2, BIS 3
```

## BIG states

Every BIG has an explicit state:

| State | Meaning |
|---|---|
| `draft` | working configuration, not transmitting |
| `ready` | the configuration is complete and can be started |
| `active` | the BIG is transmitting |
| `stopped` | transmission stopped, configuration kept in the current mode |
| `error` | the last start attempt failed |

Structural changes to the BIG, its subgroups and BISes are allowed in `draft`
and `stopped`. During `active`, commands that change the configuration should
return an error pointing at `broadcast <big> stop`, or explicitly require
`restart`.

Changing a BIS tone can remain an operation done during transmission, if the
firmware supports it. The interface should clearly distinguish such a change
from a configuration change that requires recreating the BIG.

## Command syntax

### List and state

```text
broadcast list
broadcast <big> show
broadcast <big> subgroup list
broadcast <big> bis list
broadcast budget
```

`broadcast list` shows all BIGs:

```text
BIG  NAME              STATE    SUBGROUPS  BIS  ID
1    "Concert"         active   2          4    0x123456
2    "Announcements"   draft    1          2    0x234567
3    "Emergency"       stopped  1          1    0x345678
```

`broadcast <big> show` shows the configuration of one BIG, and the subgroup
and BIS lists show the details in separate tables. The default view should be
human-readable; the machine format stays available through the existing JSON
protocol.

### Creating and deleting a BIG

```text
broadcast 1 create name="Concert"
broadcast 2 create name="Announcements" id=0x234567 phy=1m
broadcast 1 set phy=2m interval=10 rtn=2 latency=40 pd=40000 pa=200
broadcast 1 txpower -8
broadcast 1 delete
```

BIG parameters:

| Parameter | Meaning |
|---|---|
| `name` | transmission name |
| `code` | Broadcast Code; enables encryption |
| `pbp` | `sq`, `hq` or `off` |
| `id` | Broadcast ID |
| `phy` | `1m` or `2m` |
| `interval` | SDU/BIG interval in ms: `7.5` or `10` |
| `rtn` | number of retransmissions |
| `latency` | maximum transport latency in ms |
| `iso` | common ISO interval in ms (`15`, `20`, `30`, `40`; `0` off): several SDUs per BIG event, so BIGs of 7.5 and 10 ms frames can run side by side (`iso=30` on both); sets the latency |
| `pd` | presentation delay in µs |
| `pa` | periodic advertising interval in ms (8–10000, 1.25 ms step) |
| `packing` | `seq` or `int` |
| `framing` | `unframed` or `framed` |

`create` makes a draft. It does not start transmitting.

### Managing subgroups

```text
broadcast 1 subgroup add \
  preset=48_2 lang=pol ctx=media program="Stereo music"

broadcast 1 subgroup set 1 lang=eng program="English commentary"  # also on an active BIG (metadata)
broadcast 1 subgroup delete 2
broadcast 1 subgroup list
```

Subgroup parameters:

| Parameter | Meaning |
|---|---|
| `preset` | base LC3/BAP preset, e.g. `48_2` |
| `lang` | ISO 639-3 code, e.g. `pol` |
| `ctx` | `media`, `live`, `conversational`, `instructional`, `sound_effects`, `notifications`, `alerts`, `emergency`, `unspecified` |
| `program` | program name or metadata |

### Managing BISes

```text
broadcast 1 bis add \
  subgroup=1 location=left rate=48 octets=100 \
  freq=440 level=-10 pattern=cont

broadcast 1 bis set 1 freq=1000 level=-6
broadcast 1 bis delete 2
broadcast 1 bis list
```

BIS parameters:

| Parameter | Meaning |
|---|---|
| `subgroup` | number of an existing subgroup |
| `location` | `mono`, `left`, `right`, `front_center`, `back_left`, `back_right`, `side_left`, `side_right` |
| `rate` | `8`, `16`, `24`, `32`, `44.1` or `48` kHz |
| `octets` | number of LC3 data octets |
| `freq` | tone frequency in Hz; `0` means silence |
| `exact` | reject the configuration if the exact tone does not exist |
| `level` | level from `-40` to `-2` dBFS (0 dBFS would clip in the decoder at low bitrates) |
| `pattern` | `cont`, `ring` or `beep` |

The names `subgroup` and `location` are recommended in the user-facing layer.
A compatibility layer may accept the existing short forms `sg` and `loc`.

If `freq` is not exactly available in the frame table, the reply should show
both values:

```text
Requested frequency: 440 Hz
Actual frequency: 450 Hz
```

The `rate` and `octets` parameters are constrained by the subgroup's preset
and the available LC3 configurations. The error should point at the
unsupported combination, not just return a generic argument error.

### BIS tone

Changing the tone is a separate, short operation:

```text
broadcast 1 bis tone 1 freq=440
broadcast 1 bis tone 2 freq=880 level=-12 pattern=beep
```

If the firmware allows changing the tone while transmitting, the command can
work on an `active` BIG. The reply always gives the actual value after fitting
it to the frame table.

### Starting and stopping

```text
broadcast 1 start
broadcast 1 stop
broadcast 1 restart
```

Bulk operations:

```text
broadcast start --all
broadcast stop --all
broadcast restart --all
```

Starting a single BIG is the basic operation. `--all` is a shorthand for an
explicit operation on every configured BIG, not a mechanism for selecting an
active BIG.

## Radio budget

Since all BIGs share airtime, `broadcast budget` is a separate diagnostic
command:

```text
Budget:
  BIG 1 "Concert":        38%
  BIG 2 "Announcements":  24%
  Total:                 62%
  Available:             38%
```

If `start --all` cannot start one of the BIGs, the result should identify the
specific conflict:

```text
BIG 1 "Concert".......... started
BIG 2 "Announcements".... started
BIG 3 "Emergency"........ failed

Cannot start BIG 3:
not enough air time.
```

The operation should not present a partial success as a full success.

## Example configurations

### One BIG, stereo

```text
broadcast 1 create name="Concert" phy=2m interval=10 rtn=2
broadcast 1 subgroup add preset=48_2 lang=pol ctx=media program="Stereo music"
broadcast 1 bis add subgroup=1 location=left rate=48 octets=100 freq=440 level=-10
broadcast 1 bis add subgroup=1 location=right rate=48 octets=100 freq=880 level=-10
broadcast 1 start
```

Model:

```text
BIG 1: Concert
└── Subgroup 1: Stereo music
    ├── BIS 1: left, 440 Hz
    └── BIS 2: right, 880 Hz
```

### One BIG, two programs

```text
broadcast 1 create name="Concert"
broadcast 1 subgroup add preset=48_2 lang=pol ctx=media program="Music"
broadcast 1 subgroup add preset=16_2 lang=eng ctx=conversational program="Commentary"
broadcast 1 bis add subgroup=1 location=left freq=440
broadcast 1 bis add subgroup=1 location=right freq=880
broadcast 1 bis add subgroup=2 location=mono freq=1000
broadcast 1 start
```

### Two independent BIGs

```text
broadcast 1 create name="Concert" id=0x123456 phy=2m
broadcast 1 subgroup add preset=48_2 lang=pol ctx=media program="Music"
broadcast 1 bis add subgroup=1 location=left freq=440
broadcast 1 bis add subgroup=1 location=right freq=880

broadcast 2 create name="Announcements" id=0x234567 phy=1m
broadcast 2 subgroup add preset=16_2 lang=pol ctx=instructional program="Announcements"
broadcast 2 bis add subgroup=1 location=mono freq=1000

broadcast 1 start
broadcast 2 start
```

## Errors and predictability

Every error should contain:

1. the cause,
2. the resource it concerns,
3. the next possible step,
4. a technical detail, if useful.

Example:

```text
Cannot add BIS to BIG 1:
subgroup 3 does not exist.

Existing subgroups: 1, 2
Try:
  broadcast 1 subgroup list
```

Trying to change an active BIG:

```text
Cannot change BIG 1 while it is active.

Stop it first:
  broadcast 1 stop
```

Trying to start an incomplete BIG:

```text
Cannot start BIG 2:
no BIS configured.

Add at least one BIS:
  broadcast 2 bis add subgroup=1 location=mono
```

## Migrating the earlier CLI

The earlier firmware interface:

```text
bcast new 1 ...
bcast subgroup add 1 preset=48_2 ...
bcast bis add 1 sg=1 loc=fl ...
bcast tone 1 bis=1 ...
bcast start 1
```

The target, only public interface:

```text
broadcast 1 create ...
broadcast 1 subgroup add preset=48_2 ...
broadcast 1 bis add subgroup=1 location=left ...
broadcast 1 bis tone 1 ...
broadcast 1 start
```

The migration changes naming and argument layout, but keeps the existing data
model:

- a BIG still contains subgroups and BISes,
- a BIS stays local to its BIG,
- the preset stays a property of the subgroup,
- audio parameters stay properties of the BIS,
- configuration validation stays shared,
- the JSON format stays the shared automation format.

After the migration, the documentation, generated clients and tests use only
`broadcast`. The old commands are not part of the target public CLI.

## UX of the work areas

The broadcast design should be part of one consistent CLI. The user has two
radio modes and three work areas:

1. `broadcast` — building and transmitting several BIGs,
2. `le-source` — simulating an LE Audio source, e.g. a phone, tablet or TV,
3. `bonding` — simulating many BLE identities, connections and bonds.

The internal `dev` is the name of the module implementing `bonding`, not an
additional user-facing area. The user should not have to learn separate rules
for every implementation detail:

- a resource is referred to explicitly if several instances of it can exist,
- a command that changes state returns an unambiguous confirmation,
- asynchronous operations show progress and the final result as an event,
- full parameters stay available in the same command.

### The `bonding` area: simulating bonds

This area is for testing many independent BLE identities against a receiver
or another device. Every identity has its own address, keys, connection and
bond. It is not yet a simulation of a specific product type.

The user-facing layer offers the `bonding` commands:

```text
bonding list
bonding create Phone
bonding show Phone
bonding connect Phone address=AA:BB:CC:DD:EE:FF/random
bonding pair Phone sec=4
bonding unpair Phone
bonding disconnect Phone
bonding delete Phone
```

`Phone` is only the name of a test identity. It does not mean that `bonding`
emulates a phone or that this identity has phone services.

The internal `dev ...` commands are rewritten to `bonding ...` in the
migration. We do not design a separate `device ...` interface; one name and
one syntax apply across the whole public CLI.

`connect` and `pair` are independent operations:

- `connect` establishes a BLE connection with a specific device,
- `pair` starts the security procedure on an existing connection,
- `unpair` removes the locally stored keys and bond,
- `disconnect` ends the connection, but does not remove the bond.

A connection alone should not start pairing automatically. This lets the
tester separately check an unsecured connection, a reconnection with known
keys and the full pairing procedure.

The name should be the primary human identifier, and the ID number stays
available as an unambiguous script identifier:

```text
le-source attach Phone
le-source attach 1
```

`bonding create` does not silently change the identity used by `le-source`.
`le-source attach` is the only operation that says which identity acts as the
source. Commands should not rely on a hidden global selection, especially
when several identities exist.

#### Connecting by address or Bluetooth name

The address is the exact identifier of the remote device and should be
preferred in automation:

```text
bonding connect Phone address=AA:BB:CC:DD:EE:FF/random
```

In manual work the device can be found by its Bluetooth name:

```text
bonding scan
bonding connect Phone bluetooth-name="Living Room Buds"
```

`name` in `bonding create` means the name of the local virtual identity.
`bluetooth-name` means the name of the remote device read from the scan. An
unlabelled argument that would sometimes mean a name and sometimes an address
must not be used.

A Bluetooth name does not have to be unique or present in the advertising.
With several results the command should not pick the first device:

```text
Found 2 Bluetooth devices named "Living Room Buds":
  1. AA:BB:CC:DD:EE:01/random  RSSI=-48
  2. AA:BB:CC:DD:EE:02/random  RSSI=-71

Connection not started.
Use an address:
  bonding connect Phone address=AA:BB:CC:DD:EE:01/random
```

After a successful connection both the device name and the address should be
shown, even if the user connected by name. With an RPA the address can change
on the next scan, so the name should not be treated as a permanent substitute
for the address.

#### Device parameters

```text
bonding create Phone io=yesno addr=AA:BB:CC:DD:EE:FF/random
bonding io Phone keyboard
bonding txpower Phone -8
bonding param Phone interval=30 latency=0 sto=2000
bonding phy Phone 2m
bonding dle Phone 251
bonding policy param_req=accept
```

#### Pairing and bonds

Pairing is a separate step after `connect`. The command requires an active
connection:

```text
bonding connect Phone address=AA:BB:CC:DD:EE:FF/random
bonding pair Phone sec=4
```

```text
Connection established with "Living Room Buds".
Pairing requested for "Phone".
Passkey required. Continue with:
  bonding passkey Phone 123456
```

With numeric comparison:

```text
Confirm passkey 483921 for "Phone":
  bonding confirm Phone yes
```

When the procedure completes:

```text
Pairing completed.
Bond stored for "Phone" and "Living Room Buds".
```

`bonding disconnect Phone` only ends the connection. The keys stay stored, and
the next:

```text
bonding connect Phone address=AA:BB:CC:DD:EE:FF/random
```

can establish an encrypted connection without pairing again, if the device
recognizes the stored bond. `bonding unpair Phone` removes the local keys, and
should not be presented as an ordinary disconnection.

The states should be kept apart:

| State | Meaning |
|---|---|
| `disconnected` | no active connection |
| `connected` | the BLE connection is active |
| `encrypted` | the connection is encrypted |
| `paired` | the pairing procedure completed and the bond is stored |
| `unpaired` | there is no bond locally |

`security enabled` should not be presented as `paired`. The tester must be
able to tell encryption from an existing bond apart from a new pairing
procedure.

#### Device status

```text
bonding list
```

Default view:

```text
ID  NAME       CONNECTION  DEVICE              SECURITY   BONDS  SOURCE
1   Phone      connected   Living Room Buds    encrypted  1      no
2   Tablet     disconnected -                 unpaired   0      attached
```

Link parameters, RSSI and PHY should be available through:

```text
bonding show Phone --verbose
```

#### `le-source`: simulating an LE Audio source

`le-source` is the name of a functional role, not a declaration of a form
factor. The module emulates an LE Audio source with functions associated with
a phone: music and calls. Tests of a phone, tablet, TV, computer or another LE
Audio source can use the same role.

`le-source` uses one identity created in the `bonding` area, but does not
create bonds by itself. The identity has to be prepared and connected first,
and then explicitly attached to the source:

```text
le-source attach Tablet
le-source music start [preset=] [freq=]
le-source music stop
le-source call incoming [uri=] [name=] [ring_freq=] [call_freq=]
le-source call outgoing [uri=]
le-source call accept
le-source call end
le-source call hold
le-source call retrieve
le-source call remote_answer
le-source alert [ms=] [freq=]
le-source volume set <0..255>
le-source volume up|down
le-source mute on|off
le-source preset list|next|prev|set <index>
le-source mic mute on|off
le-source mic gain <value>
le-source balance set <-255..255>
le-source status
```

`bonding create TV` still only creates a BLE identity. `le-source` runs the LE
Audio source profile regardless of that identity's name.

`volume`, `mute`, `preset`, `mic` and `balance` always send control to the
currently connected device. They do not change the level or parameters of the
generated test signal locally. The remote device can be a hearing aid,
earbuds, a speaker or another LE Audio receiver.

In this document the **connected device** means the remote Bluetooth device
with which the local virtual identity has an active BLE connection. It is not
another identity created by the tool. For example:

```text
Identity: Tablet
Bluetooth device: Living Room Buds
```

means that `Tablet` is a local identity of the tool, and `Living Room Buds`
are the remote earbuds to which the tool sends control and the audio stream.

Example of a typical session:

```text
mode unicast
bonding create Phone
bonding connect Phone bluetooth-name="Living Room Buds"
bonding pair Phone sec=4
le-source attach Phone
le-source music start freq=440
le-source call incoming name="John"
le-source call accept
le-source call end
le-source music stop
```

`le-source attach` does not establish a connection. `bonding connect` makes
the connection beforehand, and `le-source attach` selects the identity used by
the source. `le-source music start` and the control commands should require a
connected device, but the error must point at the specific next step.
`le-source` handles one attached identity at a time. Changing it requires an
explicit detach:

```text
le-source detach
le-source attach Phone-2
```

```text
Cannot start le-source music:
attached identity "Phone" is not connected.

Try:
  bonding connect Phone
```

`bonding pair` has its own precondition:

```text
Cannot pair "Phone":
no active BLE connection.

Pairing does not connect automatically. Run:
  bonding connect Phone
  bonding pair Phone
```

#### Asynchronous source results

A command can immediately confirm that the request was accepted:

```text
OK queued=1
```

The human mode should additionally translate the events:

```text
Connecting audio stream...
Audio stream active.
Music started: requested 440 Hz, actual 450 Hz.
```

The `text` and `json` formats can keep the raw `EVT` events, because
automation and tests need them. The `human` format should show a descriptive
message and, optionally, the raw details through `--verbose`.

#### Operation requirements

| Operation | Requires a connection | Requires pairing |
|---|---:|---:|
| `bonding connect` | no | no |
| `bonding pair` | yes | no |
| `le-source attach` | no | no |
| `le-source music start` | yes | depends on the profile |
| `le-source volume up` | yes | depends on the service |
| `le-source` control | yes | depends on the service |
| `broadcast start` | no | no |

`attach` assigns a local identity to `le-source`, `connect` establishes the
BLE connection, and `pair` starts the security procedure on an existing
connection. None of these operations should silently perform the others.

#### Source status

```text
le-source status
```

```text
Identity: Phone
Bluetooth device: Living Room Buds
State: music
Context: Media
Preset: 48_2
Stream: active
RX packet loss: 0.0% (0/1200)
```

The status should clearly distinguish:

- the source state,
- the BLE connection state,
- the audio stream state,
- the phone call state,
- reception statistics,
- the state of control sent to the device.

### General operations

#### State of the whole tool

```text
status
status --verbose
version
limits
id
```

A short `status` should be enough to tell what is active:

```text
Mode: unicast
Identity: Phone
Bluetooth device: Living Room Buds
Connection: connected
Security: encrypted
Bond: present
LE source: music
Broadcasts: 0 active, 0 configured
```

`status --verbose` can include radio parameters, limits, events and CLI
statistics.

The states belong to different layers and should not be merged into one
generic `state` field:

- `mode` — `unicast` or `broadcast`,
- `connection` — `disconnected`, `connecting` or `connected`,
- `security` — `unencrypted` or `encrypted`,
- `bond` — `absent` or `present`,
- `audio` — `stopped`, `starting`, `active` or `error`,
- BIG — `draft`, `ready`, `active`, `stopped` or `error`.

#### Operating mode

The mode is an implementation detail, but it should still be visible:

```text
mode
mode unicast
mode broadcast
```

The internal names of the earlier firmware map to the target user-facing layer
as follows:

| User-facing layer | Earlier firmware mode | Meaning |
|---|---|---|
| `bonding` | `dev` | identities, connections and bonds |
| `le-source` | `phone` | unicast LE Audio source |
| `broadcast` | `bcast` | Auracast transmissions |

`le-source` can use a prepared bonding identity, but does not take over
identity management. `bonding` and `le-source` can work together in a unicast
session. `broadcast` is an independent test goal and cannot run at the same
time as either of them because of the radio module's limitations.

`bonding` and `le-source` commands can use the same unicast session without
changing `mode`. The `mode` command stops and clears the whole context of the
previous mode before making the new one available. For example, `mode
broadcast` removes active unicast connections, identities, bonds and the whole
`le-source` context. `mode unicast` removes active BIGs, their configuration
and the whole broadcast context.

#### Reset and stopping

The operations should be kept apart:

```text
stop
reset
```

`stop` stops all radio activity in the current mode, but does not remove its
configuration or data. Narrower commands stop only the given activity:

```text
le-source music stop
broadcast 1 stop
broadcast stop --all
```

`reset` returns to the initial state and removes the data of the current mode.
Removing a single identity or BIG should have its own unambiguous command:

```text
bonding delete Tablet
broadcast 1 delete
```

In automation, commands stay without confirmation. In interactive use, a
destructive operation should require an explicit option:

```text
reset --yes
```

`reset` clears the current context and leaves the tool without an active
mode. `mode <mode>` clears the current context and then immediately starts the
given mode, empty.

#### Help and diagnostics

No menu means that help must be available without knowing the whole
interface:

```text
help
help bonding
help bonding connect
help le-source
help broadcast
help broadcast 1 bis add
```

A wrong command should suggest valid variants, for example:

```text
Unknown command: le-source volume increase

Did you mean:
  le-source volume up
  le-source volume down
  le-source volume set <0..255>
```

Boolean options keep one `--flag` style, and configuration parameters stay in
the `key=value` form:

```text
broadcast start --all
reset --yes
le-source music start preset=48_2 freq=440
```

These forms must not be mixed for the same kind of option.

## Consistency across modes

A full example flow should look like this:

```text
mode unicast
bonding create Phone
bonding connect Phone bluetooth-name="Living Room Buds"
bonding pair Phone sec=4
le-source attach Phone
le-source music start freq=440
le-source volume up
le-source music stop
mode broadcast
broadcast 1 create name="Test stereo"
broadcast 1 subgroup add preset=48_2 lang=pol ctx=media program="Music"
broadcast 1 bis add subgroup=1 location=left freq=440
broadcast 1 bis add subgroup=1 location=right freq=880
broadcast 1 start
broadcast 1 show
broadcast 1 stop
```

In this flow `bonding` and `le-source` work together within one unicast
session, and `broadcast` stays strictly separate:

- `bonding` manages the identity and the connection,
- `le-source` uses the identity prepared earlier by `bonding`,
- unicast audio is handled by `le-source`, and `bonding` deals only with
  identities, connections and keys,
- `broadcast` manages an independent BIG,
- `status` and `show` show the state without requiring knowledge of the
  command history,
- `mode broadcast` cleared the whole previous unicast context before the new
  BIG was created.

## UX acceptance criteria

The design meets its goals if the tester can:
1. use `bonding` and `le-source` in one unicast session,
2. not run `broadcast` in parallel with any unicast activity,
3. get an empty context, without data from the previous mode, after changing
   the mode,
4. create at least two BIGs without using a selection command,
5. add different subgroups to each BIG,
6. add BISes to a chosen subgroup,
7. set different presets, languages, contexts, locations and tones,
8. start and stop BIGs independently,
9. start or stop all BIGs with `--all`,
10. read the full configuration from `list`, `show`, `subgroup list` and
    `bis list`,
11. get a clear error on a radio budget conflict,
12. prepare a bond and explicitly attach the identity with `le-source attach`,
13. send `volume`, `mute`, `preset`, `mic` and `balance` to the connected
    device,
14. use the same operations from the terminal and from a script,
15. configure the full range of parameters without moving to a separate
    expert mode.

## Implementation decisions

Clarifications adopted during implementation (the `ha_tool` firmware, the
`protocol/hatool.yaml` specification):

- The BIG `interval` is given in ms: `7.5` or `10`.
- `broadcast <n> create` on an existing BIG returns an error with the hint
  `broadcast <n> delete`. The optional `preset=` and `bis=` create a subgroup
  and the given number of BISes right away (a shorthand for quick tests).
- BIG states: `ready` = at least one subgroup and one BIS; `error` = the last
  start failed (until the next configuration change or start); `stopped` = was
  active, configuration kept.
- `broadcast <n> subgroup set` can change only metadata (`lang`, `ctx`,
  `program`) on an active BIG; changing `preset` requires `stop`.
- `subgroup delete` works only for a subgroup without BISes. Subgroup and BIS
  numbers stay contiguous (the BASE requires consecutive indices), so after a
  deletion the following numbers shift down by one; the reply gives the new
  numbering.
- From the idle mode (`none`, after boot and after `reset`) the first
  `bonding`/`le-source` command enters `unicast`, and the first `broadcast`
  command enters `broadcast` (with a `mode` event). Switching between `unicast`
  and `broadcast` requires an explicit `mode` and clears the whole context,
  identities and bonds included.
- Identity names are unique and cannot consist of digits only, so
  `bonding show 1` always means ID 1.
- `le-source call remote_answer` simulates the remote party answering an
  outgoing call (needed in tests). `le-source alert` plays a short
  notification sound.
- In the `text` and `json` formats an error stays one line, and the hint goes
  into the `hint` field (e.g. `hint="broadcast 1 stop"`). The `human` format
  shows it as separate "Try:" lines. Tables and budget percentages are a
  `human` view; `text` and `json` keep data lines with fields.
- `reset --yes` is required only in the `human` (interactive) format.
