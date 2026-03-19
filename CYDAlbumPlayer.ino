// ============================================================
// CYD Album Player (by folders) - Functional sketch
// ============================================================

#include <Arduino.h>
#include <stdint.h>
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

// ── Albums / tracks ────────────────────────────────────────
#define MAX_ALBUMS 32
#define MAX_TRACKS 128
#define MAX_ALBUM_NAME_LEN 48

static char albums[MAX_ALBUMS][MAX_ALBUM_NAME_LEN];
static int albumCount = 0;
static int albumScroll = 0;

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
  if (playerState == STATE_PLAYING) playerState = STATE_PAUSED;
  else if (playerState == STATE_PAUSED) playerState = STATE_PLAYING;
}

// ── Cassete animation (reels) ───────────────────────────────
static unsigned long lastReelAnimMs = 0;
static int reelAngleDeg = 0;

static void drawReelSpokes(int cx, int cy, int angleDeg, uint16_t spokeColor) {
  // Clear only the core area to avoid excessive flicker.
  uint16_t reelCore = tft.color565(30, 30, 30);
  tft.fillCircle(cx, cy, 10, reelCore);

  // Inner ring for finishing.
  tft.drawCircle(cx, cy, 8, COL_ACCENT);

  // Arduino/ESP32 already defines DEG_TO_RAD; don't redeclare.
  const float degToRad = 1.0f / 57.2957795f; // PI/180
  const int len = 15;

  // 3 "spokes" to simulate rotation
  for (int i = 0; i < 3; i++) {
    float a = (angleDeg + i * 120) * degToRad;
    int x2 = cx + (int)(cos(a) * len);
    int y2 = cy + (int)(sin(a) * len);
    tft.drawLine(cx, cy, x2, y2, spokeColor);
  }

  // Center point
  tft.fillCircle(cx, cy, 3, spokeColor);
}

static void updateReelAnimation() {
  if (playerState != STATE_PLAYING) return;
  if (screenMode != SCREEN_PLAYER) return;

  unsigned long now = millis();
  if (now - lastReelAnimMs < 60) return; // ~16 FPS
  lastReelAnimMs = now;

  reelAngleDeg = (reelAngleDeg + 18) % 360; // velocidade da rotação

  // Reel positions in the current layout
  int reelCy = 145;
  int reelLx = 110;
  int reelRx = 210;

  // Left and right rotate in opposite directions for a more "realistic" look
  drawReelSpokes(reelLx, reelCy, reelAngleDeg, COL_ACCENT);
  drawReelSpokes(reelRx, reelCy, -reelAngleDeg, COL_ACCENT);
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

static void drawPlayer() {
  tft.fillScreen(COL_BG);

  tft.fillRect(0, 0, 320, 24, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(1);
  tft.setCursor(8, 9);
  tft.print("Player");

  tft.fillRoundRect(252, 2, 64, 20, 4, COL_ACCENT);
  tft.setTextColor(COL_BG, COL_ACCENT);
  tft.setTextSize(1);
  tft.setCursor(268, 9);
  tft.print("BACK");

  // ============================================================
  // Retro design: "cassette tape" (behind the buttons)
  // ============================================================
  uint16_t cassetteEdge  = tft.color565(170, 170, 170);
  uint16_t cassetteMain  = tft.color565(60, 60, 60);
  uint16_t cassetteLabel = tft.color565(24, 24, 24);
  uint16_t windowBg      = tft.color565(8, 8, 8);
  uint16_t reelEdge      = tft.color565(120, 120, 120);
  uint16_t reelCore      = tft.color565(30, 30, 30);

  // Outer body
  tft.fillRoundRect(8, 55, 304, 160, 14, cassetteEdge);
  tft.fillRoundRect(12, 59, 296, 152, 12, cassetteMain);

  // Label
  tft.fillRoundRect(70, 72, 180, 28, 6, cassetteLabel);
  tft.fillRoundRect(74, 76, 172, 20, 4, cassetteMain);

  // Side panels like a "speaker grill"
  uint16_t hole = tft.color565(10, 10, 10);
  for (int gx = 22; gx <= 44; gx += 5) {
    for (int gy = 96; gy <= 166; gy += 8) {
      tft.fillCircle(gx, gy, 1, hole);
      tft.fillCircle(320 - gx, gy, 1, hole);
    }
  }

  // Tape window
  tft.fillRoundRect(48, 105, 224, 80, 12, windowBg);

  // Reels
  int reelCy = 145;
  int reelLx = 110;
  int reelRx = 210;

  tft.fillCircle(reelLx, reelCy, 20, reelCore);
  tft.drawCircle(reelLx, reelCy, 20, reelEdge);
  tft.drawCircle(reelLx, reelCy, 8, COL_ACCENT);

  tft.fillCircle(reelRx, reelCy, 20, reelCore);
  tft.drawCircle(reelRx, reelCy, 20, reelEdge);
  tft.drawCircle(reelRx, reelCy, 8, COL_ACCENT);

  // Reels at rest + animated in loop
  drawReelSpokes(reelLx, reelCy, reelAngleDeg, COL_ACCENT);
  drawReelSpokes(reelRx, reelCy, -reelAngleDeg, COL_ACCENT);

  // Simple texture (diagonal lines)
  uint16_t tapeLine = tft.color565(20, 160, 90);
  for (int i = 0; i < 14; i++) {
    int x1 = 60 + i * 6;
    int y1 = 112 + i;
    int x2 = x1 + 20;
    int y2 = y1 + 12;
    tft.drawLine(x1, y1, x2, y2, tapeLine);
  }

  // "REC" indicator LED (static)
  uint16_t ledCol = (playerState == STATE_PLAYING) ? COL_ACCENT : COL_DIM;
  tft.fillRoundRect(16, 70, 22, 10, 3, ledCol);
  tft.setTextColor(cassetteMain, ledCol);
  tft.setTextSize(1);
  tft.setCursor(19, 73);
  tft.print("REC");

  char title[64];
  if (albumTrackCount > 0) getDisplayName(albumTracks[currentTrack], title, sizeof(title));
  else strncpy(title, "No track", sizeof(title) - 1);
  title[sizeof(title) - 1] = '\0';

  // Text inside the cassette label
  tft.setTextColor(COL_TEXT, cassetteLabel);
  tft.setTextSize(1);
  tft.setCursor(84, 80);
  tft.print(title);

  int y = 188;
  tft.fillRoundRect(10, y, 70, 50, 8, COL_BTN);
  tft.fillRoundRect(90, y, 140, 50, 8, COL_BTN);
  tft.fillRoundRect(240, y, 70, 50, 8, COL_BTN);

  // Navigation icons (ShuffleCYDgen style)
  uint16_t fg = COL_TEXT;
  int cy = y + 50 / 2;

  // PREV: |◄◄
  {
    int cxPrev = 10 + 70 / 2; // 45
    tft.fillRect(cxPrev - 20, cy - 16, 3, 32, fg); // barra
    // Double triangle pointing left
    tft.fillTriangle(cxPrev - 2, cy, cxPrev - 2 + 12, cy - 12, cxPrev - 2 + 12, cy + 12, fg);
    tft.fillTriangle(cxPrev + 10, cy, cxPrev + 10 + 12, cy - 12, cxPrev + 10 + 12, cy + 12, fg);
  }

  // PLAY/PAUSE
  {
    int cxPlay = 90 + 140 / 2; // 160
    if (playerState == STATE_PLAYING) {
      // pause: ||
      int wBar = 8;
      int gap = 6;
      tft.fillRect(cxPlay - gap - wBar, cy - 18, wBar, 36, fg);
      tft.fillRect(cxPlay + gap,        cy - 18, wBar, 36, fg);
    } else {
      // play: ►
      int size = 26;
      tft.fillTriangle(cxPlay - size / 3, cy - size / 2,
                       cxPlay - size / 3, cy + size / 2,
                       cxPlay + size / 2, cy,
                       fg);
    }
  }

  // NEXT: ►►|
  {
    int cxNext = 240 + 70 / 2; // 275
    // Double triangle pointing right
    tft.fillTriangle(cxNext - 10 - 12, cy - 12, cxNext - 10 - 12, cy + 12, cxNext - 10, cy, fg);
    tft.fillTriangle(cxNext - 2 - 12,  cy - 12, cxNext - 2 - 12,  cy + 12, cxNext - 2,  cy, fg);
    // final bar
    tft.fillRect(cxNext + 8, cy - 16, 3, 32, fg);
  }

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(10, 92);
  tft.printf("%d/%d", currentTrack + 1, albumTrackCount);
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
    if (ty <= 24 && tx >= 252) {
      stopTrack();
      screenMode = SCREEN_BROWSER;
      drawBrowser();
      return;
    }

    // controls (center area)
    int y = 188;
    if (ty >= y && ty <= y + 50) {
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
  while (!a2dp.is_connected() && millis() - t0 < 15000) delay(50);
  btConnected = a2dp.is_connected();

  scanAlbums();
  screenMode = SCREEN_BROWSER;
  albumScroll = 0;
  drawBrowser();
}

void loop() {
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

