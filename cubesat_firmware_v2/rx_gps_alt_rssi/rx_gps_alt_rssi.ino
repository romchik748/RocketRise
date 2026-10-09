#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <GyverOLED.h>

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

GyverOLED<SSH1106_128x64> oled;

// Принятые данные
float alt    = NAN;   // высота по барометру
float gpsAlt = NAN;   // высота по GPS
float lat    = NAN;
float lng    = NAN;
int   sat    = 0;
int   lastRssi = 0;

unsigned long lastPacketTime = 0;  // когда пришёл последний пакет

// ТАКАЯ ЖЕ СТРУКТУРА, КАК НА ПЕРЕДАТЧИКЕ
//
// temp, hum и lux убраны: приёмник их никуда не выводил, а в эфире они
// занимали 12 байт из 32. На карту передатчика они пишутся по-прежнему.
// Пакет стал 20 байт вместо 32, время в эфире ~1.12 с вместо ~1.51 с.
//
// int32_t вместо int: размер int зависит от платформы, а структура уходит
// по воздуху и должна совпадать с передатчиком байт в байт.
//
// ВАЖНО: формат пакета изменился, поэтому передатчик и приёмник надо
// прошить ОБА. Старый приёмник увидит 20 байт вместо ожидаемых 32 и честно
// назовёт это помехой — данные не перепутаются, но связи не будет.
struct Telemetry {
  float   alt;
  float   lat;
  float   lng;
  float   gpsAlt;
  int32_t sat;
};
Telemetry rxData;

unsigned long lastScreenUpdate = 0;

// Передатчик присылает NAN, пока нет фикса GPS. Показываем "--", чтобы
// отсутствие данных не выглядело как измерение (раньше приходил 0, и на
// экране был честный с виду ноль).
String txtNum(float v, int dec) {
  if (isnan(v)) return "--";
  return String(v, dec);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  oled.init();
  oled.clear();
  oled.update();

  SPI.begin(18, 19, 23);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(434.5E6)) {
    Serial.println("LORA FAIL");
    oled.setCursor(0, 0); oled.print("LORA FAIL"); oled.update();
    while (1);
  }

  // ===== НАСТРОЙКИ (СТРОГОЕ СОВПАДЕНИЕ С ПЕРЕДАТЧИКОМ) =====
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(62.5E3);
  LoRa.setCodingRate4(8);
  LoRa.enableCrc();
  // ==========================================================

  Serial.println("LORA RX READY ON 434.5 MHz");
  Serial.print("Ожидаемый размер пакета: ");
  Serial.print(sizeof(Telemetry));
  Serial.println(" байт");

  oled.setCursor(0, 0); oled.print("LORA READY");
  oled.setCursor(0, 2); oled.print("WAIT DATA...");
  oled.update();
  delay(1000);
}

void loop() {
  // ============ ПРИЁМ ============
  int packetSize = LoRa.parsePacket();

  if (packetSize == sizeof(Telemetry)) {
    LoRa.readBytes((uint8_t*)&rxData, sizeof(Telemetry));

    lastRssi = LoRa.packetRssi();   // RSSI меряется здесь, на приёмнике
    lastPacketTime = millis();

    alt    = rxData.alt;
    gpsAlt = rxData.gpsAlt;
    lat    = rxData.lat;
    lng    = rxData.lng;
    sat    = rxData.sat;

    Serial.print("RSSI: ");   Serial.print(lastRssi);
    Serial.print(" dBm | ALT: "); Serial.print(alt, 1);
    Serial.print(" | GALT: ");    Serial.print(gpsAlt, 1);
    Serial.print(" | SAT: ");     Serial.print(sat);
    Serial.print(" | LAT: ");     Serial.print(lat, 6);
    Serial.print(" | LNG: ");     Serial.println(lng, 6);

  } else if (packetSize > 0) {
    Serial.print("Помеха! Размер пакета: ");
    Serial.print(packetSize);
    Serial.print(" (ожидался ");
    Serial.print(sizeof(Telemetry));
    Serial.println(")");
  }

  // ============ ЭКРАН ============
  unsigned long now = millis();
  if (now - lastScreenUpdate >= 500) {
    lastScreenUpdate = now;

    oled.clear();

    // Высота
    oled.setCursor(0, 0);
    oled.print("ALT:  "); oled.print(txtNum(alt, 1)); oled.print(" m");

    // GPS
    oled.setCursor(0, 2);
    oled.print("LAT: "); oled.print(txtNum(lat, 5));
    oled.setCursor(0, 3);
    oled.print("LNG: "); oled.print(txtNum(lng, 5));
    oled.setCursor(0, 4);
    oled.print("GALT:"); oled.print(txtNum(gpsAlt, 0));
    oled.print(" m  SAT:"); oled.print(sat);

    // RSSI
    oled.setCursor(0, 6);
    oled.print("RSSI: "); oled.print(lastRssi); oled.print(" dBm");

    // Статус связи — чтобы не смотреть на протухшие цифры
    oled.setCursor(0, 7);
    if (lastPacketTime == 0) {
      oled.print("[ NO SIGNAL ]");
    } else {
      unsigned long ageSec = (now - lastPacketTime) / 1000;
      if (ageSec <= 3) {
        oled.print("[ LINK OK ]");
      } else {
        oled.print("[ LOST "); oled.print(ageSec); oled.print("s ]");
      }
    }

    oled.update();
  }
}
