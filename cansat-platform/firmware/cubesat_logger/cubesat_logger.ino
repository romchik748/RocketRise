#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LoRa.h>

#include <Adafruit_BMP280.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>

#define MAG_ADDRESS 0x2C

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

#define SD_CS 13
#define BUZZER_PIN 12

// Новый файл, чтобы не смешивать со старым форматом log.csv
#define LOG_FILE "/data.csv"

Adafruit_BMP280 bmp;
Adafruit_SHT31 sht31;
BH1750 lightMeter;
Adafruit_MPU6050 mpu;

TinyGPSPlus gps;
HardwareSerial GPS(2);

// MAG
int16_t mx0, my0, mz0;

// BUZZER
unsigned long startTime;
bool buzzerActive = false;
unsigned long lastBeep = 0;

// ================= FORMAT HELPERS =================
// Excel (русская локаль): разделитель столбцов ';', десятичный разделитель ','
String fnum(float v, int decimals = 2) {
  if (isnan(v) || isinf(v)) return "";   // пустая ячейка, если датчик не ответил
  String s = String(v, decimals);
  s.replace('.', ',');
  return s;
}

String two(int v) {
  return (v < 10 ? "0" : "") + String(v);
}

String gpsTimeStr() {
  if (!gps.time.isValid()) return "";
  return two(gps.time.hour()) + ":" + two(gps.time.minute()) + ":" + two(gps.time.second());
}

// ================= MAG =================
void magInit() {
  Wire.beginTransmission(MAG_ADDRESS);
  Wire.write(0x0A);
  Wire.write(0x01);
  Wire.endTransmission();
}

void magRead(int16_t &x, int16_t &y, int16_t &z) {
  Wire.beginTransmission(MAG_ADDRESS);
  Wire.write(0x00);
  Wire.endTransmission(false);
  Wire.requestFrom(MAG_ADDRESS, 6);

  if (Wire.available() >= 6) {
    x = Wire.read() | (Wire.read() << 8);
    y = Wire.read() | (Wire.read() << 8);
    z = Wire.read() | (Wire.read() << 8);
  }
}

// ================= SD =================
void writeHeaderIfNeeded() {
  if (SD.exists(LOG_FILE)) return;

  File file = SD.open(LOG_FILE, FILE_WRITE);
  if (!file) return;

  // Подсказка Excel, какой разделитель использовать (строка скрывается при открытии)
  file.println("sep=;");
  file.println(
    "Time_s;GPS_Time_UTC;Lat;Lon;GPS_Alt_m;Speed_kmh;Sats;"
    "Alt_m;T_BMP_C;Press_hPa;"
    "T_SHT_C;Hum_%;"
    "Lux;"
    "Mag_X;Mag_Y;Mag_Z;Heading_deg;"
    "Acc_X_ms2;Acc_Y_ms2;Acc_Z_ms2;"
    "Gyro_X_rads;Gyro_Y_rads;Gyro_Z_rads;T_MPU_C"
  );
  file.close();
  Serial.println("CSV header written");
}

bool initSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  SPI.begin(18, 19, 23);

  Serial.println("=== SD INIT ===");

  bool ok = false;

  if (SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("SD OK 4MHz");
    ok = true;
  } else if (SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("SD OK 1MHz");
    ok = true;
  } else if (SD.begin(SD_CS, SPI, 400000)) {
    Serial.println("SD OK 400KHz");
    ok = true;
  }

  if (ok) {
    writeHeaderIfNeeded();
  } else {
    Serial.println("SD FAIL");
  }
  return ok;
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  Serial.println("\n=== FULL SYSTEM START ===");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  startTime = millis();

  bmp.begin(0x76);
  sht31.begin(0x44);
  lightMeter.begin();
  mpu.begin();

  magInit();

  GPS.begin(9600, SERIAL_8N1, 16, 17);

  SPI.begin(18, 19, 23);

  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);

  initSD();

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.begin(433E6);

  // ===== НАСТРОЙКИ МАКСИМАЛЬНОЙ ДАЛЬНОСТИ ДЛЯ ПЕРЕДАТЧИКА =====
  LoRa.setTxPower(20);             // Мощность на максимум (20 dBm)
  LoRa.setSpreadingFactor(10);     // Медленный, но «пробивной» режим (SF10)
  LoRa.setSignalBandwidth(62.5E3); // Суженная полоса (62.5 kHz)
  LoRa.setCodingRate4(8);          // Максимальное исправление ошибок (CR 4/8)
  // ==========================================================

  delay(1000);

  int16_t x, y, z;
  magRead(x, y, z);
  mx0 = x; my0 = y; mz0 = z;
}

// ================= LOOP =================
void loop() {
  while (GPS.available()) gps.encode(GPS.read());

  float alt   = bmp.readAltitude(1013.25);
  float t1    = bmp.readTemperature();
  float press = bmp.readPressure() / 100.0;

  float t2  = sht31.readTemperature();
  float hum = sht31.readHumidity();

  float lux = lightMeter.readLightLevel();

  int16_t x = 0, y = 0, z = 0;
  magRead(x, y, z);

  float hdg = atan2(y - my0, x - mx0) * 180.0 / PI;
  if (hdg < 0) hdg += 360;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float timeS = (millis() - startTime) / 1000.0;

  // GPS (пустые ячейки, пока нет фикса)
  bool   fix    = gps.location.isValid();
  float  lat    = fix ? gps.location.lat() : NAN;
  float  lon    = fix ? gps.location.lng() : NAN;
  float  gpsAlt = gps.altitude.isValid() ? gps.altitude.meters() : NAN;
  float  speed  = gps.speed.isValid()    ? gps.speed.kmph()      : NAN;
  int    sats   = gps.satellites.value();

  // ================= SERIAL =================
  Serial.println("\n========= SENSOR DATA =========");
  Serial.printf("Time:   %.1f s   GPS UTC: %s\n", timeS, gpsTimeStr().c_str());
  Serial.printf("GPS:    lat %.6f  lon %.6f  alt %.1f m  speed %.1f km/h  sats %d\n",
                lat, lon, gpsAlt, speed, sats);
  Serial.printf("BMP280: alt %.2f m  T %.2f C  P %.2f hPa\n", alt, t1, press);
  Serial.printf("SHT31:  T %.2f C  Hum %.2f %%\n", t2, hum);
  Serial.printf("BH1750: %.2f lux\n", lux);
  Serial.printf("MAG:    X %d  Y %d  Z %d  Heading %.1f deg\n", x, y, z, hdg);
  Serial.printf("ACC:    X %.2f  Y %.2f  Z %.2f m/s2\n",
                a.acceleration.x, a.acceleration.y, a.acceleration.z);
  Serial.printf("GYRO:   X %.3f  Y %.3f  Z %.3f rad/s\n", g.gyro.x, g.gyro.y, g.gyro.z);
  Serial.printf("MPU T:  %.2f C\n", temp.temperature);

  // ================= SD =================
  String row = "";
  row += fnum(timeS, 1) + ";";
  row += gpsTimeStr() + ";";
  row += fnum(lat, 6) + ";";
  row += fnum(lon, 6) + ";";
  row += fnum(gpsAlt, 1) + ";";
  row += fnum(speed, 1) + ";";
  row += String(sats) + ";";

  row += fnum(alt) + ";";
  row += fnum(t1) + ";";
  row += fnum(press) + ";";

  row += fnum(t2) + ";";
  row += fnum(hum) + ";";

  row += fnum(lux) + ";";

  row += String(x) + ";";
  row += String(y) + ";";
  row += String(z) + ";";
  row += fnum(hdg, 1) + ";";

  row += fnum(a.acceleration.x) + ";";
  row += fnum(a.acceleration.y) + ";";
  row += fnum(a.acceleration.z) + ";";

  row += fnum(g.gyro.x, 3) + ";";
  row += fnum(g.gyro.y, 3) + ";";
  row += fnum(g.gyro.z, 3) + ";";
  row += fnum(temp.temperature);

  File file = SD.open(LOG_FILE, FILE_APPEND);

  if (file) {
    file.println(row);
    file.close();
    Serial.println("💾 SD OK");
  } else {
    Serial.println("SD ERROR → REINIT");
    SD.end();
    delay(50);
    initSD();
  }

  // ================= LORA =================
  digitalWrite(LORA_SS, LOW);

  String msg = "";
  msg += String(alt) + ",";
  msg += String(t1) + ",";
  msg += String(hum) + ",";
  msg += String(lux) + ",";
  msg += String(gps.satellites.value());

  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();

  digitalWrite(LORA_SS, HIGH);

  // ================= BUZZER =================
  unsigned long now = millis();

  if (!buzzerActive && now - startTime >= 180000) {
    buzzerActive = true;
    Serial.println("🔊 BUZZER ON");
  }

  if (buzzerActive && now - lastBeep >= 2000) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    lastBeep = now;
  }

  delay(1000);
}
