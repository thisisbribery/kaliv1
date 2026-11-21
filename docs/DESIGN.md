# KALI SMASH: stereo mix-buss destroyer firmware for dsp.coffee Kali

Working name. Design draft, 2026-11-14. Phase 1 built (firmware/, dsp/, harness/); test1 not yet tried on hardware.

## Verdict

Doable, and the hardware suits it well. Kali is a Daisy Patch SM (STM32H750 at 480 MHz, 64 MB SDRAM, 24-bit / 48 kHz stereo codec), so this is new firmware on a well-known platform. You can flash back to stock Kali at any time (`Kali-v2.22.bin` is in `~/Downloads/KALI-2.23.0-beta.3-Test-Pack/rollback/`).

The DSP below fits in roughly half the CPU. The two real unknowns are hardware:

1. **The pin map for the panel parts.** We still need to find which pins the encoders, the two buttons, the OLED and the two small knobs use. The standard Patch SM pins (8 CV ins, 2 gate ins, 2 gate outs, 2 DAC outs, audio) are known. dsp.coffee's sibling module, DPT, publishes its board file (github.com/joemisra/dpt), which gives the CV DAC wiring. We can read the rest from the stock Kali binary's GPIO setup, or ask joemisra.
2. **How the knobs and CV jacks combine** (see "Knob + CV behaviour" below). This shapes the UI.

## Hardware, as confirmed

| Part | What it is | Notes |
|---|---|---|
| Brain | Daisy Patch SM | Stock Kali is closed source, so this is a clean rewrite on libDaisy |
| Audio | Stereo in / out, PCM3060 codec, 48 kHz | R input normalled to L |
| 8 main knobs | Patch SM CV_1..CV_8 | **Each knob is an attenuator on its CV jack, normalled to +5 V** |
| 2 small knobs | Patch SM ADC 9-12 (exact pins TBC) | No CV |
| 2 push encoders | GPIO (pins TBC) | |
| 2 buttons + 2 gate ins | FREEZE + gate, SYNC + gate | Whether each button is wired in parallel with its gate jack or has its own pin is TBC |
| Outs 1L 2L 3L 1R | TI DAC7554, 12-bit, about +-7 V, SPI2, CS on D1 | Stock firmware updates at 500 Hz (2 kHz in the 2.23 beta). The SPI is fast enough for audio rate (4 ch x 16 bit x 48 kHz = 3 Mbit/s), not yet tested |
| Outs 2R 3R | Patch SM built-in DAC, 12-bit | DMA-capable at audio rate in libDaisy. **These two can be real (lo-fi) audio outs** |
| 2 clock outs | Patch SM gate outs B5 / B6 | Digital 0/5 V only |
| MIDI | TRS in/out on UART1 | Ignored for now |

So the "8 CV outs" are 6 DAC outs plus the 2 clock outs.

### Knob + CV behaviour (the important constraint)

With nothing patched, a knob reads 0..100%. Once you patch CV into a jack, its knob becomes the depth of that CV, and **there is no base value any more**. If you patch an LFO into DRIVE A, the drive swings between 0 and the knob setting. You can't have "drive at noon, LFO wobbling around it".

Fix in firmware: each knob gets a hidden **BASE** value, 0 by default, which you set with the encoder. The parameter is then BASE + knob x CV. Unpatched, it works as normal. Patched, the encoder sets the centre and the knob sets the depth. The ADC sees bipolar voltages, so a bipolar LFO swings above and below the BASE. Stock Kali rectifies negative CV; we won't.

## Signal chain

```
IN L/R
 └─ INPUT trim
     └─ HPF  (resonant, 12/24 dB)                          [K1]
         ├─ dry branch (delayed to match latency; also gets the LPF when FILTERS = ALL)
         └─ [MOTION / RESO here if set to PRE]
             └─ DRIVE A  (15 types, oversampled)            [K2 amount, K3 shape]
                 └─ DRIVE B  (15 types, oversampled)        [K4 amount, K5 shape]
                     └─ LPF  (resonant, SVF / ladder / DIRT) [K7]
                         └─ MOTION  (flanger / chaos chorus / ...) [K6]
                             └─ RESO  (comb / Rings resonator / formant) [mini 1]
 MIX dry/wet ───────────────────┘                            [K8]
     └─ SQUASH master: compressor > master clipper           [mini 2]
         └─ OUT level
```

- HPF before the drives and LPF after, as specified. The filters are on CV-able knobs at the outer ends of the row.
- **FILTERS = ALL (default) / WET.** On ALL, the dry branch is taken after the HPF and gets its own clean copy of the LPF. That way the filters work like a DJ filter at any MIX setting, and a sweep at 50% mix doesn't leave half the mix unfiltered.
- MOTION and RESO each have a POSITION setting: POST (default, shown above) or PRE (between the HPF and Drive A). A flanger into fuzz and a comb into a folder are the two big sounds this unlocks.
- **SQUASH is the master section after MIX**, so it acts on the whole blended signal, dry included, like a mastering chain: a compressor, then a selectable master clipper.
- Each block has an OFF type, so the module can be fully transparent.
- **The dry path is delayed to match the wet path's latency** (oversampling filters add a few samples). Without this, parallel blending at MIX phases and sounds thin. On a mix buss this matters.
- **Level behaviour follows the Bribes lesson:** each drive gets louder as you turn it up, with no auto-gain that makes it quieter. Only the SQUASH master clipper catches the ceiling.

## Controls reference

The 8 main knobs are **fixed performance controls and never change meaning when you page**. That way a CV patched into a knob always does the same thing. The encoders and screen handle everything deep. When you touch a knob, the screen jumps to that block for 2 seconds and shows the value.

**CV on the main knobs:** with nothing patched, the knob is the value. With CV patched, the knob is the CV depth, and the block's **BASE** setting (on its encoder page) is the centre the CV swings around. BASE defaults to 0, so unpatched use is unaffected. The two small knobs have no CV.

The KNOBS page can reassign any main knob to any parameter, but the defaults below are the design.

| Knob | Kali label (stock) | KALI SMASH |
|---|---|---|
| K1 | META 1 | **HPF** |
| K2 | META 2 | **DRV A** |
| K3 | LFO RATE | **SHP A** |
| K4 | L TIME | **DRV B** |
| K5 | R TIME | **SHP B** |
| K6 | CUTOFF | **LPF** |
| K7 | FEEDBACK | **MOTION** |
| K8 | MIX | **MIX** |
| mini 1 | LFO ADJ | **RESO** |
| mini 2 | DELAY ADJ | **SQUASH** |

The label order assumes stock Kali's knob numbering. Check it against the physical panel before final placement; an overlay sticker will likely be needed.

### K1 HPF

- **Knob:** cutoff. Fully CCW = off (switched out of the circuit), then an exponential sweep from 20 Hz up to the RANGE top (default 4 kHz).
- **Page settings:**
  - RES: flat up to the edge of self-oscillation.
  - SLOPE: 12 or 24 dB/oct.
  - RANGE: top of the sweep, 1 / 4 / 8 kHz.
  - FILTERS: ALL (dry gets filtered too, the default) or WET.
  - BASE.

### K2 DRV A / K4 DRV B

- **Knob:** how hard the signal hits the drive type.
  - Fully CCW, the stage is bypassed and clean.
  - Turning up adds 0 to +36 dB into the shaper. The curve is slow at the bottom, so the first third is the "barely there to warm" zone and the top third is where it goes brutal.
  - **It gets louder as it goes up.** Nothing turns it back down except the SQUASH master clipper.
- **Page settings** (left encoder push picks the type):
  - TONE: tilt EQ after the stage, dark to bright.
  - BIAS: asymmetry, which adds even harmonics.
  - STEREO: LINK, DUAL, MID only or SIDE only. SIDE only dirties and widens the stereo edges while the kick and bass stay clean.
  - OS: 4x oversampling (smooth) or OFF (aliasing grit).
  - STAGE MIX: parallel blend inside the stage.
  - TRIM: stage output level, +-12 dB.
  - BASE.

### K3 SHP A / K5 SHP B

**Knob:** the character control for whichever type that stage is running.

| # | Type | Character | SHAPE does |
|---|---|---|---|
| 1 | TAPE | Soft, compressing, rolls off the highs | CCW: gentle knee, little HF loss. CW: hard knee, dark and squashed |
| 2 | **TANH** | Classic smooth symmetric saturation, odd harmonics | Curve hardness: gentle, then true tanh at noon, then close to hard clip |
| 3 | TUBE | Asymmetric, even harmonics, warm | Bias: from symmetric to strongly lopsided |
| 4 | SEAL | The Bribes v1.5 drive (modelled on the measured Sealegs behaviour) | Knee: soft to hard |
| 5 | DENSITY | Sine-shaper saturation that stays loud and dense | Iterations: thick, then thicker, then mangled |
| 6 | PEDAL | Mid-hump overdrive, like the Bribes 303 drive | Tone of the hump: low-mid to upper-mid |
| 7 | FUZZ | High gain; starves and sputters on quiet parts | Starve: full fuzz to gated velcro |
| 8 | HARD | Digital brickwall clip | Knee: rounded to brick |
| 9 | RECTIFY | Octave-up fuzz | Half-wave to full-wave |
| 10 | FOLD | Wavefolder, metallic | Symmetry of the folds |
| 11 | CHEBY | Adds chosen harmonics | Odd-heavy to even-heavy |
| 12 | CRUSH | Bit and sample-rate reduction (never oversampled) | Sample rate, 48 kHz down to about 500 Hz (DRV sets the bit depth) |
| 13 | WRAP | Integer overflow: louder signal tears and flips | Where it wraps |
| 14 | XOR | Bitwise L-against-R, or against a delayed copy of itself | Delay time / blend of the two |
| 15 | RING | Diode ring mod | Carrier pitch; fully CCW rings against the mix's own low band |

Both stages use the same list, so any pairing works: TANH into FOLD, FUZZ into CRUSH, TUBE into TUBE.

### K6 MOTION

- **Knob:**
  - Fully CCW is off.
  - The first half brings depth and wet level up to full.
  - The second half pushes feedback from its page setting towards maximum: flangers scream and choruses smear.
- **Types:**

| Type | What it does |
|---|---|
| FLANGER | Wide stereo, optional through-zero |
| CHAOS | 3-voice chorus moved by a Lorenz attractor; never repeats |
| ENSEMBLE | The Rings string-machine ensemble |
| PHASER | 4-12 stage stereo phaser |
| WARP | Tape wow, flutter and dropouts |

- **Page settings:**
  - RATE: 0.02-20 Hz.
  - FEEDBACK: +-.
  - SPREAD: L/R phase, 0-180 deg.
  - CHAOS: randomness.
  - POSITION: POST, after the LPF (default), or PRE, before Drive A, so the motion gets distorted.
  - SYNC: FREE, or retrigger on transients.
  - BASE.

### K7 LPF

- **Knob:** cutoff. Fully CW = off (switched out), then an exponential sweep down to 60 Hz.
- **Page settings:**
  - TYPE: SVF 12, SVF 24, LADDER, or DIRT (the Bribes 303 ladder with clip in the resonance loop).
  - RES: up to self-oscillation.
  - DRIVE: into the filter.
  - ENV: envelope follower to cutoff, +-, for auto-wah from the mix itself.
  - BASE.

### K8 MIX

- **Knob:** dry/wet crossfade, LINEAR by default (no level bump when the drives are clean; EQ POWER is a setting).
  - Fully CCW is dry. With FILTERS = ALL, dry still passes through the filters.
  - The dry signal is delayed to match the effect's processing delay, so blends never sound thin or hollow.
  - Both dry and wet then go through SQUASH.
- **Page settings:** LAW (equal power / linear), BASE.

### mini 1 RESO

- **Knob:**
  - Fully CCW is off.
  - The first half fades the resonator in.
  - The second half raises decay/feedback towards infinite; the last few degrees are close to a freeze.
- **Types:**

| Type | What it does |
|---|---|
| COMB | Tuned feedback comb, +- feedback |
| CHORD | 3 combs at chord intervals |
| RESONATOR | Rings modal/string resonator |
| FORMANT | Vowel bank |
| KARPLUS | Transients pluck strings |

- **Page settings:**
  - PITCH: 20 Hz to 5 kHz.
  - SCALE: quantise off or to a scale.
  - DAMP.
  - DETUNE: L/R.
  - POSITION: PRE or POST.
  - FREEZE.

### mini 2 SQUASH (master section)

- **Knob:** gain into the master section, the last thing before the output jacks.
  - Fully CCW, it's at unity: the compressor is idle and the clipper sits at the ceiling, catching only overs. That's transparent.
  - Turning up pushes 0 to +24 dB into the compressor and then the clipper.
  - The output can't pass the ceiling, so the sound gets denser, flatter and more distorted rather than louder past that point.
- **COMP types:**

| Type | What it does |
|---|---|
| OFF | No compression |
| GLUE (default) | Gentle bus compressor |
| SLAM | Fast, ratio 20; pulls the room and tails up |
| PUMP | Ducks the whole mix on detected kicks |
| GATE | Chops tails |

- **CLIP types:**

| Type | What it does |
|---|---|
| SOFT | Cubic, gentle |
| TANH (default) | Smooth |
| HARD | Brickwall |
| TAPE | Rounded, darkens as it's pushed |
| FOLD | Folds back from the ceiling; brutal |

- **Page settings:**
  - CEILING: -12 to 0 dBFS.
  - ATTACK.
  - RELEASE.
  - SC HPF: the compressor ignores lows below this.
  - OUT level.

### Left encoder: where you are

- **Turn:** select a block: HOME, HPF, DRIVE A, DRIVE B, LPF, MOTION, RESO, SQUASH, OUTS, KNOBS, PRESETS, SYSTEM.
- **Push:** swap the selected block's type. A cycling list opens; turn to choose and push to confirm (for example, DRIVE A: FUZZ > TANH).
- **Hold:** back to HOME. HOME shows the chain as a strip of blocks with their types, plus input/output meters and a BITE meter (how hard the drives are clipping).

### Right encoder: what you change

- **Turn:** change the selected setting. On HOME, it browses presets.
- **Push:** move to the next setting in the block. On HOME, it loads the browsed preset.
- **Hold:** reset the setting to its default.

### FREEZE button + its gate jack: SMASH

- **Held, or gate high:** +18 dB into Drive A, Drive B jumps to full, and SQUASH goes to full. Patch a drum trigger in and every hit smashes the mix.
- **Release:** everything snaps back.
- **Other actions it can be set to:** BYPASS, KILL LOWS, RESO FREEZE, MUTE, SHAPE FLIP.

### SYNC button + its gate jack: BYPASS

- **Button:** toggles between clean and destroyed for A/B comparison. Bypass is the dry signal through SQUASH at its current setting, so levels stay comparable.
- **Gate:** a momentary flip, so a clock or pattern rhythmically gates the whole effect.
- **Other actions:** the same list as FREEZE.

## Block sources

| Block | Basis |
|---|---|
| HPF / LPF | ZDF state-variable filters (Simper/Cytomic paper); DaisySP LadderFilter (MIT); Bribes DIRT ladder (Open303, MIT) |
| TAPE, TUBE, SEAL, HARD, FOLD | Bribes saturator.h (our code); Airwindows ToTape idea (MIT); DaisySP Wavefolder (MIT) |
| TANH | New; tanh with a hardness morph, ADAA to keep it clean |
| DENSITY, CHEBY | Airwindows Density / Pafnuty (MIT) |
| PEDAL | Bribes 303 drive (v1.22.1) |
| FUZZ, RECTIFY, WRAP | New |
| CRUSH | DaisySP Decimator (MIT) |
| XOR, RING | MI Warps (MIT) |
| FLANGER, PHASER | DaisySP (MIT), extended |
| ENSEMBLE | MI Rings fx (MIT), already in Bribes v1.25 |
| CHAOS, WARP | New |
| RESONATOR | MI Rings (MIT), already ported as BRIBERING in Bribes v1.25 |
| COMB, CHORD, FORMANT, KARPLUS | New, simple |
| SQUASH comp + clipper | New; DaisySP Compressor (MIT) as a starting point |

The **Pyros Versio** (Audiority, free for Versio, 9 modes, **closed source**) is a reference for which modes earn their place, not a code source. That's the same rule as for Pyros and Sealegs in Bribes.

## The 8 outputs: making the mix a modulation source

Every output picks a SOURCE on the OUTS page. Defaults are chosen so patching anything out of Kali straight back into its own knob CVs (or into other modules) animates the rig from the music itself.

**Sources:**

- **ENV**: envelope follower on FULL, LOW (kick), MID, HIGH (hats), L, R or SIDE, with attack/release and gain.
- **TRANSIENT**: AD envelope fired by onsets in a chosen band.
- **BITE**: how hard the drives are clipping right now. Patch it back into SQUASH or MOTION and the destruction feeds itself.
- **S&H**: a random value stepped on each transient.
- **LFO**: the MOTION LFO (so other modules follow the flanger) or a free LFO.
- **CHAOS**: the Lorenz X/Y/Z from the CHAOS chorus.
- **PITCH**: rough zero-crossing pitch follower (lo-fi, fun on vocals).
- **CONST**: a fixed voltage.
- **AUDIO** (2R and 3R only, to start): a 12-bit audio tap: mono wet, SIDE, pre-drive send, or LOW band. Useful as a lo-fi third audio out or a send into another processor. **These outs are DC-coupled, so a cap or AC-coupled input downstream is advisable.**
- Gate outs: METER (default), TRANSIENT (band), BITE (high while clipping), CLOCK (tempo estimated from transients, with div/mult).

**Default map:**

| Out | Default | Why |
|---|---|---|
| 1L | ENV LOW | Kick follower, for sidechaining other modules |
| 2L | ENV FULL | Mix level |
| 3L | ENV HIGH | Hats/air follower |
| 1R | BITE | Destruction amount, for self-patching |
| 2R | LFO (MOTION) | Lets the rig follow the flanger |
| 3R | AUDIO: SIDE | Lo-fi side-signal out |
| Clock L | METER L | Its LED is an output level meter for the left channel |
| Clock R | METER R | Its LED is an output level meter for the right channel |

The CV out LEDs (bicolour on the DPT design) show each output's value, so they work as a 6-segment meter for free.

### Clock LEDs as level meters (added 2026-11-14, first tried in hwtest4)

- Each clock out LED is a fast meter: an envelope follower on the left or right **final output**, with about 0.3 ms attack and 25 ms release. 120 ms (hwtest4) blurred hi-hats together; METER SPEED on the OUTS page can offer slower settings.
- The scale runs from -42 dBFS (off) to 0 dBFS (full), with a brightness curve so quiet levels still glow.
- **Hardware catch:** each LED is wired to its clock jack's signal; the stock firmware only sets up the two gate pins. So the LED is dimmed by switching the jack on and off fast (PWM at 1 kHz, 32 steps).
- In METER mode, the jack carries that PWM signal. Raw, it's a buzzy pulse train. Through a slew or low-pass filter it becomes a usable envelope CV.
- Choosing TRANSIENT, BITE or CLOCK for a clock out turns its LED back into a plain gate light.

### Clip flash (added 2026-11-14, first tried in hwtest4)

- When the final output clips, all six LFO LEDs flash three times: 60 ms on, 60 ms off, 360 ms in total. If clipping continues, the flashing continues.
- **Hardware catch:** the LFO LEDs show the jack voltage, so for those 360 ms the six CV jacks output the flash pulses instead of their own CV.
- A CLIP FLASH setting on the OUTS page picks ON (default) or OFF, for when steady CV matters more.
- The BITE meter on HOME covers the drives clipping inside the chain. This flash is only for the output itself.

Audio rate on the 4 DAC7554 outs is a stretch goal: run them at 4 kHz first, and try 48 kHz once the basics work.

## Presets

Presets live in QSPI flash, 8 slots to start. A preset stores block types, hidden settings, BASE values, the knob map and the output map. Knobs are live, so presets don't store knob positions. This is the same reason stock Kali uses soft takeover.

## Code sources and licences

| Source | Licence | Use |
|---|---|---|
| libDaisy, DaisySP | MIT | Platform, Patch SM board, OLED, encoders, SVF/ladder/flanger/phaser/decimator/wavefolder/compressor |
| DaisySP-LGPL (moogladder, reverbsc) | LGPL | **Avoid**; static linking makes LGPL awkward |
| Mutable Instruments (Rings, Warps, Beads, stmlib) | MIT | Resonator, ensemble, XOR/ring/fold, limiter, parameter smoothing. We've already ported Rings (Deluge) and Beads (Seal Beads) |
| Airwindows | MIT | Saturation algorithm ideas and code (Density, ToTape, Pafnuty...) |
| Open303 | MIT | DIRT ladder basis |
| Our Bribes DSP (saturator.h, DIRT, Rings glue) | We wrote it. It lives in the GPL-3 Deluge repo | Reuse our own parts freely; anything lifted from Deluge-originated code makes the firmware GPL-3, which is fine if it's public |
| joemisra/dpt DAC7554 driver | **No licence file** | Write our own; it's about 40 lines (four 16-bit SPI words) |
| Stock Kali, Pyros Versio, Sealegs | Closed | Reference behaviour only, never code |

## CPU and memory budget (estimate, H750 at 480 MHz, 48 kHz)

That's about 10,000 cycles per stereo sample.

| Block | Cycles / stereo sample |
|---|---|
| HPF + LPF (SVF) | ~150 |
| Drive A + B at 4x oversampling (halfband up/down + shaper) | ~1,000-1,400 |
| MOTION | ~200-400 |
| RESO: comb types | ~150 |
| RESO: Rings resonator | ~1,500-3,000 (the heaviest block) |
| SQUASH + envelope followers + transients | ~250 |
| Mix, output, CV out sources | ~150 |
| **Worst case** | **~5,000, about 50%** |

Memory isn't a concern: 64 MB SDRAM covers all comb, flanger and dropout buffers many times over.

Latency: 24-48 sample blocks plus oversampling come to about 1.5-2.5 ms in to out.

## Open questions and risks

1. **Pin map** for the encoders, buttons, OLED (controller type and bus), small knobs, and button/gate wiring. Plan: disassemble the GPIO/SPI/I2C setup in `Kali-v2.22.bin` (the same approach as the Sealegs work), cross-check with the DPT board file, and ask joemisra if anything's ambiguous. The Kali Vhikk Performer build in Downloads (24 Oct) already drove the OLED, so someone has at least the display working.
2. **The knob-attenuator CV behaviour.** The BASE fix covers it, but it needs a hardware check that negative CV reaches the ADC unrectified.
3. **The input converter clip point** on hot mixes.
4. **The DAC7554 at audio rate** is untested.
5. **The physical knob layout** vs labels; probably needs an overlay.

## Build plan

0. **Hardware bring-up:** pin map, then a test build that passes audio through and shows every knob, CV, small knob, encoder, button and gate on the OLED, ramping all 8 outputs. Check the existing `Kali-PatchSM-Audio-Diagnostic.bin` (Downloads, 24 Oct) first.
1. **Skeleton:** IN > HPF > Drive A/B (TAPE, TANH, TUBE, SEAL, HARD, FOLD, CRUSH) > LPF > MIX > SQUASH > OUT, plus the UI framework (blocks, type swap, params, HOME).
2. **All 15 drive types** with oversampling, plus an offline Mac harness that renders level-true WAVs from drum/synth/vocal loops through the same DSP code. This is the Bribes workflow, so you can judge sounds before flashing.
3. **MOTION, RESO, SQUASH.**
4. **Outputs:** sources, default map, buttons/gates, presets.
5. **KNOB MAP, BASE, polish,** then hardware tuning passes.

## Build status (2026-11-14)

- **Phase 0:** done. The pin map is confirmed on hardware (hardware/PINMAP.md).
- **Phase 1 test1:**
  - What's in it: HPF, Drive A/B (OFF, TAPE, TANH, TUBE, SEAL, HARD, FOLD, CRUSH), LPF, MIX, SQUASH (comp OFF/GLUE/SLAM; clip SOFT/TANH/HARD/TAPE/FOLD), all encoder pages, CV BASE, knob pop, clock LED meters and clip flash.
  - How it was checked: harness tests pass (wet/dry sample-aligned; every type gets louder with DRV). The emulator shows the worst-case callback at about 167k instructions per 1 ms block, roughly 35% CPU.
  - Size: 123 KB of 128 KB internal flash. **Phase 2 moves to the Daisy bootloader + BOOT_SRAM**, as Kali's own 2.23 beta does.
- **Not yet:** saving settings and presets.

### Phase 1 test2 changes (Mitch's test1 feedback, 2026-11-14)

- **Knobs:** LPF moved to K6 (the CUTOFF knob) and MOTION to K7.
- **Filters:** HPF sweeps 20 Hz to 16 kHz with no RANGE setting; LPF sweeps 20 kHz to 20 Hz. The last 3% of each knob fades to silence (CLOSED).
- **CV:** stacking isn't possible on Kali's hardware. The jack's switch contact breaks the +5 V normal, and the knob is a passive attenuator, so only knob x CV reaches the ADC. Mitch accepted keeping CV BASE.
- **Drive types:** 13 now. Added FUZZ, DIST, METAL (all +48 dB), RECT, WRAP and XOR. SHAPE was redesigned per type so it makes a big difference (the harness checks that every type's SHP sweep changes the sound by more than -12 dB).
- **Whine:**
  - SQUASH has a noise-floor guard: no added gain or makeup below about -60 dBFS.
  - The clock LEDs use sigma-delta, not 1 kHz PWM.
  - The OLED only redraws when the screen changes, and the CV DACs only update when their value changes.
  - OUTS METER OFF is there for diagnosis.
- **Clip flash:** needs at least 3 samples per block more than 2.5 dB past the ceiling.
- **Meters:** input by default, 15 ms fall, -36 dB range.
- **Build:** shipped as test2b, a plain internal-flash app at 120.6 KB of 128 KB, flashed the same way as test1. The emulator puts the worst case at about 39% CPU. The BOOT_SRAM route failed on hardware: the Kali kept landing in ROM DFU, the flasher rejected the file, and with the bootloader installed it booted a leftover Vhikk app. Phase 3 needs BOOT_SRAM plus a menu item that reboots into the Daisy bootloader (`System::ResetToBootloader`).

## Phase 1 test3 changes (2026-11-14)

Hardware notes on test2b: it sounds good. SQUASH no longer whines in silence. CRUSH sounded bad, the clip flash was still too eager, SHAPE was still too subtle on many types, and with both drives up the CPU read 65%.

- **CRUSH became the Bribes v1.27 crushers**, ported from Deluge `dsp/crusher.h` (our own code): CRUSH, VINT, ULAW and JITR. They are zero-order hold with continuous bits. DRV sets the bits and SHP the rate, which is counted in 48 kHz samples, so at 4x the minimum hold is 4 samples. There are now 16 types. The screen names fit 5 characters (VINT, JITR).
- **SHAPE:**
  - TAPE, TUBE, SEAL and HARD now add pre-gain with SHAPE (up to +6, +12, +18 and +12 dB), with partial post cut.
  - TAPE wear adds wow and flutter (a modulated delay of up to about 1.2 ms, 0.55 / 1.7 / 6.3 Hz), a +4 dB head bump that stays in the output, and a 2-pole roll-off to 3 kHz.
  - TANH undoes only half of its ±18 dB emphasis.
  - FUZZ gates on the input, up to 0.12 (-18 dBFS).
  - DIST's high-pass sweeps 700→40 Hz and its low-pass 12k→900 Hz.
  - METAL's scoop is -30 dB across 180 Hz to 3 kHz.
  - The harness SHAPE test now measures the average octave-band level change at DRV 30% and 60%. It fails a type below 3 dB unless the waveform difference is above -6 dB. All 16 types pass.
- **The clip flash is removed**, along with the OUTS CLIP FLASH setting and Squash hard-clip counting. The LFO outs are written to 0 V once at boot.
- **CPU:**
  - The drives run a block at a time: the type switch happens once per block, each type has its own loop, and both channels step through the shared smoothers and clocks from the same snapshot.
  - The oversampler is stereo and block-based with folded symmetric taps.
  - SQUASH's per-sample logs and exps use fastLog2/fastExp2.
  - In the emulator, both drives up went from 195k to 110k instructions per block. The weighted estimate is about 29%, against 54% for test2b, which read 65% on hardware.
- **Size:** UI and setup code is compiled with -Os and the DSP with -O2 (pragma around the chain.h include). That's 126.5 KB of 128 KB internal flash. Phase 3 has to move to BOOT_SRAM.

## Phase 1 test4 changes (2026-11-14)

Hardware notes on test3: "actually bloody good". The new crushers are better, but the one still called CRUSH was bad. The user wants a drive output boost. Knob pop flipped back to the previous page. The HPF resonance wasn't aggressive enough; they asked for an MS-20 style filter with a DRIVE.

- **CRUSH type removed.** 15 types now; the crush family is VINT, ULAW and JITR.
- **Drive OUTPUT** (was TRIM) is the first DRIVE page setting, -12..+24 dB.
- **Knob pop** now sets `ui.block` (permanent) and exits type mode.
  - CV guard: a knob moving nonstop for over 2 s (gaps under 1 s) stops switching pages until it's been still for a second.
- **Ms20Hpf** (svf.h, our own design):
  - Structure: a Simper SVF with damping k = 2 - R·secant(clip(bp)). The clip is tanh(2·v + 0.25) normalised to unity slope, an asymmetric diode-like curve.
  - Solve: two passes per sample. The clip gain is first taken at the last band-pass value, then re-solved at the new estimate; one pass ran away at 2 kHz and up.
  - Resonance: R = 2.6·res^1.2, which self-oscillates from about res 0.8. Silent input carries a 1e-5 noise kick.
  - Howl level: about 0.38 at res 0.85 and about 0.91 at full res, at any cutoff, with pitch within 4% of the cutoff.
  - DRIVE: 0..+24 dB in, with the output soft-clipping at 1.2 (blend ramps over the first 25% of DRIVE). SLOPE 24 puts a clean Butterworth HP in front.
  - The harness tests self-oscillation, silence below the threshold, and a finite cutoff × res × drive × slope sweep.
- **CPU:** 119k instructions per block in the emulator (est. about 32%).
- **Flash:** 126.9 KB of 128 KB.

## Phase 3 test1 (2026-11-14)

Hardware notes on test4: "HELL YEAH that was RAD". The resonance is much better and so are the knob jumps. They asked for TYPE in each page's list instead of click-to-select, and to move on with the build.

- **UI:**
  - TYPE is the first ParamDef on DRIVE A/B, MOTION and RESO; COMP is first on SQUASH. Type mode is removed.
  - Left click jumps to setting 0, which is TYPE. A ParamDef `fmt` hook handles RATE in Hz, PITCH as note plus Hz, and COLOR per type.
  - HOME line 2 shows MOTION and RESO; line 3 shows MIX, SQ, a 5-char BITE bar and CPU.
  - Knobs: K7 opens MOTION, mini L opens RESO.
- **MOTION** (dsp/motion.h, our own code, 48 kHz, delays in SDRAM 2×4096):
  - Types: FLANGER (tri, 0.15–7 ms, fb through tanh), CHAOS (3 voices per side, sine→Lorenz blend by CHAOS), ENSEMBLE (3 taps, slow + 10x fast sines, 120°), PHASER (6 allpass, 150 Hz–3.4 kHz), WARP (wow, flutter, drift, dropouts, dull).
  - Knob: the first half brings wet and depth up; the second half pushes fb from the page value to ±0.97.
- **RESO** (dsp/reso.h, our own code, lines in SDRAM 2×3×4096):
  - COMB: ± sign by COLOR, damping LP, tanh loop.
  - CHORD: 3 combs, 6 chords.
  - MODAL: 10 two-pole modes, odd partials L, even R. COLOR goes string→stiff→bar→bell. Input gain is broadband-normalised, tilted (f/130)^0.8 and scaled by decay. Coefficients are cached and only recomputed when a setting moves.
  - FORMANT: 3 SVF band-passes over AEIOU, Q 4–44, makeup 1.6·√Q.
  - KARPLUS: an onset-gated input burst plucks a comb with a two-point average.
  - Knob: the first half fades in; the second half sets decay (comb fb 0.6–0.995, modal T60 0.15–30 s); the top 3% or FREEZE freezes it (input cut, fb 1 with the clipper).
- **POSITION PRE** runs MOTION/RESO on the wet path before the oversampler. POST runs them after the DC blocker and LPF.
- **Build:**
  - BOOT_SRAM (146 KB of 480 KB SRAM, 128 KB SDRAM), -O2.
  - SYSTEM → UPDATE (- > >> GO) calls `System::ResetToBootloader(DAISY_INFINITE_TIMEOUT)` after drawing an UPDATE MODE screen.
  - `chain.Init()` runs after hw.Init, since SDRAM isn't up at static init.
- **Flashing:**
  - tools/flash-kali.sh (bundled in ~/Downloads/KaliSmash-phase3-test1/) polls `dfu-util -l`. "Internal Flash" means ROM; "0x90040000" means the Daisy bootloader.
  - `--first` installs the bootloader, then catches the 2 s window. Success is judged by "Download done".
  - Untested on hardware.
- **CPU** (emulator, instructions per block):
  - FLANGER+COMB with both drives up: 144k.
  - CHAOS+CHORD: 183k (est. 50–60% on hardware).
  - The emulator NaN'd on MODAL because newlib's expf/cosf/powf use f64, which unicorn skips. The harness with the same settings is clean.
- **Harness:** all pass, including the MOTION/RESO finite, bounded and effect checks and the freeze-sustain tests. Renders are 50–63.

## Phase 4 test1 (2026-11-14)

The user installed phase 3 test1 ("actually great!") via flash-kali.sh. The first run dropped at 56% of the bootloader write ("Error during download get_status", Kali on a USB hub); the immediate rerun worked. They didn't like RESO freezing at the top of the knob.

- **RESO top:**
  - No knob freeze any more; FREEZE is page-only.
  - fb runs 0.6–1.02, so it self-oscillates through the tanh. KARPLUS goes up to 1.003, MODAL T60 to 120 s, FORMANT Q to 80.
  - Checked: 2 s of loop then 2 s of silence still sings at -19..-22 dB in the last second.
- **dsp/outs.h OutSources:**
  - Envelopes: FULL, LOW (150 Hz, 2-pole), MID (300–2k), HIGH (>4k) and SIDE. 0.3 ms attack, ENV REL release, mapped dB→0..1 over ENV RANGE.
  - Transients: fast env > ratio×slow env (HIT SENS 2.6/1.9/1.45), 60 ms hold-off. They drive TRANSNT (150 ms decay), S&H and a 10 ms gate pulse.
  - Also: BITE, SQ GR (/18 dB), MOT LFO, a free LFO and Lorenz X/Y (LFO RATE).
  - Gate sources: METER, TRANSNT, BITE (>0.25), OFF.
- **Outputs:** written in the audio callback (1 kHz). DAC7554 codes are 2048 + v·2047, written only when changed; CV_OUT is 2.5 + 2.5v. OUT1–4 are DAC ch0–3, OUT5–6 are CV_OUT_1/2.
- **Buttons** (gate_in_2 = FREEZE, gate_in_1 = SYNC; button and jack share one input):
  - Modes: MOMENT or LATCH (rising edge toggles).
  - Actions: SMASH (Drive A boostDb +18 and amount ≥ 0.35, Drive B amount 1, SQUASH 1), BYPASS (chain crossfades to a raw latency-delayed input before SQUASH; nulls at -97 dB), KILL LOWS (hpfCut ≥ 0.38, about 250 Hz), MUTE (after SQUASH).
  - HOME line 3 shows the active actions.
- **Saving:**
  - A Bank {magic "KSM1", sizeof(Settings), used mask, cur, slot[8]} sits at QSPI offset 0x7F0000. That's outside both bootloader DFU regions (0x90040000.. and 0x90400000..0x907BFFFF).
  - Autosave runs 3 s after the last change. PRESETS page: SLOT / LOAD / SAVE (turn to GO). SYSTEM has INIT ALL.
  - A size mismatch means defaults. ClampAll runs after every load.
  - One-shot actions live in a separate struct A, so they're never saved.
- **CPU:** worst case (CHAOS+CHORD, both drives, outs) is 193k instructions per block, against 183k for phase 3.
- **Size:** 154.6 KB SRAM. sha 3e3a8b2b…, in the ~/Downloads/KaliSmash-phase4-test1/ bundle.

## Phase 4 test5 (2026-11-21)

MOTION type 6, SHIFT: a three-band frequency shifter, after the Self-Care lever in Freakshow MISHBY (studied by decompiling the plugin: per-band Hilbert SSB shift scaled by the lever, two fixed offset sets on a switch; no code taken).

- Per side: ~10 Hz DC blocker, Hilbert allpass pair (Niemitalo 4+4), then 2-pole Butterworth crossovers at 250 Hz and 2.5 kHz run on re and im alike (low = LP, high = HP of the rest, mid = remainder, so the bands sum exactly), one complex oscillator per band, real part out.
- Knob: shift 0..full offsets over the whole travel, wet over the first quarter, with a power-held blend (dry and shifted are unrelated, so a straight crossfade dipped 3 dB).
- CHAOS morphs set A {+90, -300, -1500 Hz} to set B {-190, +900, +2500 Hz}. RATE is the glide time (30 ms to 2.4 s, shown in ms/s). SPREAD turns the R oscillators 0-180°. FEEDBACK goes through 30 ms, comes in with the knob (not pushed to max), with (1 - fb²)^0.75 level compensation.
- Harness test 9: sine in each band, shift within 2 dB of input level, mirror image 49-64 dB down. On the Mac SHIFT costs less than FLANGER or CHAOS.
- Added as the last type, so saved settings and presets load unchanged.

## Phase 4 test6 (2026-11-21)

HPF click fix. With RES above 0, crossing the 1% "off" point at the bottom of the HPF knob snapped the resonance between 0 and full, which clicked (10-37 dB bursts above 4 kHz in the harness, worst on release with DRIVE). Now the resonance fades in over the bottom 3% of the knob, and Ms20Hpf glides R and the input drive per sample (~10 ms), which also smooths RES turns and the KILL LOWS jump. Harness test 10 steps the HPF on and off at several RES/DRIVE settings and compares the >4 kHz burst at each step with both settled states: within 0.3 dB now. Confirmed click-free on hardware.
