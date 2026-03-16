#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <SwitecX25.h>

// ===================== PIN CONFIG =====================
#define OLED_SDA    D4
#define OLED_SCL    D5
#define LED_PIN     D7
#define BUTTON_PIN  D8
#define MOTOR_P1    D0
#define MOTOR_P2    D1
#define MOTOR_P3    D2
#define MOTOR_P4    D3

// ===================== HARDWARE CONSTANTS =====================
#define SCREEN_W    128
#define SCREEN_H    64
#define OLED_ADDR   0x3C
#define NUM_LEDS    16
#define MOTOR_STEPS 945  // X27: 315 deg * 3 steps/deg

// ===================== PACKET (must match sensor) =====================
typedef struct __attribute__((packed)) {
  int bpm;
  int peak;
  int level;
  int beat;
  int activity;
} SensorPacket;

// ===================== VISUAL MODES =====================
enum VisualMode {
  MODE_STANDBY,
  MODE_FACE,
  MODE_BIG_BPM,
  MODE_SYNTH,
  MODE_LASER
};

static const char* modeName(VisualMode m) {
  switch (m) {
    case MODE_STANDBY: return "STANDBY";
    case MODE_FACE:    return "FACE";
    case MODE_BIG_BPM: return "BIG_BPM";
    case MODE_SYNTH:   return "SYNTH";
    case MODE_LASER:   return "LASER";
    default:           return "???";
  }
}

// Deterministic mode cycle when music is playing.
// BIG_BPM appears 3 times per 42-second cycle and cannot be skipped.
struct ModeSlot { VisualMode mode; unsigned long ms; };

static const ModeSlot MODE_CYCLE[] = {
  { MODE_BIG_BPM, 3000 },  // 0  show BPM first thing when music starts
  { MODE_FACE,    5000 },  // 1
  { MODE_FACE,    5000 },  // 2
  { MODE_BIG_BPM, 3000 },  // 3
  { MODE_FACE,    5000 },  // 4
  { MODE_SYNTH,   4000 },  // 5
  { MODE_FACE,    5000 },  // 6
  { MODE_BIG_BPM, 3000 },  // 7
  { MODE_FACE,    5000 },  // 8
  { MODE_LASER,   4000 },  // 9
};
static const int CYCLE_LEN = sizeof(MODE_CYCLE) / sizeof(MODE_CYCLE[0]);

// ===================== HARDWARE OBJECTS =====================
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
SwitecX25 motor(MOTOR_STEPS, MOTOR_P1, MOTOR_P2, MOTOR_P3, MOTOR_P4);

// ===================== RECEIVED SENSOR DATA =====================
volatile bool rxFlag = false;
SensorPacket rxBuf = {};

int bpm      = 0;
int peak     = 0;
int level    = 0;
int activity = 0;

// ===================== TIMING / STATE =====================
bool musicActive              = false;
unsigned long lastActiveMs    = 0;
unsigned long musicConfirmMs  = 0;   // when we first started seeing BPM
int musicConfirmCount         = 0;   // consecutive packets with BPM > 0
static const unsigned long MUSIC_TIMEOUT = 1500;
static const int CONFIRM_PACKETS = 6; // need ~6 consecutive BPM packets to activate

VisualMode currentMode     = MODE_STANDBY;
int cycleIdx               = 0;
unsigned long modeStartMs  = 0;

// Rave
bool raveMode              = false;
unsigned long highBpmStartMs   = 0;
unsigned long raveStartMs      = 0;
bool laserShow             = false;
unsigned long laserShowStartMs = 0;
static const unsigned long RAVE_THRESHOLD_MS = 10000;
static const unsigned long LASER_SHOW_MS     = 10000;

// Beat pulse timestamp (updated every time a beat packet arrives)
unsigned long lastBeatMs   = 0;

// Face blink animation
unsigned long lastBlinkMs    = 0;
unsigned long nextBlinkDelay = 3000;
bool eyesClosed              = false;
unsigned long blinkStartMs   = 0;

// LED fade
unsigned long ledPulseMs = 0;

// Standby screen falling notes state
static float notePos[4] = {-10, 20, 50, 80};
static float noteSpeed[4] = {0.08f, 0.12f, 0.10f, 0.09f};
static float noteX[4] = {20, 45, 80, 105};
static bool notesInitialized = false;

// ===================== ESP-NOW RECEIVE CALLBACK =====================
#if ESP_IDF_VERSION_MAJOR >= 5
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(SensorPacket)) {
    memcpy((void *)&rxBuf, data, sizeof(SensorPacket));
    rxFlag = true;
  }
}

// ===================== DRAW HELPERS =====================
static void centeredText(const char *txt, int y, int sz) {
  oled.setTextSize(sz);
  int16_t x1, y1;
  uint16_t tw, th;
  oled.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
  oled.setCursor((SCREEN_W - tw) / 2, y);
  oled.print(txt);
}

// ===================== STANDBY SCREEN =====================
static void drawStandby() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  unsigned long now = millis();

  // --- Background: two slow synth wave lines (no fill) ---
  float phase1 = now * 0.0008f;
  float phase2 = now * 0.0012f;
  for (int x = 0; x < SCREEN_W; x++) {
    int y1 = 54 + (int)(6.0f * sinf(x * 0.06f + phase1));
    int y2 = 58 + (int)(4.0f * sinf(x * 0.09f + phase2));
    y1 = constrain(y1, 0, SCREEN_H - 1);
    y2 = constrain(y2, 0, SCREEN_H - 1);
    oled.drawPixel(x, y1, SSD1306_WHITE);
    oled.drawPixel(x, y2, SSD1306_WHITE);
  }

  // --- Falling music notes ---
  if (!notesInitialized) {
    notesInitialized = true;
    for (int i = 0; i < 4; i++) {
      notePos[i] = -10.0f + i * 18.0f;
      noteX[i] = 8 + random(SCREEN_W - 16);
    }
  }

  for (int i = 0; i < 4; i++) {
    notePos[i] += noteSpeed[i];
    if (notePos[i] > SCREEN_H + 8) {
      notePos[i] = -8;
      noteX[i] = 8 + random(SCREEN_W - 16);
    }
    int ny = (int)notePos[i];
    int nx = (int)noteX[i];
    if (ny >= -4 && ny < SCREEN_H) {
      // Note head (filled oval)
      oled.fillCircle(nx, ny, 2, SSD1306_WHITE);
      // Stem going up
      oled.drawFastVLine(nx + 2, ny - 6, 7, SSD1306_WHITE);
      // Flag
      oled.drawPixel(nx + 3, ny - 6, SSD1306_WHITE);
      oled.drawPixel(nx + 4, ny - 5, SSD1306_WHITE);
    }
  }

  // --- Text (drawn last so it's always on top) ---
  centeredText("Let's", 10, 2);
  centeredText("VIBE", 30, 3);

  // Animated dots
  int dots = (now / 500) % 4;
  for (int i = 0; i < dots; i++)
    oled.fillCircle(48 + i * 12, 58, 2, SSD1306_WHITE);

  oled.display();
}

// ===================== BIG BPM SCREEN =====================
static void drawBigBpm() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2 - 6;

  bool recentBeat = (millis() - lastBeatMs) < 250;
  if (recentBeat) {
    int t = millis() - lastBeatMs;
    oled.drawCircle(cx, cy, 18 + t / 8,  SSD1306_WHITE);
    oled.drawCircle(cx, cy, 10 + t / 12, SSD1306_WHITE);
  } else {
    float breath = sinf((millis() % 2000) * 3.14159f / 1000.0f);
    oled.drawCircle(cx, cy, 26 + (int)(breath * 3), SSD1306_WHITE);
  }

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", bpm);
  oled.setTextSize(4);
  int16_t x1, y1;
  uint16_t tw, th;
  oled.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  oled.setCursor((SCREEN_W - tw) / 2, cy - th / 2);
  oled.print(buf);

  centeredText("BPM", 54, 1);
  oled.display();
}

// ===================== FACE SCREEN =====================
static void drawFace(bool rave) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  unsigned long now = millis();

  bool recentBeat = (now - lastBeatMs) < 180;

  // --- Blink state machine ---
  if (!eyesClosed && now - lastBlinkMs > nextBlinkDelay) {
    eyesClosed   = true;
    blinkStartMs = now;
  }
  if (eyesClosed && now - blinkStartMs > 150) {
    eyesClosed     = false;
    lastBlinkMs    = now;
    nextBlinkDelay = 2000 + random(3000);
  }

  // --- Eyes with pupils ---
  const int eyeW = 24, eyeY = 8;
  int eyeH = eyesClosed ? 3 : 16;
  const int lx = 14, rx = 90;
  oled.fillRoundRect(lx, eyeY, eyeW, eyeH, 3, SSD1306_WHITE);
  oled.fillRoundRect(rx, eyeY, eyeW, eyeH, 3, SSD1306_WHITE);

  if (!eyesClosed) {
    int pupilR = 4;
    int lookX = (int)(2.0f * sinf(now * 0.0008f));
    int pupilY = eyeY + eyeH / 2;
    oled.fillCircle(lx + eyeW / 2 + lookX, pupilY, pupilR, SSD1306_BLACK);
    oled.fillCircle(rx + eyeW / 2 + lookX, pupilY, pupilR, SSD1306_BLACK);
  }

  // --- Eyebrows that bounce on beat ---
  int baseTilt = rave ? 7 : 3;
  int beatBounce = recentBeat ? 4 : 0;
  int browY = eyeY - 4 - beatBounce;

  for (int t = 0; t < 2; t++) {
    oled.drawLine(lx - 2,        browY - baseTilt + t,
                  lx + eyeW + 2, browY + t,             SSD1306_WHITE);
    oled.drawLine(rx - 2,        browY + t,
                  rx + eyeW + 2, browY - baseTilt + t,   SSD1306_WHITE);
  }

  // --- Mouth / teeth visualizer ---
  const int mouthY = 34, mouthH = 28;
  const int nTeeth = 8, gap = 2;
  int tw = (SCREEN_W - 4 - (nTeeth - 1) * gap) / nTeeth;
  int totalW = nTeeth * tw + (nTeeth - 1) * gap;
  int startX = (SCREEN_W - totalW) / 2;

  oled.drawRect(startX - 2, mouthY - 1, totalW + 4, mouthH + 2, SSD1306_WHITE);

  static const int shape[] = {-3, 1, -1, 4, 2, -2, 3, 0};

  for (int i = 0; i < nTeeth; i++) {
    int h;
    if (recentBeat) {
      h = mouthH - 2 + (shape[i] % 3);
    } else {
      int base = map(constrain(level, 0, 400), 0, 400, 4, mouthH - 6);
      h = base + shape[i];
    }
    h = constrain(h, 2, mouthH - 1);
    int x = startX + i * (tw + gap);
    oled.fillRect(x, mouthY + mouthH - h, tw, h, SSD1306_WHITE);
  }

  oled.display();
}

// ===================== SYNTH WAVE SCREEN =====================
static void drawSynthWave(bool rave) {
  oled.clearDisplay();

  int waves = rave ? 4 : 2;

  // Wave scroll speed scales with BPM: faster BPM = faster waves
  float bpmFactor = constrain(bpm, 60, 200) / 120.0f;
  float speed = (rave ? 0.006f : 0.003f) * bpmFactor;
  float freqBase = 0.05f + 0.02f * bpmFactor;

  bool recentBeat = (millis() - lastBeatMs) < 150;
  float beatBoost = recentBeat ? 6.0f : 0.0f;

  for (int w = 0; w < waves; w++) {
    float freq  = freqBase + w * 0.025f;
    float phase = millis() * speed + w * 1.5f;
    int yBase   = SCREEN_H / 2 + (w - waves / 2) * 10;
    int prevY   = -1;

    for (int x = 0; x < SCREEN_W; x++) {
      float amp = 14.0f + beatBoost + 5.0f * sinf(millis() * 0.001f + w);
      int y = yBase + (int)(amp * sinf(x * freq + phase));
      y = constrain(y, 0, SCREEN_H - 1);
      oled.drawPixel(x, y, SSD1306_WHITE);

      if (prevY >= 0 && abs(y - prevY) > 1) {
        int lo = min(y, prevY), hi = max(y, prevY);
        for (int ly = lo; ly <= hi; ly++)
          oled.drawPixel(x, ly, SSD1306_WHITE);
      }
      prevY = y;
    }
  }

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 2);
  oled.printf("%d BPM", bpm);
  oled.display();
}

// ===================== LASER SCREEN =====================
static void drawLaser(bool rave) {
  oled.clearDisplay();
  unsigned long now = millis();
  bool pulse = (now - lastBeatMs) < 200;

  // --- Animation 1: zoom rings (expanding/contracting circles) ---
  float breathPeriod = 1500.0f;
  float t = fmodf(now, breathPeriod) / breathPeriod;
  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2;
  int nRings = rave ? 6 : 4;

  for (int i = 0; i < nRings; i++) {
    float phase = fmodf(t + (float)i / nRings, 1.0f);
    int r = (int)(phase * 40);
    if (pulse) r += 4;
    if (r > 2 && r < 50)
      oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  }

  // --- Animation 2: bottom-up laser beams (concert POV) ---
  int nBeams = rave ? 10 : 6;
  float sway = now * (rave ? 0.003f : 0.0015f);

  for (int i = 0; i < nBeams; i++) {
    float spread = -0.8f + (float)i / (nBeams - 1) * 1.6f;
    float angle  = spread + 0.15f * sinf(sway + i * 0.9f);

    int baseX = SCREEN_W / 2 + (int)((SCREEN_W / 2) * sinf(spread * 0.5f));
    int topX  = baseX + (int)(SCREEN_H * tanf(angle));
    int topY  = pulse ? -4 : 0;

    oled.drawLine(baseX, SCREEN_H - 1, topX, topY, SSD1306_WHITE);
    if (rave)
      oled.drawLine(baseX + 1, SCREEN_H - 1, topX + 1, topY, SSD1306_WHITE);
  }

  // --- Ground line ---
  oled.drawFastHLine(0, SCREEN_H - 1, SCREEN_W, SSD1306_WHITE);

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 2);
  oled.printf("%d BPM", bpm);
  oled.display();
}

// ===================== LED UPDATE =====================
static void updateLeds() {
  unsigned long now = millis();
  unsigned long elapsed = now - ledPulseMs;

  if (!musicActive) {
    // Standby: rainbow scroll matching the OLED wave speed
    uint16_t hueBase = (uint16_t)(now * 0.0008f * 65536.0f) % 65536;
    for (int i = 0; i < NUM_LEDS; i++) {
      uint16_t hue = hueBase + (i * 65536 / NUM_LEDS);
      strip.setPixelColor(i, strip.ColorHSV(hue, 255, 40));  // dim rainbow
    }
    strip.show();
    return;
  }

  // Beat pulse: sharp flash that fades quickly
  uint8_t beatBr = 0;
  if (elapsed < 200) {
    // Exponential decay for snappier feel
    float t = elapsed / 200.0f;
    beatBr = (uint8_t)(255 * (1.0f - t * t));
  }

  // Base brightness from audio level (subtle background glow)
  uint8_t baseBr = map(constrain(peak, 0, 2000), 0, 2000, 5, 40);
  
  // Activity boost: adds brightness on peaks
  uint8_t activityBr = map(constrain(activity, 0, 1000), 0, 1000, 0, 30);

  // Combine: beat flash + base + activity
  uint8_t totalBr = min(255, beatBr + baseBr + activityBr);

  // Color based on BPM
  uint32_t c;
  if (bpm < 80) {
    // Teal/cyan
    c = strip.Color(0, totalBr, totalBr / 2);
  } else if (bpm < 120) {
    // Blue
    c = strip.Color(0, totalBr / 3, totalBr);
  } else {
    // Magenta/pink
    c = strip.Color(totalBr, 0, totalBr / 3);
  }

  // Apply with slight variation for visual interest
  for (int i = 0; i < NUM_LEDS; i++) {
    // Center LEDs get full brightness, edges slightly dimmer
    float pos = (float)i / (NUM_LEDS - 1);
    float fade = 0.7f + 0.3f * (1.0f - abs(pos - 0.5f) * 2.0f);
    uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * fade);
    uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * fade);
    uint8_t b = (uint8_t)((c & 0xFF) * fade);
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

// ===================== MOTOR UPDATE =====================
static void updateMotor() {
  int target = map(constrain(bpm, 0, 200), 0, 200, 0, MOTOR_STEPS - 1);
  motor.setPosition(target);
  motor.update();
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========== Vibe Bot Display ==========");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[DISP] OLED FAIL");
    while (true) delay(100);
  }
  oled.clearDisplay();
  oled.display();
  Serial.println("[DISP] OLED OK");

  strip.begin();
  strip.setBrightness(80);
  strip.show();
  Serial.println("[DISP] LEDs OK");

  motor.zero();
  Serial.println("[DISP] Motor OK");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(200);
  Serial.print("[DISP] MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[DISP] ESP-NOW FAIL");
    while (true) delay(100);
  }
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("[DISP] ESP-NOW ready – waiting for sensor");

  currentMode  = MODE_STANDBY;
  modeStartMs  = millis();
  lastActiveMs = 0;
}

// ===================== MAIN LOOP =====================
void loop() {
  unsigned long now = millis();

  // ---- Consume incoming packet ----
  if (rxFlag) {
    rxFlag   = false;
    bpm      = rxBuf.bpm;
    peak     = rxBuf.peak;
    level    = rxBuf.level;
    activity = rxBuf.activity;

    if (rxBuf.beat) {
      lastBeatMs = now;
      ledPulseMs = now;
    }

    // Music confirmation: require multiple consecutive packets with real BPM
    if (rxBuf.bpm > 30) {
      musicConfirmCount++;
      if (musicConfirmCount >= CONFIRM_PACKETS)
        lastActiveMs = now;
    } else {
      musicConfirmCount = 0;
    }
  }

  // ---- Music-active with hysteresis ----
  bool wasActive = musicActive;
  musicActive = (lastActiveMs > 0) && (now - lastActiveMs < MUSIC_TIMEOUT);

  // ==========================================================
  //  STATE MACHINE
  // ==========================================================

  if (!musicActive) {
    // ---- NO MUSIC -> standby ----
    if (currentMode != MODE_STANDBY) {
      currentMode = MODE_STANDBY;
      cycleIdx    = 0;
      raveMode    = false;
      laserShow   = false;
      Serial.println("[DISP] -> STANDBY");
    }

  } else {
    // ---- MUSIC ACTIVE ----

    // Returning from standby / first packet -> start the cycle
    if (!wasActive || currentMode == MODE_STANDBY) {
      cycleIdx       = 0;
      modeStartMs    = now;
      currentMode    = MODE_CYCLE[0].mode;
      raveMode       = false;
      highBpmStartMs = 0;
      laserShow      = false;
      Serial.printf("[DISP] Music ON -> %s\n", modeName(currentMode));
    }

    // ---- Rave detection (BPM > 120 for 10 s) ----
    if (bpm > 120) {
      if (highBpmStartMs == 0) highBpmStartMs = now;
      if (!raveMode && (now - highBpmStartMs >= RAVE_THRESHOLD_MS)) {
        raveMode    = true;
        raveStartMs = now;
        Serial.println("[DISP] RAVE ON");
      }
    } else {
      highBpmStartMs = 0;
      if (raveMode) {
        raveMode  = false;
        laserShow = false;
        Serial.println("[DISP] Rave OFF");
      }
    }

    // ---- Laser-show override (10 s into rave) ----
    if (raveMode && !laserShow && (now - raveStartMs >= RAVE_THRESHOLD_MS)) {
      laserShow        = true;
      laserShowStartMs = now;
      Serial.println("[DISP] LASER SHOW START");
    }
    if (laserShow) {
      if (now - laserShowStartMs < LASER_SHOW_MS)
        currentMode = MODE_LASER;
      else {
        laserShow   = false;
        modeStartMs = now;
        Serial.println("[DISP] LASER SHOW END");
      }
    }

    // ---- Normal cycle advance (skipped while laser show owns the screen) ----
    if (!laserShow) {
      if (now - modeStartMs >= MODE_CYCLE[cycleIdx].ms) {
        cycleIdx    = (cycleIdx + 1) % CYCLE_LEN;
        modeStartMs = now;
        currentMode = MODE_CYCLE[cycleIdx].mode;
        Serial.printf("[DISP] -> %s [%d] %lums\n",
                      modeName(currentMode), cycleIdx, MODE_CYCLE[cycleIdx].ms);
      }
    }
  }

  // ==========================================================
  //  DRAW
  // ==========================================================
  switch (currentMode) {
    case MODE_STANDBY: drawStandby();           break;
    case MODE_FACE:    drawFace(raveMode);      break;
    case MODE_BIG_BPM: drawBigBpm();            break;
    case MODE_SYNTH:   drawSynthWave(raveMode); break;
    case MODE_LASER:   drawLaser(raveMode);     break;
  }

  // ==========================================================
  //  PERIPHERALS
  // ==========================================================
  updateLeds();
  updateMotor();

  // ==========================================================
  //  DEBUG LOG (every 500 ms)
  // ==========================================================
  static unsigned long lastLog = 0;
  if (now - lastLog > 500) {
    lastLog = now;
    Serial.printf("[DISP] mode=%-8s bpm=%3d pk=%4d lv=%3d act=%3d music=%d rave=%d laser=%d\n",
                  modeName(currentMode), bpm, peak, level, activity,
                  musicActive, raveMode, laserShow);
  }

  delay(16);
}
