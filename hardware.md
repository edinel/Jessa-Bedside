# Hardware Notes — Bedside Speaker Project

## Overview
A handmade hexagonal Baltic birch speaker driven by an ESP32 Feather V2 with
AirPlay 2. Primary use case: myNoise.net ambient soundscapes at bedside listening
levels. A gift — finish aesthetic was chosen with the recipient in mind.

## Enclosure
- **Type:** Regular hexagonal prism
- **Material:** 1/8" (3.175mm) Baltic birch plywood, raw finish
- **Generated with:** [boxes.py RegularBox](https://boxes.hackerspace-bamberg.de/RegularBox)
- **Hex side length:** 2 7/8" (73.0mm) external
- **Panel height:** 4.25" (107.9mm)
- **Flat-to-flat diameter:** ~4.98" (126.5mm) external
- **Usable baffle width (inside finger joints):** 64.63mm
- **Internal volume:** ~1.29 liters
- **Joinery:** Finger joints — seal all interior joints with wood glue or silicone
- **Damping:** Add polyfill stuffing to raise effective Qtc toward 0.5–0.6

## Driver
- **Model:** Tang Band W2-2136S 2" Aluminum Full-Range Neodymium
- **Source:** Parts Express
- **Impedance:** 4Ω
- **Power handling:** 10W RMS / 20W max
- **Frequency response:** 120–15,000Hz
- **Sensitivity:** 83dB @ 2.83V/1m
- **Fs:** 120Hz, **Qts:** 0.31, **Vas:** 0.258L
- **Frame:** 57mm square (proud mount — sits on top of baffle face)
- **Baffle cutout diameter:** 56mm (clears the irregular ~55mm diagonal of driver basket)
- **Cutout method:** SendCutSend laser cut into hex face panel

## Mounting Hole Positions
The driver has a square mounting pattern, 46mm center-to-center in all 4 directions.
Measured from inside edge of finger joints, along panel centerline:

- **Panel width (inside joints):** 64.63mm
- **Panel center:** 32.315mm from either edge
- **Hole 1:** 9.165mm from inside edge
- **Hole 2:** 55.465mm from inside edge
- **Hole spacing check:** 55.465 - 9.165 = 46.3mm ✓
- **Pattern:** Square — same spacing applies on both axes

## Grille & Escutcheon
- **Grille material:** Natural mulberry silk (3 colors ordered, recipient chooses)
- **Method:** Fabric stretched over driver cutout, clamped under escutcheon
- **Escutcheon:** Square, slightly larger than 57mm driver frame, material TBD
  (options: darker wood veneer, brass sheet, black acrylic — laser cut)
- **Aesthetic:** Raw birch + silk + escutcheon — Arts & Crafts / craft-forward

## DAC + Amplifier
- **Board:** Adafruit TLV320DAC3100 I2S DAC with built-in Class D amp (Product #6309)
- **No external amplifier needed** — TLV320DAC3100 drives the 4Ω speaker directly
- **Output power:** 2.5W into 4Ω (sufficient for bedside myNoise at close range)
- **Configuration:** Requires I2C initialization before audio flows — not plug-and-play
  like PCM5102A. Must call Adafruit_TLV320_I2S library (Arduino) or
  adafruit_tlv320 (CircuitPython) at startup. Integrate into AirPlay firmware
  startup sequence.


### TLV320DAC3100 Wiring
| TLV320DAC3100 Pin | Feather V2 silkscreen | GPIO |
|---|---|---|
| VIN | USB | 5V (must be 5V for speaker output) |
| GND | GND | — |
| SCL | SCL | GPIO20 |
| SDA | SDA | GPIO22 |
| BCK | 27 | GPIO27 |
| WSEL | A0 | GPIO26 |
| DIN | A1 | GPIO25 |
| RST | 33 | GPIO33 |

### Touch Pad Wiring
| Component | Feather V2 silkscreen | GPIO |
|---|---|---|
| Copper hex wire | A5 | GPIO4 |
| 1MΩ resistor → GND | GND | — |

### NeoPixel Chain Wiring
| NeoPixel | Feather V2 silkscreen | Notes |
|---|---|---|
| 5V | USB | 5V rail |
| GND | GND | — |
| DIN | 14 | GPIO14 |

- I2S pins are software-assigned on ESP32 — any available GPIO works
- Avoid GPIO 34–39 (input-only), GPIO 0/2/5/12/15 (strapping), GPIO 6–11 (flash)

## Power
- **Simple 5V USB-C wall wart** — no Power Delivery negotiation needed
- TLV320DAC3100 VIN → ESP32 Feather V2 USB/5V pin
- No PD trigger board, no buck converter, no barrel jack required
- ESP32 USB-C → wall wart directly

## Microcontroller
- **Board:** Adafruit ESP32 Feather V2 (Product #5400)
- **Chip:** ESP32 dual-core 240MHz, 8MB Flash, 2MB PSRAM
- **Power input:** USB-C only (no direct 5V pin bypass)
- **Built-in:** NeoPixel RGB LED (pin 0), red LED (pin 13), user button (GPIO38)

## Touch Interface
- **Method:** ESP32 built-in capacitive touch sensing — no external IC needed
- **Touch pin:** GPIO4 / A5 (Touch0 / T0) — confirmed touch-capable on ESP32
- **Touch pad:** 3/4" copper hexagon stamping blank, 16 gauge (Etsy)
  - Mounted on one hex side panel — hex-on-hex geometry, deliberate design element
  - Large enough to locate and touch confidently in the dark
  - Raw copper patinas over time — complements raw Baltic birch beautifully
- **Wiring:** Solder wire to back of copper hex, run to GPIO4
  - Copper is a significant heat sink — use flux liberally, crank iron to 380–400°C,
    pre-tin both wire and pad separately, heat the copper not the solder
- **Required:** 1MΩ pulldown resistor between GPIO4 and GND for reliable readings
- **Code:** `touchRead(4)` returns lower values when touched; calibrate threshold
  by printing raw values to serial — untouched ~1200, touched ~200 (newer Arduino core)
- **Note:** GPIO37 (also on Feather V2) is NOT touch-capable — input-only pin

## LEDs
- External LEDs planned (count/type TBD)
- Power budget: 5V rail has ~500mA from regulator; ESP32 draws ~240mA peak,
  TLV320 ~30mA, leaving ~230mA for LEDs (~11 standard 20mA LEDs)
- Drive via GPIO with current-limiting resistors, or use NeoPixel chain for
  simpler wiring with more flexibility

## Audio Chain
```
ESP32 (I2S + I2C) → TLV320DAC3100 (built-in Class D) → W2-2136S speaker
```

## Software
- **Framework:** Arduino (ESP32 Arduino core)
- **Bluetooth audio:** [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) by Phil Schatzmann
  - Implements Bluetooth A2DP sink — phone pairs once and connects automatically
  - Outputs audio via I2S directly to TLV320DAC3100
- **DAC init:** Adafruit_TLV320_I2S Arduino library — handles I2C configuration at startup
- **Primary use:** [myNoise.net](https://mynoise.net) ambient soundscapes via Bluetooth
- **Expected firmware size:** ~50–100 lines of straightforward Arduino code
- **Note:** One device pairs at a time — fine for a personal bedside speaker

## Acoustic Notes
- Box volume (~1.29L) is ~5× larger than driver Vas (0.258L)
- Sealed alignment gives Qtc ≈ 0.34 (overdamped) — gentle rolloff from ~130Hz
- Polyfill stuffing recommended to raise effective Qtc
- No port planned — myNoise content needs no deep bass
- At 2.5W max output and 83dB sensitivity, SPL is more than adequate for bedside

---

# Hardware Notes — Bathroom Ceiling Speaker Project

## Overview
An ESP32-based AirPlay 2 receiver driving a passive ceiling speaker, powered
entirely from an existing attic light bulb socket. Primary use case: podcast
listening in the bathroom. All electronics live in the attic above the bathroom.

## Speaker
- **Type:** Passive ceiling speaker (150W max rating)
- **Model:** OSD Audio ICE800TTWRS (or equivalent single passive ceiling speaker)
- **Location:** Installed in bathroom ceiling, wired up through ceiling to attic

## Amplifier
- **Model:** HiLetgo TPA3116D2 Mono 100W
- **ASIN:** B082F7P184
- **Chip:** TPA3116D2 (Class D)
- **Supply voltage:** DC 12–26V
- **Output:** 100W mono (BTL)
- **Impedance:** 4–8Ω compatible

## DAC
- **Board:** Adafruit PCM5100 I2S DAC (Product #6251) — 100dB SNR
- **Note:** PCM5102A (#6250) was preferred but out of stock; PCM5100 is functionally
  identical for this application
- **No I2C configuration needed** — plug-and-play unlike TLV320DAC3100

### PCM5100 DAC Wiring (I2S)
| PCM5100 Pin | ESP32 Feather V2 |
|---|---|
| BCK (Bit Clock) | Any free GPIO (I2S bit clock) |
| LRCK (Word Select) | Any free GPIO (I2S word select) |
| DIN (Data) | Any free GPIO (I2S data) |
| GND | GND |
| VCC | 3.3V |
| FMT | GND (I2S format) |
| XMT | 3.3V (unmute) |
| SCK | GND (use internal clock) |

## Power Chain
```
E26 attic light socket
  → Light socket to AC outlet adapter (~$8)
  → USB-C PD wall wart (65W recommended)
  → USB-C PD trigger board (set to 20V)
  → TPA3116D2 amp board (via barrel jack)
  → Also tapped: LM2596 buck converter (20V → 5V)
  → USB-C breakout board (5V)
  → Adafruit ESP32 Feather V2 (USB-C powered)
```

- Attic light bulb remains functional — outlet adapter passes through to E26 socket
- LM2596 buck converter: adjust trimmer pot to exactly 5V before connecting ESP32
- USB-C PD trigger board negotiates 20V from PD charger via PD protocol

## Microcontroller
- **Board:** Adafruit ESP32 Feather V2 (Product #5400) — identical to bedside project
- **Power:** USB-C only — no direct 5V pin bypass available on this board

## Audio Chain
```
ESP32 (I2S) → PCM5100 DAC → line-level analog → TPA3116D2 amp → ceiling speaker
```

## Software / Firmware
- **Framework:** ESP-IDF (NOT Arduino) — the airplay-esp32 library requires ESP-IDF
- **AirPlay 2:** [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)
  - Presents as AirPlay 2 receiver to iPhone/iPad/Mac — appears like a HomePod
  - Also supports Bluetooth A2DP as fallback
  - No cloud, no app — configure via web interface
  - Actively maintained (updated April 2026)
- **Primary use:** Podcast listening via AirPlay from iPhone/Mac

## Attic Environment Notes
- Electronics are dry (attic, not bathroom) — no moisture concern
- Attic can get hot in summer — avoid placing enclosure in direct sun path
- ESP32 and TPA3116D2 rated for elevated temps but worth noting
- Use twisted wire pairs for signal cables (Class D amp EMI sensitivity)
- Keep input signal cables routed away from amp output/switching components

## Context Notes for Code Generation
- Both projects use Adafruit ESP32 Feather V2 (#5400)
- **Bedside:** Arduino framework — ESP32-A2DP + Adafruit_TLV320_I2S
  — TLV320DAC3100 (#6309) requires I2C init — drives speaker directly
  — powered by plain 5V USB-C — hexagonal birch enclosure — W2-2136S driver
  — capacitive touch on GPIO4 with 1MΩ pulldown to GND — external LEDs TBD
- **Ceiling:** ESP-IDF framework — rbouteiller/airplay-esp32 — AirPlay 2 target
  — PCM5100 (#6251) plug-and-play I2S — feeds TPA3116D2 amp
  — powered from attic light socket via PD chain — passive ceiling speaker
- Neither project requires battery power — both permanently mains-powered
- I2S pins are software-assigned on ESP32 — any available GPIO can be used
