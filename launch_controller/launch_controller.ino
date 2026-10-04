#include <Wire.h>
#include <GyverOLED.h>

// Контроллер SH1106 128x64 (OLED 1.3")
GyverOLED<SSH1106_128x64> oled;

// --- Настройка пинов ---
const int PIN_FIRE      = 2;  // Красная кнопка (Пуск)
const int PIN_TIME_DOWN = 3;  // Уменьшение времени (-)
const int PIN_MODE      = 4;  // Активация пульта (D4)
const int PIN_TIME_UP   = 5;  // Увеличение времени (+)
const int PIN_OUTPUT    = 6;  // Выход управления (Реле / MOSFET)

// --- Переменные состояния ---
int delaySeconds = 10;        // Стартовое время: 10 секунд
const int MAX_DELAY = 60;
const int MIN_DELAY = 1;

bool isSystemActive = false;  // Флаг: перешли ли мы по кнопке D4 к задержке
bool needLcdUpdate = true;

unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

// --- Звёздное небо (фон для анимации запуска) ---
const int NUM_STARS = 18;
int starX[NUM_STARS];
int starY[NUM_STARS];

// --- Заливка треугольного плавника разверткой по X ---
// (xa, ya)-(xa, yb): вертикальное ребро у корпуса ракеты
// (xt, yt): вершина плавника, направленная наружу
void drawFin(int xa, int ya, int yb, int xt, int yt) {
  int steps = abs(xt - xa);
  if (steps == 0) steps = 1;
  for (int i = 0; i <= steps; i++) {
    float t = (float)i / steps;
    int x = xa + (int)((xt - xa) * t);
    int yTopEdge = ya + (int)((yt - ya) * t);
    int yBotEdge = yb + (int)((yt - yb) * t);
    oled.line(x, yTopEdge, x, yBotEdge);
  }
}

// --- Детальная отрисовка ракеты (по мотивам референсного рисунка) ---
void drawDetailedRocket(int x, int y, int frame) {
  // 0. Тонкая антенна на самом кончике носа
  oled.line(x, y - 4, x, y);

  // 1. Полный обтекаемый залитый конус (нос) - шире и объёмнее
  for (int i = 0; i <= 9; i++) {
    int hw;
    if (i < 2) hw = 0;
    else if (i < 4) hw = 1;
    else if (i < 6) hw = 2;
    else if (i < 8) hw = 3;
    else hw = 4;
    oled.line(x - hw, y + i, x + hw, y + i);
  }

  // 2. Основной корпус
  oled.rect(x - 4, y + 10, x + 4, y + 26, OLED_STROKE);
  // Технологические полосы (панели обшивки) + заклёпки по бокам
  oled.line(x - 4, y + 14, x + 4, y + 14);
  oled.line(x - 4, y + 21, x + 4, y + 21);
  oled.dot(x - 4, y + 17);
  oled.dot(x + 4, y + 17);

  // 3. Иллюминатор: двойное кольцо + блик, как на референсе
  oled.circle(x, y + 17, 3, OLED_STROKE);
  oled.circle(x, y + 17, 1, OLED_STROKE);

  // 4. Широкие скошенные залитые стабилизаторы (крылья)
  drawFin(x - 4, y + 19, y + 26, x - 10, y + 29); // левый
  drawFin(x + 4, y + 19, y + 26, x + 10, y + 29); // правый
  // Внутренняя линия на плавниках для объёма/тени
  oled.line(x - 6, y + 22, x - 8, y + 27);
  oled.line(x + 6, y + 22, x + 8, y + 27);

  // 5. Юбка двигателя и сопло
  oled.rect(x - 3, y + 27, x + 3, y + 29, OLED_FILL);
  oled.line(x - 4, y + 30, x - 2, y + 31);
  oled.line(x + 4, y + 30, x + 2, y + 31);
  oled.line(x - 2, y + 31, x + 2, y + 31);

  // 6. Слоистое анимированное пламя
  int flick = frame % 3;
  // Внешние языки пламени (виляют по бокам)
  if (flick == 0) {
    oled.line(x - 2, y + 31, x - 4, y + 37);
    oled.line(x + 2, y + 31, x + 4, y + 37);
  } else if (flick == 1) {
    oled.line(x - 2, y + 31, x - 3, y + 36);
    oled.line(x + 2, y + 31, x + 3, y + 38);
  } else {
    oled.line(x - 2, y + 31, x - 4, y + 35);
    oled.line(x + 2, y + 31, x + 3, y + 36);
  }
  // Яркий центральный факел (пульсирует по длине)
  oled.line(x, y + 31, x, y + 35 + (frame % 2) * 3);
  oled.line(x - 1, y + 32, x + 1, y + 32);
}

// --- Мерцающее звёздное небо (используется и на заставке, и в полёте) ---
void drawStars(int frame) {
  for (int i = 0; i < NUM_STARS; i++) {
    // Часть звёзд "гаснет" по очереди, создавая эффект мерцания
    if ((i + frame) % 5 != 0) {
      oled.dot(starX[i], starY[i]);
    }
  }
}

// --- Длинный шлейф пламени от сопла ракеты до низа экрана ---
void drawFlameTrail(int x, int startY, int frame) {
  for (int ty = startY; ty < 64; ty += 3) {
    // Лёгкое "виляние" пламени в стороны для эффекта живого огня
    int wave = ((ty / 3) + frame) % 3;
    int dx = (wave == 0) ? -1 : (wave == 1 ? 0 : 1);
    oled.dot(x + dx, ty);
  }
}

// --- Пышное облако дыма у стартового стола (только в первые кадры взлёта) ---
void drawLaunchSmoke(int frame) {
  if (frame > 14) return; // дым рассеивается после первых кадров, когда ракета уже высоко
  int g = frame; // рост облака со временем
  // Кластер из нескольких перекрывающихся клубов - как пышное облако на референсе
  oled.circle(20, 64, 7 + g / 3, OLED_STROKE);
  oled.circle(42, 61, 5 + g / 4, OLED_STROKE);
  oled.circle(64, 63, 6 + g / 3, OLED_STROKE);
  oled.circle(86, 61, 5 + g / 4, OLED_STROKE);
  oled.circle(108, 64, 7 + g / 3, OLED_STROKE);
}

// --- Логотип RocketRise, сконвертированный из PNG в битмап 31x40 для GyverOLED ---
const uint8_t PROGMEM logoBitmap[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0x38, 0x7C, 0xFE, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x80, 0xC0, 0xE0, 0x70, 0x30, 0x18, 0x08, 0x0C, 0x04, 0x84, 0x80, 0xC0, 0xC0, 0x80,
  0xF0, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xFE, 0x3F, 0x07, 0x01, 0x00, 0x00, 0x00, 0xE0, 0xFC,
  0xFF, 0x1F, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0F, 0x07, 0x87, 0xC4, 0x4F, 0x8F,
  0x93, 0x1F, 0x1F, 0x4F, 0xF7, 0xF9, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x1C, 0xF0, 0x1F, 0xFF, 0xFF,
  0xFC, 0xE0, 0x80, 0x80, 0x80, 0xC0, 0xC0, 0xE0, 0xF0, 0xFC, 0xFE, 0x7F, 0x3F, 0x0F, 0x03, 0x00,
  0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x80, 0xC0, 0xF0, 0x7F, 0x1F, 0x00, 0x00, 0x03, 0x07,
  0x0F, 0x1F, 0x1F, 0x1F, 0x4F, 0x6F, 0x67, 0xF7, 0xF3, 0xF1, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
  0x78, 0x78, 0x3C, 0x3C, 0x1E, 0x0F, 0x0F, 0x03, 0x01, 0x00, 0x00,
};
const int LOGO_W = 31;
const int LOGO_H = 40;

void setup() {
  Serial.begin(9600);

  oled.init();
  oled.clear();
  oled.update();

  // Настройка кнопок (срабатывание по HIGH)
  pinMode(PIN_FIRE, INPUT);
  pinMode(PIN_TIME_DOWN, INPUT);
  pinMode(PIN_MODE, INPUT);
  pinMode(PIN_TIME_UP, INPUT);

  // Силовой выход
  pinMode(PIN_OUTPUT, OUTPUT);
  digitalWrite(PIN_OUTPUT, LOW);

  // Генерация случайного звёздного неба (один раз при старте)
  randomSeed(analogRead(A0));
  for (int i = 0; i < NUM_STARS; i++) {
    starX[i] = random(0, 128);
    starY[i] = random(0, 60);
  }
}

void loop() {
  unsigned long currentMillis = millis();

  // --- 1. Активация пульта кнопкой D4 ---
  if (digitalRead(PIN_MODE) == HIGH && (currentMillis - lastDebounceTime > debounceDelay)) {
    isSystemActive = true;
    needLcdUpdate = true;
    lastDebounceTime = currentMillis;
  }

  // --- 2. Логика работы после нажатия D4 ---
  if (isSystemActive) {

    // Уменьшение времени (D3)
    if (digitalRead(PIN_TIME_DOWN) == HIGH && (currentMillis - lastDebounceTime > debounceDelay)) {
      if (delaySeconds > MIN_DELAY) {
        delaySeconds--;
        needLcdUpdate = true;
      }
      lastDebounceTime = currentMillis;
    }

    // Увеличение времени (D5)
    if (digitalRead(PIN_TIME_UP) == HIGH && (currentMillis - lastDebounceTime > debounceDelay)) {
      if (delaySeconds < MAX_DELAY) {
        delaySeconds++;
        needLcdUpdate = true;
      }
      lastDebounceTime = currentMillis;
    }

    // --- 3. Пуск (D2) ---
    if (digitalRead(PIN_FIRE) == HIGH && (currentMillis - lastDebounceTime > debounceDelay)) {
      lastDebounceTime = currentMillis;

      // Обратный отсчет
      for (int i = delaySeconds; i > 0; i--) {
        oled.clear();
        oled.setScale(2);
        oled.setCursor(8, 1);
        oled.print(F("COUNTDOWN"));

        oled.setScale(3);
        oled.setCursor(45, 4);
        if (i < 10) oled.print("0");
        oled.print(i);

        oled.update();
        delay(1000);
      }

      // --- ПОДАЧА ТОКА И КРАСИВАЯ АНИМАЦИЯ ---
      digitalWrite(PIN_OUTPUT, HIGH); // Включаем силовое реле

      int frameCounter = 0;

      // Взлет ракеты снизу вверх (от Y=32 до Y=-40)
      for (int y = 32; y >= -40; y -= 2) {
        oled.clear();

        // Лёгкая тряска экрана в первые кадры (вибрация двигателей)
        int shakeX = (frameCounter < 6) ? random(-1, 2) : 0;

        // Фон: мерцающие звёзды
        drawStars(frameCounter);

        // Клубы дыма у стартового стола
        drawLaunchSmoke(frameCounter);

        // Длинный шлейф пламени от сопла до низа экрана
        drawFlameTrail(64 + shakeX, y + 31, frameCounter);

        // Отрисовка ракеты по центру (X=64)
        drawDetailedRocket(64 + shakeX, y, frameCounter);

        oled.update();
        frameCounter++;
        delay(35); // Плавность анимации
      }

      digitalWrite(PIN_OUTPUT, LOW); // Отключаем ток

      oled.clear();
      needLcdUpdate = true;
    }
  }

  // --- 4. Отрисовка экрана ---
  if (needLcdUpdate) {
    updateDisplay();
    needLcdUpdate = false;
  }
}

void updateDisplay() {
  oled.clear();

  if (!isSystemActive) {
    // --- Стартовый экран: логотип + подпись ---
    oled.drawBitmap((128 - LOGO_W) / 2, 2, logoBitmap, LOGO_W, LOGO_H);

    oled.setScale(1);
    oled.setCursor(31, 6);
    oled.print(F("ROCKET RISE"));

  } else {
    // --- Экран настройки задержки (после нажатия D4) ---
    oled.setScale(1);
    oled.setCursor(0, 0);
    oled.print(F("DELAY TIME SETTING"));

    oled.line(0, 14, 127, 14);

    // Крупные цифры времени
    oled.setScale(4);
    oled.setCursor(35, 3);
    if (delaySeconds < 10) oled.print("0");
    oled.print(delaySeconds);

    oled.setScale(1);
    oled.setCursor(55, 7);
    oled.print(F("SEC"));
  }

  oled.update();
}
