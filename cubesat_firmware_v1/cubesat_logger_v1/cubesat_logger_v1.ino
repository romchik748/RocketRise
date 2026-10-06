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

  int16_t x, y, z;
  magRead(x, y, z);

  float hdg = atan2(y - my0, x - mx0) * 180.0 / PI;
  if (hdg < 0) hdg += 360;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // ================= SERIAL =================
  Serial.println("\n========= SENSOR DATA =========");
  Serial.print("ALT: "); Serial.println(alt);
  Serial.print("T1: "); Serial.println(t1);
  Serial.print("PRESS: "); Serial.println(press);
  Serial.print("T2: "); Serial.println(t2);
  Serial.print("HUM: "); Serial.println(hum);
  Serial.print("LUX: "); Serial.println(lux);
  Serial.print("HDG: "); Serial.println(hdg);
  Serial.print("SAT: "); Serial.println(gps.satellites.value());

  // ================= SD =================
  File file = SD.open("/log.csv", FILE_APPEND);

  if (file) {
    file.print(alt); file.print(",");
    file.print(t1); file.print(",");
    file.print(press); file.print(",");
    file.print(t2); file.print(",");
    file.print(hum); file.print(",");
    file.print(lux); file.print(",");
    file.print(hdg); file.print(",");
    file.print(gps.satellites.value());
    file.println();
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
