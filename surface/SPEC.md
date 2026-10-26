# LMS Rig control surface — MIDI / SysEx contract (v1)

Two USB-MIDI class-compliant boxes. Hardware does no DSP and holds no rig state beyond
what the host last told it; the host (LMS Rig) is the source of truth and echoes every
change back. USB product strings: **`LMS Head`**, **`LMS Floor`** (the host finds its
outputs by these names). Every value a box sends is **absolute**, so duplicates are harmless.

| Box | MIDI ch | Status bytes | SysEx box id |
|---|---|---|---|
| HEAD  | 1 | `B0` CC | `01` |
| FLOOR | 2 | `B1` CC | `02` |

## Control Change map

**14-bit** = MSB on CC *n*, LSB on CC *n*+32, 0–16383 over the full slider range. A sender
always sends MSB then LSB. A receiver applies on MSB (with LSB=0) and refines on LSB, so a
7-bit-only controller still works. **Switch** = 7-bit, `0` off / `127` on.
**Index** = 7-bit selection number, 0-based.

| CC | Box | Kind | Box → host | Host → box |
|---|---|---|---|---|
| 20/52 … 25/57 | HEAD | 14-bit | fixed pots: GAIN, BASS, MID, TREBLE, PRESENCE, MASTER | current value = pickup target |
| 26/58 … 28/60 | HEAD | 14-bit | soft encoders 0–2 (absolute; the box accumulates) | current value (re-seeds the accumulator) |
| 80 … 83 | HEAD | switch | soft buttons 0–3 pressed: the new state wanted | LED state |
| 85 / 86 / 87 | HEAD | switch | push on AMP / CAB A / CAB B encoder (127 on press) | — |
| 102 / 103 / 104 | HEAD | index | AMP head / CAB A model / CAB B model wanted | current selection |
| 11/43 | FLOOR | 14-bit | expression pedal (calibrated, polarity applied) | — |
| 20/52 … 23/55 | FLOOR | 14-bit | soft encoders 0–3 (the focused effect's params) | current value |
| 80 … 87 | FLOOR | switch | footswitch *s*: the new on/off wanted for the section's selected effect | LED state |
| 88 … 95 | FLOOR | switch | push on section *s* encoder (127): focus *s*, next param page if already focused | — |
| 102 … 109 | FLOOR | index | section *s*: the effect wanted | current selection |
| 110 | FLOOR | switch | tuner on/off wanted (long-press any footswitch) | tuner state |
| 111 | FLOOR | switch | Master Bypass wanted (long-press section 0's encoder) | state |

The host doesn't echo a value straight back to a control while that control is still moving
(a 300 ms hold per CC). Turning a section encoder also focuses that section.

**Soft takeover is in the firmware.** A pot sends nothing until its physical position crosses
the host's last value for that CC (a window of ±2 % counts as crossing). Until then the LCD shows
`^`/`v` and which way to turn. Swapping an amp or loading a preset sends new targets, which
re-arms pickup.

## SysEx: `F0 7D <box> <msg> <slot> <data…> F7`

`7D` is the non-commercial manufacturer id. `data` is ASCII 0x20–0x7E, at most 20 bytes, and is
truncated by the receiver to fit its field.

| msg | Dir | slot | data |
|---|---|---|---|
| `01` LABEL | host → box | control slot | short name (`Gain`, `Mic Dist`) |
| `02` VALUE | host → box | control slot | formatted value (`48.8%`, `-12.0 dB`, `4x12 V30`) |
| `03` RANGE | host → box | control slot | 2 bytes: step count `hi7 lo7`. 0 = continuous; *n* = enum with *n* steps (one detent per step) |
| `04` POPUP | host → box | 0 | message shown for 1.5 s (`SAVED`, `NO CAB B`) |
| `05` TUNER | host → FLOOR | MIDI note (0 = no pitch) | 2 bytes: `cents+64`, flags (bit0 tuner on, bit1 output muted) |
| `10` HELLO | box → host | firmware version | `HEAD` or `FLOOR` |
| `12` PING | host → box | 0 | — (the box answers with HELLO) |
| `13` BEAT | host → box | 0 | heartbeat, once a second; 3 s without one and the box shows "waiting for the rig" |

The host sends a full dump (every LABEL, VALUE, RANGE and CC) when a box says HELLO and when its
output port appears. After that it sends only what changed.

### Slots

| Slot | HEAD | FLOOR |
|---|---|---|
| `00`–`05` | fixed pots (label = the bound slider's name on this amp, `--` if none) | section 0–5: LABEL = section name, VALUE = selected effect |
| `08`–`0A` | soft encoders 0–2 | — |
| `10`–`13` | soft buttons 0–3 (button 3 is always PAGE) | soft encoders 0–3 |
| `18` / `19` / `1A` | AMP / CAB A / CAB B (VALUE = name, RANGE = count) | expression (label/value of its target) |
| `20` | page (`2/5`) | focus (`DRIVE: GREEN SIN 1/2`) |
| `21` | channel (VALUE `Dirty`) | — |

## Host-side mapping

- **Fixed pots** bind by slider *name* through the map, so they survive head swaps:
  - GAIN `Gain|Pre Gain`
  - BASS `Bass|Low`
  - MID `Mid|Tone`
  - TREBLE `Treble|High`
  - PRESENCE `Presence|Tone Cut|Resonance`
  - MASTER `@master`: the master of the selected channel (`<Channel> Master`), else `Master`, else `Output Level`.
- **Head soft pages.** The amp's remaining sliders, minus the fixed six and the two cab models:
  - continuous sliders fill the encoders 3 per page;
  - switches and enums fill buttons 0–2, 3 per page, with Channel first; a press cycles an enum;
  - button 3 = PAGE.
- **Floor sections** (the default table; the box only knows the count):

  | Section | Effects |
  |---|---|
  | 0 DYN | In Comp, Out Comp |
  | 1 PITCH | 12 STEPPER |
  | 2 DRIVE | DOA666, HARDWARE INCOMPATIBLE, MARMOT, GREEN SIN, GOLDEN CALF |
  | 3 MOD | BLACK MASS, PIPEWORKS |
  | 4 TIME | THE ELEPHANT, UNHOLY BOWL |
  | 5 EQ | JUDGEMENT DAY |

  A footswitch toggles its section's selected effect. LED = that effect's On.
- **Pedal board sliders.** Slider indices are 1-based (the JSFX `sliderN`); `ysfx` uses N−1.

  | Effect | On | Params |
  |---|---|---|
  | In Comp | s40 | s41–45 |
  | Out Comp | s50 | s51–55 |
  | 12 STEPPER | s37 | s61–64 |
  | DOA666 | s5 | s6–9 |
  | HARDWARE INCOMPATIBLE | s10 | s11–14 |
  | MARMOT | s4 | s57–59 |
  | GREEN SIN | s38 | s66–68 |
  | GOLDEN CALF | s46 | s47–49 |
  | BLACK MASS | s15 | s16–19 |
  | PIPEWORKS | s26 | s27–31 |
  | THE ELEPHANT | s20 | s21–25 |
  | UNHOLY BOWL | s32 | s33–35 |
  | JUDGEMENT DAY | s39 | s71–76 |

  Section Order is s36 and Master Bypass is s3. The host binds these by name (the pedal's On
  slider is named after it; its params share a prefix such as `GS `), so renumbering the JSFX
  doesn't break the surface.
- **The map** (persisted in the rig state, i.e. `~/.config/LMS Rig.settings`) is a list of
  `(ch, cc, 14-bit?, plugin, slider-name-alternatives, lo, hi)`. The defaults are the fixed
  pots plus expression → pedal board `Output Trim (dB)` from −24 to 0 (a volume pedal). Any other
  controller can be added to it; everything outside the map is the fixed protocol above.
