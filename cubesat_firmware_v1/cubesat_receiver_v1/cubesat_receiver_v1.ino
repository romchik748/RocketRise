#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <GyverOLED.h>

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

GyverOLED<SSH1106_128x64> oled;

// Переменные для хранения принятых данных телеметрии
float alt = 0;
float temp = 0;
float hum = 0;
float lux = 0;
int sat = 0;
int lastRssi = 0;

// Таймеры и переменные для смены экранов
unsigned long lastScreenUpdate = 0;
int currentScreen = 1;

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  oled.init();
  oled.clear();
  oled.update();

  SPI.begin(18, 19, 23);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LORA FAIL");
    oled.setCursor(0, 0); oled.print("LORA FAIL"); oled.update();
    while (1);
  }

  // ===== НАСТРОЙКИ МАКСИМАЛЬНОЙ ДАЛЬНОСТИ ДЛЯ ПРИЕМНИКА =====
  // Должны строго совпадать с настройками передатчика!
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(62.5E3);
  LoRa.setCodingRate4(8);
  // ==========================================================

  Serial.println("LORA READY");
  oled.setCursor(0, 0); oled.print("LORA READY"); oled.update();
  delay(1000);
}

void loop() {
  // 1. Прием данных из радиоэфира
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String packet = "";
    while (LoRa.available()) {
      packet += (char)LoRa.read();
    }
    Serial.print("Rx Packet: ");
    Serial.println(packet);
    lastRssi = LoRa.packetRssi();

    // Парсим ровно 5 параметров, прилетающих от твоего передатчика
    sscanf(
      packet.c_str(),
      "%f,%f,%f,%f,%d",
      &alt, &temp, &hum, &lux, &sat
    );
  }

  // 2. Автоматическое обновление и переключение экранов (раз в 1000 мс)
  unsigned long now = millis();
  if (now - lastScreenUpdate >= 1000) {
    lastScreenUpdate = now;

    oled.clear();

    switch (currentScreen) {

      case 1: // ЭКРАН 1 — высота, температура, влажность, свет
        oled.setCursor(0, 0); oled.print("--- SCREEN 1/4 ---");
        oled.setCursor(0, 2); oled.print("ALT:  "); oled.print((int)alt); oled.print(" m");
        oled.setCursor(0, 3); oled.print("TEMP: "); oled.print(temp, 1); oled.print(" C");
        oled.setCursor(0, 4); oled.print("HUM:  "); oled.print((int)hum); oled.print(" %");
        oled.setCursor(0, 5); oled.print("LUX:  "); oled.print((int)lux);
        break;

      case 2: // ЭКРАН 2 — GPS (спутники, широта, долгота)
        oled.setCursor(0, 0); oled.print("--- SCREEN 2/4 ---");
        oled.setCursor(0, 2); oled.print("SAT: "); oled.print(sat);
        oled.setCursor(0, 4); oled.print("LAT: NO DATA (TX)"); // Пометка, так как передатчик их не шлет
        oled.setCursor(0, 5); oled.print("LNG: NO DATA (TX)");
        oled.setCursor(0, 7);
        if (sat > 0) oled.print("[ GPS FIXED ]");
        else         oled.print("[ NO GPS FIX ]");
        break;

      case 3: // ЭКРАН 3 — MPU6050
        oled.setCursor(0, 0); oled.print("--- SCREEN 3/4 ---");
        oled.setCursor(0, 3); oled.print("MPU6050 DATA:");
        oled.setCursor(0, 4); oled.print("NOT SENT BY TX");
        break;

      case 4: // ЭКРАН 4 — компас/давление
        oled.setCursor(0, 0); oled.print("--- SCREEN 4/4 ---");
        oled.setCursor(0, 2); oled.print("COMPASS: NO DATA");
        oled.setCursor(0, 3); oled.print("BARO:    NO DATA");
        oled.setCursor(0, 5); oled.print("RSSI:    "); oled.print(lastRssi); oled.print(" dBm");
        break;
    }

    oled.update();

    // Инкремент номера экрана для следующего шага таймера
    currentScreen++;
    if (currentScreen > 4) {
      currentScreen = 1;
    }
  }
}
