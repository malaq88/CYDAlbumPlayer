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

// Player screen (estilo DAP / fita)
static const int PL_HEADER_H       = 22;
static const int PL_CASSETTE_TOP   = 40;
static const int PL_REEL_CY        = 94;
static const int PL_REEL_LX        = 108;
static const int PL_REEL_RX        = 212;
static const int PL_PROGRESS_TOP   = 144;
static const int PL_PROGRESS_H     = 54;
static const int PL_TRANSPORT_Y    = 200;

static inline uint16_t colReelRed()   { return tft.color565(215, 45, 50); }
static inline uint16_t colReelHub()   { return tft.color565(160, 160, 165); }
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

static const int browseHeaderH = 26;
static const int browsePathY = 30;
static const int browseListY = 44;
static const int browseFooterH = 30;
static const int browseItemH = 24;

static inline int footerY() { return 240 - browseFooterH; }
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
  tft.setCursor(160 - w / 2, y);
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

// ── Cassete animation (reels) ───────────────────────────────
static unsigned long lastReelAnimMs = 0;
static int reelAngleDeg = 0;

static void drawReelSpokes(int cx, int cy, int angleDeg, uint16_t spokeColor, uint16_t hubRingColor) {
  uint16_t reelCore = tft.color565(16, 16, 18);
  tft.fillCircle(cx, cy, 10, reelCore);
  tft.drawCircle(cx, cy, 8, hubRingColor);

  const float degToRad = 1.0f / 57.2957795f;
  const int len = 14;
  for (int i = 0; i < 3; i++) {
    float a = (angleDeg + i * 120) * degToRad;
    int x2 = cx + (int)(cos(a) * len);
    int y2 = cy + (int)(sin(a) * len);
    tft.drawLine(cx, cy, x2, y2, spokeColor);
  }
  tft.fillCircle(cx, cy, 3, hubRingColor);
}

static void updateReelAnimation() {
  if (playerState != STATE_PLAYING) return;
  if (screenMode != SCREEN_PLAYER) return;

  unsigned long now = millis();
  if (now - lastReelAnimMs < 60) return; // ~16 FPS
  lastReelAnimMs = now;

  reelAngleDeg = (reelAngleDeg + 18) % 360; // velocidade da rotação

  drawReelSpokes(PL_REEL_LX, PL_REEL_CY, reelAngleDeg, colReelHub(), colReelRed());
  drawReelSpokes(PL_REEL_RX, PL_REEL_CY, -reelAngleDeg, colReelHub(), colReelRed());
}

// ── Render ─────────────────────────────────────────────────
static void drawBrowser() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, 320, browseHeaderH, COL_BTN);

  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(2);
  tft.setCursor(10, 4);
  tft.print("Albums");

  // BT status: icon + headset name
  uint16_t btCol = btConnected ? COL_ACCENT : TFT_RED;
  tft.setTextSize(1);
  // "BT" badge
  tft.fillRoundRect(212, 5, 26, 16, 4, btCol);
  tft.setTextColor(COL_BG, btCol);
  tft.setCursor(219, 9);
  tft.print("BT");

  tft.setTextColor(btCol, COL_BTN);
  tft.setCursor(242, 9);
  if (btConnected) tft.print(BT_SPEAKER_NAME);
  else              tft.print("BT..");

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(10, browsePathY);
  tft.print("Touch an album");

  int fY = footerY();
  int vis = visibleSlots();

  tft.fillRoundRect(8, fY + 4, 70, 22, 5, COL_BTN);
  tft.fillRoundRect(242, fY + 4, 70, 22, 5, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(25, fY + 11);
  tft.print("PREV");
  tft.setCursor(262, fY + 11);
  tft.print("NEXT");

  int totalPages = (albumCount + vis - 1) / vis;
  if (totalPages < 1) totalPages = 1;
  int page = (albumScroll / vis) + 1;
  if (page > totalPages) page = totalPages;
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(132, fY + 11);
  tft.printf("%d/%d", page, totalPages);

  for (int i = 0; i < vis; i++) {
    int idx = albumScroll + i;
    if (idx >= albumCount) break;
    int y = browseListY + i * browseItemH;
    tft.fillRoundRect(6, y + 1, 308, browseItemH - 3, 5, COL_DIR);
    tft.setTextColor(COL_TEXT, COL_DIR);
    tft.setTextSize(1);
    tft.setCursor(12, y + 7);
    tft.print(albums[idx]);
  }
}

/** Barra de progresso, tempos e metadados técnicos (atualização periódica no loop). */
static void drawPlayerProgressArea() {
  if (screenMode != SCREEN_PLAYER) return;

  const int y0 = PL_PROGRESS_TOP;
  tft.fillRect(0, y0, 320, PL_PROGRESS_H, COL_BG);

  uint32_t el = elapsedPlaybackSec();
  char tEl[16], tTot[16];
  formatTimeHMS(tEl, sizeof(tEl), el);
  if (cachedDurationSec > 0) formatTimeHMS(tTot, sizeof(tTot), cachedDurationSec);
  else {
    strncpy(tTot, "--:--:--", sizeof(tTot) - 1);
    tTot[sizeof(tTot) - 1] = '\0';
  }

  const int bx = 10, by = y0 + 2, bw = 300, bh = 6;
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
  tft.setCursor(232, y0 + 16);
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

static void drawPlayer() {
  tft.fillScreen(COL_BG);

  // ── Barra de estado (estilo DAP) ─────────────────────────
  tft.fillRect(0, 0, 320, PL_HEADER_H, colTopBarBg());
  tft.drawFastHLine(0, PL_HEADER_H - 1, 320, tft.color565(40, 40, 48));

  tft.drawRoundRect(4, 4, 11, 13, 2, COL_DIM);
  tft.drawFastVLine(15, 8, 6, COL_TEXT);
  tft.drawFastHLine(15, 8, 4, COL_TEXT);

  tft.setTextColor(COL_TEXT, colTopBarBg());
  tft.setTextSize(1);
  tft.setCursor(22, 7);
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
    tft.setCursor(72, 7);
    tft.print(abuf);
  }

  uint16_t btCol = a2dp.is_connected() ? tft.color565(60, 200, 90) : tft.color565(200, 60, 60);
  tft.fillRoundRect(196, 4, 26, 14, 3, tft.color565(30, 30, 34));
  tft.setTextColor(btCol, colTopBarBg());
  tft.setCursor(202, 8);
  tft.print(BT_SPEAKER_NAME);

  tft.drawRect(228, 6, 20, 11, COL_DIM);
  tft.fillRect(230, 8, 14, 7, tft.color565(70, 140, 80));

  tft.fillRoundRect(256, 3, 60, 16, 3, tft.color565(36, 36, 42));
  tft.setTextColor(colInfoCyan(), tft.color565(36, 36, 42));
  tft.setCursor(266, 8);
  tft.print("BACK");

  // ── Título da faixa ─────────────────────────────────────
  char title[64];
  if (albumTrackCount > 0) getDisplayName(albumTracks[currentTrack], title, sizeof(title));
  else strncpy(title, "Sem faixa", sizeof(title) - 1);
  title[sizeof(title) - 1] = '\0';
  drawTitleCentered(24, 300, title);

  // ── Corpo da cassetes / fita ────────────────────────────
  tft.fillRoundRect(10, PL_CASSETTE_TOP, 300, 98, 11, colTapeEdge());
  tft.fillRoundRect(14, PL_CASSETTE_TOP + 4, 292, 90, 8, colTapeBody());

  for (int gx = 22; gx <= 40; gx += 6) {
    for (int gy = PL_CASSETTE_TOP + 18; gy <= PL_CASSETTE_TOP + 84; gy += 10) {
      tft.fillCircle(gx, gy, 1, tft.color565(18, 18, 22));
      tft.fillCircle(320 - gx, gy, 1, tft.color565(18, 18, 22));
    }
  }

  tft.fillRoundRect(44, PL_CASSETTE_TOP + 16, 232, 70, 9, tft.color565(4, 4, 6));

  for (int side = 0; side < 2; side++) {
    int lx = (side == 0) ? PL_REEL_LX : PL_REEL_RX;
    tft.fillCircle(lx, PL_REEL_CY, 21, colReelRed());
    tft.fillCircle(lx, PL_REEL_CY, 14, TFT_BLACK);
    tft.fillCircle(lx, PL_REEL_CY, 11, tft.color565(38, 38, 42));
  }

  drawReelSpokes(PL_REEL_LX, PL_REEL_CY, reelAngleDeg, colReelHub(), colReelRed());
  drawReelSpokes(PL_REEL_RX, PL_REEL_CY, -reelAngleDeg, colReelHub(), colReelRed());

  uint16_t tapeLine = tft.color565(35, 150, 95);
  for (int i = 0; i < 16; i++) {
    int x1 = 58 + i * 5;
    int y1 = PL_CASSETTE_TOP + 22 + i;
    tft.drawLine(x1, y1, x1 + 18, y1 + 11, tapeLine);
  }

  drawPlayerProgressArea();
  lastProgressUiMs = millis();

  // ── Transporte ───────────────────────────────────────────
  const int y = PL_TRANSPORT_Y;
  tft.fillRoundRect(10, y, 70, 38, 7, COL_BTN);
  tft.fillRoundRect(90, y, 140, 38, 7, COL_BTN);
  tft.fillRoundRect(240, y, 70, 38, 7, COL_BTN);

  uint16_t fg = COL_TEXT;
  int cy = y + 38 / 2;

  {
    int cxPrev = 10 + 70 / 2;
    tft.fillRect(cxPrev - 18, cy - 14, 3, 28, fg);
    tft.fillTriangle(cxPrev - 2, cy, cxPrev - 2 + 11, cy - 11, cxPrev - 2 + 11, cy + 11, fg);
    tft.fillTriangle(cxPrev + 8, cy, cxPrev + 8 + 11, cy - 11, cxPrev + 8 + 11, cy + 11, fg);
  }
  {
    int cxPlay = 90 + 140 / 2;
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
    int cxNext = 240 + 70 / 2;
    tft.fillTriangle(cxNext - 10 - 11, cy - 11, cxNext - 10 - 11, cy + 11, cxNext - 10, cy, fg);
    tft.fillTriangle(cxNext - 2 - 11, cy - 11, cxNext - 2 - 11, cy + 11, cxNext - 2, cy, fg);
    tft.fillRect(cxNext + 7, cy - 14, 3, 28, fg);
  }
}

// ── Touch ─────────────────────────────────────────────────
static bool getTouchXY(int16_t &tx, int16_t &ty) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  tx = map(p.x, TS_MINX, TS_MAXX, 0, 320);
  ty = map(p.y, TS_MINY, TS_MAXY, 0, 240);
  tx = constrain(tx, 0, 319);
  ty = constrain(ty, 0, 239);
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
    int btnY1 = fY + browseFooterH; // 240-browseFooterH ... 239

    // PREV
    if (ty >= btnY0 && ty <= btnY1 && tx >= 0 && tx <= 90) {
      albumScroll = max(0, albumScroll - vis);
      drawBrowser();
      return;
    }
    // NEXT
    if (ty >= btnY0 && ty <= btnY1 && tx >= 230 && tx <= 320) {
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
    tft.fillRoundRect(6, y + 1, 308, browseItemH - 3, 5, COL_BTN_ACT);
    delay(60);

    loadAlbumTracks(albums[idx]);
    if (albumTrackCount > 0) {
      startTrack(0);
      screenMode = SCREEN_PLAYER;
      drawPlayer();
    }
  } else { // SCREEN_PLAYER
    // BACK: stop playback and return to browser
    if (ty < PL_HEADER_H && tx >= 256) {
      stopTrack();
      screenMode = SCREEN_BROWSER;
      drawBrowser();
      return;
    }

    // controls (center area)
    int y = PL_TRANSPORT_Y;
    if (ty >= y && ty <= y + 40) {
      if (tx < 80) prevTrack();
      else if (tx < 230) togglePause();
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
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  SPI.begin();
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(20, 90);
    tft.print("SD Failed!");
    while (1) delay(1000);
  }

  audioOut = new RingBufOutput();
  audioOut->gain = 0.7f;

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(20, 75);
  tft.print("BT connecting...");

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

    // Reel animation while playing
    updateReelAnimation();
  }

  handleTouch();
}

