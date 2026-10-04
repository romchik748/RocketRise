/* =====================================================================
   CUBESAT TELEMETRY — ПРИЁМНИК (ESP32 DevKit + LoRa Ra-01 + OLED SH1106)
   ---------------------------------------------------------------------
   Под НАШ передатчик (cubesat_logger.ino), блок LoRa которого не менялся.

   Что передаёт кубсат (текст, раз в ~2,4 с):
       "alt,t1,hum,lux,sats"     пример: 295.85,27.71,36.61,1730.83,0
       alt  — высота BMP280, м (от 1013,25 гПа)
       t1   — температура BMP280, °C
       hum  — влажность SHT31, %
       lux  — освещённость BH1750, лк
       sats — число спутников GPS

   Радио (ДОЛЖНО совпадать с передатчиком):
       433 МГц, SF10, BW 62.5 кГц, CR 4/8,
       sync word и CRC — по умолчанию библиотеки (0x12, CRC выключен),
       т.к. передатчик их не меняет.

   В пакете нет счётчика, поэтому потери ОЦЕНИВАЮТСЯ по паузам между
   пакетами. Высота макс. и вертикальная скорость считаются здесь, на
   приёмнике, по принятой высоте.

   РАСПИНОВКА:
     LoRa:  SS=5, RST=14, DIO0=26, SCK=18, MISO=19, MOSI=23
     OLED:  SDA=21, SCL=22
   Питание модуля и экрана — от вывода 3.3V платы.
   ===================================================================== */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <GyverOLED.h>

// ==================== НАСТРОЙКИ ====================
#define LORA_SS    5
#define LORA_RST   14
#define LORA_DIO0  26
#define I2C_SDA    21
#define I2C_SCL    22

// Радиоканал — как в передатчике
#define LORA_FREQ  433E6
#define LORA_SF    10
#define LORA_BW    62.5E3
#define LORA_CR    8

#define VERBOSE      1          // 1 = сводка + CSV, 0 = только CSV
#define PAGE_TIME    3000UL     // смена страницы OLED, мс
#define TX_INTERVAL  2450UL     // примерный период пакетов передатчика, мс
                                // (~1,4 с в эфире + delay(1000) + опрос датчиков)
#define SIGNAL_LOST  10000UL    // ~4 пропущенных пакета подряд = связь потеряна

// Wi-Fi точка доступа с веб-панелью.
// 1 = сеть "CubeSat", пароль "cubesat123", панель: http://192.168.4.1
#define USE_WIFI 0

// формат CSV: ';' + десятичная запятая = русский Excel
#define DLM ";"

#if USE_WIFI
  #include <WiFi.h>
  #include <WebServer.h>
  WebServer server(80);
#endif

GyverOLED<SSH1106_128x64, OLED_BUFFER> oled;

// ==================== ДАННЫЕ ====================
struct Telemetry {
  float alt  = NAN;
  float t1   = NAN;
  float hum  = NAN;
  float lux  = NAN;
  int   sats = 0;
};
Telemetry d;

float maxAlt  = NAN;
float vSpeed  = NAN;
float prevAlt = NAN;
uint32_t prevAltTime = 0;

// ==================== СОСТОЯНИЕ ====================
uint32_t rxCount = 0, badPkt = 0, lostCount = 0;
int      lastRssi = 0;
float    lastSnr = 0;
uint32_t lastRxTime = 0, lastPageTime = 0;
uint8_t  page = 0;
bool     havePacket = false;

// связь и статистика для теста дальности
bool     linkDown = false;
uint32_t outageCount = 0, longestOutage = 0, downSince = 0;
int      rssiMin = 999, rssiMax = -999;
long     rssiSum = 0;
uint32_t lastSummary = 0;

// ==================== ПАРСИНГ ПАКЕТА ====================
bool parseFloatField(const char *s, float &out) {
  char *end;
  out = strtof(s, &end);               // "nan" тоже распознаётся
  return end != s && *end == '\0';
}

// Строгая проверка: ровно 5 полей, все числа, значения в разумных пределах.
// CRC в эфире выключен, поэтому битые пакеты отсеиваем здесь.
bool parsePacket(char *buf, Telemetry &t) {
  char *fields[6];
  int n = 0;
  char *save;
  for (char *tok = strtok_r(buf, ",", &save); tok && n < 6;
       tok = strtok_r(NULL, ",", &save)) {
    fields[n++] = tok;
  }
  if (n != 5) return false;

  Telemetry r;
  if (!parseFloatField(fields[0], r.alt)) return false;
  if (!parseFloatField(fields[1], r.t1))  return false;
  if (!parseFloatField(fields[2], r.hum)) return false;
  if (!parseFloatField(fields[3], r.lux)) return false;

  char *end;
  long s = strtol(fields[4], &end, 10);
  if (end == fields[4] || *end != '\0' || s < 0 || s > 99) return false;
  r.sats = (int)s;

  // NaN пропускаем (датчик не ответил), остальное — проверка диапазонов
  if (!isnan(r.alt) && (r.alt < -1000 || r.alt > 40000))  return false;
  if (!isnan(r.t1)  && (r.t1  < -60   || r.t1  > 125))    return false;
  if (!isnan(r.hum) && (r.hum < 0     || r.hum > 100.5))  return false;
  if (!isnan(r.lux) && (r.lux < 0     || r.lux > 120000)) return false;

  t = r;
  return true;
}

// ==================== ФОРМАТИРОВАНИЕ ====================
// CSV: пустая ячейка вместо NaN, десятичная запятая
String csvNum(float v, int dec) {
  if (isnan(v)) return "";
  String s = String(v, dec);
  s.replace('.', ',');
  return s;
}

// Serial/OLED: "--" вместо NaN
String txtNum(float v, int dec) {
  if (isnan(v)) return "--";
  return String(v, dec);
}

// ==================== ВЕБ-ПАНЕЛЬ ====================
#if USE_WIFI
const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CubeSat</title><style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}
h1{font-size:18px;font-weight:500;margin:0 0 12px}
.g{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.c{background:#1e1e1e;border-radius:8px;padding:10px}
.k{font-size:11px;color:#999}.v{font-size:19px}
</style></head><body>
<h1>CubeSat telemetry</h1><div class="g" id="g"></div>
<script>
const F=[["rx","Принято"],["lost","Потеряно (оценка)"],["rssi","RSSI, дБм"],["snr","SNR, дБ"],
["alt","Высота, м"],["maxalt","Макс. высота, м"],["vs","Верт. скорость, м/с"],["t1","Темп. BMP, C"],
["hum","Влажность, %"],["lux","Освещ., лк"],["sats","Спутники"],["age","Посл. пакет, с"]];
async function u(){try{const d=await(await fetch('/data')).json();
document.getElementById('g').innerHTML=F.map(f=>
`<div class="c"><div class="k">${f[1]}</div><div class="v">${d[f[0]]??'--'}</div></div>`).join('');}catch(e){}}
u();setInterval(u,1000);
</script></body></html>
)HTML";

String jNum(float v, int dec) { return isnan(v) ? "null" : String(v, dec); }

void handleData() {
  String j = "{";
  j += "\"rx\":"     + String(rxCount) + ",";
  j += "\"lost\":"   + String(lostCount) + ",";
  j += "\"rssi\":"   + String(lastRssi) + ",";
  j += "\"snr\":"    + String(lastSnr, 1) + ",";
  j += "\"alt\":"    + jNum(d.alt, 2) + ",";
  j += "\"maxalt\":" + jNum(maxAlt, 2) + ",";
  j += "\"vs\":"     + jNum(vSpeed, 2) + ",";
  j += "\"t1\":"     + jNum(d.t1, 2) + ",";
  j += "\"hum\":"    + jNum(d.hum, 1) + ",";
  j += "\"lux\":"    + jNum(d.lux, 0) + ",";
  j += "\"sats\":"   + String(d.sats) + ",";
  j += "\"age\":"    + String(havePacket ? (millis() - lastRxTime) / 1000 : 0);
  j += "}";
  server.send(200, "application/json", j);
}
#endif

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CUBESAT GROUND STATION (ESP32) ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  oled.init();
  oled.clear();
  oled.setCursor(0, 0); oled.print(F("GROUND STATION"));
  oled.setCursor(0, 2); oled.print(F("LoRa init..."));
  oled.update();

  SPI.begin(18, 19, 23);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LORA FAIL");
    oled.clear();
    oled.setCursor(0, 3); oled.print(F("LORA FAIL"));
    oled.update();
    while (1) delay(1000);
  }

  // setGain() перезаписывает MODEM_CONFIG_3 и сбрасывает бит
  // LowDataRateOptimize. При SF10 / 62.5 кГц он ОБЯЗАТЕЛЕН (символ 16,4 мс),
  // а setSpreadingFactor/setSignalBandwidth выставляют его сами —
  // поэтому setGain() строго ДО них.
  LoRa.setGain(0);                       // 0 = АРУ
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  // sync word (0x12), преамбула (8) и CRC (выкл.) — по умолчанию, как у передатчика

  Serial.println("LoRa ok: 433 MHz, SF10, BW 62.5 kHz, CR 4/8. Listening...");

#if USE_WIFI
  WiFi.softAP("CubeSat", "cubesat123");
  server.on("/", []() { server.send_P(200, "text/html", PAGE_HTML); });
  server.on("/data", handleData);
  server.begin();
  Serial.print("WiFi AP \"CubeSat\", panel: http://");
  Serial.println(WiFi.softAPIP());
#endif

  // Заголовок CSV (строки CSV начинаются с '$', чтобы их легко отфильтровать)
  Serial.println("sep=" DLM);
  Serial.println(
    "RX" DLM "RX_TIME_S" DLM "RSSI_DBM" DLM "SNR_DB" DLM "LOST_EST" DLM
    "ALT_M" DLM "ALT_MAX_M" DLM "VSPEED_MS" DLM
    "T_BMP_C" DLM "HUM_PCT" DLM "LUX" DLM "SATS");

  oled.clear();
  oled.setCursor(0, 3); oled.print(F("WAITING SIGNAL..."));
  oled.update();

  lastRxTime   = millis();
  lastPageTime = millis();
}

// ==================== ВЫВОД ====================
void printCsv() {
  String line = "$";
  line += String(rxCount)                       + DLM;
  line += csvNum(millis() / 1000.0, 1)          + DLM;
  line += String(lastRssi)                      + DLM;
  line += csvNum(lastSnr, 1)                    + DLM;
  line += String(lostCount)                     + DLM;
  line += csvNum(d.alt, 2)                      + DLM;
  line += csvNum(maxAlt, 2)                     + DLM;
  line += csvNum(vSpeed, 2)                     + DLM;
  line += csvNum(d.t1, 2)                       + DLM;
  line += csvNum(d.hum, 2)                      + DLM;
  line += csvNum(d.lux, 2)                      + DLM;
  line += String(d.sats);
  Serial.println(line);
}

#if VERBOSE
void printHuman() {
  float lossPct = (rxCount + lostCount) ? (100.0 * lostCount / (rxCount + lostCount)) : 0;
  Serial.println("\n----------- TELEMETRY -----------");
  Serial.printf("RX #%u | RSSI %d dBm | SNR %.1f dB\n", rxCount, lastRssi, lastSnr);
  Serial.printf("LOST ~%u (%.1f%%) | BAD %u\n", lostCount, lossPct, badPkt);
  Serial.printf("ALT %s m | MAX %s m | VS %s m/s\n",
                txtNum(d.alt, 2).c_str(), txtNum(maxAlt, 2).c_str(), txtNum(vSpeed, 2).c_str());
  Serial.printf("T %s C | RH %s %% | LUX %s | SATS %d\n",
                txtNum(d.t1, 2).c_str(), txtNum(d.hum, 2).c_str(),
                txtNum(d.lux, 0).c_str(), d.sats);
  Serial.println("---------------------------------");
}
#endif

// ==================== OLED ====================
void drawNoSignal() {
  oled.clear();
  oled.setCursor(0, 0); oled.print(F("*** NO SIGNAL ***"));
  oled.setCursor(0, 2);
  oled.print(F("Last ")); oled.print((millis() - lastRxTime) / 1000); oled.print(F(" s ago"));
  oled.setCursor(0, 3);
  oled.print(F("RX:")); oled.print(rxCount);
  oled.print(F(" LOST:~")); oled.print(lostCount);
  oled.setCursor(0, 5); oled.print(F("LAST ALT ")); oled.print(txtNum(d.alt, 1)); oled.print(F(" m"));
  oled.setCursor(0, 6); oled.print(F("MAX ALT  ")); oled.print(txtNum(maxAlt, 1)); oled.print(F(" m"));
  oled.update();
}

void drawPage() {
  oled.clear();

  if (page == 0) {
    oled.setCursor(0, 0); oled.print(F("FLIGHT    RX ")); oled.print(rxCount);
    oled.setCursor(0, 1); oled.print(F("ALT ")); oled.print(txtNum(d.alt, 2));    oled.print(F(" m"));
    oled.setCursor(0, 2); oled.print(F("MAX ")); oled.print(txtNum(maxAlt, 2));   oled.print(F(" m"));
    oled.setCursor(0, 3); oled.print(F("VS  ")); oled.print(txtNum(vSpeed, 2));   oled.print(F(" m/s"));
    oled.setCursor(0, 4); oled.print(F("T   ")); oled.print(txtNum(d.t1, 1));     oled.print(F(" C"));
    oled.setCursor(0, 5); oled.print(F("RH  ")); oled.print(txtNum(d.hum, 1));    oled.print(F(" %"));
    oled.setCursor(0, 6); oled.print(F("LUX ")); oled.print(txtNum(d.lux, 0));
    oled.setCursor(0, 7); oled.print(F("SATS ")); oled.print(d.sats);
  } else {
    oled.setCursor(0, 0); oled.print(F("LINK"));
    oled.setCursor(0, 1); oled.print(F("RSSI ")); oled.print(lastRssi); oled.print(F(" dBm"));
    oled.setCursor(0, 2); oled.print(F("SNR  ")); oled.print(lastSnr, 1); oled.print(F(" dB"));
    oled.setCursor(0, 3); oled.print(F("RX ")); oled.print(rxCount);
    oled.print(F(" LOST ~")); oled.print(lostCount);
    oled.setCursor(0, 4); oled.print(F("BAD ")); oled.print(badPkt);
    oled.setCursor(0, 5); oled.print(F("RSSI min ")); oled.print(rssiMin == 999 ? 0 : rssiMin);
    oled.setCursor(0, 6); oled.print(F("RSSI max ")); oled.print(rssiMax == -999 ? 0 : rssiMax);
    oled.setCursor(0, 7); oled.print(F("Outages ")); oled.print(outageCount);
  }

  oled.update();
}

// ==================== LOOP ====================
void loop() {
#if USE_WIFI
  server.handleClient();
#endif

  int packetSize = LoRa.parsePacket();

  if (packetSize > 0) {
    // читаем как текст (пакет ~30 байт, с запасом 63)
    char buf[64];
    int len = 0;
    while (LoRa.available()) {
      char c = (char)LoRa.read();
      if (len < (int)sizeof(buf) - 1) buf[len++] = c;
    }
    buf[len] = '\0';

    lastRssi = LoRa.packetRssi();
    lastSnr  = LoRa.packetSnr();

    char raw[64];
    strcpy(raw, buf);                    // strtok портит строку, копия для лога

    Telemetry t;
    if (packetSize > 63 || !parsePacket(buf, t)) {
      badPkt++;
      Serial.printf("# bad packet (%d bytes, RSSI %d): %s\n", packetSize, lastRssi, raw);
      return;
    }

    uint32_t now = millis();

    // оценка потерь по паузе (в пакете нет счётчика)
    if (rxCount > 0) {
      uint32_t gap = now - lastRxTime;
      long missed = lround((double)gap / TX_INTERVAL) - 1;
      if (missed > 0) lostCount += missed;
    }

    d = t;
    rxCount++;
    havePacket = true;
    lastRxTime = now;

    // макс. высота и вертикальная скорость (считаются на приёмнике)
    if (!isnan(d.alt)) {
      if (isnan(maxAlt) || d.alt > maxAlt) maxAlt = d.alt;
      if (!isnan(prevAlt) && prevAltTime) {
        float dt = (now - prevAltTime) / 1000.0;
        if (dt > 0.5 && dt < 15.0) {
          float v = (d.alt - prevAlt) / dt;
          vSpeed = isnan(vSpeed) ? v : 0.6 * vSpeed + 0.4 * v;   // сглаживание
        }
      }
      prevAlt = d.alt;
      prevAltTime = now;
    }

    if (linkDown) {
      uint32_t gap = (lastRxTime - downSince) / 1000;
      if (gap > longestOutage) longestOutage = gap;
      Serial.printf("\n>>> СВЯЗЬ ВОССТАНОВЛЕНА после %u с молчания, RSSI %d dBm <<<\n",
                    gap, lastRssi);
      linkDown = false;
    }

    if (lastRssi < rssiMin) rssiMin = lastRssi;
    if (lastRssi > rssiMax) rssiMax = lastRssi;
    rssiSum += lastRssi;

#if VERBOSE
    printHuman();
#endif
    printCsv();
    drawPage();
  }

  uint32_t now = millis();
  uint32_t silence = now - lastRxTime;

  // --- явная индикация потери связи ---
  if (havePacket && silence >= SIGNAL_LOST) {
    if (!linkDown) {
      linkDown = true;
      downSince = lastRxTime;
      outageCount++;
      Serial.printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
                    "!!!  СИГНАЛ ПОТЕРЯН  (обрыв №%u)\n"
                    "!!!  последний пакет RX #%u, RSSI %d dBm\n"
                    "!!!  ДАННЫЕ НА ЭКРАНЕ БОЛЬШЕ НЕ ОБНОВЛЯЮТСЯ\n"
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
                    outageCount, rxCount, lastRssi);
    }
    static uint32_t lastAlarm = 0;
    if (now - lastAlarm >= 2000) {
      lastAlarm = now;
      Serial.printf("!!! НЕТ СИГНАЛА: %u с. Последняя высота: %s м\n",
                    silence / 1000, txtNum(d.alt, 1).c_str());
      drawNoSignal();
    }
  }

  // --- сводка для теста дальности каждые 15 с ---
  if (rxCount > 0 && now - lastSummary >= 15000) {
    lastSummary = now;
    Serial.printf("=== ЛИНК: RSSI сейчас %d | мин %d | макс %d | средний %ld dBm\n"
                  "=== принято %u | потеряно ~%u (%.1f%%) | битых %u | обрывов %u | макс. обрыв %u с\n",
                  lastRssi, rssiMin, rssiMax, rssiSum / (long)rxCount,
                  rxCount, lostCount,
                  100.0 * lostCount / (rxCount + lostCount),
                  badPkt, outageCount, longestOutage);
  }

  // автоперелистывание страниц
  if (now - lastPageTime >= PAGE_TIME) {
    lastPageTime = now;
    page = (page + 1) % 2;
    if (havePacket && !linkDown) drawPage();
  }
}
