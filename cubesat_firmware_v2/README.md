# CubeSat Firmware v2

The current generation of the CubeSat flight computer and its ground station. Both run on
ESP32 DevKit modules and talk over LoRa at 434.5 MHz.

| Sketch | Role |
|---|---|
| [`tx_full_log`](tx_full_log) | flight computer — 6 sensors + GPS → microSD, LoRa downlink |
| [`rx_gps_alt_rssi`](rx_gps_alt_rssi) | ground station — LoRa receive, OLED, link status |

Two things separate this generation from [`../cubesat_firmware_v1`](../cubesat_firmware_v1):
**GPS coordinates now go down the radio link**, so the satellite can be located during and
after the flight, and the link is **CRC-protected**, so corrupt frames are discarded by the
radio rather than displayed as data.

---

## Hardware

ESP32 DevKit carrying BMP280 (pressure, temperature, barometric altitude),
SHT31 (temperature, humidity), BH1750 (illuminance), MPU6050 (accelerometer, gyroscope),
a magnetometer at I²C `0x2C`, a GPS receiver on UART2, a microSD card, a recovery buzzer
and an SX1278-class LoRa module (Ra-01).

The ground station is an ESP32 DevKit with the matching Ra-01 and a 1.3" SH1106 OLED.

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

| Parameter | Value |
|---|---|
| Frequency | **434.5 MHz** (was 433 MHz in v1) |
| Spreading factor | SF10 |
| Bandwidth | 62.5 kHz |
| Coding rate | 4/8 |
| TX power | 20 dBm |
| CRC | **enabled on both sides** |
| Payload | 20 bytes, binary |
| Airtime | ≈ 1.12 s per packet |
| Packet interval | ≈ 2.2 s |

Both ends must agree on every modem parameter. Neither sketch calls `LoRa.setGain()`, so
the `LowDataRateOptimize` trap that bit earlier versions does not apply here — but if you
ever add it, it has to come **before** `setSpreadingFactor()` and `setSignalBandwidth()`,
because it rewrites the same register that holds that bit.

With CRC on, `parsePacket()` returns 0 for a frame that failed its checksum, so corruption
is dropped by the radio itself. The receiver additionally requires
`packetSize == sizeof(Telemetry)` and reports anything else as interference.

### Packet format

A raw binary struct, not text — identical declaration on both sides:

```cpp
struct Telemetry {
  float   alt;      // barometric altitude, m (referenced to 1013.25 hPa)
  float   lat;      // latitude, degrees  — NAN until GPS has a fix
  float   lng;      // longitude, degrees — NAN until GPS has a fix
  float   gpsAlt;   // GPS altitude, m    — NAN until GPS has a fix
  int32_t sat;      // satellites in view
};
```

`int32_t` rather than `int`: the width of `int` is platform-dependent, and this struct
goes over the air, so both sides must agree byte for byte. Both sketches print
`sizeof(Telemetry)` at startup, which makes a format mismatch visible in the first line of
the serial monitor instead of as unexplained silence in the field.

Temperature, humidity and illuminance are deliberately **not** transmitted: the ground
station never displayed them, and they cost 12 bytes of airtime. They are still written to
the card. Trimming them took the packet from 32 to 20 bytes and the airtime from ~1.51 s
to ~1.12 s.

The receiver derives two more things locally, which are not in the packet:

- **RSSI** — measured by its own radio with `LoRa.packetRssi()`
- **Link freshness** — from the age of the last packet: `[ LINK OK ]` up to 3 s,
  then `[ LOST 12s ]`, and `[ NO SIGNAL ]` before the first packet ever arrives

That last one matters. In v0 and v1 the display only refreshed when a packet arrived, so
the last received numbers sat there looking current through an outage of any length.

## Output format

One file, `/data.csv`, appended across flights. The schema is identical to
[`../cubesat_firmware/cubesat_logger`](../cubesat_firmware/cubesat_logger), so both
aircraft produce files one script can read.

First line is `sep=;`, then 24 named columns, separated by `;`, decimals written with
commas — a Russian-locale Excel opens it on a double click:

```
Time_s;GPS_Time_UTC;Lat;Lon;GPS_Alt_m;Speed_kmh;Sats;
Alt_m;T_BMP_C;Press_hPa;T_SHT_C;Hum_%;Lux;
Mag_X;Mag_Y;Mag_Z;Heading_deg;
Acc_X_ms2;Acc_Y_ms2;Acc_Z_ms2;
Gyro_X_rads;Gyro_Y_rads;Gyro_Z_rads;T_MPU_C
```

A sensor that does not answer writes an **empty cell** rather than `nan`, so charts keep
working across the gap.

`GPS_Time_UTC` is the only absolute timestamp in the log — `Time_s` counts from power-up
and restarts at zero every flight. Use UTC to line the CubeSat's log up against the ground
station's record or against video.

> The file is called `data.csv`, not `log.csv`, on purpose. An older `log.csv` on the card
> uses comma separators and dot decimals; appending the new format to it would give Excel
> a file with two incompatible conventions in it.

## Building

Arduino IDE with the ESP32 core by Espressif. Board: **ESP32 Dev Module** for both
sketches, upload at 115200.

| Library | Used by |
|---|---|
| Adafruit BMP280 Library | flight computer |
| Adafruit Unified Sensor | flight computer |
| Adafruit MPU6050 | flight computer |
| Adafruit SHT31 | flight computer |
| BH1750 (Christopher Laws) | flight computer |
| TinyGPSPlus | flight computer |
| LoRa (Sandeep Mistry) | both |
| GyverOLED | ground station |

## Before a launch

- **Sea-level pressure.** `bmp.readAltitude(1013.25)` is hardcoded. Replace it with the
  day's QNH — altitude comes from `h = 44330 × (1 − (P/101325)^0.1903)`, so the default
  biases every reading.
- **Accelerometer range.** `mpu.begin()` leaves the MPU6050 at ±2 g and ±250 °/s. Launch
  and landing will clip. Add `mpu.setAccelerometerRange(MPU6050_RANGE_8_G)` and
  `mpu.setGyroRange(MPU6050_RANGE_1000_DEG)`.
- **BH1750 sensitivity.** At the default MTreg of 69 the sensor saturates around
  65 000 lx; direct sunlight is roughly 100 000 lx. `lightMeter.setMTreg(32)` extends the
  range to about 140 000 lx.
- **Buzzer delay.** Beeping starts 180 s after power-up so the satellite can be found
  after landing. Shorten it for bench work.

## Known issues

- **A dead radio stops the flight log.** `LoRa.begin()` failing drops the sketch into
  `while (1);` inside `setup()`, before the loop ever runs — so a faulty radio module takes
  the microSD logging and the buzzer down with it. Telemetry is the secondary system and
  the log is the primary result of the flight, so this is backwards. It should set a
  `loraOk = false` flag and carry on. The empty `while (1);` also starves the ESP32's idle
  task, so the watchdog resets the board and the symptom looks like a random boot loop
  rather than a clear failure message.
- **Magnetometer byte order is undefined.** `x = Wire.read() | (Wire.read() << 8)` leaves
  the order of the two reads up to the compiler — C++ does not sequence the operands of
  `|`. The axes may come out byte-swapped, and a toolchain update can change the result
  with no code change. Read into named variables first.
- **Heading is relative to power-up, not to north.** `mx0/my0` are captured from a single
  reading 1 s after boot and subtracted as an offset. Read `Heading_deg` as "degrees
  turned since startup", not as a compass bearing.
- **The magnetometer is not identified anywhere.** Only the address `0x2C` and register
  `0x0A ← 0x01` appear. Record the part number so the scale factor and register map can be
  checked.
- **Sensor `begin()` results are ignored.** A sensor that failed to initialise is
  indistinguishable at startup from one that works; it only shows up as empty cells later.
- **The manual `digitalWrite()` calls on `SD_CS` and `LORA_SS`** around the card access do
  nothing — both libraries drive their own chip-select on every transaction and overwrite
  these immediately. Harmless, but they imply a bus-sharing trick that is not there.
- **One append-only log file** with no marker between flights and no reset-reason record.
  Watch for `Time_s` jumping back to zero mid-file.
