# KALI SMASH

Alternative firmware for the dsp.coffee **Kali** Eurorack module (Electrosmith Daisy Patch SM inside). It turns Kali into a stereo end-of-chain mix-buss destroyer that runs from transparent to completely wrecked:

```
IN L/R
 └ MS-20 style HPF
    ├ dry branch (latency-matched, filtered too when FILTERS = ALL)
    └ [MOTION, RESO when set to PRE]
       └ DRIVE A ─ DRIVE B   (4x oversampled, 15 types each)
          └ LPF
             └ [MOTION, RESO when set to POST]
 MIX dry/wet ──┘
    └ SQUASH: gain in > compressor > master clipper
       └ OUT L/R
```

Plus: six CV outs that follow the music (envelopes, transients, LFOs, chaos), two clock outs that work as LED level meters or gates, SMASH and BYPASS on the two buttons, autosave, and 8 preset slots.

**Status:** phase 4 test3. Every block is in and has been played on hardware. Phase 5 (KNOB MAP, polish) is next.

## Contents

- [Installing](#installing)
- [The panel](#the-panel)
- [Encoders and screen](#encoders-and-screen)
- [Pages and settings](#pages-and-settings)
  - [HOME](#home)
  - [HPF](#hpf)
  - [DRIVE A / DRIVE B](#drive-a--drive-b)
  - [LPF](#lpf)
  - [MOTION](#motion)
  - [RESO](#reso)
  - [MIX](#mix)
  - [SQUASH](#squash)
  - [CV OUTS](#cv-outs)
  - [OUT SETUP](#out-setup)
  - [BUTTONS](#buttons)
  - [PRESETS](#presets)
  - [SYSTEM](#system)
- [How CV in works](#how-cv-in-works)
- [Saving](#saving)
- [Building](#building)
- [Offline harness](#offline-harness)
- [Repo layout](#repo-layout)
- [Design notes](#design-notes)
- [Credits and licences](#credits-and-licences)

## Installing

KALI SMASH runs from RAM under the **Daisy bootloader** (v6.4, built-in USB, 2000 ms), the same arrangement as Kali's own 2.23 beta. `tools/flash-kali.sh` does the fiddly part: it watches USB and flashes inside the bootloader's 2-second window, so you never have to time a button press.

Needs macOS with `dfu-util` (`brew install dfu-util`) and a USB **data** cable. Plug Kali straight into the computer: a hub dropped a write once.

**First install** (from stock Kali or anything older):

```bash
tools/flash-kali.sh --first release/KaliSmash-phase4-test3.bin
```

When it asks: hold **BOOT**, press and release **RESET**, release BOOT. It installs the bootloader, catches it as it starts and installs KALI SMASH.

**Updates:**

```bash
tools/flash-kali.sh release/KaliSmash-phase4-test3.bin
```

While it waits, press **RESET** on the Daisy board (no BOOT). SYSTEM → UPDATE → GO on the module is the button-free route.

**Without the script:** flash.daisy.audio, File Upload, choose the `.bin`, pick the **Daisy Bootloader** device (not "DFU in FS Mode").

**Back to stock:** flash Kali's v2.22 image the factory way (hold BOOT, tap RESET, File Upload, "DFU in FS Mode"). That also removes the Daisy bootloader.

## The panel

| Control | KALI SMASH | Fully CCW | Fully CW |
|---|---|---|---|
| Knob 1 | **HPF** cutoff | off (10 Hz) | 16 kHz, then closed (silent) in the last 3% |
| Knob 2 | **DRV A** amount | clean bypass | +36 dB into the curve (+48 dB on FUZZ, DIST, METAL) |
| Knob 3 | **SHP A**, Drive A's character | see each type | |
| Knob 4 | **DRV B** amount | clean bypass | as DRV A |
| Knob 5 | **SHP B** | | |
| Knob 6 (CUTOFF) | **LPF** cutoff | closed (silent) | off (22 kHz) |
| Knob 7 (FEEDBACK) | **MOTION** | off | first half: depth and wet to full. Second half: feedback to max |
| Knob 8 | **MIX** | dry | wet |
| Mini left (LFO ADJ) | **RESO** | off | first half: fades in. Second half: decay up; the top self-oscillates |
| Mini right (DELAY ADJ) | **SQUASH** | transparent | +24 dB into the compressor and clipper |
| FREEZE button + gate | **SMASH** (default, while held) | | |
| SYNC button + gate | **BYPASS** (default, press to toggle) | | |
| CV outs 1-6 | music-following CV, see [CV OUTS](#cv-outs) | | |
| Clock L / R | LED level meters, or gates | | |

Turning any knob switches the screen to that knob's page, and it stays there.

## Encoders and screen

| | Turn | Click | Hold |
|---|---|---|---|
| **Left** | change page | jump to the page's first setting (TYPE) | back to HOME |
| **Right** | change the selected setting (marked `>`) | next setting | reset that setting to its default |

Each page shows its title and type, a live line (knob values, cutoff in Hz, volts on the CV outs...), then the selected setting and the one after it. Settings that do something once (UPDATE, LOAD, SAVE, INIT ALL) step `-` `>` `>>` `GO`: three turns, so they can't fire by accident.

## Pages and settings

### HOME

```
A TANH  42 B FOLD   0      drive types and DRV amounts
HP OFF     LP 8.2k         filter cutoffs
M FLANG 35 R COMB  20      MOTION and RESO types and amounts
MIX100 SQ 30 ##... 41      mix, SQUASH, BITE bar, CPU %
```

While a button action is on, the last line shows it instead (`>> SMASH`, `>> BYPASS`...).

**BITE** is how hard the drives are bending the signal right now.

### HPF

An MS-20 style high-pass. The resonance feeds back through an asymmetric diode-style clipper, so it howls, and driving it squashes and dirties the resonance the way a pushed MS-20 does. The input audio can also modulate its own cutoff at audio rate (AUDIO FM), for gritty, unstable filtering of a whole mix.

| Setting | Range | What it does |
|---|---|---|
| RES | 0-100% | Resonance. Self-oscillates from about 80% (pitch follows the cutoff); full RES howls at about 0.9 of full scale |
| DRIVE | 0-100% | 0 to +24 dB into the filter; the output soft-clips as it rises |
| AUDIO FM | 0-100% | the input audio into the cutoff, at audio rate: up to ±6 octaves per full-scale swing (capped at ±5). Gritty sidebands, and the cutoff jumps with every hit. The output soft-clip comes in with it |
| FM SRC | SELF / CROSS / MONO | SELF: L audio moves the L cutoff, R moves R, so any imbalance in the mix pushes the two filters apart and the stereo field wobbles. CROSS: L moves R and R moves L, wilder. MONO: the sum moves both, stays centred |
| SPREAD | -100..+100 | L cutoff down and R up (or the reverse), up to 2 octaves each way, QPAS style |
| SLOPE | 12 / 24 dB | 24 adds a clean 2-pole section in front |
| FILTERS | ALL / WET | ALL: the dry path is filtered too (works like a DJ filter at any MIX). WET: only the effected signal |
| CV BASE | 0-100% | added to the knob, see [CV in](#how-cv-in-works) |

### DRIVE A / DRIVE B

Two drive stages in series, inside 4x linear-phase oversampling (37 samples of latency, matched on the dry path so blends never comb). DRV always gets louder as it goes up; nothing auto-compensates the level, and SQUASH catches the top.

| Setting | Range | What it does |
|---|---|---|
| TYPE | 16 choices | OFF plus the 15 below |
| OUTPUT | -12 to +24 dB | the stage's output level |
| TONE | -100..+100 | tilt after the curve: dark to bright (±4.5 dB around 800 Hz) |
| BIAS | 0-100% | extra asymmetry (even harmonics) |
| STEREO | LINK / MID / SIDE | drive both channels, only the centre, or only the sides (kick and bass stay clean, the width gets dirty) |
| STAGE MIX | 0-100% | parallel blend inside the stage |
| DRV BASE / SHP BASE | 0-100% | CV base for the two knobs |

| Type | Sound | SHP knob |
|---|---|---|
| TAPE | soft, compressing tape | wear: clean deck → worn cassette (pushed harder, head bump, top end down to ~3 kHz, wow and flutter) |
| TANH | classic smooth saturation | focus: CCW drives the lows (fat, darker), noon flat, CW drives the highs (fizzy, brighter) |
| TUBE | warm, even harmonics | bias and push: clean → one-sided → starved and spitting |
| SEAL | the Bribes v1.5 drive | push: up to +18 dB more into an ever harder knee |
| HARD | digital clip | asymmetry and push: symmetric → bottom half clipped nearly flat |
| FOLD | wavefolder | density: 1x to 6x more folding |
| FUZZ | transistor fuzz, +48 dB | starve: full fuzz → quiet parts gate out → sputtering |
| DIST | op-amp distortion pedal, +48 dB | filter: CCW tight and cutting, CW thick and dark |
| METAL | two high-gain stages, +48 dB | scoop: flat → -30 dB mids (180 Hz-3 kHz) |
| RECT | rectifier | plain clip → half-wave → full-wave (octave up) |
| WRAP | digital overflow: loud parts tear and flip | wrap point: full scale down to a quarter |
| XOR | bitwise against a delayed copy of itself | delay: 1 sample (fizz) to 5 ms (garbled) |
| VINT | old sampler: soft input, always 12-bit, DRV down to 3 bits | rate: ~26 kHz down to ~600 Hz, one soft pole after the hold |
| ULAW | companded like a phone line, quiet tails grainy; DRV smooth → 8-bit → one level per octave | rate: 48 kHz down to ~190 Hz |
| JITR | unstable clock and dithered levels, crackly; DRV 12 bits → 1 | rate, with the clock wobbling ±50% |

VINT, ULAW and JITR are the Bribes v1.27 crushers: zero-order hold, so low rates alias like a real low sample rate instead of sounding filtered.

### LPF

| Setting | Range | What it does |
|---|---|---|
| RES | 0-100% | resonance (clean SVF) |
| SLOPE | 12 / 24 dB | |
| CV BASE | 0-100% | |

Cutoff runs 20 Hz to 20 kHz; fully CW is off, and the last 3% at the bottom closes to silence.

### MOTION

| Setting | Range | What it does |
|---|---|---|
| TYPE | FLANGER, CHAOS, ENSEMBLE, PHASER, WARP | |
| RATE | 0.02-20 Hz | LFO speed |
| FEEDBACK | -100..+100 | base feedback; the knob's second half pushes it to ±0.97 |
| SPREAD | 0-180° | L/R phase offset |
| CHAOS | 0-100% | randomness (meaning per type, below) |
| POSITION | POST / PRE | after the LPF, or before Drive A (so the drives distort it) |
| CV BASE | 0-100% | |

| Type | Sound | CHAOS |
|---|---|---|
| FLANGER | wide stereo flanger, triangle sweep, 0.15-7 ms | random wander on the sweep |
| CHAOS | three chorus voices per side moved by a Lorenz attractor, never repeats | steady sines → full chaos |
| ENSEMBLE | string-machine ensemble: three taps, slow and fast wobble 120° apart | jitter on the wobble |
| PHASER | 6-stage stereo phaser, 150 Hz-3.4 kHz | |
| WARP | worn tape: wow, flutter, drift; fully wet it's the warped signal alone | how often it drops out and dulls |

### RESO

| Setting | Range | What it does |
|---|---|---|
| TYPE | COMB, CHORD, MODAL, FORMANT, KARPLUS | |
| PITCH | E0-D#8 (20 Hz-5 kHz) | shown as note and Hz |
| COLOR | 0-100% | per type, below |
| DAMP | 0-100% | the top end dies faster |
| DETUNE | 0-100% | 0-50 cents, L down and R up |
| POSITION | POST / PRE | |
| FREEZE | OFF / ON | cuts the input and holds whatever is ringing |

| Type | Sound | COLOR |
|---|---|---|
| COMB | one tuned feedback comb | below 50% HOLLOW (negative feedback, an octave down), above FULL |
| CHORD | three combs at chord intervals | OCT+5, MAJ, MIN, SUS4, MIN7, 5+9 |
| MODAL | ten tuned resonators, odd partials left, even right | STRING → stiff string → BAR → BELL |
| FORMANT | vowel filter; the knob's second half narrows it to a whistle | VOWEL A, E, I, O, U (PITCH shifts it) |
| KARPLUS | hits in the mix pluck a string under it | pluck brightness |

The top of the RESO knob goes past unity feedback: COMB, CHORD and KARPLUS sing on their own and MODAL rings for up to two minutes, while the input keeps coming through. Every loop runs through a soft clipper, so it sings instead of exploding.

### MIX

| Setting | Range | What it does |
|---|---|---|
| LAW | LINEAR / EQ POWER | linear has no level bump when the drives are clean |
| IN TRIM | ±12 dB | input level into everything |
| CV BASE | 0-100% | |

### SQUASH

The master section after MIX, so it acts on the blended signal, dry included. The knob pushes 0 to +24 dB into the compressor and then the clipper; the compressor's ratio ramps in over the first third. Near-silence is left alone (no gain pushed into the noise floor).

| Setting | Range | What it does |
|---|---|---|
| COMP | OFF / GLUE / SLAM | GLUE: -10 dB, 2.5:1, 10/150 ms. SLAM: -18 dB, 20:1, 0.5/60 ms. Both with static make-up |
| CLIP | SOFT / TANH / HARD / TAPE / FOLD | the master clipper's curve |
| CEILING | -12 to 0 dB | where the clipper tops out |
| OUT | -24 to +6 dB | final level |

### CV OUTS

Each of the six outs picks a source, a polarity and an offset. The live line shows the volts at each jack.

| Setting | Range | What it does |
|---|---|---|
| OUT n | source, below | |
| n POLARITY | POS / NEG / BIPOLAR | POS 0..+5 V, NEG 0..-5 V, BIPOLAR -5..+5 V |
| n OFFSET | -5.0 to +5.0 V | added after the polarity (clipped to ±5 V). With OUT set to OFF the jack just puts out the offset |

Defaults: 1 ENV LOW, 2 ENV FULL, 3 ENV HIGH, 4 BITE (all POS), 5 MOT LFO, 6 CHAOS X (BIPOLAR).

| Source | What it follows |
|---|---|
| ENV FULL / LOW / MID / HIGH / SIDE | envelope of the input: whole mix, below ~150 Hz (kick), 300 Hz-2 kHz, above ~4 kHz (hats, air), stereo difference. In dB over ENV RANGE, so quiet parts still move it |
| TRANSNT | a 150 ms decay fired by each hit |
| BITE | how hard the drives are clipping |
| SQ GR | SQUASH gain reduction (0-18 dB) |
| S&H | a new random value on each hit |
| MOT LFO | the MOTION LFO |
| FREE LFO | a sine at LFO RATE |
| CHAOS X / Y | a Lorenz attractor at LFO RATE |

Unipolar sources (envelopes, TRANSNT, BITE, SQ GR) use their 0..1 as is; bipolar ones are folded onto 0..1 for POS and NEG.

### OUT SETUP

| Setting | Range | What it does |
|---|---|---|
| CLOCK L / R | METER / TRANSNT / BITE / OFF | METER: the LED is a level meter (the jack carries the dimming pulses). TRANSNT: 10 ms pulse per hit. BITE: high while the drives clip hard |
| ENV REL | 10-500 ms | envelope release |
| ENV RANGE | 24 / 36 / 48 dB | how much level range the envelopes cover |
| HIT SENS | LOW / MED / HIGH | transient detector sensitivity |
| LFO RATE | 0.02-20 Hz | FREE LFO and chaos speed |
| METER | FAST / MED / SLOW | LED meter fall (15 / 40 / 120 ms) |
| METER SRC | INPUT / OUTPUT | what the meters follow |

### BUTTONS

Each button and its gate jack share one input, so a gate does exactly what the button does.

| Setting | Range | What it does |
|---|---|---|
| FREEZE DO | SMASH / BYPASS / KILL LOWS / MUTE | default SMASH |
| FREEZE | MOMENT / LATCH | on while held, or each press toggles. Default MOMENT |
| SYNC DO | as above | default BYPASS |
| SYNC | MOMENT / LATCH | default LATCH |

- **SMASH:** +18 dB into Drive A (and at least 35% DRV), Drive B to full, SQUASH to full.
- **BYPASS:** crossfades to the untouched input (latency-matched, still through SQUASH so levels compare).
- **KILL LOWS:** HPF up to at least ~250 Hz.
- **MUTE:** output off (smoothed).

### PRESETS

| Setting | What it does |
|---|---|
| SLOT | 1-8, shows SAVED or EMPTY |
| LOAD | turn to GO: loads the slot |
| SAVE | turn to GO: stores the current settings in the slot |

Presets store every page setting (types, page values, CV BASEs, output map, button actions). Knobs are live, so knob positions aren't stored.

### SYSTEM

| Setting | What it does |
|---|---|
| OVERSAMPLE | 4x (default), or OFF for aliasing grit and less CPU |
| KNOB POP | ON: turning a knob switches to its page |
| UPDATE | turn to GO: reboots into the Daisy bootloader and waits for a firmware upload |
| INIT ALL | turn to GO: every setting back to default |

The live line shows CPU load now and peak.

## How CV in works

On Kali each big knob is a passive attenuator on its CV jack, and the jack is normalled to +5 V. With nothing plugged in the knob works normally. With a cable in, the knob sets how much of the CV gets through; the knob and the CV can't simply add, because that's how the hardware is wired. The **CV BASE** setting on each page adds an offset, so a patched knob can move around a centre other than zero. A knob that keeps moving for more than 2 s (CV, not a hand) stops switching pages until it settles.

The two mini knobs (RESO, SQUASH) have no CV.

## Saving

Settings save themselves 3 seconds after your last change and come back at power-up. The current settings and the 8 preset slots live in the last 64 KB of the Patch SM's 8 MB QSPI flash (offset 0x7F0000), outside both regions the bootloader writes. A firmware with a different settings layout starts from defaults rather than loading mismatched data.

## Building

Needs [libDaisy](https://github.com/electro-smith/libDaisy) (built against commit `0808720`, Sept 2026) and the Arm GNU toolchain 14.2.

```bash
cd firmware
make LIBDAISY_DIR=/path/to/libDaisy GCC_PATH=/path/to/arm-none-eabi/bin
```

The output is `firmware/build/KaliSmash.bin`, a `BOOT_SRAM` app (runs from SRAM, stored in QSPI by the Daisy bootloader). Delay lines live in SDRAM. `hwtest/` is the hardware bring-up firmware used to confirm the pin map (a plain internal-flash app).

## Offline harness

`harness/render.cpp` runs the exact DSP code on a Mac, so sounds can be judged before flashing:

```bash
clang++ -std=c++17 -O2 -o harness/render harness/render.cpp
harness/render test                       # the test suite
harness/render make-loop loop.wav         # a stereo test loop
harness/render in.wav out.wav a.type=FUZZ a.amt=0.7 re=0.6 re.type=MODAL mo=0.5 mo.type=CHAOS
```

The tests cover the oversampler null (wet and dry line up to the sample), drive loudness never dropping as DRV rises, SHAPE making a clear difference on every type, HPF self-oscillation level and pitch, every MOTION and RESO type staying finite and bounded, the RESO freeze, BYPASS nulling against the clean input (-97 dB) and the output sources staying in range. The keys accepted on the command line are listed at the top of the file.

## Repo layout

| Path | What's there |
|---|---|
| `dsp/` | all the audio code, plain C++ with no platform dependencies: `chain.h` (the whole chain), `drive.h`, `oversampler.h` (+ `halfband_coefs.h`), `svf.h` (SVF + MS-20 HPF), `motion.h`, `reso.h`, `squash.h`, `outs.h`, `util.h` |
| `firmware/` | Kali app: `main.cpp` (UI, pages, audio callback, saving), `kali_hw.h` (OLED and DAC7554 drivers, pin names) |
| `harness/` | offline renderer and tests |
| `hwtest/` | hardware bring-up firmware |
| `tools/` | `flash-kali.sh`, `halfband.py` (generates the oversampling filters), `boot_emu.py` (Unicorn emulator used to boot-check builds and count CPU) |
| `release/` | the current firmware, the Daisy bootloader and its licence |
| `docs/` | `DESIGN.md` (full design and build history), `PINMAP.md` (confirmed hardware map) |

## Design notes

- **CPU:** worst case (both drives up, CHAOS and CHORD on, all outs) is about 193k instructions per 48-sample block in the emulator, an estimated 50-60% of the H750 on hardware. The drives and oversampler work a block at a time with one type decision per block; SQUASH uses fast log and exp.
- **Levels:** the Bribes rule. Every drive gets louder as DRV rises, never quieter; only SQUASH catches the top.
- **HPF audio FM:** each channel already has its own filter, so per-channel FM is all the stereo movement needs; SPREAD is a fixed L/R offset on top. The cutoff glides on the knob only (5 ms), so the FM isn't smoothed away, and it runs through a per-sample Pade tan instead of `tanf`.
- **Output stages:** Kali's CV out stages invert (higher DAC code = lower voltage); the firmware flips it, so POS really is positive.

`docs/DESIGN.md` has the full design, the reasoning behind each block and the hardware feedback that shaped each test build.

## Credits and licences

KALI SMASH's DSP and firmware are original code. Ideas and our own earlier code come from the Bribes Deluge firmware (the drive curves, the v1.27 crushers). The G.711 mu-law segment idea follows Mutable Instruments Clouds/Beads (MIT). The platform is Electrosmith's libDaisy (MIT). `release/dsy_bootloader_v6_4-intdfu-2000ms.bin` is Electrosmith's Daisy bootloader, MIT licensed (`release/LICENSE-DaisyBootloader.txt`). Stock Kali firmware was only examined to confirm the hardware pin map; none of its code is used.
