# CubeSat Firmware v1 (superseded)

The first working versions of the CubeSat flight logger and its ground station. They
flew, and the current sketches grew out of them. Kept here for reference and for the
record of what changed. **For a real flight use [`../cubesat_firmware`](../cubesat_firmware)
instead.**

| Sketch | Superseded by |
|---|---|
| [`cubesat_logger_v1`](cubesat_logger_v1) | [`../cubesat_firmware/cubesat_logger`](../cubesat_firmware/cubesat_logger) |
| [`cubesat_receiver_v1`](cubesat_receiver_v1) | [`../cubesat_firmware/cubesat_receiver`](../cubesat_firmware/cubesat_receiver) |

The LoRa block is byte-identical between v1 and the current logger — same frequency, SF10,
62.5 kHz, CR 4/8, 20 dBm, same five-field packet. So **v1 and the current ground station
interoperate in both directions**, which is useful if one board is flashed and the other
is not.

---

## What changed in the logger

**The log file.** v1 appends to `/log.csv`: eight unlabelled columns, dot decimals, comma
separators, no header line. Nothing in the file says what the columns are, so it only
makes sense next to the source code. The current version writes `/data.csv` with a `sep=;`
hint for Excel, a named header row, comma decimals for a Russian locale, and an empty cell
instead of `nan` when a sensor does not answer.

**What actually gets recorded.** v1 logs eight values:

```
alt, t1, press, t2, hum, lux, hdg, sat
```

It reads the MPU6050 every cycle via `mpu.getEvent()` and throws the result away, and it
has GPS parsed and available but stores neither coordinates nor time. The current version
logs 24 columns, including GPS time, latitude, longitude, GPS altitude and speed, raw
magnetometer X/Y/Z, all three accelerometer axes, all three gyro axes and the MPU's own
temperature.

So the flight data from v1 has no trajectory and no attitude history — only a scalar
altitude curve. That is the main reason to prefer the current version.

**Uninitialised magnetometer variables.** In v1's loop:

```cpp
int16_t x, y, z;
magRead(x, y, z);
```

`magRead` only writes to them when at least six bytes arrive on the I²C bus. If the read
fails, `x`, `y`, `z` keep whatever was on the stack, and that garbage goes into the heading
calculation and the log. The current version declares them as `int16_t x = 0, y = 0, z = 0`,
so a failed read logs a clean zero instead of noise.

**Serial output.** v1 prints one `Serial.print` per value. The current version prints a
formatted block including the GPS fields, which makes bench testing far easier to read.

## What changed in the ground station

**Packet validation.** This is the significant one. v1 parses with:

```cpp
sscanf(packet.c_str(), "%f,%f,%f,%f,%d", &alt, &temp, &hum, &lux, &sat);
```

`sscanf` has no idea whether the packet was complete or corrupt. It writes however many
fields it managed to match and returns a count that v1 never checks, so a truncated or
garbled frame silently overwrites the display with partial or nonsense values, and the old
values of the unmatched fields stay on screen looking current. CRC is off in the air on
both versions, so nothing upstream catches this either.

The current receiver splits the packet itself, requires exactly five numeric fields,
range-checks each one against physical limits, and rejects the packet otherwise — counting
it as a bad frame rather than displaying it.

**Three of the four screens show nothing.** v1 rotates through four pages, but the
transmitter only ever sends five values, so screens 2, 3 and 4 are mostly
`NO DATA (TX)` placeholders for GPS coordinates, MPU data, compass and pressure. The
current receiver has two pages that are both full: flight data on one, link quality on the
other.

**No link statistics.** v1 shows RSSI and nothing else. The current version tracks SNR,
minimum/maximum/mean RSSI, an estimated packet-loss count, bad-frame count, outage count
and longest outage, announces loss and recovery of the link in the serial log, prints a
CSV row per packet, derives peak altitude and vertical speed from the received altitude,
and can serve a live web panel over its own Wi-Fi access point.

**`while (1);` on radio failure.** A tight empty loop with no `delay()`. On an ESP32 that
starves the idle task and the watchdog may reset the board, which looks like a boot loop
rather than a clear "LoRa failed" message. The current version uses `while (1) delay(1000);`.

**`LoRa.setTxPower(20)` on a receiver** does nothing useful — transmit power has no effect
on reception. Harmless, but it suggests the settings block was copied from the transmitter
wholesale, which is also why the comment claims every setting must match when only the
modem parameters actually must.

**`setGain()` ordering.** v1 never calls `setGain()`, so it avoids the trap by accident.
The current receiver does call it, deliberately before `setSpreadingFactor()` and
`setSignalBandwidth()`, because `setGain()` rewrites `MODEM_CONFIG_3` and would otherwise
clear the `LowDataRateOptimize` bit that SF10 / 62.5 kHz requires.

## What did not change

Both versions still carry these, and the notes in [`../cubesat_firmware/README.md`](../cubesat_firmware/README.md) apply to them equally:

- the magnetometer byte-order problem — `Wire.read() | (Wire.read() << 8)` leaves the
  order of the two reads up to the compiler
- heading measured from the orientation at power-up rather than from magnetic north
- the magnetometer at `0x2C` identified nowhere
- `begin()` return values ignored for every sensor
- MPU6050 left at its ±2 g / ±250 °/s defaults
- BH1750 left at its default MTreg, saturating around 65 000 lx
- sea-level pressure hardcoded to 1013.25 hPa
- one append-only log file with no marker between flights
- the pointless manual `LORA_SS` toggling around `LoRa.beginPacket()`
