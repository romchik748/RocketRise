/* =====================================================================
   CUBESAT TELEMETRY — ПРИЁМНИК, самая первая версия (v0)
   ---------------------------------------------------------------------
   Один экран, всё сразу: высота, температура, влажность, освещённость,
   спутники, RSSI и признак фикса GPS. Без страниц, без статистики связи,
   без вывода в CSV.

   Экран обновляется только в момент прихода пакета, поэтому при потере
   связи на дисплее остаются последние принятые цифры и выглядят так,
   будто они актуальные.

   РАСПИНОВКА:
     LoRa:  SS=5, RST=14, DIO0=26, SCK=18, MISO=19, MOSI=23
     OLED:  SDA=21, SCL=22
   ===================================================================== */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <GyverOLED.h>

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

GyverOLED<SSH1106_128x64> oled;

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

    oled.setCursor(0, 0);
    oled.print("LORA FAIL");
    oled.update();

    while (1);
  }

  Serial.println("LORA READY");

  oled.setCursor(0, 0);
  oled.print("LORA READY");
  oled.update();

  delay(1000);
}

void loop() {
  int packetSize = LoRa.parsePacket();

  if (packetSize) {
    String packet = "";

    while (LoRa.available()) {
      packet += (char)LoRa.read();
    }

    Serial.println(packet);

    float alt  = 0;
    float temp = 0;
    float hum  = 0;
    float lux  = 0;
    int   sat  = 0;

    sscanf(
      packet.c_str(),
      "%f,%f,%f,%f,%d",
      &alt,
      &temp,
      &hum,
      &lux,
      &sat
    );

    oled.clear();

    oled.setCursor(0, 0);
    oled.print("ALT:");
    oled.print((int)alt);

    oled.setCursor(0, 1);
    oled.print("TMP:");
    oled.print(temp, 1);

    oled.setCursor(0, 2);
    oled.print("HUM:");
    oled.print((int)hum);

    oled.setCursor(0, 3);
    oled.print("LUX:");
    oled.print((int)lux);

    oled.setCursor(0, 4);
    oled.print("SAT:");
    oled.print(sat);

    oled.setCursor(0, 5);
    oled.print("RSSI:");
    oled.print(LoRa.packetRssi());

    oled.setCursor(0, 6);

    if (sat > 0)
      oled.print("GPS OK");
    else
      oled.print("NO FIX");

    oled.update();
  }
}
