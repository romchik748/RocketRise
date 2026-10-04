# CubeSat Firmware

Flight computer and ground station for the CubeSat. Both run on ESP32 DevKit modules and
talk to each other over LoRa at 433 MHz.

| Sketch | Role |
|---|---|
| [`cubesat_logger`](cubesat_logger) | flight computer — 6 sensors + GPS → microSD, LoRa downlink |
| [`cubesat_receiver`](cubesat_receiver) | ground station — LoRa receive, OLED, link statistics |

The satellite logs everything to its own card at full rate and sends a short summary over
the radio, so the detailed data survives even if the link does not.

---

## Hardware

**Flight computer** — ESP32 DevKit with BMP280 (pressure, temperature, barometric
altitude), SHT31 (temperature, humidity), BH1750 (illuminance), MPU6050 (accelerometer,
gyroscope), a magnetometer at I²C `0x2C`, a GPS receiver on UART2, a microSD card, a
recovery buzzer, and an SX1278-class LoRa module (Ra-01).

**Ground station** — ESP32 DevKit, the matching Ra-01 module, and a 1.3" SH1106 OLED. It
can raise its own Wi-Fi access point with a live web panel: set `USE_WIFI 1`, join
`CubeSat`, open `http://192.168.4.1`. Off by default.

## Pinout

### Flight computer

| Signal | GPIO | Bus |
|---|---|---|
| SDA | 21 | I²C — BMP280, SHT31, BH1750, MPU6050, magnetometer |
| SCL | 22 | I²C |
| SCK | 18 | SPI — shared by microSD and LoRa |
| MISO | 19 | SPI |
| MOSI | 23 | SPI |
| SD CS | 13 | SPI |
| LoRa NSS | 5 | SPI |
| LoRa RST | 14 | — |
| LoRa DIO0 | 26 | — |
| GPS RX | 16 | UART2 @ 9600 |
| GPS TX | 17 | UART2 |
| Buzzer | 12 | — |

### Ground station

| Signal | GPIO |
|---|---|
| LoRa NSS / RST / DIO0 | 5 / 14 / 26 |
| SCK / MISO / MOSI | 18 / 19 / 23 |
| OLED SDA / SCL | 21 / 22 |

LoRa module and display both run from the board's 3.3 V pin.

### I²C addresses

| Device | Address |
|---|---|
| BMP280 | `0x76` |
| MPU6050 | `0x68` |
| BH1750 | `0x23` |
| SHT31 | `0x44` |
| Magnetometer | `0x2C` |

> **GPIO6–11 are unusable on the classic ESP32** — they are wired to the chip's own SPI
> flash. Driving them disconnects the CPU from its firmware and the board dies instantly
> with no serial output. Nothing here touches them; keep it that way when adding devices.

## LoRa link

Both ends must agree on every parameter below. The transmitter sets them explicitly; the
receiver mirrors them and leaves sync word, preamble and CRC at the library defaults
because the transmitter does too.

| Parameter | Value | Why |
|---|---|---|
| Frequency | 433 MHz | — |
| Spreading factor | SF10 | slow but long-reaching |
| Bandwidth | 62.5 kHz | narrower band, better sensitivity |
| Coding rate | 4/8 | maximum forward error correction |
| TX power | 20 dBm | maximum for the module |
| Sync word | `0x12` | library default, unchanged on both sides |
| CRC | disabled | library default — see Known issues |
| Effective bit rate | ≈ 305 bit/s | `SF × BW/2^SF × 4/(4+4)` |

At SF10 / 62.5 kHz a symbol lasts 16.4 ms, so a ~30-byte packet occupies the air for
roughly 1.4 s. With the logger's `delay(1000)` and sensor reads on top, packets leave
about every 2.4 s — which is what the receiver's `TX_INTERVAL` is set to.

> **Ordering trap, already handled in the receiver.** `LoRa.setGain()` rewrites
> `MODEM_CONFIG_3`, and that register also holds the `LowDataRateOptimize` bit, which is
> mandatory at SF10 / 62.5 kHz. `setSpreadingFactor()` and `setSignalBandwidth()` set that
> bit themselves, so `setGain()` has to be called **before** them or it silently clears it
> and the link stops working.

### Packet format

Plain ASCII, comma-separated, no header and no trailing newline:

```
alt,t1,hum,lux,sats
295.85,27.71,36.61,1730.83,0
```

| Field | Meaning | Source |
|---|---|---|
| `alt` | barometric altitude, m (referenced to 1013.25 hPa) | BMP280 |
| `t1` | temperature, °C | BMP280 |
| `hum` | relative humidity, % | SHT31 |
| `lux` | illuminance, lx | BH1750 |
| `sats` | GPS satellites in view | GPS |

There is no sequence counter in the packet, so the receiver **estimates** losses from the
gaps between arrivals: a silence of roughly *n* × `TX_INTERVAL` counts as *n* − 1 missed
packets. Peak altitude and vertical speed are not transmitted either — the ground station
derives them from the altitude it receives, smoothing vertical speed with a 0.6/0.4
exponential filter.

Because CRC is off in the air, the receiver's parser is the only thing standing between a
corrupted frame and the log. It therefore insists on exactly five numeric fields and
range-checks each one before accepting a packet.

## Building

Arduino IDE with the ESP32 core by Espressif. Board: **ESP32 Dev Module** for both
sketches. Upload at 115200 — at higher speeds long uploads drop frames and leave the
flash half-written, which shows up as a serial port that appears and disappears while the
chip boot-loops.

| Library | Used by |
|---|---|
| Adafruit BMP280 Library | logger |
| Adafruit Unified Sensor | logger |
| Adafruit MPU6050 | logger |
| Adafruit SHT31 | logger |
| BH1750 (Christopher Laws) | logger |
| TinyGPSPlus | logger |
| LoRa (Sandeep Mistry) | both |
| GyverOLED | receiver |

## Output formats

Both write CSV for a Russian-locale Excel: `sep=;` on the first line, `;` between columns,
commas as the decimal separator. A sensor that stops answering writes an empty cell rather
than `nan`, so charts keep working across the gap.

**Flight computer** — a single `data.csv` on the card, one row per second:

```
Time_s;GPS_Time_UTC;Lat;Lon;GPS_Alt_m;Speed_kmh;Sats;Alt_m;T_BMP_C;Press_hPa;
T_SHT_C;Hum_%;Lux;Mag_X;Mag_Y;Mag_Z;Heading_deg;Acc_X_ms2;Acc_Y_ms2;Acc_Z_ms2;
Gyro_X_rads;Gyro_Y_rads;Gyro_Z_rads;T_MPU_C
```

**Ground station** — printed to its serial port, one row per received packet. Data rows
are prefixed with `$` so they can be separated from the human-readable summary that
`VERBOSE` also prints:

```
RX;RX_TIME_S;RSSI_DBM;SNR_DB;LOST_EST;ALT_M;ALT_MAX_M;VSPEED_MS;T_BMP_C;HUM_PCT;LUX;SATS
```

## Before a launch

- **Sea-level pressure.** `bmp.readAltitude(1013.25)` is hardcoded. Replace it with the
  day's QNH — altitude comes from `h = 44330 × (1 − (P/101325)^0.1903)`, so the default
  biases every reading.
- **Accelerometer range.** `mpu.begin()` leaves the MPU6050 at ±2 g and ±250 °/s. Launch
  and landing will clip. Add `mpu.setAccelerometerRange(MPU6050_RANGE_8_G)` and
  `mpu.setGyroRange(MPU6050_RANGE_1000_DEG)`.
- **BH1750 sensitivity.** At the default MTreg of 69 the sensor saturates around
  65 000 lx, and direct sunlight is roughly 100 000 lx. `lightMeter.setMTreg(32)` extends
  the range to about 140 000 lx and shortens a conversion from 120 ms to ~55 ms.
- **Buzzer delay.** The logger starts beeping 180 s after power-up so it can be found
  after landing. Shorten it for bench work or it will beep at you all session.

## Known issues

### Flight computer

- **Magnetometer byte order is undefined.** `x = Wire.read() | (Wire.read() << 8)` leaves
  the order in which the two reads happen up to the compiler — C++ does not sequence the
  operands of `|`. The axes may come out byte-swapped, and a toolchain update can change
  the result without any code change. Read into named variables first.
- **Heading is relative to power-up, not to north.** `mx0/my0` are captured from a single
  reading 1 s after boot and subtracted as an offset. That zeroes the heading at whatever
  orientation the satellite happened to be in, and it is not a hard-iron calibration, so
  `Heading_deg` should be read as "degrees turned since startup".
- **The magnetometer is not identified anywhere.** Only the address `0x2C` and register
  `0x0A ← 0x01` appear. The part number should be recorded so the scale factor and
  register map can be checked.
- **Sensor `begin()` results are ignored.** A sensor that failed to initialise is
  indistinguishable at startup from one that works; it just produces NaN or nonsense
  later.
- **`data.csv` is one append-only file.** Flights concatenate with no marker between them,
  and there is no reset-reason logging. Watch for `Time_s` jumping back to zero mid-file.
- **The manual `LORA_SS` toggling around `LoRa.beginPacket()` does nothing.** The LoRa
  library drives its own NSS line on every register access, so the explicit
  `digitalWrite()` pair is overridden immediately. Harmless, but misleading to read.

### Link

- **No CRC in the air.** Both ends leave it at the library default (off), so corrupt frames
  reach the parser and are rejected by its range checks. Calling `LoRa.enableCrc()` on
  *both* sides would let the radio itself discard them, which is cheaper and catches
  corruption that happens to land inside the valid ranges.
- **No sequence counter**, so packet loss can only be estimated from timing. One byte of
  counter would make the figure exact and remove the dependence on a guessed
  `TX_INTERVAL`.
- **The receiver's CSV header line is not `$`-prefixed** while its data rows are, so a
  filter on `$` drops the header along with the prose.
