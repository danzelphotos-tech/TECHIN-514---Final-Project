#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>

// ===================== PIN CONFIG =====================
#define I2S_WS   D1
#define I2S_SCK  D0
#define I2S_SD   D2

// ===================== AUDIO CONFIG =====================
#define SAMPLE_RATE     16000
#define BUFFER_SAMPLES  512

// ===================== ESP-NOW =====================
// Replace with your display device's MAC address
uint8_t displayMAC[] = {0x1C, 0xDB, 0xD4, 0x75, 0xAA, 0xD4};

// ===================== PACKET =====================
typedef struct __attribute__((packed)) {
  int bpm;
  int peak;
  int level;
  int beat;
  int activity;
} SensorPacket;

// ===================== GLOBALS =====================
int32_t audioBuffer[BUFFER_SAMPLES];
SensorPacket packet;

// Envelope followers for onset detection
float envFast  = 0.0f;
float envSlow  = 0.0f;
float onsetEnv = 0.0f;
float onsetAvg = 0.0f;

// Beat tracking
unsigned long lastBeatMs = 0;
bool beatLatch = false;

#define BEAT_HISTORY 8
unsigned long beatIntervals[BEAT_HISTORY] = {0};
int beatIdx = 0;

// BPM
int bpmSmoothed = 0;
int bpmLocked   = 0;            // once locked, holds steady
bool bpmIsLocked = false;
unsigned long bpmLockTime = 0;  // when we locked
unsigned long lastMusicMs = 0;
unsigned long lastAudioMs = 0;  // any audio energy (not just "music")

// Beat confidence
int consecutiveBeats = 0;
static const int MIN_BEATS_TO_LOCK = 4;

// Relock: count how many consecutive candidates disagree with lock
int relockCount = 0;
static const int RELOCK_THRESHOLD = 8; // need 8 consecutive outliers to re-lock

// ===================== HELPERS =====================
int clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void clearBeatHistory() {
  for (int i = 0; i < BEAT_HISTORY; i++) beatIntervals[i] = 0;
  beatIdx = 0;
}

void pushInterval(unsigned long ms) {
  if (ms >= 230 && ms <= 2000) {
    beatIntervals[beatIdx] = ms;
    beatIdx = (beatIdx + 1) % BEAT_HISTORY;
  }
}

int computeBpm() {
  unsigned long now = millis();

  if (lastBeatMs == 0 || (now - lastBeatMs) > 3000)
    return 0;

  // Collect valid intervals
  unsigned long valid[BEAT_HISTORY];
  int n = 0;
  for (int i = 0; i < BEAT_HISTORY; i++) {
    if (beatIntervals[i] >= 230 && beatIntervals[i] <= 2000)
      valid[n++] = beatIntervals[i];
  }
  if (n < 2) return 0;

  // Find minimum interval — the shortest gap is most likely correct
  // because missed beats can only create longer gaps, never shorter.
  unsigned long minVal = valid[0];
  for (int i = 1; i < n; i++)
    if (valid[i] < minVal) minVal = valid[i];

  // Reject anything > 1.5x the minimum (likely a missed beat)
  unsigned long maxOk = minVal + (minVal / 2);

  unsigned long sum = 0;
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (valid[i] <= maxOk) {
      sum += valid[i];
      count++;
    }
  }
  if (count < 2) return 0;

  float avg = (float)sum / count;

  // Consistency check: reject if intervals vary too much (speech = random)
  float variance = 0;
  for (int i = 0; i < n; i++) {
    if (valid[i] <= maxOk) {
      float diff = (float)valid[i] - avg;
      variance += diff * diff;
    }
  }
  variance /= count;
  float stddev = sqrtf(variance);
  if (stddev / avg > 0.25f) return 0;  // coefficient of variation > 25% = not music

  int raw = (int)(60000.0f / avg);

  if (raw > 0 && raw < 70) raw *= 2;
  if (raw > 180) raw /= 2;

  return clamp(raw, 0, 230);
}

// ===================== I2S SETUP =====================
void setupI2S() {
  i2s_config_t cfg = {};
  cfg.mode             = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate      = SAMPLE_RATE;
  cfg.bits_per_sample  = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format   = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count    = 4;
  cfg.dma_buf_len      = 256;
  cfg.use_apll         = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_SCK;
  pins.ws_io_num    = I2S_WS;
  pins.data_out_num = -1;
  pins.data_in_num  = I2S_SD;

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// ===================== ESP-NOW SETUP =====================
void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(200);

  Serial.print("[SENSOR] MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[SENSOR] ESP-NOW init FAILED");
    while (true) delay(100);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, displayMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[SENSOR] Add peer FAILED");
    while (true) delay(100);
  }

  Serial.println("[SENSOR] ESP-NOW ready");
}

// ===================== BEAT DETECTION =====================
bool detectBeat(int peak, int level) {
  unsigned long now = millis();

  envFast = 0.30f * envFast + 0.70f * peak;   // near-instant attack
  envSlow = 0.995f * envSlow + 0.005f * peak;

  float onset = max(0.0f, envFast - envSlow);
  onsetEnv = 0.35f * onsetEnv + 0.65f * onset;  // minimal smoothing
  onsetAvg = 0.995f * onsetAvg + 0.005f * onsetEnv;

  float thresh = onsetAvg * 1.4f + 50.0f;
  bool energyOk = (level > 120 && peak > 600);
  bool timeOk   = (now - lastBeatMs) > 230;

  bool beat = false;
  if (energyOk && onsetEnv > thresh && timeOk && !beatLatch) {
    beat = true;
    if (lastBeatMs > 0) pushInterval(now - lastBeatMs);
    lastBeatMs = now;
    beatLatch = true;
  }

  if (onsetEnv < thresh * 0.55f) beatLatch = false;  // faster release

  return beat;
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========== Vibe Bot Sensor ==========");

  setupI2S();
  setupEspNow();
}

// ===================== LOOP =====================
void loop() {
  size_t bytesRead = 0;
  i2s_read(I2S_NUM_0, audioBuffer, sizeof(audioBuffer), &bytesRead, portMAX_DELAY);

  int samples = bytesRead / 4;
  if (samples <= 0) return;

  long sum = 0;
  int peak = 0;
  for (int i = 0; i < samples; i++) {
    int v = abs(audioBuffer[i] >> 14);
    sum += v;
    if (v > peak) peak = v;
  }

  int level    = sum / samples;
  int activity = (int)(0.65f * level + 0.35f * peak);

  // Track any audio energy (for breakdown vs true silence)
  bool hasAudio = (peak > 200 || level > 50);
  if (hasAudio) lastAudioMs = millis();

  // Music detection: high thresholds to reject speech
  bool musicNow = (activity > 350 && peak > 800);
  if (musicNow) lastMusicMs = millis();

  bool beatNow = false;
  if (musicNow) {
    beatNow = detectBeat(peak, level);

    if (beatNow) consecutiveBeats++;

    // Only compute BPM after enough consistent beats
    if (consecutiveBeats >= MIN_BEATS_TO_LOCK) {
      int candidate = computeBpm();
      if (candidate > 0) {

        if (!bpmIsLocked) {
          // First lock
          bpmLocked = candidate;
          bpmSmoothed = candidate;
          bpmIsLocked = true;
          bpmLockTime = millis();
          relockCount = 0;
          Serial.printf("[SENSOR] BPM LOCKED at %d\n", bpmLocked);

        } else {
          // Already locked: only accept candidates within ±12% of lock
          int tolerance = bpmLocked * 12 / 100;
          if (abs(candidate - bpmLocked) <= tolerance) {
            // Good candidate, gently smooth
            bpmSmoothed = (int)(0.85f * bpmSmoothed + 0.15f * candidate);
            relockCount = 0;
          } else {
            // Outlier: might be half-time, double-time, or wrong detection
            relockCount++;
            if (relockCount >= RELOCK_THRESHOLD) {
              // Sustained different tempo: allow re-lock
              bpmLocked = candidate;
              bpmSmoothed = candidate;
              bpmLockTime = millis();
              relockCount = 0;
              Serial.printf("[SENSOR] BPM RE-LOCKED at %d\n", bpmLocked);
            }
          }
        }
      }
    }
  }

  // Silence handling: distinguish breakdown vs true silence
  unsigned long silenceMs = millis() - lastMusicMs;
  unsigned long audioGoneMs = millis() - lastAudioMs;

  if (silenceMs > 600) {
    beatNow = false;

    // Breakdown (still audio, no beats): HOLD the locked BPM for up to 6 seconds
    if (hasAudio && bpmIsLocked && silenceMs < 6000) {
      // Keep bpmSmoothed as-is, don't decay
      activity = (int)(0.65f * level + 0.35f * peak);
    } else {
      // No audio or extended silence: decay
      bpmSmoothed -= 15;
      if (bpmSmoothed < 0) bpmSmoothed = 0;
      activity = 0;
    }

    // True silence: full reset
    if (audioGoneMs > 2000 || silenceMs > 8000) {
      clearBeatHistory();
      envFast = envSlow = onsetEnv = onsetAvg = 0.0f;
      lastBeatMs = 0;
      consecutiveBeats = 0;
      bpmSmoothed = 0;
      bpmLocked = 0;
      bpmIsLocked = false;
      relockCount = 0;
    }
  }

  bpmSmoothed = clamp(bpmSmoothed, 0, 230);

  packet.bpm      = bpmSmoothed;
  packet.peak     = peak;
  packet.level    = level;
  packet.beat     = beatNow ? 1 : 0;
  packet.activity = activity;

  esp_now_send(displayMAC, (uint8_t *)&packet, sizeof(packet));

  Serial.printf("[SENSOR] BPM=%3d Lk=%3d Pk=%4d Act=%3d Bt=%d Sil=%lu Locked=%d\n",
                packet.bpm, bpmLocked, packet.peak,
                packet.activity, packet.beat, silenceMs, bpmIsLocked);

  delay(12);
}
