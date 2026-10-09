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

// Формат записи теперь такой же, как в cubesat_firmware/cubesat_logger:
// 24 именованных колонки, разделитель ';', запятая в дробях, подсказка
// sep=; в первой строке. Поэтому и файл называется так же — /data.csv.
// Старый /log.csv с запятыми и точками в дробях трогать нельзя: смешивать
// два формата в одном файле нельзя, Excel на этом сломается.
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

// === СТРУКТУРА ДЛЯ ПЕРЕДАЧИ ДАННЫХ ===
//
// В эфир идёт только то, что нужно на земле во время полёта: высота и
// координаты для поиска аппарата. Температура, влажность и освещённость
// убраны — приёмник их никуда не выводил, а 12 байт эфира они занимали.
// На карту они по-прежнему пишутся, все 24 колонки на месте.
//
// 5 полей, 20 байт, ~1.12 с в эфире.
//
// int32_t вместо int: размер int зависит от платформы, а эта структура
// уходит по воздуху и должна совпадать на обеих сторонах байт в байт.
struct Telemetry {
  float   alt;
  float   lat;
  float   lng;
  float   gpsAlt;
  int32_t sat;
};
Telemetry txData;

// ================= FORMAT HELPERS =================
// Excel (русская локаль): разделитель столбцов ';', десятичный разделитель ','
// NAN превращается в пустую ячейку, а не в слово "nan", которое ломает
// построение графиков.
String fnum(float v, int decimals = 2) {
  if (isnan(v) || isinf(v)) return "";
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

// ================= SD INIT =================
bool initSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  SPI.begin(18, 19, 23);

  Serial.println("=== SD INIT ===");

  if (SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("SD OK 4MHz");
    return true;
  }
  if (SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("SD OK 1MHz");
    return true;
  }
  if (SD.begin(SD_CS, SPI, 400000)) {
    Serial.println("SD OK 400KHz");
    return true;
  }

  Serial.println("SD FAIL");
  return false;
}

// ================= ЗАГОЛОВОК ТАБЛИЦЫ =================
// Пишется один раз — только если файла ещё нет на карте
void writeCsvHeader() {
  if (SD.exists(LOG_FILE)) {
    Serial.println("Лог-файл уже есть, заголовок не пишем");
    return;
  }

  File f = SD.open(LOG_FILE, FILE_WRITE);
  if (f) {
    // Подсказка Excel, каким символом разделены колонки.
    // Без неё русский Excel свалит всю строку в одну ячейку.
    f.println("sep=;");
    f.println(
      "Time_s;GPS_Time_UTC;Lat;Lon;GPS_Alt_m;Speed_kmh;Sats;"
      "Alt_m;T_BMP_C;Press_hPa;"
      "T_SHT_C;Hum_%;"
      "Lux;"
      "Mag_X;Mag_Y;Mag_Z;Heading_deg;"
      "Acc_X_ms2;Acc_Y_ms2;Acc_Z_ms2;"
      "Gyro_X_rads;Gyro_Y_rads;Gyro_Z_rads;T_MPU_C"
    );
    f.close();
    Serial.println("Заголовок таблицы записан");
  } else {
    Serial.println("Не удалось создать лог-файл");
  }
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

  // Конфигурируем CS пины
  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  if (initSD()) {
    writeCsvHeader();
  }

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(434.5E6)) {
    Serial.println("LORA TX FAIL");
    while (1);
  }

  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(62.5E3);
  LoRa.setCodingRate4(8);
  LoRa.enableCrc();

  Serial.println("LORA TX READY ON 434.5 MHz");
  Serial.print("Размер пакета: ");
  Serial.print(sizeof(Telemetry));
  Serial.println(" байт (должен совпадать с приёмником)");
  delay(1000);

  int16_t x, y, z;
  magRead(x, y, z);
  mx0 = x; my0 = y; mz0 = z;
}

// ================= LOOP =================
void loop() {
  while (GPS.available()) gps.encode(GPS.read());

  float timeS = (millis() - startTime) / 1000.0;

  float alt   = bmp.readAltitude(1013.25);
  float t1    = bmp.readTemperature();
  float press = bmp.readPressure() / 100.0;

  float t2  = sht31.readTemperature();
  float hum = sht31.readHumidity();

  float lux = lightMeter.readLightLevel();

  // Инициализируем нулями: magRead пишет в них только если с шины пришло
  // шесть байт. Без инициализации при неудачном чтении в лог уехал бы мусор
  // со стека.
  int16_t x = 0, y = 0, z = 0;
  magRead(x, y, z);

  float hdg = atan2(y - my0, x - mx0) * 180.0 / PI;
  if (hdg < 0) hdg += 360;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // GPS значения.
  // Нет фикса — NAN, а не 0: ноль это точка в Гвинейском заливе, которая
  // в таблице выглядит как настоящее измерение. NAN даёт пустую ячейку.
  bool  fix    = gps.location.isValid();
  float gLat   = fix ? gps.location.lat() : NAN;
  float gLng   = fix ? gps.location.lng() : NAN;
  float gAlt   = gps.altitude.isValid() ? gps.altitude.meters() : NAN;
  float gSpeed = gps.speed.isValid()    ? gps.speed.kmph()      : NAN;
  int   gSat   = gps.satellites.value();

  // ================= SERIAL =================
  Serial.println("\n========= SENSOR DATA =========");
  Serial.printf("TIME:  %.1f s   GPS UTC: %s\n", timeS, gpsTimeStr().c_str());
  Serial.printf("GPS:   lat %.6f  lng %.6f  alt %.1f m  speed %.1f km/h  sat %d\n",
                gLat, gLng, gAlt, gSpeed, gSat);
  Serial.printf("BMP:   alt %.2f m  T %.2f C  P %.2f hPa\n", alt, t1, press);
  Serial.printf("SHT:   T %.2f C  Hum %.2f %%\n", t2, hum);
  Serial.printf("BH:    %.2f lux\n", lux);
  Serial.printf("MAG:   X %d  Y %d  Z %d  HDG %.1f deg\n", x, y, z, hdg);
  Serial.printf("ACC:   %.2f %.2f %.2f m/s2\n",
                a.acceleration.x, a.acceleration.y, a.acceleration.z);
  Serial.printf("GYRO:  %.3f %.3f %.3f rad/s\n", g.gyro.x, g.gyro.y, g.gyro.z);

  // ================= SD: ЗАПИСЬ СТРОКИ ТАБЛИЦЫ =================
  // Порядок колонок строго как в заголовке и как в
  // cubesat_firmware/cubesat_logger — чтобы оба аппарата давали
  // файлы одной схемы и их можно было обрабатывать одним скриптом.
  digitalWrite(LORA_SS, HIGH); // Отключаем LoRa на время работы с картой
  digitalWrite(SD_CS, LOW);    // Включаем карту

  File file = SD.open(LOG_FILE, FILE_APPEND);

  if (file) {
    String row = "";

    row += fnum(timeS, 1) + ";";        // Time_s
    row += gpsTimeStr()   + ";";        // GPS_Time_UTC
    row += fnum(gLat, 6)  + ";";        // Lat
    row += fnum(gLng, 6)  + ";";        // Lon
    row += fnum(gAlt, 1)  + ";";        // GPS_Alt_m
    row += fnum(gSpeed, 1) + ";";       // Speed_kmh
    row += String(gSat)   + ";";        // Sats

    row += fnum(alt)   + ";";           // Alt_m
    row += fnum(t1)    + ";";           // T_BMP_C
    row += fnum(press) + ";";           // Press_hPa

    row += fnum(t2)    + ";";           // T_SHT_C
    row += fnum(hum)   + ";";           // Hum_%

    row += fnum(lux)   + ";";           // Lux

    row += String(x)   + ";";           // Mag_X
    row += String(y)   + ";";           // Mag_Y
    row += String(z)   + ";";           // Mag_Z
    row += fnum(hdg, 1) + ";";          // Heading_deg

    row += fnum(a.acceleration.x) + ";";   // Acc_X_ms2
    row += fnum(a.acceleration.y) + ";";   // Acc_Y_ms2
    row += fnum(a.acceleration.z) + ";";   // Acc_Z_ms2

    row += fnum(g.gyro.x, 3) + ";";        // Gyro_X_rads
    row += fnum(g.gyro.y, 3) + ";";        // Gyro_Y_rads
    row += fnum(g.gyro.z, 3) + ";";        // Gyro_Z_rads

    row += fnum(temp.temperature);         // T_MPU_C

    file.println(row);
    file.close();
    Serial.println("💾 SD OK");
  } else {
    Serial.println("SD ERROR → REINIT");
    SD.end();
    delay(50);
    if (initSD()) writeCsvHeader();
  }
  digitalWrite(SD_CS, HIGH); // Освобождаем шину SPI от карты

  // ================= LORA БИНАРНАЯ ОТПРАВКА =================
  digitalWrite(SD_CS, HIGH);

  txData.alt    = alt;
  txData.lat    = gLat;     // NAN, пока нет фикса — приёмник покажет "--"
  txData.lng    = gLng;
  txData.gpsAlt = gAlt;
  txData.sat    = gSat;

  LoRa.beginPacket();
  LoRa.write((uint8_t*)&txData, sizeof(txData));
  LoRa.endPacket();
  Serial.println("📡 Структура успешно отправлена в эфир");

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
