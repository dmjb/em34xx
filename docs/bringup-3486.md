# 3486 bring-up notes

Hardware facts and hypotheses for the IBM 3486 InfoWindow II and its
siblings, the 3487 and 3488, which share one firmware. They were first
worked out from the **partial** content of the 3486's own ROM chip
(`54G0011`) and compared with the 3477 (`docs/bringup-3477.md`). A complete
image of the same firmware family, `54g0010.bin` from a 3487 logic module,
is now available (see `roms/ROMS.md`), and the model
(`src/models/model_3486.cpp`) runs it; its status is **bring-up**. Each
entry is labelled **observed** (seen in emulation or in the firmware's use
of the hardware), **documented**, or **hypothesis**. This file describes
hardware only.

Most of what follows was written from segments `A000`, `B000`, `E000` and
`F000` of `54G0011` alone, before `8000`, `9000`, `C000` and `D000` could be
read. Addresses and sums quoted for `54G0011` are for that firmware level
and have not all been rechecked against `54G0010`. "Boot progress" below
gives the current state.

## Summary

The 3486 is a 3477 derivative. It has the same CPU class, the same gate
array and display controller bases, the same keyboard, Twinax, printer and
I2C register use, and some new registers in the unused parts of both pages
(observed). One ROM serves three products: the VPD model table lists
**3487, 3486 and 3488**, and the configuration straps select the model and
the tube (observed). It also has a second table, `3476/3477`, with the 3477
suffixes (hypothesis: a 3477-compatibility identity).

## CPU

- 8088-class (observed): no 80186-only opcode anywhere in the code reached
  by recursive descent from the reset vector (about 35,000 instructions).
  The code is generated for an 8086 (repeated single-bit shifts instead of
  shift-by-immediate), and there is no port I/O. All hardware is memory
  mapped, as on the 3477.
- **The CPU is an NEC V20** (D70108L-8, 8 MHz part), U12, PLCC-44 (observed,
  board photo). It runs 8088 code, but differs from an Intel 8088 in some
  undocumented opcodes, AAM/AAD, its `0F` extended-instruction prefix and
  timing. None of that is modelled yet; the firmware doesn't need it so far.
- Clock: the only oscillator is a **30.000 MHz** module (Y1, IBM 0666269;
  observed). A CPU clock of 7.5 MHz (30/4) is a hypothesis that fits the
  delay-loop estimate of about 7 MHz; the model uses it for pacing.

## Logic board (observed, photo of a 3486 board, IBM 9325)

| Ref | Part | Notes |
| --- | --- | --- |
| U11 | IBM 0666317 / Toshiba TC140G54AF, large QFP | Custom gate array. Almost certainly the system logic, display controller and Twinax engine in one chip (hypothesis); the Toshiba TC-family lineage matches the 3477 and IBM's 5250 adapters. |
| U12 | NEC D70108L-8 (V20) | CPU, 8-bit bus. |
| Y1 | 30.000 MHz oscillator, IBM 0666269 | Only clock source. |
| U15 | Mitsubishi M5M51008BFP-10LL, 128K x 8 SRAM | Low-power part next to the battery position BT1: the battery-backed RAM (hypothesis). |
| U14 | Sony CXK58257AM-70L, 32K x 8 SRAM | Fast SRAM, possibly display or character-generator memory (hypothesis). |
| U4, U5 | Mitsubishi M5M4C264AL-10, ZIP | Multiport video DRAM, 64K x 4 each = 64 KiB display memory (observed by the dumper; corrects an earlier guess of 256 KiB general DRAM). |
| U2 | Sharp mask ROM 54G0011 (c) IBM 1993, 32-pin DIP | The firmware ROM, 256 KiB (removed for dumping when photographed; corrects an earlier guess that U2 was an empty optional-ROM footprint). |
| U8, U9 | 74ALS273, 74LS373 | Address/data latches for the multiplexed CPU bus. |
| U6, U7, U13, U16 | 74F38/74LS38-type, 74LS14, 7406 | Open-collector drivers and Schmitt-trigger receivers for the external ports. |
| U10 | National M9324, IBM 33G2729, 16-pin DIP | Unknown. |
| U17 | Bell hybrid 38F5775 (LM431, 2903 comparator, discretes) | Twinax line driver/receiver (observed by the dumper). |
| PS1 | Optical sensor | Keylock detector (observed by the dumper). |
| J1 | DE-15 with varistors RV1-RV4 and Q1 | Twinax port with surge protection (hypothesis). |
| J2 | DB-25 | Parallel printer port. |
| J6, J4 | Modular jacks with TDK filters | Keyboard, and the **pointing-device port**: a PS/2 mouse or a light pen (observed by the owner). This is the second keyboard-style serial port at `20144-20146`. |
| J5 | 50-position card-edge receptacle (4 staggered pad rows of 13/12/13/12 on the solder side) | Expansion connector, empty on this board. Wide enough for the whole 8088 local bus (observed: pad count; use: hypothesis, see "Where the missing half comes from"). |
| J7 | Pin header | Analog board / power supply. |

The board's ROM label reads `54G0015`; the analysed dump is `54G0011`, so
several firmware levels exist.

## Memory map

| Range | Contents | Status |
| --- | --- | --- |
| `00000-5FFFF` | RAM | Observed in use up to about `52000`: stack and data at `38000-3C07F` (tested by the POST pre-test), Twinax data buffers in segment `5000`, a checksummed block at `469E0`, buffers cleared at `3F080` and `41082`. 384 KiB is a **hypothesis** that the board does **not** obviously support: the visible RAM is 128 KiB (U15) + 32 KiB (U14) SRAM, and U4/U5 are 64 KiB of video DRAM. How `00000-5FFFF` is decoded is unknown. |
| `20000-200FF` | System gate array | Observed (same base as the 3477). |
| `20100-201FF` | Display controller, now with registers up to `2014F` | Observed. |
| `60000-7FFFF` | Optional 128 KiB ROM | Observed: POST checksums segments `6000-7000` (word sum `619A`) only when strap `2000A` bit 6 is 0, and copies 7 bytes from `60050` into the setup block. Absent (open bus) in the model. |
| `80000-FFFFF` | ROM `54G0011`, reset vector at `FFFF0` | Observed. |

ROM integrity (observed): POST sums the 16-bit words of segments `8000`
to `F000` and requires `619A`. A flag word of `E13C` at `F000:0003`
skips the check (not set in this ROM). The surviving half sums to `DBE9`,
so **the four missing segments must sum to `85B1`**. This is a quick
check of the re-dump.

RAM areas (observed, named after the 3477 equivalents): the system state
block is at `0500` (3477: `0AA0`), the setup/configuration block at `0280`
(3477: `01A0`), another state block at `0560` (3477: `0A20`).

## Configuration straps (`2000A`)

The 3486 decoder differs from the 3477 one (observed):

| Bits | Meaning |
| --- | --- |
| 3-1 | Display type, see below |
| 5-4 | Boot mode: `00` normal, `01`/`10`/`11` manufacturing or test modes. Bits 3-1 = `101` with `2012F` bits 1-0 = `11` also selects a test mode. |
| 6 | 0 = optional ROM at `60000` fitted (POST checks it) |
| 0 | Read by the later boot-mode code, as on the 3477 (meaning hypothesis) |

Display type decoding (observed). "Family" is a second value the firmware
keeps with the type; the VPD suffixes come from the table at the top of
`F000`:

| Bits 3-1 | `2012F` bit 1 | Type | Family | VPD suffix |
| --- | --- | --- | --- | --- |
| `001` | 1 / 0 | 1 / 2 | 1 | `BA0` (3486) |
| `101` | 1 / 0 | 3 / 4 | 1 | `HG0` / `HA0` (3487 green / amber) |
| `100` | - | 5 | 1 | `HW0` (3487 white) |
| `110` | - | 6 | 2 | `HC0` (3487 colour) |
| `010` | - | 7 | 3 | `XXX` (3488, which drives an external monitor) |
| `011` | - | `80` | `FF` | rejected |
| other | - | 3 | `FF` | rejected (POST beeps for ever) unless a test boot mode is selected |

Model numbers per family (3486 for types 1/2, 3487 for types 3-6, 3488 for
type 7) are a **hypothesis**: the code that indexes the table is in a
missing segment. The model uses straps `42` (type 2, no optional ROM;
hypothesis for a real 3486). The `--display` subtype does not change them.
`--straps` overrides the byte, so the 3487 codes can be tried.

Compared with the 3477, `0C` now means colour (3487 HC0), and `0E` is
rejected.

## System gate array (`20000`)

Same use as on the 3477 for `20002-2000A`, `20010-20015`, `20020-2002B`
and `20030-20031` (observed). The new registers (observed accesses,
**hypothesis** meanings):

| Address | Use |
| --- | --- |
| `2000D` | Written `FF` when the third Twinax channel is set up. Hypothesis: channel 2 request register (write 1 to clear), the counterpart of `20002/3`. |
| `2000F` | Saved and restored together with the 16-bit mask `20004` by the interrupt code, written 0 at channel 2 set-up. Hypothesis: channel 2 enable mask (third mask byte). |
| `20017` | Written `20` at reset. |
| `2001A` | Station address of the third Twinax channel (written next to `20008`, bits 2-0). |
| `2001B` | Written by the Twinax and loop-back test code; channel 2 counterpart of `20009` (hypothesis). |
| `2002A` | Written by the loop-back test (next to the frame generator `20024-28`). |

`20006` (written `FF` on the 3477) is not written by the surviving code.

## Display controller (`20100`)

- Same register block as the 3477 (timing block `00-17`, mode block
  `25-2E`, `2F` status/I2C, window starts, interrupt acknowledge `36`,
  block request `38-3B`) (observed).
- Timing values differ (observed). The horizontal timing registers
  `00-05` are smaller and register `0E` (vertical blanking) is `1C`
  instead of `1A`. In the 24x80 format the frame is 553 lines, against 551
  on 3477 types 3/4. So the 3486 tube has different timing (hypothesis:
  this is not visible to the operator, and the user's photos show the same
  screen layout as a 3477).
- `2012F` is written `00` at reset (3477: `18`). Bit 1 is read as a sense
  input during display-type decoding (see the straps).
- New registers at `20140-2014F` (observed accesses):
  - `20140` (`02`), `20141`, `20149` are written with the timing block.
    `20141` is also written in many display mode changes (hypothesis:
    monitor power or blanking control, the Energy Star feature).
  - `20142` is written `FF` once.
  - `20144-20146` are a **second keyboard-style serial port** with the same
    protocol as `20012-20014`. `20144` is the data byte. `20145` is
    line control: written `02` released, `03` held. Its read bits are
    bit 0 parity, bit 1 busy, bit 2 byte received (as on the keyboard
    port; observed: the firmware's receive handler tests bit 2), and bit 7,
    see below.
    `20146` is written `FF` to clear. The firmware runs a keyboard-type
    exchange on it. It takes the pointing device (see below). The model has
    the port (`src/devices/ibm3486/aux_port.*`) with its wrap path and
    interrupt, enabled by `20007` bit 1, and no device on it: a byte sent
    with the line released gets no answer.

### `20145` bit 7: interrupt layout

The firmware reads `20145` bit 7 in the keyboard, Twinax and interrupt
code and switches layout on it (observed):

| Bit 7 | Keyboard (`20012`) | Second port (`20144`) | Third Twinax channel |
| --- | --- | --- | --- |
| 1 | vector `0Eh` (as the 3477) | vector `0Dh` | not set up |
| 0 | vector `0Fh` | vector `0Eh` | set up (descriptor `01A0`, programs `0200-027F`, `2000D/F`, `2001A`) |

Which value the board gives is **unknown**. It may be a board-revision or
product (3486/3487) input. The model's second port reads bit 7 as 0, and
the interrupt controller has the layout the firmware then uses. It is read
off the vectors on which the running firmware installs handlers (observed
after POST, one display configured; the others point at an `IRET` stub):

| Vector | Source | 3477 |
| --- | --- | --- |
| `06h` | timer | `06h` |
| `07h`, `08h`, `09h` | Twinax event, channels 0, 1, 2 | `07h`, `08h` |
| `0Ah`, `0Bh`, `0Ch` | Twinax status, channels 0, 1, 2 | `09h`, `0Ah` |
| `0Dh` | display | `0Ch` |
| `0Eh` | second serial port | - |
| `0Fh` | keyboard | `0Eh` |

`06h`, `07h`, `0Ah`, `0Dh`, `0Eh` and `0Fh` have handlers and are delivered
in emulation (observed); `08h`, `09h`, `0Bh` and `0Ch` follow from the
pattern (hypothesis: no second or third session has been configured yet).
With the display on the 3477's `0Ch` the firmware waited for ever for a
display block request to finish while the interrupt landed on the stub
(observed). With bit 7 forced to 1 in an earlier private experiment on
`54G0011`, the keyboard interface completed the reset/ID handshake on
`0Eh` (`FF` -> `FA`, `AA`, `BF BF`) (observed).

## Twinax

- Same engine design: command-class table at `0060`, channel 0/1 programs
  at `0080-017F`, station addresses in `20008` (observed).
- A **third station channel** exists in the firmware (observed): programs
  at `0200-027F`, descriptor at `01A0`, station address in `2001A`.
  Station-address selection (observed): three addresses are configured.
  Depending on a setup mode, channel 1 gets the second or the third
  address, and the third goes to `2001A`. Hypothesis: two display sessions
  plus one printer session, each on its own channel.
- Registers of the third channel (observed in POST's loop-back test, which
  runs every sub-test on all three channels): station address `2001A`,
  enable `2001B` bit 0 (written `06` with the channel off and `07` with it
  on; bits 1-2 unknown), channel control `2000B` (bit 7 drops busy, as
  `20000/1`), request register `2000D` (write 1 to clear) and mask `2000F`.
  Its programs are at `0200-027F`, not at the stride of the first two.
- Frame generator: for the third channel the test writes `42` to `20025`
  where it writes `06` and `0B` for the first two, and expects a two-frame
  message each time. The model sends two frames for bit 6 as for bits 2
  and 3 (hypothesis; the register's real layout is not known). `2002A` is
  written with the second frame's data like `20028`; no modelled effect.
- The model has the three channels; the third one's requests interrupt on
  `09h`/`0Ch` (see the layout above). Offline Setup offers the terminal
  modes One-Display, Two-Display, Display-Printer and Two-Display-Printer
  (observed), so the third channel is the printer next to two displays.
  The emulator still offers two sessions (two displays, or display +
  printer), as on the 3477.

## Display test

POST's `VDO` test is the 3477's (see docs/bringup-3477.md, "Display test and
the sense lines"), with these differences (observed):

- The selector is `2012F` bits 5-4 (3477: bits 6-5); values `01`, `10` and
  `11` are compared, `00` only has to read non-zero.
- Timing windows: after the measurement is started at the beginning of
  vertical retrace, the retrace flag (bit 3) must change within 3 to 9
  status reads (3477: 10 to 31), and the next retrace must begin after 384
  to 1439 reads. The model's retrace is 1/140 of the frame, about 120 us,
  which gives 6 reads (hypothesis: the real length is unknown; the
  3477's 1/90 gives 10).
- The firmware expects the retrace flag inverted on the tubes for which it
  sets `20128` bit 3 (types 3, 4 and 5; clear on type 2). The model inverts
  bit 3 of `2012F` when `20128` bit 3 is set (hypothesis: a sync polarity
  select).
- The values it requires differ per tube. The temporary signature shim has
  them for strap codes `42`, `4A`, `48` and `4C`, keyed on the frame
  fingerprint as for the 3477. On colour (`4C`) all three selectors are
  compared on the first screen, which fits one selector per colour signal
  (hypothesis). Type 1 (`42` with `2012F` bit 1 set) and
  type 7 (`44`) are not calibrated.

## EEPROM and monitor alignment

Same I2C bus and EEPROM as the 3477 (`A0/A1`). What the firmware does with
it at power-on (observed on colour, type 6, once the setup is valid; the
mono types made no EEPROM access in the same boots):

- It reads byte `00`. If that is not `AA` it writes bytes `00-37` with its
  default alignment values and then `AA` to `00`.
- POST's `FOS` step then reads a list of bytes and requires `AA` at `00`
  and `AA` at `30`. Its own defaults put `55` at `30`.
- With `55` there the unit does not go on line: it shows a grid of dots
  over the whole screen with `D` in the status line and stays there
  (hypothesis: `30` is "monitor aligned", `55` = not yet, and the grid is
  the alignment pattern a factory or service technician would work
  through). Enter, F3, Space and F12 did nothing visible; how the procedure
  is driven and completed from the keyboard is not known.
- With byte `30` changed to `AA` by hand the same unit starts normally and
  goes on line (observed).

More about when the firmware looks (observed):

- It reads the EEPROM once per power-on. In the start in which it wrote
  its defaults it shows the pattern even when byte `30` is `AA` by the time
  it reads it back (the same 16 reads with the same values as in a start
  that goes on line); with `30` = `AA` it goes on line from the next
  power-on.
- Marking a blank EEPROM beforehand does not help: with `00` = `AA`, `30` =
  `AA` and the other bytes blank (`FF` or `00`) the firmware reads `00` and
  `01` and writes its defaults again.

**The alignment stand-in (temporary).** A colour unit (display type 6) is
set up with its alignment marked done, since there is no tube to align and
the procedure is not known (`src/devices/ibm3486/alignment_shim.hpp`):
whenever the EEPROM holds the firmware's "initialised" mark (`00` = `AA`),
the model makes byte `30` `AA`; when that happens while the firmware runs
(a new unit's first power-on) it switches the unit off and on once, so the
first start runs the self test twice and then goes on line (observed:
`--model 3487 --display color` with a saved setup and no EEPROM file goes
on line under `--twinax-host wsc`). The alignment values stay the
firmware's own; the emulator ships none. `--no-alignment-shim`
(`[emulator] alignment_shim = false`) switches it off; the unit then shows
the pattern as above, and `eeprom set 30 aa` in the monitor followed by a
restart does by hand what the stand-in does. To be replaced by the real
procedure once it is understood.

## Video enable and the tube bits of `20128`

- `20128` bits 4-3 are constants of the tube (observed): the firmware ORs
  a per-type value into every write, `10` on type 2, `18` on types 3-5 and
  `00` on type 6 (colour). Bit 3 inverts the retrace flag (above). What
  bit 4 does is unknown. On the 3477 model bit 4 is "video on"; that cannot
  hold here, since colour never sets it and shows a picture on a real
  unit.
- The firmware writes `20128` without bit 6 while it changes the display
  mode and with bit 6 when the screen is ready (observed on all types).
  The model shows the picture while bit 6 is set (hypothesis). The 3477
  model reads the same bit as the rule above the status line; the rule is
  still drawn from it here.
- `20140`, `20141` and `20149` are written with the timing block and differ
  per tube (observed: `01 40 08` on types 2-4, `02 00 00` on type 5), so
  they are timing or monitor settings, not a blanking control.

## Display subtype and straps

Without `--straps`, the model picks the strap code from `--display`:
`color` -> `4C` (3487 HC0, type 6), `paperwhite` -> `48` (3487 HW0,
type 5); `green` and `amber` -> `42` (3486, type 2) on model 3486 and
`4A` (3487 HG0/HA0, types 3/4) on model 3487. Changing the subtype of a
running terminal power-cycles it when the straps change. Colour cells are drawn with the 5250
colour decoding of the 3477 model (hypothesis); only the single-colour
setup screen has been seen so far, so the colours are unverified.

## Battery-backed RAM and EEPROM

- Not yet established for the 3486. The setup block moved to `0280`. The
  model keeps the whole RAM in the `--nvram` image (384 KiB) and the I2C
  EEPROM at `A0` as on the 3477 (hypothesis).
- The IBM 3489 maintenance manual (SY18-2345, a later member of the
  family) documents a "ROS patch" utility that stores patch data in NVM
  (documented for the 3489). The checksummed block at `469E0` (length word,
  data, sum `619A`) is the 3486's patch area (observed: the patch code
  validates it, refills it from the `60000` option ROM or a default record,
  and applies it; see "Where the missing half comes from").

## Where the missing half comes from

The ROM chip `54G0011` is complete (256 KiB) and appears only where CPU
A17 = 1. The firmware also needs `8000`, `9000`, `C000` and `D000`. Findings:

- **One image, not a download** (observed). The data tables in `A000` hold
  absolute far pointers into `8000`, `9000` and `C000` (18 tables in three
  sets of the same sizes, reaching up to `8000:DB26`, `9000:F9ED`,
  `C000:E56E`). POST sums `80000-FFFFF` as one image against one target.
  No code in the chip writes, copies, decompresses or downloads anything into
  `80000-DFFFF`, and no code in the chip reads the `A000` tables, so their
  users are in the missing half too.
- **No presence detection** (observed). The call into `D000` at the end of
  early POST is unconditional; no code reads the missing range to look for a
  signature or header; no strap or register bit gates it. In emulation, after
  the checksum is skipped, nothing touches `80000-9FFFF`/`C0000-DFFFF` until
  the CPU fetches the first `D000` instruction. The only optional region the
  firmware detects is the `60000` ROM (strap `2000A` bit 6), which is the
  source of ROS patches (below).
- **Not battery-backed RAM** (observed by size and order). The referenced
  ranges need about 200 KiB or more. The board has 160 KiB of SRAM, which
  also holds the working RAM, and POST sums the region before any code could
  have loaded it.
- **Service table** (observed). Most calls between modules go through a table
  of about 1030 far pointers in RAM (`3800:2F80`, entry *n* at `2F7C + 4n`).
  Nothing in the chip fills it (checked with a write watchpoint up to the
  `D000` call); the first `D000` routine (`D000:13B6`) is presumably the
  system init that installs it (hypothesis, strong: POST calls through the
  table right after it returns).
- **ROS patches** (observed). Patch records come from the `60000` option
  ROM, keyed by the firmware level, or from a checksummed RAM block at
  `469E0` (at most 24 KiB). They copy code into RAM and replace individual
  service-table entries. They cannot replace the missing half.
- **Leading hypothesis: a 256 KiB ROM on a card in J5**, decoded for
  A19 = 1, A17 = 0. J5 is the only part of the board that can carry it. This
  conflicts with the owner's understanding that J5 was never used, so it must
  be confirmed: whether the terminal ever ran with J5 empty, and by
  continuity from the gate array's ROM select and the bus lines to J5.
- **Without the half** (observed): POST sums the ROM, gets `DBE9` instead
  of `619A`, writes alarm code `9B`, beeps twice and halts with interrupts
  off. Only an NMI could resume it, and no interrupt vector has been set up
  by then. The skip flag (`F000:0003` = `E13C`) is not set in this ROM, and
  no boot mode avoids the check.

What a 3486 needs (hypothesis, from the references): `D000:13B6`, a far
routine entered with `SS = DS = 3800`, `SP = 2F7C`, that installs the
service table and returns; `D000:143D`, the non-returning main-loop entry;
the resource data behind the `A000` tables; the control program, keyboard and
5250 handling; and the fonts (none are in the chip). The four segments
must word-sum to `85B1`.

An emulator-only stub of that shape would not boot the terminal: which
service index maps to which routine is fixed by the missing install list. At
most a temporary trap stub could log the services POST calls.

The IBM 3489's dump (`3489emul`) has a similar gap (two of four quarters),
but there the chip is marked 8 Mbit and the gap is best explained by the
read. The same family conventions appear there (RAM service table, `619A`
sums, a 50-pin expansion connector). The `E13C` word that the 3489 code
compares is the same checksum-skip flag as on the 3486, not a required
signature.

## Boot progress

Observed with `--model 3486` and `54g0010.bin`:

1. Reset at `FFFF0` -> `F000:0000`: gate array masked, alarm latch `9B`.
2. ROM checksum: POST sums segments `8000` to `F000`, gets `619A` and
   goes on.
3. POST resets the keyboard. The interrupts arrive on vector `0Fh` and the
   firmware's handler takes them.
4. POST runs its test table. The state block at `0500` holds the current
   test's three-letter EBCDIC name at `+20` and, after a failure, an error
   record at `+24..+2F`; a fatal failure ends in `HLT` at `F000:0889` with
   interrupts off. The tests that needed hardware the 3477 does not have:
   - `COM` (Twinax loop-back): four phases, each run on **three** channels.
     Needs the third channel (see [Twinax](#twinax)).
   - `VDO` (display): the 3477's video-signature test with other timing
     windows and selector bits (see [Display test](#display-test)).
   - `MWP`: the manual-clock wrap test of the second serial port (see
     `20144-20146` above); the keyboard port has the same test.
5. POST completes. With an empty battery-backed RAM the terminal shows
   **Offline Setup** ("Selections have been reset to the IBM default
   values") with `SETUP` in the status line (observed with straps `42`,
   `4A`, `48` and `4C`).

After POST (observed on the 3487 model, straps `4A`, and on colour, `4C`):

6. Offline Setup works from the keyboard. A new unit needs two things
   before it leaves setup: a station address (Customize workstation: cursor
   down to the address row, Space on an address, F3 = Save and Exit) and a
   serial number (Update serial number: Space on a character in each of the
   nine rows, F3 = Save and Exit). F3 on the menu then leaves setup. With
   `--nvram FILE` the next start skips setup.
7. With a saved setup the terminal shows the on-line screen (empty, rule
   and status line with the cursor position). `--twinax-host wsc` finds the
   station at its address, identifies it (display, model ID `24` on mono
   and `50` on colour) and polls it; typed characters are echoed.
8. With the TN5250 bridge the host's screen is displayed (observed against
   an OS/400 sign-on over TN5250, terminal type `IBM-3179-2`). Not checked:
   signing on, the cursor position the host asks for (the status line showed
   `1/1` on the sign-on screen), 27x132, a second session, a printer
   session.

Colour needs one more thing, see "EEPROM and monitor alignment".

Not working (observed): `--straps 44` (type 7, 3488) does not start, and
not because of the model. See "The 3488 strap code" below.

### The 3488 strap code (type 7)

`54G0010` on its own cannot run as a 3488 (observed):

- The strap decoder accepts `010` as type 7, family 3, and the VPD model
  table has a `3488` entry, but its suffix is the placeholder `XXX`.
- During system start-up a routine in segment `D000` installs a
  per-tube list of service-table entries. It selects the list by display
  type: one list for types 1-2, one for 3-5, one for 6. For any other type
  it selects nothing and still runs the installer, with the list pointer
  left at whatever the previous routine put in `DS:SI`.
- That value does not come from the hardware. The previous routine loads
  `SI` with a constant and restores the caller's `DS`, so the pointer is
  `D000:0025`, in the middle of code. The installer reads code bytes as a
  list, overwrites service-table entries (the first one included) with
  them and the CPU soon runs through a bad pointer into RAM. No register of
  the gate array or display controller is read on the way that could change
  this, so a real board with only this ROM would do the same (hypothesis:
  not tested on hardware). The one input that could is the patch area: the
  previous routine looks at the patch block in RAM at `469E0`, which is
  empty here.

So a 3488 needs code this image does not have. Hypothesis: the 3488 logic
unit carries the optional ROM at `60000-7FFFF` (strap bit 6 = 0), whose
patch records replace service-table entries by firmware level and could
supply the type 7 case; or it has a ROM of its own. Either needs a dump
from a 3488. The model warns when the straps select type 7.

### With the 3486's own chip

`54G0011` placed as `54g0010.bin` and loaded with `--allow-bad-rom` is
mapped as its four 64 KiB blocks at their linked segments (`A000`, `B000`,
`E000`, `F000`); the others stay unmapped. Observed before `54G0010` was
available: the first read of segment `8000` by the ROM checksum is an
unmapped access, so the machine stops there; with `policy log` the sum is
`DBE9`, POST beeps and halts. With the checksum skipped by hand POST
decodes the straps and the display type, waits for vertical retrace, loads
the display timing and mode blocks and `20140/41/49`, resets the keyboard
and reaches `call D000:13B6`, the first call into a missing segment.

## Pointing device and the enhanced user interface (observed by the owner)

The second port takes a **PS/2 mouse or a light pen**. With a mouse plugged
in before power-on, the terminal shows a mouse pointer, has clickable
hotspots, and menus can be driven with the mouse. The 5250 "enhanced"
windowing elements (windows, selection fields, scroll bars and the like,
sent by the host as Write Data Structured Field orders) also work with it.

Consequences for the emulator:
- the second port at `20144-20146` needs a PS/2 mouse device model
  (standard PS/2 mouse protocol on the same serial interface as the
  keyboard), fed from host mouse events; detection happens at power-on;
- the `20145` bit 7 interrupt layout switch is probably tied to whether a
  pointing device is present (hypothesis);
- the TN5250 engine currently parses and skips Write Data Structured
  Field; the enhanced interface needs it implemented, and the Query Reply
  must only claim it for a terminal that supports it.

### The mouse in emulation (experimental, `--mouse`)

**Experimental.** The mouse is off unless `--mouse` is given, and no menu or
configuration file turns it on. What works: the firmware finds the mouse,
sets it up and moves its pointer. What does not: clicks have no effect yet,
with or without a host; only the SDL front end forwards the host's mouse,
and it neither hides nor confines the host pointer. The option is there for
bring-up work and may change.

A standard two-button PS/2 mouse model (`src/input/ps2_mouse.*`) on the
second port makes the firmware's pointer work (observed, 3487 mono, on
line with `--twinax-host wsc`):

- At power-on the firmware reads the mouse's `AA 00`, asks for its ID
  (`F2`, seven times) and status (`E9`), resets it (`FF`), then sends
  `E8 00`, `E6` three times and `E9` (a model query), `EA`, `F3 28` (40
  reports a second), `E8 03` and `F4` (enable). After that it sends `F4`
  about every tenth of a second (hypothesis: a presence check, like `EE` to
  the keyboard).
- The port behaves like the keyboard port: bit 2 of `20145` is "byte
  received", the end of a transmission interrupts, and the firmware
  discards that interrupt.
- The firmware starts its reset by holding the line while an earlier byte
  (`00`, from a state variable that is still zero) is on its way, and then
  reads "not busy". The model drops a byte when the line is held during its
  transmission (hypothesis). Without that the `00` reached the mouse, the
  mouse answered `FE` and the firmware gave up on the pointer.
- The first movement only wakes the screen. After that the firmware keeps a
  position in pixels and cells (starting at the top left, clamped to the
  screen) and draws the pointer itself in display memory, at the cell under
  it; no display register is involved. In the emulator it shows as a solid
  block; what a real tube shows there is not known.
- What the terminal reports to the controller, and how the built-in
  controller switches button events on, is in `docs/twinax-bridge.md`,
  "Pointing device". With `--twinax-host wsc` a left press moves the local
  screen's cursor to the pointer; with the TN5250 bridge the click is
  processed as in `docs/tn5250.md`, "Pointing device".

`mouse DX DY [BUTTONS]` in the monitor moves it. The SDL front end forwards
the host's mouse (relative movement and three buttons) without hiding or
confining the host pointer; the other front ends do not forward it yet.

## Checklist for the complete dump

1. Done with `54g0010.bin`: 524288 bytes, no invariant address bit, word
   sum `619A`, SHA-256 recorded in `roms/ROMS.md` and in `info_.roms`. Its
   `A000/B000/E000/F000` content was not compared with `54G0011`'s (another
   firmware level; the sums differ). The mapping path for the 3486's chip
   is kept.
2. Done: the model status is `Bringup`.
3. RAM size: find the POST RAM test (probably in `C000/D000`) and the
   highest address it touches; adjust `kRamSize`. Check whether
   `60000-7FFFF` is probed as RAM or only as the option ROM.
4. `20145` bit 7: the model takes the bit-7-is-0 layout for the keyboard
   (vector `0Fh`). Still to decide from the code in `8000/9000/C000/D000`
   (or from the unit) whether that is what the board gives. The second port
   on `0Eh`, the third Twinax channel and the whole three-channel vector
   layout are in.
5. Fonts: no 3477-format compressed font plane is present in the surviving
   segments (observed). Look for them in `8000/9000/C000/D000`, and check
   the character-generator RAM location (`18000-1FFFF` on the 3477; this
   firmware references `1800:0000` too).
6. Display: confirm the `20140-2014F` registers (power saving/blanking),
   the VPD model-number indexing, and how `2012F` bit 1 differs between
   types 1 and 2.
7. POST tests: list the 3486 POST test table and its display test.
   Check whether the video-signature test of the 3477 (`2012F` bits 7-4)
   is still there.
8. Setup: locate the setup block validation (block at `0280`) and the
   battery-backed range; check the EEPROM use.
9. Compare against the real unit: the "3486 Control Program" logo screen
   and the sign-on screen with the status line (owner's photos).
10. Sessions: confirm two display sessions and the printer session on a
    real 3486 (Read Info VPD, Read Device ID model ID).
