# Launch Controller

Handheld ignition controller for the rocket. An Arduino board with a 1.3" SH1106 OLED,
four buttons and one power output: it sets a delay, counts it down, then energises a relay
or MOSFET that fires the igniter. The liftoff is accompanied by an animated rocket on the
display.

This is the only sketch in the project that does **not** run on an ESP32.

---

## Hardware

- **MCU** — Arduino (Uno / Nano class, 5 V, ATmega328P)
- **Display** — 1.3" OLED, SH1106 controller, I²C
- **Controls** — four momentary buttons
- **Output** — one digital pin driving a relay module or a MOSFET gate

## Pinout

| Pin | Function |
|---|---|
| D2 | **FIRE** — red launch button |
| D3 | delay − |
| D4 | **ARM** — wakes the controller from the splash screen |
| D5 | delay + |
| D6 | power output → relay / MOSFET |
| A4 / A5 | I²C to the display (SDA / SCL on an Uno or Nano) |
| A0 | unused — read once as a random seed for the starfield |

Buttons are read **active-HIGH**, which means each one needs an external pull-down
resistor to GND (10 kΩ is the usual value) with the button connecting the pin to +5 V.
See Known issues — this matters more here than on an ordinary project.

## How it works

1. **Splash screen** — the RocketRise logo, drawn from a 31 × 40 bitmap held in PROGMEM.
2. **Press ARM (D4)** — switches to the delay screen.
3. **Set the delay** with − and + (D3 / D5). Range 1 … 60 s, default 10 s.
4. **Press FIRE (D2)** — a one-second-per-step countdown fills the screen.
5. **At zero** — D6 goes HIGH and the liftoff animation plays: a starfield, smoke clouds
   at the pad, a flame trail, screen shake for the first six frames, and the rocket rising
   out of the top of the display.
6. **When the animation ends** — D6 returns LOW and the controller goes back to the delay
   screen.

The animation is drawn with GyverOLED primitives rather than stored frames: the fins are
filled by sweeping vertical lines across the triangle, the flame cycles through three
shapes per frame, and the stars blink by skipping every fifth one on a rolling offset.

## Building

Arduino IDE. Board: **Arduino Uno** or **Arduino Nano** to match the hardware.

| Library | Note |
|---|---|
| GyverOLED | by AlexGyver — provides the SH1106 driver and the drawing primitives |

`Wire` is part of the core and needs no installation.

## Known issues

These are worth reading before the next launch. The first three concern a pin that fires
an igniter, so they are not cosmetic.

- **Floating inputs on a firing circuit.** The buttons are configured as plain `INPUT`
  and read active-HIGH. An `INPUT` pin with nothing driving it is floating: it picks up
  noise and can read HIGH on its own, and on D2 that starts a countdown and fires the
  igniter with nobody touching the controller. This is safe **only** if every button pin
  has an external pull-down resistor fitted on the board. If they are not fitted, the fix
  is to wire the buttons between the pin and GND and switch to `INPUT_PULLUP` with
  active-LOW reads (`== LOW`), which needs no external parts because the pull-up is inside
  the chip.

- **The countdown cannot be aborted.** The countdown is a blocking
  `for (…) { … delay(1000); }` loop that polls nothing, so once FIRE is pressed there is no
  way to stop it short of cutting power. An abort path — polling a button inside the loop
  and breaking out of it — is standard on ignition controllers.

- **The firing pulse has no defined length.** D6 is raised before the animation and
  lowered after it, so the igniter stays energised for however long 37 frames of drawing
  happen to take — roughly 2 to 5 seconds depending on the I²C clock, and it changes
  whenever the animation is edited. The pulse length should be an explicit constant
  (`FIRE_DURATION_MS`) that the animation cannot influence.

- **The controller never disarms.** `isSystemActive` is set once and never cleared, so
  after a launch the controller returns straight to the delay screen, one button press
  away from firing again. Returning to the splash screen — disarmed — after each launch
  would match how the rest of the flow is built.

- **All four buttons share one debounce timestamp.** `lastDebounceTime` is global, so any
  press blocks every other button for 200 ms, and holding ARM keeps FIRE from registering.
  One timestamp per button fixes it.

- **The display is declared without a frame buffer.** This sketch uses
  `GyverOLED<SSH1106_128x64>`, which defaults to the unbuffered mode, while the CubeSat
  ground station uses `GyverOLED<SSH1106_128x64, OLED_BUFFER>`. Pixel graphics —
  `dot`, `line`, `circle`, `rect` — need the buffer, because the SH1106 cannot be read back
  over I²C to do a read-modify-write on a byte. Expect the animation to show artefacts or
  not render at all until `OLED_BUFFER` is added. The cost is 1 KB of SRAM, which is half
  of an ATmega328P's total, so check for stability after the change.

- **`Serial.begin(9600)` is called but nothing is ever printed.** Harmless, though a few
  status lines during arming and firing would make bench testing much easier.
