# CanSat Platform

Flight firmware for a set of student spacecraft electronics: two custom CanSat sensor
boards, a CubeSat flight logger with LoRa telemetry, and the ground station that receives
it. The CanSat boards are custom 2-layer PCBs designed in KiCad and assembled by hand
with through-hole parts; the CubeSat and the ground station are built on ESP32 DevKit
modules.

| Sketch | Platform | Role |
|---|---|---|
| [`cansat_mpu6050_v3`](firmware/cansat_mpu6050_v3) | ESP32-C3 SuperMini | CanSat board 1 — BMP280 + MPU6050 → microSD |
| [`cansat_bh1750_v2`](firmware/cansat_bh1750_v2) | ESP32-C3 SuperMini | CanSat board 2 — BMP280 + BH1750 → microSD |
| [`cubesat_logger`](firmware/cubesat_logger) | ESP32 DevKit | CubeSat flight computer — 6 sensors + GPS → microSD, LoRa downlink |
| [`cubesat_receiver`](firmware/cubesat_receiver) | ESP32 DevKit | Ground station — LoRa receive, OLED, link statistics |
| [`sd_test_v2`](firmware/sd_test_v2) | either ESP32 | Bench tool — microSD bring-up and integrity test |

The two CanSat boards fly without a radio: data is recovered from the card after landing.
The CubeSat logs to its own card *and* sends a live summary over LoRa, so the ground
station can follow the flight and the full-rate data survives even if the link does not.

---

## Hardware

### CanSat boards

- **MCU** — ESP32-C3 SuperMini (RISC-V, native USB-CDC, 3.3 V logic)
- **Pressure / temperature** — BMP280 breakout, I²C
- **IMU** — MPU6050 breakout, I²C (board 1)
- **Ambient light** — BH1750 GY-302 breakout, I²C (board 2)
- **Storage** — microSD breakout, SPI
- **Power** — single 18650 cell into the module's 5 V pin through a slide switch and a
  JST-PH 2-pin connector; the module's on-board LDO produces the 3.3 V rail
- **Decoupling** — bulk electrolytic plus local ceramics at each module's supply pin

Boards are 36 × 90 mm, two layers, ground pour on both sides, 0.5 mm power tracks and
0.3 mm signal tracks.

### CubeSat flight computer

ESP32 DevKit carrying BMP280 (pressure, temperature, barometric altitude),
SHT31 (temperature, humidity), BH1750 (illuminance), MPU6050 (accelerometer, gyroscope),
a magnetometer at I²C `0x2C`, a GPS receiver on UART2, a microSD card, a recovery buzzer,
and an SX1278-class LoRa module (Ra-01) at 433 MHz.

### Ground station

ESP32 DevKit, the matching Ra-01 LoRa module, and a 1.3" SH1106 OLED. It can optionally
raise its own Wi-Fi access point with a live web panel — set `USE_WIFI 1` in the sketch,
then join `CubeSat` and open `http://192.168.4.1`. It is off by default.

## Pinout

### CanSat boards

Identical on both boards, so one wiring diagram covers them.

| Signal | GPIO | Bus | Goes to |
|---|---|---|---|
| SDA | 4 | I²C | BMP280, MPU6050 / BH1750 |
| SCL | 5 | I²C | BMP280, MPU6050 / BH1750 |
| MOSI | 6 | SPI | microSD |
| CS | 7 | SPI | microSD |
| CLK | 10 | SPI | microSD |
| MISO | 20 | SPI | microSD |
| LED | 8 | — | status LED on the SuperMini module (active low) |

The ESP32-C3 routes SPI through its GPIO matrix, so these pin choices are free rather
than fixed by silicon.

### CubeSat flight computer

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

| Device | Address | Set by |
|---|---|---|
| BMP280 | `0x76` | SD0 pulled to GND (CSB pulled to 3V3 selects I²C) |
| MPU6050 | `0x68` | AD0 pulled to GND |
| BH1750 | `0x23` | ADDR pulled to GND |
| SHT31 | `0x44` | default (CubeSat only) |
| Magnetometer | `0x2C` | CubeSat only |

### Status LED (CanSat boards)

| Pattern | Meaning |
|---|---|
| 3 short flashes at startup | all sensors and the card were found |
| 1 long flash at startup | something is missing — read the serial output |
| short flash once per second | alive and flushing to the card |
| fast continuous blinking | no card; sensors are still being read |

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

Arduino IDE with the ESP32 core by Espressif.

| Sketch | Board | Notes |
|---|---|---|
| `cansat_*` | **ESP32C3 Dev Module** | USB CDC On Boot: **Enabled** |
| `cubesat_*` | **ESP32 Dev Module** | — |
| `sd_test_v2` | either | picks its pins from the selected target |

Upload at **115200**. At 921600 long uploads drop frames and leave the flash
half-written, which shows up as a serial port that appears and disappears while the chip
boot-loops.

On the ESP32-C3, `USB CDC On Boot` must be enabled or the chip prints nothing to the
monitor even when the sketch is running perfectly — the port is created by the firmware
itself.

**Libraries**

| Library | Used by |
|---|---|
| Adafruit BMP280 Library | all flight sketches |
| Adafruit Unified Sensor | all flight sketches |
| Adafruit MPU6050 | CanSat board 1, CubeSat |
| BH1750 (Christopher Laws) | CanSat board 2, CubeSat |
| Adafruit SHT31 | CubeSat |
| TinyGPSPlus | CubeSat |
| LoRa (Sandeep Mistry) | CubeSat, ground station |
| GyverOLED | ground station |

`<WiFi.h>` is deliberately **not** included in the CanSat sketches. The radio does not
start unless it is initialised, so there is nothing to switch off — and including the
header pushed the binary close to 1 MB, which was enough to make uploads fail partway
through.

## Output formats

Every CSV is written for a Russian-locale Excel: the first line is `sep=;`, columns are
separated by `;`, and decimals use commas. A sensor that stops answering writes an empty
cell rather than `nan`, so charts keep working across the gap and logging never stops
because one sensor dropped out.

The CanSat sketches have an `EXCEL_RU` switch — set it to `false` for dot-decimal,
comma-separated output that pandas reads with no arguments.

**CanSat board 1** — `log_001.csv`, `log_002.csv`, … (one file per flight)

```
time_s;temp_C;pressure_Pa;altitude_m;ax_m_s2;ay_m_s2;az_m_s2;gx_rad_s;gy_rad_s;gz_rad_s;mpu_temp_C;boot
```

**CanSat board 2**

```
time_s;temp_C;pressure_Pa;altitude_m;lux;boot
```

**CubeSat** — a single `data.csv`, appended across flights, one row per second

```
Time_s;GPS_Time_UTC;Lat;Lon;GPS_Alt_m;Speed_kmh;Sats;Alt_m;T_BMP_C;Press_hPa;
T_SHT_C;Hum_%;Lux;Mag_X;Mag_Y;Mag_Z;Heading_deg;Acc_X_ms2;Acc_Y_ms2;Acc_Z_ms2;
Gyro_X_rads;Gyro_Y_rads;Gyro_Z_rads;T_MPU_C
```

**Ground station** — printed to its serial port, one row per received packet. Data rows
are prefixed with `$` so they can be separated from the human-readable summary that
`VERBOSE` also prints.

```
RX;RX_TIME_S;RSSI_DBM;SNR_DB;LOST_EST;ALT_M;ALT_MAX_M;VSPEED_MS;T_BMP_C;HUM_PCT;LUX;SATS
```

### Things to set before a launch

- **`SEALEVEL_HPA`** (CanSat) and the `1013.25` argument to `bmp.readAltitude()`
  (CubeSat) — the day's QNH. Altitude is derived from pressure with
  `h = 44330 × (1 − (P/101325)^0.1903)`, so leaving the default in place biases every
  altitude reading.
- **`BH1750_MTREG`** — 32 on CanSat board 2. At the library default of 69 the sensor
  saturates around 65 000 lx, and direct sunlight is roughly 100 000 lx, so daytime
  readings would simply sit at the ceiling. MTreg 32 extends the range to about
  140 000 lx and shortens a conversion from 120 ms to ~55 ms. The CubeSat sketch still
  uses the default.
- **Buzzer delay** — the CubeSat starts beeping 180 s after power-up, to be found after
  landing. Shorten it for bench work or it will beep at you for the whole session.

## Reset diagnostics (CanSat boards)

A board that reboots mid-flight used to start a new log file each time, which looked like
a filename bug but was not one: `setup()` runs again, so it picks the next free name.

The current firmware handles this directly:

- `esp_reset_reason()` is printed at startup and appended to **`boots.txt`** on the card,
  naming the cause — `BROWNOUT` (supply sag), `PANIC` (crash), `TASK_WDT` (hung loop),
  `POWERON`, `EXT` (reset button).
- The log index and accumulated uptime live in RTC memory, which survives every reset
  except a full power loss. A reboot therefore **appends to the same file** instead of
  starting a new one, and `time_s` continues rather than dropping back to zero.
- The `boot` column carries the run number, so the exact row where a reset happened is
  visible in Excel.

```
log_009.csv  boot #1  t_base=0.0 s   reset=POWERON - normal start
log_009.csv  boot #2  t_base=14.0 s  reset=BROWNOUT - supply sag
```

One detail worth knowing if you read the code: on ESP32, `FILE_WRITE` **truncates** and
only `FILE_APPEND` appends — the opposite of the classic Arduino SD library, where
`FILE_WRITE` appended.

## Repository layout

```
firmware/
  cansat_mpu6050_v3/   CanSat board 1 — BMP280 + MPU6050 + SD
  cansat_bh1750_v2/    CanSat board 2 — BMP280 + BH1750  + SD
  cubesat_logger/      CubeSat flight computer — sensors + GPS + SD + LoRa downlink
  cubesat_receiver/    ground station — LoRa + OLED + link statistics
  sd_test_v2/          microSD bring-up test (ESP32-C3 and classic ESP32)
```

## Known issues

### CanSat boards

- **Accelerometer scale.** Over 172 quiet windows of real flight data, the magnitude of
  gravity read 9.399 m/s² against an expected 9.807 — about 4.2 % low. A six-position
  calibration would correct it; it is not applied yet.
- **Gyro range.** `±500 °/s` came close to the rail during descent.
  `MPU6050_RANGE_1000_DEG` is the safer setting.
- **SD module supply.** The small bare microSD breakout runs from 3V3. The larger HW-125
  style module (AMS1117 regulator plus a 74LVC125 buffer) needs **5 V** — its regulator
  drops out at 3.3 V and leaves the card at roughly 2.2 V. The current boards route SD
  power to +3.3V, so a large module needs a cut and a jumper to +5V.
- **J1 annular ring** is 0.225 mm, under JLCPCB's recommended 0.25 mm but above the
  0.18 mm absolute minimum. Everything else clears their 2-layer limits.

### CubeSat logger

- **Magnetometer byte order is undefined.** `x = Wire.read() | (Wire.read() << 8)` leaves
  the order in which the two reads happen up to the compiler — C++ does not sequence the
  operands of `|`. The axes may come out byte-swapped, and a toolchain update can change
  the result without any code change. Read into named variables first.
- **Heading is relative to power-up, not to north.** `mx0/my0` are captured from a single
  reading 1 s after boot and subtracted as an offset. That zeroes the heading at the
  orientation the satellite happened to be in, and it is not a hard-iron calibration, so
  `Heading_deg` should be read as "degrees turned since startup".
- **The magnetometer is not identified anywhere.** Only the address `0x2C` and register
  `0x0A ← 0x01` appear. The part number should be recorded so the scale factor and
  register map can be checked.
- **MPU6050 runs at its defaults** — ±2 g and ±250 °/s, because `mpu.begin()` is not
  followed by range calls. Launch and landing will clip. The CanSat sketches set ±8 g.
- **Sensor `begin()` results are ignored.** A sensor that failed to initialise is
  indistinguishable at startup from one that works; it just produces NaN or nonsense
  later. The CanSat sketches check and report each one.
- **BH1750 uses the default MTreg**, so it saturates around 65 000 lx — see the pre-launch
  notes above.
- **`data.csv` is one append-only file.** Flights concatenate with no marker between them,
  and there is no reset-reason logging as on the CanSat boards. Watch for `Time_s` jumping
  back to zero mid-file.
- **The manual `LORA_SS` toggling around `LoRa.beginPacket()` does nothing.** The LoRa
  library drives its own NSS line on every register access, so the explicit
  `digitalWrite()` pair is overridden immediately. Harmless, but misleading to read.

### Link

- **No CRC in the air.** Both ends leave it at the library default (off), so corrupt
  frames reach the parser and are rejected by its range checks. Calling `LoRa.enableCrc()`
  on *both* sides would let the radio itself discard them, which is cheaper and catches
  corruption that happens to land inside the valid ranges.
- **No sequence counter**, so packet loss can only be estimated from timing. One byte of
  counter in the packet would make the loss figure exact.
- **The receiver's CSV header line is not `$`-prefixed** while its data rows are, so a
  filter on `$` drops the header along with the prose.
