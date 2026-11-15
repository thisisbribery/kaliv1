# Kali pin map (Daisy Patch SM)

Recovered 2026-11-14. I booted the stock `Kali-v2.22.bin` in an emulator (`re/boot_emu.py`) and logged every GPIO/peripheral setup call, then cross-checked against dsp.coffee's public DPT board file (github.com/joemisra/dpt).

"Confirmed" = seen in the stock firmware's init. Physical assignments were confirmed on Mitch's Kali with hwtest2 on 2026-11-14.

## Confirmed on hardware (hwtest2)

| Physical control | Signal |
|---|---|
| Big knobs left to right | CV_1 .. CV_8 (K1..K8). Bipolar CV reads negative through the knob (attenuator, not rectified) |
| LFO ADJ small knob (left) | ADC_9 (A2, PA1) |
| DELAY ADJ small knob (right) | ADC_10 (A3, PA0). ADC_12 (D8) is unused |
| SYNC button + jack | GATE IN 1 (B10) |
| FREEZE button + jack | GATE IN 2 (B9) |
| LEFT encoder | A = D6, B = D7, click = D5 |
| RIGHT encoder | A = D2, B = D3, click = D4. Clockwise counts up on both |
| CV outs LFO 1-4 | DAC7554 channels 0-3 in order |
| CV outs LFO 5-6 | Patch SM CV_OUT_1 (C10), CV_OUT_2 (C1) |
| Clock outs | GATE OUT 1 (B5) = left clock (L time), GATE OUT 2 (B6) = right clock (R time) |
| Screen | 128x32 SSD1306, I2C1, 0x3C |
| Audio | L/R in and out correct |

All six LFO outs confirmed with hwtest3 (triangles and 5 steps clean). Mitch's left clock (L time) jack is intermittent depending on cable position, probably a dirty socket (hardware, not firmware).

## Stock firmware init


| Part | Patch SM pin | MCU pin | Setup in stock firmware | Status |
|---|---|---|---|---|
| Main knob/CV 1-8 | C5 C4 C3 C2 C6 C7 C8 C9 (CV_1..CV_8) | PA3 PA6 PA2 PA7 PB1 PC4 PC0 PC1 | ADC, standard Patch SM | Confirmed. Which knob is which CV_n is TBC |
| Small knobs (2) | Among ADC_9 (A2), ADC_10 (A3), ADC_12 (D8) | PA1, PA0, PC2 | All three left analog (ADC_11 / D9 is SPI MOSI) | Which two are wired is TBC |
| Encoder X | A = D2, B = D3, click = D4 | PC11, PC10, PC9 | Inputs, pull-up, initialised as one encoder | Confirmed. Left/right TBC |
| Encoder Y | A = D6, B = D7, click = D5 | PC12, PD2, PC8 | Inputs, pull-up, initialised as one encoder | Confirmed. Left/right TBC |
| FREEZE button + gate | B10 GATE IN 1 or B9 GATE IN 2 | PG13 / PG14 | Only the two gate inputs are set up, so each button shares its gate jack | Confirmed shared. Which is which TBC |
| SYNC button + gate | the other gate in | | | |
| OLED | B7 SCL / B8 SDA (I2C1) | PB8 / PB9 | I2C1, address 0x3C | Confirmed. Assumed SSD1306 128x64 (libDaisy's default I2C config matches exactly) |
| CV outs x4 (DAC7554) | SPI2: SCK D10, MOSI D9, NSS D1 (hardware NSS) | PD3, PC3 (AF5), PB4 (AF7) | TX-only SPI, no MISO | Confirmed. Jack order TBC |
| CV outs x2 (Patch SM DAC) | C10 CV_OUT_1, C1 CV_OUT_2 | PA4, PA5 | DAC | Confirmed. Which is 2R / 3R TBC |
| Clock outs x2 | B5 GATE OUT 1, B6 GATE OUT 2 | PC14, PC13 | GPIO out | Confirmed |
| MIDI | USART1 | | Peripheral set up; pins not resolved in the emulator | Not needed |
| Codec | Internal (SAI1, I2C2) | | Standard Patch SM | Confirmed |

DAC7554 command word: `(2 << 14) | (channel << 12) | code12` = write and update that channel. It's sent as 2 bytes per channel, CPOL high, first edge (same as the DPT driver).
