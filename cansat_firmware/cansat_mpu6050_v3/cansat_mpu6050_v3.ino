/*
 * CanSat — плата №1 (MPU6050)   версия v3
 * ESP32-C3 SuperMini + BMP280 + MPU6050 + microSD
 *
 * ЧТО ИЗМЕНИЛОСЬ В v3 — про «несколько файлов за одну сессию»
 *
 * Причина была не в именовании файлов. nextLogName() вызывается ровно один
 * раз, в setup(), поэтому за один запуск файл может появиться только один.
 * Несколько файлов — это несколько ПЕРЕЗАГРУЗОК платы: каждый ресет заново
 * проходил setup() и честно заводил себе новый лог.
 *
 * v3 делает три вещи:
 *
 *   1. Печатает и записывает ПРИЧИНУ последнего сброса (esp_reset_reason).
 *      Теперь не надо гадать — плата сама скажет, был это brownout
 *      (просадка питания), panic (ошибка в коде), watchdog или нажатие RST.
 *
 *   2. Если сброс был НЕ по включению питания, скетч понимает, что это
 *      продолжение того же полёта, и ДОПИСЫВАЕТ в тот же файл, а не плодит
 *      новый. Номер файла и накопленное время живут в RTC-памяти, которая
 *      переживает любой сброс кроме полного снятия питания.
 *
 *   3. Время time_s продолжается сквозь перезагрузку, а не падает в ноль.
 *      Добавлена колонка boot — номер запуска внутри файла, по ней сразу
 *      видно в Excel, где именно плата ресетнулась.
 *
 *   Плюс файл /boots.txt — журнал всех запусков с причинами. Его удобно
 *   читать глазами после полёта.
 *
 * Распиновка (соответствует разводке платы):
 *   I2C : SDA = GPIO4,  SCL = GPIO5
 *   SPI : CS  = GPIO7,  MOSI = GPIO6, CLK = GPIO10, MISO = GPIO20
 *   LED : GPIO8 (встроенный на модуле SuperMini)
 *
 * Адреса на шине I2C:
 *   BMP280  = 0x76  (вывод SD0 притянут к GND)
 *   MPU6050 = 0x68  (вывод AD0 притянут к GND)
 *
 * Библиотеки (Arduino IDE → Менеджер библиотек):
 *   Adafruit BMP280 Library
 *   Adafruit MPU6050
 *   Adafruit Unified Sensor
 */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <esp_system.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>

// ─────────── Пины ───────────
const int PIN_SDA  = 4;
const int PIN_SCL  = 5;
const int PIN_MOSI = 6;
const int PIN_CS   = 7;
const int PIN_SCK  = 10;
const int PIN_MISO = 20;
const int PIN_LED  = 8;

// На большинстве SuperMini светодиод включается низким уровнем.
// Если мигает наоборот — поменяй на false.
const bool LED_ACTIVE_LOW = true;

// ─────────── Настройки ───────────
const uint32_t SAMPLE_PERIOD_MS = 100;   // 100 мс = 10 Гц
const uint8_t  FLUSH_EVERY      = 10;    // сброс на карту раз в секунду

// Формат чисел в файле.
//   true  — запятая в дробях, разделитель ';'  → русский Excel откроет как надо
//   false — точка в дробях, разделитель ','    → удобнее для Python/pandas
const bool EXCEL_RU = true;

// !!! ЗАМЕНИТЬ на фактическое давление на уровне моря в день запуска (QNH).
// От этого числа напрямую зависит точность высоты.
const float SEALEVEL_HPA = 1013.25;

// ─────────── Память, переживающая сброс ───────────
// RTC_DATA_ATTR размещает переменную в RTC-памяти. Она сохраняется при
// программном сбросе, панике, watchdog и (обычно) при brownout, и теряется
// только когда питание снято полностью. Магическое число нужно, чтобы
// отличить осмысленные данные от мусора при первом включении.
RTC_DATA_ATTR static uint32_t rtcMagic;
RTC_DATA_ATTR static int32_t  rtcLogIndex;
RTC_DATA_ATTR static uint32_t rtcBootCount;
RTC_DATA_ATTR static uint32_t rtcUptimeMs;   // суммарное время предыдущих запусков

const uint32_t RTC_MAGIC = 0xCA115A71;

// ─────────── Объекты ───────────
Adafruit_BMP280  bmp;
Adafruit_MPU6050 mpu;
File logFile;

bool bmpOk = false;
bool mpuOk = false;
bool sdOk  = false;

uint32_t nextSample   = 0;
uint8_t  sinceFlush   = 0;
uint32_t uptimeBaseMs = 0;   // сколько натикало в предыдущих запусках
uint32_t bootNo       = 1;

const char SEP = EXCEL_RU ? ';' : ',';

// ─────────── Светодиод ───────────
void ledSet(bool on) {
  digitalWrite(PIN_LED, LED_ACTIVE_LOW ? !on : on);
}

void ledBlink(uint8_t times, uint16_t ms) {
  for (uint8_t i = 0; i < times; i++) {
    ledSet(true);  delay(ms);
    ledSet(false); delay(ms);
  }
}

// ─────────── Причина сброса человеческими словами ───────────
const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON - подано питание, нормальный старт";
    case ESP_RST_EXT:       return "EXT - внешний сброс (кнопка RST)";
    case ESP_RST_SW:        return "SW - программный перезапуск";
    case ESP_RST_PANIC:     return "PANIC - исключение в коде, плата упала";
    case ESP_RST_INT_WDT:   return "INT_WDT - watchdog прерываний";
    case ESP_RST_TASK_WDT:  return "TASK_WDT - watchdog задачи, loop завис";
    case ESP_RST_WDT:       return "WDT - сторожевой таймер";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP - выход из глубокого сна";
    case ESP_RST_BROWNOUT:  return "BROWNOUT - ПРОСАДКА ПИТАНИЯ";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN - причина не определена";
  }
}

// ─────────── Форматирование числа для файла ───────────
// NAN превращается в пустую строку — в Excel это будет пустая ячейка,
// а не слово "nan", которое ломает построение графиков.
void fnum(char *out, size_t cap, float v, uint8_t dec) {
  if (isnan(v)) { out[0] = '\0'; return; }
  snprintf(out, cap, "%.*f", dec, v);
  if (EXCEL_RU) for (char *p = out; *p; ++p) if (*p == '.') *p = ',';
}

// ─────────── Поиск свободного номера файла ───────────
int32_t nextFreeIndex() {
  char name[24];
  for (int32_t i = 1; i < 1000; i++) {
    snprintf(name, sizeof(name), "/log_%03ld.csv", (long)i);
    if (!SD.exists(name)) return i;
  }
  return 999;
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  ledSet(false);

  Serial.begin(115200);
  delay(300);   // дать порту подняться, но не ждать монитор бесконечно

  esp_reset_reason_t rr = esp_reset_reason();

  // Это продолжение того же полёта, если RTC-память цела И сброс был
  // не по включению питания. Нажатие RST тоже считаем продолжением —
  // полёт тот же, просто плата дёрнулась.
  bool continuing = (rtcMagic == RTC_MAGIC) &&
                    (rr != ESP_RST_POWERON) &&
                    (rr != ESP_RST_UNKNOWN);

  if (continuing) {
    rtcBootCount++;
    bootNo       = rtcBootCount;
    uptimeBaseMs = rtcUptimeMs;
  } else {
    rtcMagic     = RTC_MAGIC;
    rtcBootCount = 1;
    rtcUptimeMs  = 0;
    rtcLogIndex  = 0;      // номер выберем после инициализации карты
    bootNo       = 1;
    uptimeBaseMs = 0;
  }

  Serial.println();
  Serial.println(F("======================================"));
  Serial.println(F("  CanSat  —  плата 1 (MPU6050)   v3"));
  Serial.println(F("======================================"));
  Serial.print  (F("  Причина сброса : "));
  Serial.println(resetReasonText(rr));
  Serial.print  (F("  Запуск номер   : "));
  Serial.println(bootNo);
  if (continuing) {
    Serial.print(F("  Это ПЕРЕЗАГРУЗКА, дописываю в прежний файл. Накоплено, с: "));
    Serial.println(uptimeBaseMs / 1000.0f, 1);
  }

  // ─── I2C ───
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  // ─── BMP280 ───
  if (bmp.begin(0x76)) {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,    // температура
                    Adafruit_BMP280::SAMPLING_X16,   // давление
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_1);
    bmpOk = true;
    Serial.println(F("  BMP280  : OK"));
  } else {
    Serial.println(F("  BMP280  : НЕ НАЙДЕН"));
  }

  // ─── MPU6050 ───
  if (mpu.begin(0x68, &Wire)) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);   // запас на удар при посадке
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
    mpuOk = true;
    Serial.println(F("  MPU6050 : OK"));
  } else {
    Serial.println(F("  MPU6050 : НЕ НАЙДЕН"));
  }

  // ─── SD ───
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  if (SD.begin(PIN_CS, SPI, 4000000)) {

    if (!continuing || rtcLogIndex <= 0) rtcLogIndex = nextFreeIndex();

    char fname[24];
    snprintf(fname, sizeof(fname), "/log_%03ld.csv", (long)rtcLogIndex);

    bool fileExists = SD.exists(fname);

    // ВАЖНО: в ESP32 FILE_WRITE затирает файл с нуля, дописывает FILE_APPEND.
    // Это не как в классической Arduino SD, где FILE_WRITE дописывал.
    logFile = SD.open(fname, fileExists ? FILE_APPEND : FILE_WRITE);

    if (logFile) {
      if (!fileExists) {
        // Подсказка Excel, каким символом разделены колонки.
        // Без неё русский Excel свалит всю строку в одну ячейку.
        if (EXCEL_RU) logFile.println(F("sep=;"));

        logFile.print(F("time_s"));        logFile.print(SEP);
        logFile.print(F("temp_C"));        logFile.print(SEP);
        logFile.print(F("pressure_Pa"));   logFile.print(SEP);
        logFile.print(F("altitude_m"));    logFile.print(SEP);
        logFile.print(F("ax_m_s2"));       logFile.print(SEP);
        logFile.print(F("ay_m_s2"));       logFile.print(SEP);
        logFile.print(F("az_m_s2"));       logFile.print(SEP);
        logFile.print(F("gx_rad_s"));      logFile.print(SEP);
        logFile.print(F("gy_rad_s"));      logFile.print(SEP);
        logFile.print(F("gz_rad_s"));      logFile.print(SEP);
        logFile.print(F("mpu_temp_C"));    logFile.print(SEP);
        logFile.println(F("boot"));
      }
      logFile.flush();

      sdOk = true;
      Serial.print(F("  SD      : OK, файл "));
      Serial.print(fname);
      Serial.println(fileExists ? F("  (дописываю)") : F("  (новый)"));
    } else {
      Serial.println(F("  SD      : карта есть, файл не создался"));
    }

    // Журнал запусков — отдельным текстовым файлом, чтобы не ломать CSV.
    File b = SD.open("/boots.txt", FILE_APPEND);
    if (b) {
      b.printf("log_%03ld.csv  boot #%lu  t_base=%.1f s  reset=%s\n",
               (long)rtcLogIndex, (unsigned long)bootNo,
               uptimeBaseMs / 1000.0f, resetReasonText(rr));
      b.close();
    }

  } else {
    Serial.println(F("  SD      : НЕ НАЙДЕНА"));
  }

  Serial.println(F("--------------------------------------"));
  Serial.println(F("   t,s      T,C     P,Pa    H,m  |      ax      ay      az  |      gx      gy      gz"));
  Serial.println(F("--------------------------------------"));

  // Стартовый сигнал: 3 вспышки — всё нашлось, длинная — есть проблема
  if (bmpOk && mpuOk && sdOk) ledBlink(3, 120);
  else                        ledBlink(1, 800);

  nextSample = millis();
}

void loop() {
  uint32_t now = millis();
  if ((int32_t)(now - nextSample) < 0) return;
  nextSample += SAMPLE_PERIOD_MS;

  // Время сквозное: продолжается после перезагрузки, а не падает в ноль.
  float tsec = (uptimeBaseMs + now) / 1000.0f;

  // ─── Чтение датчиков ───
  // Если датчик не отвечает, пишем NAN и продолжаем. Частичные данные
  // лучше, чем остановка логирования посреди полёта.
  float tempC = NAN, pressPa = NAN, altM = NAN;
  if (bmpOk) {
    tempC   = bmp.readTemperature();
    pressPa = bmp.readPressure();
    altM    = bmp.readAltitude(SEALEVEL_HPA);
  }

  float ax = NAN, ay = NAN, az = NAN;
  float gx = NAN, gy = NAN, gz = NAN, mpuT = NAN;
  if (mpuOk) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    ax = a.acceleration.x;  ay = a.acceleration.y;  az = a.acceleration.z;
    gx = g.gyro.x;          gy = g.gyro.y;          gz = g.gyro.z;
    mpuT = t.temperature;
  }

  // ─── Вывод в Serial: читаемая таблица ───
  char mon[160];
  snprintf(mon, sizeof(mon),
           "%7.2f %8.2f %8.0f %7.2f | %7.2f %7.2f %7.2f | %7.3f %7.3f %7.3f",
           tsec, tempC, pressPa, altM, ax, ay, az, gx, gy, gz);
  Serial.println(mon);

  // ─── Запись в файл: CSV под Excel ───
  if (sdOk && logFile) {
    char f[11][16];
    fnum(f[0],  16, tsec,    2);
    fnum(f[1],  16, tempC,   2);
    fnum(f[2],  16, pressPa, 1);
    fnum(f[3],  16, altM,    2);
    fnum(f[4],  16, ax,      3);
    fnum(f[5],  16, ay,      3);
    fnum(f[6],  16, az,      3);
    fnum(f[7],  16, gx,      4);
    fnum(f[8],  16, gy,      4);
    fnum(f[9],  16, gz,      4);
    fnum(f[10], 16, mpuT,    2);

    char line[240];
    snprintf(line, sizeof(line),
             "%s%c%s%c%s%c%s%c%s%c%s%c%s%c%s%c%s%c%s%c%s%c%lu",
             f[0], SEP, f[1], SEP, f[2],  SEP, f[3], SEP, f[4], SEP,
             f[5], SEP, f[6], SEP, f[7],  SEP, f[8], SEP, f[9], SEP, f[10],
             SEP, (unsigned long)bootNo);
    logFile.println(line);

    if (++sinceFlush >= FLUSH_EVERY) {
      logFile.flush();                      // физически дописать на карту
      rtcUptimeMs = uptimeBaseMs + now;     // запомнить, докуда дошли
      sinceFlush = 0;
      ledSet(true); delay(5); ledSet(false);   // короткая вспышка = жив и пишет
    }
  } else {
    // Карты нет — частое мигание, видно снаружи без компьютера
    static bool s = false;
    s = !s;
    ledSet(s);
  }
}
