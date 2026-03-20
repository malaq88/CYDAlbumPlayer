// ============================================================
// CYD Album Player (by folders) - Functional sketch
// ============================================================

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>

#include "BluetoothA2DPSource.h"

// ── Hardware pins ───────────────────────────────────────────
#define SD_CS      5
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TFT_BL     21

// RGB traseiro da CYD (ESP32-2432S028R): R=4, G=16, B=17 — ativo em LOW (LOW = aceso).
// Alguns clones podem variar; o LED “da frente” em muitos modelos é só o reflexo/chassi, não outro GPIO.
#define RGB_LED_RED    4
#define RGB_LED_GREEN 16
#define RGB_LED_BLUE   17

#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

// ── Touch calibration (adjust if needed) ───────────────────
#define TS_MINX 300
#define TS_MAXX 3900
#define TS_MINY 300
#define TS_MAXY 3900

SPIClass touchSPI(HSPI);
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ── Cores ───────────────────────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_TEXT     TFT_WHITE
#define COL_DIM      tft.color565(120, 120, 120)
#define COL_ACCENT   tft.color565(50, 205, 50)
#define COL_BTN      tft.color565(30, 30, 30)
#define COL_BTN_ACT  tft.color565(70, 70, 70)
#define COL_DIR      tft.color565(35, 45, 70)

// Dimensoes em retrato (240x320)
static const int SCR_W = 240;
static const int SCR_H = 320;

// Player screen (estilo DAP) - layout retrato
static const int PL_HEADER_H       = 38;
static const int PL_TITLE_Y        = 40;  // título da faixa (abaixo da barra)
static const int PL_CASSETTE_TOP   = 52;  // topo do painel do visualizador (barras)
static const int PL_PROGRESS_TOP   = 164;
static const int PL_PROGRESS_H     = 56;
static const int PL_VOLUME_Y       = 224;
static const int PL_TRANSPORT_Y    = 262;
// Botão lista (topo): volta ao browser; área de toque explícita
static const int PL_BACK_BTN_X     = 166;
static const int PL_BACK_BTN_Y     = 4;
static const int PL_BACK_BTN_W     = 70;
static const int PL_BACK_BTN_H     = 30;

static inline uint16_t colReelRed()   { return tft.color565(215, 45, 50); }
static inline uint16_t colTapeBody()  { return tft.color565(42, 42, 46); }
static inline uint16_t colTapeEdge()  { return tft.color565(75, 75, 80); }
static inline uint16_t colInfoCyan()  { return tft.color565(88, 190, 245); }
static inline uint16_t colTopBarBg()  { return tft.color565(10, 10, 12); }

static unsigned long lastTouchTime = 0;
static const unsigned long TOUCH_DEBOUNCE_MS = 220;

// ── Bluetooth (fixed headset) ─────────────────────────────
static const char* BT_SPEAKER_NAME = "E6";
BluetoothA2DPSource a2dp;
static bool btConnected = false;

// ── Ring buffer for A2DP audio ────────────────────────────
#define RING_SIZE 4096
static int16_t ring[RING_SIZE * 2];
static volatile size_t rbHead = 0;
static volatile size_t rbTail = 0;

static inline size_t rbAvail() {
  size_t h = rbHead, t = rbTail;
  return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}

// Amostras mono para visualizador (Goertzel por banda)
#define VIS_BUF_LEN 512
#define VIS_N       256
#define NUM_VIS_BARS 16
static int16_t visRing[VIS_BUF_LEN];
static volatile uint32_t visWritePos = 0;
static float visBandVal[NUM_VIS_BARS];
/** Envelope lento por banda (AGC independente — evita que graves dominem e matem as agudas). */
static float visBandEnv[NUM_VIS_BARS];

static const float VIS_FREQ_HZ[NUM_VIS_BARS] = {
  80.f, 125.f, 200.f, 320.f, 500.f, 800.f, 1250.f, 2000.f,
  3000.f, 4500.f, 6000.f, 8000.f, 10000.f, 12000.f, 15000.f, 18000.f
};

static int32_t btCallback(Frame *frame, int32_t count) {
  int32_t avail = (int32_t)rbAvail();
  int32_t toSend = min(count, avail);
  for (int32_t i = 0; i < toSend; i++) {
    frame[i].channel1 = ring[rbTail * 2];
    frame[i].channel2 = ring[rbTail * 2 + 1];
    rbTail = (rbTail + 1) % RING_SIZE;
  }
  for (int32_t i = toSend; i < count; i++) {
    frame[i].channel1 = 0;
    frame[i].channel2 = 0;
  }
  return count;
}

class RingBufOutput : public AudioOutput {
public:
  float gain = 0.7f;
  bool begin() override { return true; }
  bool stop()  override { return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    size_t next = (rbHead + 1) % RING_SIZE;
    if (next == rbTail) return false;
    int32_t l = (int32_t)(sample[0] * gain);
    int32_t r = (int32_t)(sample[1] * gain);
    ring[rbHead * 2]     = constrain(l, -32768, 32767);
    ring[rbHead * 2 + 1] = constrain(r, -32768, 32767);
    rbHead = next;
    visRing[visWritePos & (VIS_BUF_LEN - 1)] = (int16_t)((l + r) >> 1);
    visWritePos++;
    return true;
  }
};

static RingBufOutput* audioOut = nullptr;
static AudioFileSourceSD* audioFile = nullptr;
static AudioGeneratorMP3* mp3 = nullptr;
static AudioGeneratorWAV* wav = nullptr;

enum AudioType { AUDIO_NONE, AUDIO_MP3, AUDIO_WAV };
static AudioType currentType = AUDIO_NONE;

enum PlayerState { STATE_STOPPED, STATE_PLAYING, STATE_PAUSED };
static PlayerState playerState = STATE_STOPPED;

// ── LED RGB traseiro (estado Bluetooth / reprodução) ─────
static unsigned long rgbLastMs = 0;
static bool rgbBlinkPhase = false;
static bool rgbPrevBt = false;
static bool rgbPrevPlaying = false;

static inline void rgbLedAllOff() {
  digitalWrite(RGB_LED_RED, HIGH);
  digitalWrite(RGB_LED_GREEN, HIGH);
  digitalWrite(RGB_LED_BLUE, HIGH);
}

static void rgbLedInit() {
  pinMode(RGB_LED_RED, OUTPUT);
  pinMode(RGB_LED_GREEN, OUTPUT);
  pinMode(RGB_LED_BLUE, OUTPUT);
  rgbLedAllOff();
}

/** Sem fone: pisca vermelho/azul (tentando conectar). Conectado + tocando: pisca azul. Conectado parado: apagado. */
static void rgbLedUpdate() {
  unsigned long now = millis();
  const bool bt = a2dp.is_connected();
  const bool playing = (playerState == STATE_PLAYING);

  if (bt != rgbPrevBt || playing != rgbPrevPlaying) {
    rgbPrevBt = bt;
    rgbPrevPlaying = playing;
    rgbLastMs = now;
    rgbBlinkPhase = false;
    rgbLedAllOff();
  }

  if (bt && playing) {
    if (now - rgbLastMs >= 450) {
      rgbLastMs = now;
      rgbBlinkPhase = !rgbBlinkPhase;
    }
    digitalWrite(RGB_LED_RED, HIGH);
    digitalWrite(RGB_LED_GREEN, HIGH);
    digitalWrite(RGB_LED_BLUE, rgbBlinkPhase ? LOW : HIGH);
  } else if (!bt) {
    if (now - rgbLastMs >= 280) {
      rgbLastMs = now;
      rgbBlinkPhase = !rgbBlinkPhase;
    }
    digitalWrite(RGB_LED_GREEN, HIGH);
    if (rgbBlinkPhase) {
      digitalWrite(RGB_LED_RED, LOW);
      digitalWrite(RGB_LED_BLUE, HIGH);
    } else {
      digitalWrite(RGB_LED_RED, HIGH);
      digitalWrite(RGB_LED_BLUE, LOW);
    }
  } else {
    rgbLedAllOff();
  }
}

// ── Albums / tracks ────────────────────────────────────────
#define MAX_ALBUMS 32
#define MAX_TRACKS 128
#define MAX_ALBUM_NAME_LEN 48

static char albums[MAX_ALBUMS][MAX_ALBUM_NAME_LEN];
static int albumCount = 0;
static int albumScroll = 0;

static char currentAlbumFolder[MAX_ALBUM_NAME_LEN];

static char albumTracks[MAX_TRACKS][128]; // full paths
static int albumTrackCount = 0;
static int currentTrack = 0;

// ── UI ─────────────────────────────────────────────────────
enum ScreenMode { SCREEN_BROWSER, SCREEN_PLAYER };
static ScreenMode screenMode = SCREEN_BROWSER;

static const int browseHeaderH = 30;
static const int browsePathY = 34;
static const int browseListY = 50;
static const int browseFooterH = 36;
static const int browseItemH = 24;

static inline int footerY() { return SCR_H - browseFooterH; }
static inline int visibleSlots() {
  int vis = (footerY() - browseListY) / browseItemH;
  return max(1, vis);
}

// ── file helpers ────────────────────────────────────────────
static bool isMP3(const char* fn) {
  int l = strlen(fn);
  return l >= 4 && strcasecmp(fn + l - 4, ".mp3") == 0;
}
static bool isWAV(const char* fn) {
  int l = strlen(fn);
  return l >= 4 && strcasecmp(fn + l - 4, ".wav") == 0;
}

static void getDisplayName(const char* path, char* out, int maxLen) {
  const char* name = strrchr(path, '/');
  if (!name) name = path;
  strncpy(out, name, maxLen - 1);
  out[maxLen - 1] = '\0';
  char* dot = strrchr(out, '.');
  if (dot) *dot = '\0';
}

/** Ordenação alfabética pelo caminho completo (sem ler metadados ID3). */
static void sortAlbumTracksByPath() {
  if (albumTrackCount < 2) return;
  for (int i = 0; i < albumTrackCount - 1; i++) {
    for (int j = 0; j < albumTrackCount - 1 - i; j++) {
      if (strcmp(albumTracks[j], albumTracks[j + 1]) > 0) {
        char tmp[128];
        memcpy(tmp, albumTracks[j], sizeof(tmp));
        memcpy(albumTracks[j], albumTracks[j + 1], sizeof(albumTracks[j]));
        memcpy(albumTracks[j + 1], tmp, sizeof(albumTracks[j + 1]));
      }
    }
  }
}

/** Duração WAV a partir do cartão (chunks fmt/data); *outRate = sample rate Hz se não for NULL. */
static uint32_t wavParseDurationAndRate(const char* path, uint32_t* outRate) {
  if (outRate) *outRate = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  uint8_t riff[12];
  if (f.read(riff, 12) < 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
    f.close();
    return 0;
  }
  uint16_t numCh = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0, dataSize = 0;
  bool haveData = false;

  while (f.available()) {
    uint8_t cid[4], szb[4];
    if (f.read(cid, 4) < 4 || f.read(szb, 4) < 4) break;
    uint32_t chunkSize = (uint32_t)szb[0] | ((uint32_t)szb[1] << 8) | ((uint32_t)szb[2] << 16) | ((uint32_t)szb[3] << 24);

    if (memcmp(cid, "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        f.seek(chunkSize + (chunkSize & 1u), SeekCur);
        continue;
      }
      uint8_t fmt[16];
      if (f.read(fmt, 16) < 16) break;
      numCh = (uint16_t)(fmt[2] | (fmt[3] << 8));
      sampleRate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      bitsPerSample = (uint16_t)(fmt[14] | (fmt[15] << 8));
      uint32_t skip = chunkSize - 16;
      if (skip > 0) f.seek(skip, SeekCur);
      if (chunkSize & 1u) f.seek(1, SeekCur);
    } else if (memcmp(cid, "data", 4) == 0) {
      dataSize = chunkSize;
      haveData = true;
      break;
    } else {
      f.seek(chunkSize + (chunkSize & 1u), SeekCur);
    }
  }
  f.close();
  if (outRate) *outRate = sampleRate;
  uint32_t bps = sampleRate * (uint32_t)numCh * ((uint32_t)bitsPerSample / 8u);
  if (bps == 0 || !haveData) return 0;
  return dataSize / bps;
}

static void formatTimeHMS(char* buf, size_t n, uint32_t sec) {
  uint32_t h = sec / 3600u;
  uint32_t m = (sec % 3600u) / 60u;
  uint32_t s = sec % 60u;
  snprintf(buf, n, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

/** Uma linha centrada (texto 1); corta com ".." se não couber em maxPx. */
static void drawTitleCentered(int y, int maxPx, const char* s) {
  char buf[52];
  strncpy(buf, s ? s : "", sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  const int charW = 6;
  int maxChars = maxPx / charW - 2;
  if (maxChars < 6) maxChars = 6;
  int len = (int)strlen(buf);
  if (len > maxChars) {
    buf[maxChars - 2] = '\0';
    strcat(buf, "..");
  }
  tft.setTextSize(1);
  int w = (int)strlen(buf) * charW;
  tft.setCursor((SCR_W / 2) - w / 2, y);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.print(buf);
}

// ── Playback timeline (relógio pausa/resume coerente) ───────
static unsigned long trackWallStartMs   = 0;
static unsigned long accumulatedPauseMs = 0;
static unsigned long pauseBeganMs       = 0;
static uint32_t      cachedDurationSec  = 0;
static uint32_t      cachedWavRateHz    = 0;
static unsigned long lastProgressUiMs   = 0;
static int           volumePercent      = 30;

static void applyVolumePercent() {
  volumePercent = constrain(volumePercent, 0, 100);
  if (audioOut) audioOut->gain = (float)volumePercent / 100.0f;
  a2dp.set_volume((int)((volumePercent * 127) / 100));
}

static uint32_t clampElapsedSec(uint32_t el) {
  if (cachedDurationSec > 0 && el > cachedDurationSec) return cachedDurationSec;
  return el;
}

static uint32_t elapsedPlaybackSec() {
  if (playerState == STATE_STOPPED || trackWallStartMs == 0) return 0;
  if (playerState == STATE_PAUSED && pauseBeganMs >= trackWallStartMs) {
    return clampElapsedSec((uint32_t)((pauseBeganMs - trackWallStartMs - accumulatedPauseMs) / 1000ul));
  }
  if (playerState == STATE_PLAYING) {
    return clampElapsedSec((uint32_t)((millis() - trackWallStartMs - accumulatedPauseMs) / 1000ul));
  }
  return 0;
}

// ── SD: scan albums (folders in root) ───────────────────────
static void scanAlbums() {
  albumCount = 0;
  File root = SD.open("/");
  if (!root || !root.isDirectory()) return;

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      if (albumCount < MAX_ALBUMS) {
        const char* nm = entry.name();
        const char* slash = strrchr(nm, '/');
        nm = slash ? slash + 1 : nm;
        // Some SD cards create this folder automatically; ignore.
        if (strcmp(nm, "System Volume Information") == 0) {
          entry.close();
          continue;
        }
        strncpy(albums[albumCount], nm, MAX_ALBUM_NAME_LEN - 1);
        albums[albumCount][MAX_ALBUM_NAME_LEN - 1] = '\0';
        albumCount++;
      }
    }
    entry.close();
  }
  root.close();
}

static void loadAlbumTracks(const char* albumName) {
  albumTrackCount = 0;
  if (!albumName || !albumName[0]) return;

  strncpy(currentAlbumFolder, albumName, sizeof(currentAlbumFolder) - 1);
  currentAlbumFolder[sizeof(currentAlbumFolder) - 1] = '\0';

  char dirPath[96];
  snprintf(dirPath, sizeof(dirPath), "/%s", albumName);

  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory() && albumTrackCount < MAX_TRACKS) {
      const char* nm = entry.name();
      const char* slash = strrchr(nm, '/');
      nm = slash ? slash + 1 : nm;
      if (isMP3(nm) || isWAV(nm)) {
        char full[128];
        snprintf(full, sizeof(full), "%s/%s", dirPath, nm);
        strncpy(albumTracks[albumTrackCount], full, sizeof(albumTracks[albumTrackCount]) - 1);
        albumTracks[albumTrackCount][sizeof(albumTracks[albumTrackCount]) - 1] = '\0';
        albumTrackCount++;
      }
    }
    entry.close();
  }
  dir.close();

  sortAlbumTracksByPath();
}

// ── Audio: stop/start/next/prev ────────────────────────────
static void stopTrack() {
  if (mp3 && mp3->isRunning()) mp3->stop();
  if (wav && wav->isRunning()) wav->stop();
  if (mp3) { delete mp3; mp3 = nullptr; }
  if (wav) { delete wav; wav = nullptr; }
  if (audioFile) { delete audioFile; audioFile = nullptr; }
  currentType = AUDIO_NONE;
  playerState = STATE_STOPPED;
  rbHead = 0;
  rbTail = 0;
  trackWallStartMs = 0;
  accumulatedPauseMs = 0;
  pauseBeganMs = 0;
  cachedDurationSec = 0;
  cachedWavRateHz = 0;
}

static void startTrack(int idx) {
  if (albumTrackCount <= 0) return;
  if (idx < 0) idx = albumTrackCount - 1;
  if (idx >= albumTrackCount) idx = 0;
  currentTrack = idx;

  stopTrack();

  audioFile = new AudioFileSourceSD(albumTracks[currentTrack]);
  if (!audioFile) return;

  if (isWAV(albumTracks[currentTrack])) {
    wav = new AudioGeneratorWAV();
    wav->begin(audioFile, audioOut);
    currentType = AUDIO_WAV;
  } else {
    mp3 = new AudioGeneratorMP3();
    mp3->begin(audioFile, audioOut);
    currentType = AUDIO_MP3;
  }

  playerState = STATE_PLAYING;
  trackWallStartMs = millis();
  accumulatedPauseMs = 0;
  pauseBeganMs = 0;
  cachedWavRateHz = 0;
  if (currentType == AUDIO_WAV) {
    cachedDurationSec = wavParseDurationAndRate(albumTracks[currentTrack], &cachedWavRateHz);
  } else if (audioFile && currentType == AUDIO_MP3) {
    uint32_t sz = audioFile->getSize();
    cachedDurationSec = (sz > 0) ? (sz / 16000u) : 0;
    if (cachedDurationSec == 0 && sz > 8000u) cachedDurationSec = 1;
  } else {
    cachedDurationSec = 0;
  }

  // Pre-fill for BT stability
  int loops = 0;
  int target = (RING_SIZE * 3) / 4;
  while ((int)rbAvail() < target && loops < 1024) {
    bool ok = false;
    if (currentType == AUDIO_MP3 && mp3 && mp3->isRunning()) ok = mp3->loop();
    else if (currentType == AUDIO_WAV && wav && wav->isRunning()) ok = wav->loop();
    if (!ok) break;
    loops++;
  }
}

static void nextTrack() { startTrack(currentTrack + 1); }
static void prevTrack() { startTrack(currentTrack - 1); }
static void togglePause() {
  if (playerState == STATE_PLAYING) {
    playerState = STATE_PAUSED;
    pauseBeganMs = millis();
  } else if (playerState == STATE_PAUSED) {
    if (pauseBeganMs) {
      accumulatedPauseMs += (millis() - pauseBeganMs);
      pauseBeganMs = 0;
    }
    playerState = STATE_PLAYING;
  }
}

// ── Visualizador (Goertzel por banda; amostras em visRing no ConsumeSample) ──
static float visGoertzelMag(const float* x, int N, float freqHz, float sampleRateHz) {
  const float PI_F = 3.14159265f;
  if (freqHz <= 0 || freqHz >= sampleRateHz * 0.48f) return 0;
  float k = (0.5f + ((float)N * freqHz) / sampleRateHz);
  int ki = (int)k;
  if (ki < 1) ki = 1;
  if (ki >= N) ki = N - 1;
  float omega = (2.0f * PI_F * (float)ki) / (float)N;
  float sine = sinf(omega);
  float cosine = cosf(omega);
  float coeff = 2.0f * cosine;
  float q0 = 0, q1 = 0, q2 = 0;
  for (int i = 0; i < N; i++) {
    q0 = coeff * q1 - q2 + x[i];
    q2 = q1;
    q1 = q0;
  }
  float real = q1 - q2 * cosine;
  float imag = q2 * sine;
  return sqrtf(real * real + imag * imag) / (float)N;
}

static float visSampleRateHz() {
  if (currentType == AUDIO_WAV && cachedWavRateHz > 0) return (float)cachedWavRateHz;
  return 44100.0f;
}

static void computeVisBands() {
  if (visWritePos < (uint32_t)VIS_N) return;
  float wf[VIS_N];
  uint32_t end = visWritePos;
  for (int i = 0; i < VIS_N; i++) {
    uint32_t idx = (end - VIS_N + i) & (VIS_BUF_LEN - 1);
    float s = (float)visRing[idx];
    float w = 0.54f - 0.46f * cosf(2.0f * 3.14159265f * i / (float)(VIS_N - 1));
    wf[i] = s * w;
  }
  float sr = visSampleRateHz();
  float raw[NUM_VIS_BARS];
  for (int b = 0; b < NUM_VIS_BARS; b++) {
    float fh = VIS_FREQ_HZ[b];
    if (fh >= sr * 0.45f) fh = sr * 0.45f;
    raw[b] = visGoertzelMag(wf, VIS_N, fh, sr);
  }

  // Boost nas agudas (Goertzel em janela curta tem menos energia nas altas) + AGC por banda.
  for (int b = 0; b < NUM_VIS_BARS; b++) {
    float treble = 0.55f + (float)b * 0.32f; // ~0.55 … ~5.5
    float scaled = raw[b] * treble;
    visBandEnv[b] = visBandEnv[b] * 0.90f + scaled * 0.10f;
    if (visBandEnv[b] < 1e-7f) visBandEnv[b] = 1e-7f;
    // Divisor baixo = mais sensível; curva suave (não t²) preserva picos médios
    float t = scaled / (visBandEnv[b] * 1.35f + 1e-6f);
    t = powf(t, 0.55f);
    if (t > 1.0f) t = 1.0f;
    if (t > visBandVal[b]) visBandVal[b] = visBandVal[b] * 0.28f + t * 0.72f;
    else visBandVal[b] = visBandVal[b] * 0.72f + t * 0.28f;
  }
}

static const int VIS_PX = 8;
static const int VIS_PY = PL_CASSETTE_TOP;
static const int VIS_PW = 224;
static const int VIS_PH = 108;
static const int VIS_IX = VIS_PX + 10;
static const int VIS_IY = VIS_PY + 22;
static const int VIS_IW = VIS_PW - 20;
static const int VIS_IH = VIS_PH - 34;

static uint16_t visBarColor(int b, float hNorm) {
  float t = (float)b / (float)((NUM_VIS_BARS > 1) ? (NUM_VIS_BARS - 1) : 1);
  uint8_t r = (uint8_t)(20.f + t * 120.f + hNorm * 80.f);
  uint8_t g = (uint8_t)(200.f - t * 140.f + hNorm * 40.f);
  uint8_t bl = (uint8_t)(160.f + (1.0f - t) * 60.f + hNorm * 30.f);
  return tft.color565(r, g, bl);
}

static void drawVisualizerBarsContent() {
  const int gap = 2;
  int barW = (VIS_IW - gap * (NUM_VIS_BARS - 1) - 4) / NUM_VIS_BARS;
  if (barW < 2) barW = 2;
  const int maxH = VIS_IH - 8;
  const int baseY = VIS_IY + VIS_IH - 4;
  for (int b = 0; b < NUM_VIS_BARS; b++) {
    int x = VIS_IX + 2 + b * (barW + gap);
    float h = visBandVal[b];
    int hh = (int)(h * (float)maxH);
    if (hh > maxH) hh = maxH;
    if (hh < 1) continue;
    tft.fillRoundRect(x, baseY - hh, barW, hh, 2, visBarColor(b, h));
  }
}

static void redrawVisualizerBarsOnly() {
  tft.fillRoundRect(VIS_IX, VIS_IY, VIS_IW, VIS_IH, 6, tft.color565(4, 4, 8));
  drawVisualizerBarsContent();
}

static void drawVisualizerPanel() {
  tft.fillRoundRect(VIS_PX, VIS_PY, VIS_PW, VIS_PH, 11, colTapeEdge());
  tft.fillRoundRect(VIS_PX + 4, VIS_PY + 4, VIS_PW - 8, VIS_PH - 8, 8, colTapeBody());
  tft.fillRoundRect(VIS_PX + 36, VIS_PY + 10, VIS_PW - 72, 12, 4, tft.color565(28, 28, 32));
  tft.drawRoundRect(VIS_PX + 36, VIS_PY + 10, VIS_PW - 72, 12, 4, tft.color565(50, 50, 56));
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(), tft.color565(28, 28, 32));
  tft.setCursor(VIS_PX + 48, VIS_PY + 14);
  tft.print("SPECTRUM");
  redrawVisualizerBarsOnly();
}

static unsigned long lastVisAnimMs = 0;

static void updateVisualizerAnimation() {
  if (screenMode != SCREEN_PLAYER) return;
  unsigned long now = millis();
  if (now - lastVisAnimMs < 50) return;
  lastVisAnimMs = now;

  if (playerState != STATE_PLAYING) {
    for (int i = 0; i < NUM_VIS_BARS; i++) {
      visBandVal[i] *= 0.82f;
      visBandEnv[i] *= 0.88f;
    }
  } else {
    computeVisBands();
  }
  redrawVisualizerBarsOnly();
}

// ── Render ─────────────────────────────────────────────────
static void drawBrowser() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, SCR_W, browseHeaderH, COL_BTN);

  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(1);
  tft.setCursor(10, 4);
  tft.print("Albums");

  // BT status: icon + headset name
  uint16_t btCol = btConnected ? COL_ACCENT : TFT_RED;
  tft.setTextSize(1);
  // "BT" badge
  tft.fillRoundRect(150, 7, 26, 14, 4, btCol);
  tft.setTextColor(COL_BG, btCol);
  tft.setCursor(157, 10);
  tft.print("BT");

  tft.setTextColor(btCol, COL_BTN);
  tft.setCursor(180, 10);
  if (btConnected) {
    char bname[10];
    strncpy(bname, BT_SPEAKER_NAME, sizeof(bname) - 1);
    bname[sizeof(bname) - 1] = '\0';
    tft.print(bname);
  }
  else              tft.print("BT..");

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(10, browsePathY);
  tft.print("Touch an album");

  int fY = footerY();
  int vis = visibleSlots();

  tft.fillRoundRect(8, fY + 6, 58, 24, 5, COL_BTN);
  tft.fillRoundRect(174, fY + 6, 58, 24, 5, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(20, fY + 15);
  tft.print("PREV");
  tft.setCursor(186, fY + 15);
  tft.print("NEXT");

  int totalPages = (albumCount + vis - 1) / vis;
  if (totalPages < 1) totalPages = 1;
  int page = (albumScroll / vis) + 1;
  if (page > totalPages) page = totalPages;
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(104, fY + 15);
  tft.printf("%d/%d", page, totalPages);

  for (int i = 0; i < vis; i++) {
    int idx = albumScroll + i;
    if (idx >= albumCount) break;
    int y = browseListY + i * browseItemH;
    tft.fillRoundRect(6, y + 1, SCR_W - 12, browseItemH - 3, 5, COL_DIR);
    tft.setTextColor(COL_TEXT, COL_DIR);
    tft.setTextSize(1);
    tft.setCursor(12, y + 7);
    tft.print(albums[idx]);
  }
}

// ── Splash / tela inicial ─────────────────────────────────
// Opcional: imagem perfeita a partir do SD — ficheiro RAW RGB565 little-endian, exatamente SP_RAW_W*SP_RAW_H*2 bytes.
static const char* const SPLASH_RAW_PATH = "/guara565.raw";
static const int SP_RAW_W = 200;
static const int SP_RAW_H = 218;

static inline void splashDrawPlus(int x, int y, uint16_t c) {
  tft.drawFastVLine(x, y - 2, 5, c);
  tft.drawFastHLine(x - 2, y, 5, c);
}

/** Tenta desenhar o logo a partir do cartão (qualidade máxima). */
static bool tryDrawSplashFromSD(int dstX, int dstY) {
  File f = SD.open(SPLASH_RAW_PATH, FILE_READ);
  if (!f) return false;
  long need = (long)SP_RAW_W * (long)SP_RAW_H * 2L;
  if (f.size() != need) {
    f.close();
    return false;
  }
  uint16_t line[SP_RAW_W];
  for (int y = 0; y < SP_RAW_H; y++) {
    if (f.read((uint8_t*)line, SP_RAW_W * 2) != (size_t)(SP_RAW_W * 2)) {
      f.close();
      return false;
    }
    tft.pushImage(dstX, dstY + y, SP_RAW_W, 1, line);
  }
  f.close();
  return true;
}

/** Logo GUARA CREW desenhada (fallback) — estilo monocromático / pixel-art. */
static void drawGuaraLogoProcedural(int ix, int iy, int iw, int ih) {
  tft.fillRect(ix, iy, iw, ih, COL_BG);

  const int cx = ix + iw / 2;
  uint16_t furHi = tft.color565(210, 210, 218);
  uint16_t furMid = tft.color565(165, 165, 175);
  uint16_t furLo = tft.color565(118, 118, 128);
  uint16_t sunHi = tft.color565(62, 62, 70);
  uint16_t sunLo = tft.color565(44, 44, 52);
  uint16_t accPurp = tft.color565(170, 80, 240);

  // “Sol” em meia-lua com scanlines (cordas horizontais)
  {
    const int syc = iy + 72;
    const int R = 58;
    for (int dy = -R; dy <= 0; dy++) {
      int rr = R * R - dy * dy;
      if (rr < 0) continue;
      int ww = (int)sqrtf((float)rr);
      uint16_t c = ((dy + R) & 2) ? sunHi : sunLo;
      tft.drawFastHLine(cx - ww, syc + dy, 2 * ww + 1, c);
    }
  }

  // Orelhas altas + pelagem
  tft.fillTriangle(cx - 54, iy + 118, cx - 26, iy + 52, cx - 10, iy + 108, furHi);
  tft.fillTriangle(cx + 54, iy + 118, cx + 26, iy + 52, cx + 10, iy + 108, furHi);
  tft.fillTriangle(cx - 50, iy + 122, cx + 50, iy + 122, cx, iy + 198, furHi);

  // Sombra lateral (volume do focinho)
  tft.fillTriangle(cx - 46, iy + 128, cx - 8, iy + 128, cx - 28, iy + 188, furMid);
  tft.fillTriangle(cx + 46, iy + 128, cx + 8, iy + 128, cx + 28, iy + 188, furMid);

  // Máscara escura em volta dos olhos
  tft.fillCircle(cx - 22, iy + 118, 10, furLo);
  tft.fillCircle(cx + 22, iy + 118, 10, furLo);

  // Focinho claro
  tft.fillTriangle(cx - 18, iy + 138, cx + 18, iy + 138, cx, iy + 186, furMid);
  tft.fillTriangle(cx - 12, iy + 144, cx + 12, iy + 144, cx, iy + 178, furHi);

  // Olhos
  tft.fillCircle(cx - 22, iy + 116, 4, COL_TEXT);
  tft.fillCircle(cx + 22, iy + 116, 4, COL_TEXT);
  tft.fillCircle(cx - 23, iy + 115, 2, COL_BG);
  tft.fillCircle(cx + 21, iy + 115, 2, COL_BG);

  // Nariz
  tft.fillTriangle(cx - 5, iy + 158, cx + 5, iy + 158, cx, iy + 168, COL_BG);

  // Bigodes / detalhe pixel
  for (int k = 0; k < 4; k++) {
    tft.drawFastHLine(cx - 34 - k * 3, iy + 152 + k, 10, furLo);
    tft.drawFastHLine(cx + 24 + k * 3, iy + 152 + k, 10, furLo);
  }

  // Cruzes decorativas (estética “glitch” suave)
  uint16_t xcol = tft.color565(90, 90, 98);
  splashDrawPlus(ix + 8, iy + 56, xcol);
  splashDrawPlus(ix + 14, iy + 92, COL_DIM);
  splashDrawPlus(ix + iw - 9, iy + 64, xcol);
  splashDrawPlus(ix + iw - 15, iy + 100, COL_DIM);
  splashDrawPlus(cx - 70, iy + 40, COL_DIM);
  splashDrawPlus(cx + 70, iy + 44, COL_DIM);

  // Escudo GUARA (trapézio simulado + sombra)
  {
    const int bx = ix + 14, by = iy + 168, bw = iw - 28, bh = 44;
    tft.fillTriangle(bx, by + bh, bx + 8, by, bx + bw - 8, by, furHi);
    tft.fillTriangle(bx, by + bh, bx + bw - 8, by, bx + bw, by + bh, furHi);
    tft.drawTriangle(bx, by + bh, bx + 8, by, bx + bw - 8, by, accPurp);
    tft.drawTriangle(bx, by + bh, bx + bw - 8, by, bx + bw, by + bh, accPurp);
    tft.setTextColor(COL_BG, furHi);
    tft.setTextSize(2);
    const char* g = "GUARA";
    int gw = (int)strlen(g) * 12;
    tft.setCursor(cx - gw / 2, by + 14);
    tft.print(g);
  }

  // Faixa CREW
  {
    const int bx2 = ix + 44, by2 = iy + 214, bw2 = iw - 88, bh2 = 28;
    tft.fillRoundRect(bx2, by2, bw2, bh2, 5, furHi);
    tft.drawRoundRect(bx2, by2, bw2, bh2, 5, accPurp);
    tft.setTextColor(COL_BG, furHi);
    tft.setTextSize(2);
    const char* c = "CREW";
    int cw = (int)strlen(c) * 12;
    tft.setCursor(cx - cw / 2, by2 + 8);
    tft.print(c);
  }
}

static void drawStartupScreen() {
  tft.fillScreen(COL_BG);

  tft.setTextSize(2);
  tft.setTextColor(COL_TEXT, COL_BG);
  const char* wel = "WELCOME";
  int welW = (int)strlen(wel) * 12;
  tft.setCursor((SCR_W - welW) / 2, 8);
  tft.print(wel);

  const int lx = 12, ly = 40, lw = 216, lh = 258;
  uint16_t frameOut = tft.color565(48, 48, 56);
  uint16_t frameIn = tft.color565(28, 28, 34);
  tft.drawRoundRect(lx, ly, lw, lh, 10, frameOut);
  tft.drawRoundRect(lx + 2, ly + 2, lw - 4, lh - 4, 8, frameIn);
  tft.drawFastHLine(lx + 16, ly + 6, lw - 32, tft.color565(170, 80, 240));

  const int innerX = lx + 6;
  const int innerY = ly + 6;
  const int innerW = lw - 12;
  const int innerH = lh - 12;

  // Posição centrada para o RAW fixo 200x218
  const int rawX = innerX + (innerW - SP_RAW_W) / 2;
  const int rawY = innerY + (innerH - SP_RAW_H) / 2;

  if (!tryDrawSplashFromSD(rawX, rawY))
    drawGuaraLogoProcedural(innerX, innerY, innerW, innerH);

  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(48, 306);
  tft.print("BT connecting...");
}

/** Barra de progresso, tempos e metadados técnicos (atualização periódica no loop). */
static void drawPlayerProgressArea() {
  if (screenMode != SCREEN_PLAYER) return;

  const int y0 = PL_PROGRESS_TOP;
  tft.fillRect(0, y0, SCR_W, PL_PROGRESS_H, COL_BG);

  uint32_t el = elapsedPlaybackSec();
  char tEl[16], tTot[16];
  formatTimeHMS(tEl, sizeof(tEl), el);
  if (cachedDurationSec > 0) formatTimeHMS(tTot, sizeof(tTot), cachedDurationSec);
  else {
    strncpy(tTot, "--:--:--", sizeof(tTot) - 1);
    tTot[sizeof(tTot) - 1] = '\0';
  }

  const int bx = 10, by = y0 + 2, bw = SCR_W - 20, bh = 6;
  tft.drawRoundRect(bx, by, bw, bh + 2, 2, COL_DIM);
  tft.fillRect(bx + 1, by + 1, bw - 2, bh, tft.color565(22, 22, 26));
  if (cachedDurationSec > 0) {
    uint32_t fw = (uint32_t)(((uint64_t)el * (uint64_t)(bw - 4)) / (uint64_t)cachedDurationSec);
    if (fw > (uint32_t)(bw - 4)) fw = (uint32_t)(bw - 4);
    if (fw > 0) tft.fillRect(bx + 2, by + 2, (int)fw, bh - 2, colReelRed());
  }

  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, COL_BG);
  if (playerState == STATE_PLAYING) {
    tft.fillTriangle(10, y0 + 18, 10, y0 + 24, 16, y0 + 21, COL_TEXT);
  } else {
    tft.fillRect(10, y0 + 17, 3, 8, COL_TEXT);
    tft.fillRect(14, y0 + 17, 3, 8, COL_TEXT);
  }
  tft.setCursor(22, y0 + 16);
  tft.print(tEl);
  tft.setCursor(SCR_W - 70, y0 + 16);
  tft.print(tTot);

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(10, y0 + 28);
  tft.print(currentAlbumFolder[0] ? currentAlbumFolder : "-");

  tft.setTextColor(colInfoCyan(), COL_BG);
  tft.setCursor(10, y0 + 40);
  if (currentType == AUDIO_WAV && cachedWavRateHz > 0)
    tft.printf("WAV / %lu Hz / PCM", (unsigned long)cachedWavRateHz);
  else if (currentType == AUDIO_MP3)
    tft.print("MP3 / ~128 kbps (est.)");
  else
    tft.print("---");
}

static void drawVolumeControls() {
  const int y = PL_VOLUME_Y;
  tft.fillRoundRect(10, y, 56, 30, 6, COL_BTN);
  tft.fillRoundRect(74, y, 92, 30, 6, COL_BTN);
  tft.fillRoundRect(174, y, 56, 30, 6, COL_BTN);

  uint16_t fg = COL_TEXT;
  const int cy = y + 15;

  // Ícone “−” (barra espessa, estilo transporte)
  {
    const int cx = 10 + 56 / 2;
    tft.fillRoundRect(cx - 10, cy - 3, 20, 6, 2, fg);
  }

  char vbuf[10];
  snprintf(vbuf, sizeof(vbuf), "%d%%", volumePercent);
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, COL_BTN);
  int tw = (int)strlen(vbuf) * 6;
  tft.setCursor((SCR_W / 2) - (tw / 2), y + 11);
  tft.print(vbuf);

  // Ícone “+” (cruz com traços espessos)
  {
    const int cx = 174 + 56 / 2;
    tft.fillRoundRect(cx - 2, cy - 10, 4, 20, 1, fg);
    tft.fillRoundRect(cx - 10, cy - 2, 20, 4, 2, fg);
  }
}

/** Ícone de lista (marcas + linhas) no botão de voltar às pastas. */
static void drawPlayerListBackIcon() {
  int bx = PL_BACK_BTN_X, by = PL_BACK_BTN_Y, bw = PL_BACK_BTN_W, bh = PL_BACK_BTN_H;
  tft.fillRoundRect(bx, by, bw, bh, 4, tft.color565(36, 36, 42));
  uint16_t fg = colInfoCyan();
  int cy = by + bh / 2;
  const int lineW = 30;
  const int rowGap = 7;
  int lx0 = bx + 14;
  for (int row = 0; row < 3; row++) {
    int ly = cy - 8 + row * rowGap;
    tft.fillRoundRect(lx0, ly + 1, 4, 4, 1, fg);
    tft.fillRoundRect(lx0 + 8, ly + 2, lineW, 2, 1, fg);
  }
}

static void drawPlayer() {
  tft.fillScreen(COL_BG);

  // ── Barra de estado (estilo DAP) ─────────────────────────
  tft.fillRect(0, 0, SCR_W, PL_HEADER_H, colTopBarBg());
  tft.drawFastHLine(0, PL_HEADER_H - 1, SCR_W, tft.color565(40, 40, 48));

  // Ícone de nota musical (roxo)
  uint16_t noteCol = tft.color565(170, 80, 240);
  tft.fillCircle(9, 17, 3, noteCol);
  tft.fillCircle(17, 15, 3, noteCol);
  tft.fillRect(11, 8, 3, 11, noteCol);
  tft.fillRect(19, 6, 3, 11, noteCol);
  tft.drawFastHLine(12, 6, 8, noteCol);

  tft.setTextColor(COL_TEXT, colTopBarBg());
  tft.setTextSize(2);
  tft.setCursor(22, 8);
  if (albumTrackCount > 0) tft.printf("%d/%d", currentTrack + 1, albumTrackCount);
  else tft.print("-/-");

  {
    char abuf[26];
    const char* src = currentAlbumFolder[0] ? currentAlbumFolder : "-";
    strncpy(abuf, src, sizeof(abuf) - 1);
    abuf[sizeof(abuf) - 1] = '\0';
    int mc = (int)sizeof(abuf) - 4;
    if ((int)strlen(abuf) > mc) {
      abuf[mc] = '\0';
      strcat(abuf, "..");
    }
    if ((int)strlen(abuf) > 14) {
      abuf[14] = '\0';
      strcat(abuf, "..");
    }
    tft.setTextSize(1);
    tft.setCursor(22, 26);
    tft.print(abuf);
  }

  uint16_t btCol = a2dp.is_connected() ? tft.color565(60, 200, 90) : tft.color565(200, 60, 60);
  tft.fillRoundRect(128, 6, 32, 16, 3, tft.color565(30, 30, 34));
  tft.setTextSize(1);
  tft.setTextColor(btCol, colTopBarBg());
  tft.setCursor(134, 10);
  tft.print("BT");

  drawPlayerListBackIcon();

  // ── Título da faixa ─────────────────────────────────────
  char title[64];
  if (albumTrackCount > 0) getDisplayName(albumTracks[currentTrack], title, sizeof(title));
  else strncpy(title, "Sem faixa", sizeof(title) - 1);
  title[sizeof(title) - 1] = '\0';
  tft.setTextSize(1);
  drawTitleCentered(PL_TITLE_Y, 220, title);

  // ── Visualizador (barras por banda de frequência) ───────
  drawVisualizerPanel();

  drawPlayerProgressArea();
  drawVolumeControls();
  lastProgressUiMs = millis();

  // ── Transporte ───────────────────────────────────────────
  const int y = PL_TRANSPORT_Y;
  tft.fillRoundRect(10, y, 56, 42, 7, COL_BTN);
  tft.fillRoundRect(74, y, 92, 42, 7, COL_BTN);
  tft.fillRoundRect(174, y, 56, 42, 7, COL_BTN);

  uint16_t fg = COL_TEXT;
  int cy = y + 42 / 2;

  {
    int cxPrev = 10 + 56 / 2;
    tft.fillRect(cxPrev - 18, cy - 14, 3, 28, fg);
    tft.fillTriangle(cxPrev - 2, cy, cxPrev - 2 + 11, cy - 11, cxPrev - 2 + 11, cy + 11, fg);
    tft.fillTriangle(cxPrev + 8, cy, cxPrev + 8 + 11, cy - 11, cxPrev + 8 + 11, cy + 11, fg);
  }
  {
    int cxPlay = 74 + 92 / 2;
    if (playerState == STATE_PLAYING) {
      int wBar = 7;
      int gap = 5;
      tft.fillRect(cxPlay - gap - wBar, cy - 15, wBar, 30, fg);
      tft.fillRect(cxPlay + gap, cy - 15, wBar, 30, fg);
    } else {
      int size = 22;
      tft.fillTriangle(cxPlay - size / 3, cy - size / 2,
                       cxPlay - size / 3, cy + size / 2,
                       cxPlay + size / 2, cy, fg);
    }
  }
  {
    int cxNext = 174 + 56 / 2;
    tft.fillTriangle(cxNext - 10 - 11, cy - 11, cxNext - 10 - 11, cy + 11, cxNext - 10, cy, fg);
    tft.fillTriangle(cxNext - 2 - 11, cy - 11, cxNext - 2 - 11, cy + 11, cxNext - 2, cy, fg);
    tft.fillRect(cxNext + 7, cy - 14, 3, 28, fg);
  }
}

// ── Touch ─────────────────────────────────────────────────
static bool getTouchXY(int16_t &tx, int16_t &ty) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  tx = map(p.x, TS_MINX, TS_MAXX, 0, SCR_W);
  ty = map(p.y, TS_MINY, TS_MAXY, 0, SCR_H);
  tx = constrain(tx, 0, SCR_W - 1);
  ty = constrain(ty, 0, SCR_H - 1);
  return true;
}

static void handleTouch() {
  int16_t tx, ty;
  if (!getTouchXY(tx, ty)) return;

  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE_MS) return;
  lastTouchTime = now;

  while (ts.touched()) delay(10);

  if (screenMode == SCREEN_BROWSER) {
    int fY = footerY();
    int vis = visibleSlots();
    int btnY0 = fY;                 // altura total do rodapé (tolerante)
    int btnY1 = fY + browseFooterH; // SCR_H-browseFooterH ... SCR_H-1

    // PREV
    if (ty >= btnY0 && ty <= btnY1 && tx >= 0 && tx <= 72) {
      albumScroll = max(0, albumScroll - vis);
      drawBrowser();
      return;
    }
    // NEXT
    if (ty >= btnY0 && ty <= btnY1 && tx >= 168 && tx <= SCR_W) {
      if (albumScroll + vis < albumCount) albumScroll += vis;
      drawBrowser();
      return;
    }

    // Taps on the list must happen within the visible rectangle.
    // Without this upper bound, button clicks may "escape" and open an item
    // that's not on screen (e.g., the last album).
    int listTop = browseListY;
    int listBottom = browseListY + vis * browseItemH; // exclusivo
    if (ty < listTop || ty >= listBottom) return;
    int indexInView = (ty - browseListY) / browseItemH;
    int idx = albumScroll + indexInView;
    if (idx < 0 || idx >= albumCount) return;

    // visual feedback
    int y = browseListY + indexInView * browseItemH;
    tft.fillRoundRect(6, y + 1, SCR_W - 12, browseItemH - 3, 5, COL_BTN_ACT);
    delay(60);

    loadAlbumTracks(albums[idx]);
    if (albumTrackCount > 0) {
      startTrack(0);
      screenMode = SCREEN_PLAYER;
      drawPlayer();
    }
  } else { // SCREEN_PLAYER
    // Lista: parar e voltar ao browser
    if (ty >= PL_BACK_BTN_Y && ty < PL_BACK_BTN_Y + PL_BACK_BTN_H &&
        tx >= PL_BACK_BTN_X && tx < PL_BACK_BTN_X + PL_BACK_BTN_W) {
      stopTrack();
      screenMode = SCREEN_BROWSER;
      drawBrowser();
      return;
    }

    // volume row (10% por toque)
    if (ty >= PL_VOLUME_Y && ty <= PL_VOLUME_Y + 32) {
      if (tx < 68) {
        volumePercent -= 10;
        applyVolumePercent();
        drawVolumeControls();
        return;
      } else if (tx >= 172) {
        volumePercent += 10;
        applyVolumePercent();
        drawVolumeControls();
        return;
      }
    }

    // controls (center area)
    int y = PL_TRANSPORT_Y;
    if (ty >= y && ty <= y + 44) {
      if (tx < 68) prevTrack();
      else if (tx < 172) togglePause();
      else nextTrack();
      drawPlayer();
    }
  }
}

// ── Setup / Loop ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  rgbLedInit();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  // "Gamma" adjustment for the ILI9341_2 (some CYD boards have washed-out colors)
  // Reported common sequence to improve quality after inversion/initial gamma.
  tft.writecommand(0x26); // ILI9341_GAMMASET
  tft.writedata(2);
  delay(120);
  tft.writecommand(0x26);
  tft.writedata(1);
  tft.setRotation(0);
  tft.fillScreen(COL_BG);

  SPI.begin();
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);

  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(20, 90);
    tft.print("SD Failed!");
    while (1) delay(1000);
  }

  audioOut = new RingBufOutput();
  audioOut->gain = 0.7f;
  applyVolumePercent();

  drawStartupScreen();

  a2dp.set_auto_reconnect(true);
  a2dp.set_volume(127);
  a2dp.start(BT_SPEAKER_NAME, btCallback);

  unsigned long t0 = millis();
  while (!a2dp.is_connected() && millis() - t0 < 15000) {
    rgbLedUpdate();
    delay(50);
  }
  btConnected = a2dp.is_connected();
  rgbPrevBt = btConnected;
  rgbPrevPlaying = (playerState == STATE_PLAYING);

  scanAlbums();
  screenMode = SCREEN_BROWSER;
  albumScroll = 0;
  drawBrowser();
}

void loop() {
  rgbLedUpdate();

  if (screenMode == SCREEN_PLAYER &&
      (playerState == STATE_PLAYING || playerState == STATE_PAUSED)) {
    unsigned long now = millis();
    if (now - lastProgressUiMs >= 450) {
      lastProgressUiMs = now;
      drawPlayerProgressArea();
    }
  }

  if (screenMode == SCREEN_PLAYER)
    updateVisualizerAnimation();

  if (playerState == STATE_PLAYING) {
    bool decoderAlive = false;
    if (currentType == AUDIO_MP3 && mp3) decoderAlive = mp3->isRunning();
    else if (currentType == AUDIO_WAV && wav) decoderAlive = wav->isRunning();

    if (decoderAlive) {
      int loops = 0;
      int target = (RING_SIZE * 3) / 4;
      bool loopFailed = false;
      while ((int)rbAvail() < target && loops < 1024) {
        bool ok = false;
        if (currentType == AUDIO_MP3 && mp3 && mp3->isRunning()) ok = mp3->loop();
        else if (currentType == AUDIO_WAV && wav && wav->isRunning()) ok = wav->loop();
        if (!ok) { loopFailed = true; break; }
        loops++;
      }

      // loop() returned false = decoder finished/errored.
      // Explicitly stop so isRunning() becomes false, and the next phase waits for the buffer to drain.
      if (loopFailed) {
        if (mp3 && mp3->isRunning()) mp3->stop();
        if (wav && wav->isRunning()) wav->stop();
      }
    } else {
      // Decoder stopped — wait for the ring buffer to drain before switching tracks with fewer glitches.
      if (rbAvail() < 200) {
        nextTrack();
        if (screenMode == SCREEN_PLAYER) drawPlayer();
      }
    }
  }

  handleTouch();
}

