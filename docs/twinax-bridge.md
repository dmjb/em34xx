# Twinax bridge: the virtual workstation controller

The emulated terminal talks 5250 Twinax. To reach an IBM i over the network,
em34xx plays the part a 5294, 5394, 5494 or a system workstation controller
plays on a real cable: it polls the terminal's station addresses, writes
their buffers with device commands, reads keystrokes, and does the 5250
field editing itself. On the other side it is a TN5250 client.

Sources are cited as:

- **PAI**: *IBM 5250 Information Display System to S/36, S/38 and AS/400
  System Units, Product Attachment Information*, October 1989.
- **FR**: *IBM 5494 Remote Control Unit Functions Reference*, SC30-3533-04.
- **3489 UG**: *IBM InfoWindow II 3489 Modular Display Station User's Guide*,
  GA18-7492-00.
- **RFC 1205 / 2877**: the 5250 Telnet interface and its enhancements.

Facts are marked **[doc]** (from those documents), **[obs]** (observed in the
emulated terminal's behaviour) or **[hyp]**.

## Layers

```
 IBM i <--TCP/TLS--> (a) TN5250 session      telnet negotiation, records  src/tn5250/
                      (b) 5250 data-stream engine
                          shadow screen, format table, field editing, AIDs
                              |  tn5250::DisplaySession (session_api.hpp)
                              v
                      (c) station controller    one per station address  src/host/wsc/
                          identification, device mirror, screen diff,
                          scan code -> operator action
                      (d) link scheduler        one per cable
                          poll cadence, half-duplex line, recovery
                              |  TwinaxHost (src/host/twinax_link.hpp)
                              v
                      terminal link device      emulated Twinax hardware     src/devices/
                              |
                          terminal firmware (unmodified ROM)
```

- **(a)** owns the socket and the RFC 1205/2877 negotiation; it runs on its
  own thread and hands whole records across a queue.
- **(b)** applies 5250 commands (FR ch. 15) to a shadow presentation space
  and implements the operator keyboard rules (FR ch. 16). It knows nothing
  about Twinax.
- **(c)** keeps a **mirror** of what each device's buffer holds and turns the
  difference between shadow and mirror into device commands. It translates
  scan codes into characters, editing functions and AID keys, because the
  terminal sends key positions only [doc PAI 2.1].
- **(d)** owns the cable. Everything below (a) runs on the emulation thread,
  in **emulated time**: the terminal's own timers (System Available, its
  automatic reset) run on emulated time too, so a scheduler on wall-clock
  time would break them whenever the emulator runs faster or slower than
  real time.

### The TwinaxHost boundary

`TwinaxHost` is the controller end of the cable:

| Call | Direction | Meaning |
| --- | --- | --- |
| `advance(emu_ns)` | link device -> host | emulated time since power-on; drives the poll cadence |
| `to_station(msg)` | host -> link device | the next message, if one is due; the first frame's address selects the station |
| `from_station(msg)` | link device -> host | the response to the last message |
| `no_response()` | link device -> host | the last message got no response (normal for queue loads and Activate Write) |

The line is half duplex [doc PAI 2.4]: after each message the link device
reports exactly one outcome before it asks for the next message. A response
therefore always belongs to the station the last message addressed. Its
frames carry address 7 on the last frame, not the station's address.

## The station model

A terminal can answer on **several station addresses**. Each address is a
complete, independent 5250 station, with its own Set Mode and power-on state,
command queue, exception latch, buffer, keyboard queue and TN5250 session.
The controller drives each one separately.

| Terminal | Sessions | Kinds | Source |
| --- | --- | --- | --- |
| 3197 (the 3477's documented base) | 2 addresses | address 1 display; address 2 display or printer (parallel port) | [doc PAI 14.0] |
| 3477 | 2 | two displays, or display + printer | [doc PAI ch. 16 via 3197; obs] |
| 3486 | 2 concurrent | displays | project requirement; no user's guide on hand [hyp] |
| 3489 | 1-4 displays, each mode with or without a printer | up to 4 displays + 1 printer | [doc 3489 UG 1.1.5, 2.9] |

Rules from the documentation:

- Addresses are 0-6; 7 marks the end of a message [doc PAI 2.4], so one
  cable carries at most 7 stations. The emulator enforces both that limit and
  each model's own session limit (3477: 2, 3486: 2, 3489: 5). Each
  workstation on the cable needs its own address [doc 3489 UG 2.10].
- Without shared addressing, every display session and the printer each
  need their own address. With **shared addressing** (3489, OS/400 V3R1 or
  5494 Release 3.0), all display sessions can share one address but the
  printer session cannot [doc 3489 UG 1.1.3, 5.2.21, App. G]. Shared
  addressing needs session multiplexing that em34xx does not model;
  em34xx gives each session its own address.
- Keys go only to the session on the screen; hidden sessions must still be
  polled [obs]. The 3197 allows concurrent viewing only at 24x80 [doc PAI
  14.0]. The 3489 allows a 27x132 vertical split [doc 3489 UG 5.5.1].

### Configuration

```toml
[twinax]
host = "tn5250"

[[twinax.station]]
address = 0
type = "display"
tn5250_device_name = "QPADEV0042"   # DEVNAME; empty = host assigned
profile = "IBM-3477-FC"             # terminal type; empty = default for the type

[[twinax.station]]
address = 1
type = "printer"
tn5250_device_name = "PRT01"
```

On the command line: `--station 0:display --station 1:printer:PRT01`
(`ADDR[:TYPE[:DEVNAME]]`, repeatable; it replaces the file's list). The old
`--station-address N` still configures a single display. Addresses must be
unique, 0-6. The number of stations may not exceed the model's
`ModelInfo::max_sessions`: 3477 2, 3486 2, 3489 4. The 3489 documents four
displays plus a printer, so its limit may grow to 5 once it is modelled.

### Per-station state machine

```
Absent --any response--> PowerOn --Set Mode--> Identify --reads--> Init --> Online
   ^                                                                         |
   +--- 3 missed responses / busy timeout --+     Recover <-- exception, LPE -+
                                            |        |
                                            +--------+--> Online (repaint) or Identify
```

| State | What the controller sends |
| --- | --- |
| Absent | a poll every 500 ms |
| PowerOn | Set Mode (fill 0). Before it, the station answers polls with one frame, status `0E` = power-on exception, and runs no other command [doc PAI 2.4.3, p.75] |
| Identify | for each read: queue the read command, poll until not busy and no exception, then Activate Read [doc PAI p.76]. Read Device ID (base), then for displays: model ID (`8C`), keyboard ID (`2C`), extended ID (`88`), Read Info (`0B 80`). A read the station refuses (exception) is skipped after an exception-reset WCD |
| Init | WCD (format), then Clear, input inhibited, cursor home; the session is told the device is ready |
| Online | one step at a time: queue load, poll until not busy, Activate Write if needed, poll again. Otherwise an idle poll every 25 ms |
| Recover | WCD with reset-exception, or a reset-LPE poll; then the mirror is marked unknown and repainted |

Polls always carry ACK (`30`, or `70` to reset a line parity error). A
second poll byte is a new keystroke only when the response-level bit has
changed since the last response [doc PAI p.39-43]. The 25 ms cadence stays
well inside the PAI limits (at least 40 polls in 2 s, no gap over 270 ms
[doc PAI p.38]). It also meets the terminals' System Available timeout,
which the 3477 checks every 200 ms [obs].

### Identification results

| Read | Answer | Used for |
| --- | --- | --- |
| Device ID, base (`0C`) | `C2` for every display [doc PAI 3.5.2.1]; a printer ID word otherwise | display vs printer |
| Model ID (`8C`) | `24` monochrome, `50` colour on the 3477 (as on the 3197 D/C) [doc PAI 16.0] | colour |
| Keyboard ID (`2C`) | `02` typewriter, `0A` World Trade; `+80` Enhanced keyboard [doc PAI p.55; obs] | scan-code table |
| Read Info (`0B 80`) | 64 bytes of VPD starting `F3 F4 F7 F7` ("3477") [doc PAI 16.1.4] | 3477 features: 27x132 on every model [doc PAI 16.0], larger command queue |

## Displays and printers at the device-command level

| | Display station | Printer station |
| --- | --- | --- |
| Read Device ID | `C2` | printer ID word: model (2 bits), station type `10`, printer type (4 bits): `0000` 5256, `1100` 4214-2, `1101` 5219/3812 [doc PAI 3.5.2.1 fig. 74] |
| Poll byte 2 | keystroke scan code, `FF` = keyboard overrun | printer status: invalid SCS code or parameter, receive buffers full, print complete, cancel, not ready, end of forms, graphic check [doc PAI 3.5.1.1.2] |
| Buffer | a presentation space addressed by row x columns + column; written with Load Address Counter, Write Data and Load Cursor, Write Data, Clear, ... | no addressable buffer; Write Data to the printer address, then Activate Write with **SCS** data [doc PAI 3.5.3, 3.6] |
| Commands | the full display set (PAI ch. 2; 3477 additions ch. 16) | Poll, Read Device ID, Write Data, WCD, Activate Write/Read, Set Mode, Read Status, Reset, Clear, Read Multiple, End of Queue [doc PAI 3.5] |
| Host side | TN5250 display session | TN5250 printer session (RFC 2877 printer device) |

On the 3489, the printer emulates one of three AS/400 printer types. The
AS/400 device is configured as 4214 for the 4201/4202 family, 4226,
2380/2381 and ThinkJet; as 5256 for the 5201; and as 5219 for the others,
using 3812 SCS emulation [doc 3489 UG 2.10]. The 3197 attaches its printer
as a 4214-2 or a 5256 [doc PAI 14.0, 3.12]. The 3477 reports 5256, 4214-2
or 5219, as set up [obs].

In em34xx, printer stations are discovered, identified, and polled for
status. Their data path is the extension point `PrinterSessionPort`
(`next_print_data` -> Write Data + Activate Write). The size of one transfer
is a [hyp] (256 bytes) until a printer session engine exists. The bridge
gives each printer station a placeholder `tn5250::PrinterSession`
(`wsc::NullPrinterSession`) through `wsc::PrinterStationPort`: it accepts
no data, opens no connection, and logs once, when the printer first comes
online, that printer sessions are not implemented. A TN5250 printer
session (RFC 4777 sections 8-10) replaces it behind the same interface.
A model's `ModelInfo::max_printer_sessions` limits printer stations: one on
the 3477 (display + printer on its two addresses) and the 3489, none on the
3486.

## Pointing device (3486/3487)

None of this is in the PAI, which predates these terminals. It comes from
the 3487 firmware running in the emulator (`54g0010.bin`, model 3487, mono,
one display session) with a PS/2 mouse on the second port; every line is
marked. What a click then does is in `docs/tn5250.md`, "Pointing device".

### What the terminal does by itself [obs]

Left alone by the controller, the terminal handles the mouse locally:

| Event | To the controller |
| --- | --- |
| Movement | nothing; the firmware draws its own pointer |
| Left button pressed | outstanding status (poll status bit `10`) on two polls, then nothing |
| Right button pressed | scan code `68` (Enter) in poll byte 2 |
| Middle button pressed | scan code `7E` (Reset) |
| Any button released | nothing |

### Identification [obs]

Read Device ID, model ID, keyboard ID and the extended ID are the same as
a 3477's, with or without a mouse. Read Info tells them apart:

| Read | 3477 | 3486/3487 |
| --- | --- | --- |
| `0B 80`, bytes 0-6 | `3477FE0` | `3477FE0` |
| `0B 80`, bytes 51-52 | `00 00` | `1F 9F`, or `1F DF` with a pointing device |
| `0B C0` | an exception that Write Control Data does not reset: the station never comes on line | the unit's own VPD, `3487HA0`; byte 52 bit `40` = pointing device attached |

The controller therefore sends `0B C0` only when byte 52 of the `0B 80`
answer is not zero. What the other bits of bytes 51 and 52 mean is
unknown.

### Switching button events on

| Step | Bytes | Effect | |
| --- | --- | --- | --- |
| 1 | `0B C0`, Activate Read | besides the VPD, unlocks a third Write Control Data byte for the session | [obs] |
| 2 | `05 b1 b2 b3` with `b2` bits `80` (byte 3 follows) and `40` (device addressing), `b3` bit `10` | button events are reported as records instead of handled locally; needs a pointing device | [obs] |
| 3 | `09 09 03` | selects record 3, the pointer record, for reads from device 101 | [obs] |

- Without step 1, the third byte is taken as a command and gives exception
  `08` [obs].
- `b3` bits `04` and `02` set an interval for movement records (`02`
  [obs], the other values [hyp]); the controller does not ask for them.
- The terminal's station reset clears all of it [hyp, from the firmware's
  reset routine], so the controller repeats the steps after every
  discovery. It sends the three-byte form in every Write Control Data.
- Whether the read in the next section needs device addressing is a [hyp];
  the controller sets it because the working exchange had it. Painting and
  the indicators work unchanged with it on [obs].

### Reading an event [obs]

An event raises outstanding status. The controller answers with Load
Address Counter 0, Load Reference Counter 7 and `BA` (read device 101
between the counters), then Activate Read, and gets 8 bytes:

| Byte | Content |
| --- | --- |
| 0 | `01` pointer event; `00` nothing pending |
| 1, 2, 3 | left, right, middle button: `1` pressed, `2` released, `3` pressed again within the double-click time |
| 4 | buttons down now: `80` left, `40` right, `20` middle |
| 5-6 | the pointer's buffer address, high byte first: row x columns + column, from 0 |
| 7 | `00` |

- Shift is not in the record. The controller takes it from the keyboard's
  shift state, which it tracks anyway.
- **The offer lasts two polls.** An event not read by then is dropped, and
  every poll counts. The controller therefore sends the read command and
  the Activate Read with no poll in between, ahead of other work at the
  next step boundary. An Activate Read sent at once is not answered, so it
  waits 2 ms first (`WscTiming::pointer_read_ns`, [design]).
- Events queue in the terminal (32 [hyp]) while nobody reads; the
  controller polls again at once after each record.
- An event that arrives while a screen is being painted can still be lost,
  because the paint's own polls use up the offer [hyp, not seen].

### Not known

- The double-click time and the movement interval in real time.
- Two display sessions, split screen, the message line: the firmware
  computes the position differently there [hyp]; untested.
- Colour models and the 3486 strap code: untested.
- What a real controller sends. With IBM's 2661 code (V3R2) the first
  press was answered with reads of devices 5 and 4 [obs on another
  branch]; that controller level may predate the pointer.

## Painting a screen

The controller keeps a per-cell mirror of the device buffer (value or
unknown), plus the cursor, indicators and Write Control Data state. When the
shadow's generation changes:

1. **Format and control.** The screen size, cursor blink, clicker or an
   alarm changed: Write Control Data. A format change marks the mirror
   unknown.
2. **Clear.** Everything unknown, or most cells must become null: Load
   Address Counter 0, Load Reference Counter (last cell), Clear.
3. **Runs.** Changed cells are grouped into runs, merging gaps under 6
   cells. A short run goes as Load Address Counter + Write Data and Load
   Cursor, with an explicit count frame (data bytes 01-0E would otherwise be
   read as a count [doc PAI 2.4.16]). Several short runs share one queue
   load while they fit. A long run goes as Load Address Counter + Write Data,
   then Activate Write after a clean poll.
4. **Cursor and indicators.** Load Cursor and Write Data and Load Cursor to
   the indicators (80 message waiting, 08 insert, 02 input inhibited [doc PAI
   2.4.16.1]).

Every queue load fits the device's command queue (16 bytes [doc PAI];
the 3477 can be switched to a larger one [obs]). The mirror is updated only
after a step finished with a clean poll. An exception or parity error marks
it unknown, and the next flush repaints from the shadow.

## Keystrokes

Poll byte 2 carries a scan code: bit 7 is break, set only for shift-type
keys [doc PAI 2.1.1]. The controller tracks Shift, Alt and Caps Lock, and
maps each key to a data character (EBCDIC), an editing function
(Field Exit, Field +/-, Tab, cursor keys, Insert, Delete, Dup, Erase EOF,
Erase Input, Reset), an AID (Enter, F1-F24, Roll, Print, Help, Clear) or a
signal (Attn, Sys Req). The engine then applies FR ch. 16.

The table for the **IBM Enhanced keyboard** is typed in from PAI fig. 147
(US). The **122-key typewriter keyboard** sends the 3180-2/3196 codes of PAI
fig. 105 (Shift as make/break codes, a CMD prefix `6F` for Alt and F1-F24);
the controller decodes them to key positions (docs/keyboard.md, "5250 scan
codes on the cable"). National layouts are not typed in yet.

## The controller without a host session

`--twinax-host wsc` runs the same controller (link scheduler and one
station state machine per configured address) with a local screen in place
of the TN5250 session (`src/host/wsc/idle_host.*`). Discovery,
identification, initialisation and polling are exactly those of the bridge;
each display station then shows a blank 24x80 screen with the keyboard
unlocked, typed characters are echoed at the cursor and an AID key clears
the screen. It brings a terminal on line without an IBM i system, which is
how the 3477 bring-up checks its link hardware:

```
build/debug/em34xx --model 3477 --nvram 3477.nvram --twinax-host wsc
```

`twinax status` shows each station's state, identity and counters.

## Running the bridge

```
build/debug/em34xx --model 3477 --twinax-host tn5250 --tn5250-host HOST
build/debug/em34xx --model 3477 --twinax-host tn5250 --tn5250-host HOST \
    --station 0:display:EM34XXA --station 1:printer:EM34XXP
```

or in a configuration file:

```toml
[twinax]
host = "tn5250"

[[twinax.station]]
address = 0
type = "display"
tn5250_device_name = "EM34XXA"

[tn5250]
host = "pub400.com"
port = 23
```

The station's address must match the address the terminal is set up for
(3477 setup; address 0 by default). `twinax status` in the monitor shows each
station's state and counters and each session's state, host, terminal type
and device name.

### How it is wired

`wsc::Tn5250Bridge` (src/host/wsc/tn5250_bridge.hpp, library
`em34xx_bridge`) is the `TwinaxHost`. `make_twinax_host()` validates the
configuration and calls the factory `main()` hands it, so the core library
does not depend on the TN5250 client. The bridge holds the
`LinkScheduler`, a `Station` per configured address, and:

- per **display** station, a `DisplayStationPort` that owns a
  `tn5250::Client` and its `Tn5250DisplayAdapter`. Each session has its own
  device name (the station's `tn5250_device_name`; the first display falls
  back to `tn5250.device_name`) and terminal type (`station_terminal_type`:
  the station's `profile`, else `--tn5250-type`, else derived from model and
  display: IBM-3477-FC / -FG). Host, port, code page, environment and all
  TLS settings are copied unchanged from `[tn5250]` into every session.
- per **printer** station, the placeholder printer session above.

Threads: each client runs its socket and telnet negotiation on its own I/O
thread and queues whole records. All controller and engine work runs on
the emulation thread: the link device calls `advance(emu_ns)`, which
pumps every session (records are applied to the engine there) and runs the
session lifecycle; `to_station`, `from_station` and `no_response` drive
the stations. Polling follows emulated time.

### The session is the cable

A TN5250 session stands for the Twinax cable between the terminal and a
live controller:

| Event | On the line |
| --- | --- |
| bridge created (`--twinax-host tn5250` at start-up, or `connect`) | every display station's session starts connecting at once; its station is **unplugged**: nothing is ever sent to it |
| session negotiated (telnet BINARY + EOR + terminal type) | the station is **plugged in**: it starts from Absent, so the terminal goes through its power-on / discovery handshake and comes on line; the host's sign-on screen follows |
| host closes, connection lost or fails, `disconnect` | the station is **unplugged**: it is no longer polled and nothing answers the terminal, which shows its normal no-controller behaviour (System Available off after about 200 ms, its automatic reset after about 2.4 s) as if the cable were pulled |
| reconnect | the full plug-in sequence again. After a host close or failure the bridge reconnects by itself after a wall-clock back-off (2 s doubling to 60 s; the host is a network peer); after `disconnect` it does not |
| terminal stops answering while the session is up (power cycle, reset) | the session stays; the engine is told the device is gone, and when the device is discovered again the controller repaints the whole screen. The host is not told: TN5250 has no device power-off signal |

`Station::set_plugged()` is the gate: an unplugged station is never due,
drops a late response to a message sent before it was pulled, and loses
its state (mirror, keyboard shift state, response level). `twinax status`
shows `cable plugged` / `cable unplugged` per station.

Printer stations have no session of their own yet: their cable counts as
plugged while at least one display session of the bridge is up (the
stations share one cable). A bridge with only printer stations therefore
never polls them.

### Connecting at run time

The monitor's `connect` command attaches the bridge while the emulator runs,
for example with `build/debug/em34xx --model 3477 --color` and no host
options:

```
connect pub400.com
connect 192.0.2.10:23 station 0 device EM34XXA
connect ibmi.example tls tls-verify none
disconnect 0
disconnect
```

`connect HOST[:PORT] [tls] [station N] [device NAME] [type TERMTYPE]
[option value ...]` builds the configuration from the start-up one plus
these settings (src/config/connect_args.hpp: every other `name value` is
passed through as `--tn5250-name value`, so the TLS options need nothing
of their own) and swaps the bridge in for the current controller under the
machine lock (`Machine::swap_twinax_host`); the link device looks the host
up on every use, and the old controller and its sessions are destroyed
outside the lock. `connect` while connected closes the existing sessions and
starts from scratch. `disconnect` closes all sessions and leaves no
controller on the line; `disconnect N` closes station N's session and keeps
its cable unplugged (no reconnect) while the other stations carry on.

### Tests

- `tests/test_bridge.cpp` (in `em34xx_tn5250_tests`): a scripted
  in-process TN5250 server -> client (I/O thread) -> engine -> controller ->
  `FakeStation`. It checks that no session starts before the station is
  online, that the sign-on panel lands in the station's buffer with the
  cursor in the user field, that the station's keystrokes (`a`, Enter)
  reach the server as the right Put/Get reply (`06 36 F1 11 06 35 C1`), the
  printer stub, the repaint after device lost, and the reconnect after the
  host hangs up.
- The same test pulls the cable: after the server closes, the station is
  unplugged and neither station gets another message; after the reconnect
  the full discovery runs again. `disconnect N` (`Tn5250Bridge::hold`)
  keeps the cable pulled past the back-off.
- `tests/test_connect.cpp`: the `connect` command line.
- Live, with a `FakeStation` in place of the terminal (a private harness):
  pub400.com shows its sign-on panel; a local OS/400 V4 system signs on,
  shows the Main Menu and signs off, typed through the Enhanced-keyboard scan
  codes.

### Live results with the 3477 firmware

Observed with the 3477 brought past its display test by hand (a private
bring-up aid, before the temporary video-signature shim made an unmodified
boot pass; see docs/bringup-3477.md) and a saved setup (`--nvram`), then
`connect HOST` at the monitor:

- pub400.com: the session negotiates, the cable is plugged, the terminal is
  discovered (display, model 24, 122-key keyboard, 27x132 capable) and
  shows the full sign-on panel in colour. `disconnect` takes the
  controller away.
- A local OS/400 V4 system: the sign-on panel; user and password typed on
  the emulated 122-key keyboard (through the host keymap, the firmware and
  the controller's 122-key cable-code table), the AS/400 Main Menu, then
  `SIGNOFF` back to the sign-on panel, and `disconnect`.
- Long runs are painted with Write Data and Activate Write from the first
  paint on; the terminal drops busy after each transfer (the engine
  reports the end of the Activate Write message, see docs/bringup-3477.md).
- The emulated terminal showed the characters typed into a non-display
  field (the password). The controller now writes nulls to the device for
  every position of a non-display field (`Tn5250DisplayAdapter`), so field
  contents the host hides never reach the cable or the screen.

## Status and next steps

- Done: configuration, the `TwinaxHost` contract, layers (c) and (d), the
  Enhanced keyboard map, the `tn5250::DisplaySession` adapter, a fake
  station for unit tests, and the bridge wired into `--twinax-host tn5250`.
- Next: the 122-key keyboard table; extended attributes (colour, ECB) on
  the 3477; a printer session engine; the emulated 3477 going on line so the
  bridge drives the real firmware.
