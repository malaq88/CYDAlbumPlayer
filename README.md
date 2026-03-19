# Acknowledgements

Special thanks to **Sparkadium** and the original project **[Cheap-Yellow-MP3-Player](https://github.com/Sparkadium/Cheap-Yellow-MP3-Player)** — it helped me get back to working on this project.

# CYD Album Player

ESP32 “CYD” music player that:
- Scans your SD card by albums (folders) and plays `.mp3` / `.wav` tracks.
- Shows a retro cassette UI on the TFT with touch controls.
- Acts as a **Bluetooth A2DP Source** (sends the audio to a Bluetooth speaker/headset).

## Hardware / Pinout used by this sketch

The pin mapping in this project is defined in `CYDAlbumPlayer.ino` and matches the included TFT configuration file (`Setup_User.h`).

- SD card (SPI):
  - `SD_CS = 5` (`#define SD_CS 5`)
- TFT backlight:
  - `TFT_BL = 21` (`#define TFT_BL 21`)
- Touch controller (XPT2046 on its own HSPI bus):
  - `TOUCH_CLK = 25`
  - `TOUCH_MISO = 39`
  - `TOUCH_MOSI = 32`
  - `TOUCH_CS = 33`
  - `TOUCH_IRQ = 36`

The TFT drawing and SPI TFT pins (TFT_MISO/TFT_MOSI/TFT_SCLK/TFT_CS/TFT_DC/…) are configured by TFT_eSPI using `Setup_User.h`.

## TFT Setup (must match your exact CYD display variant)

This repository includes a reference copy of the TFT_eSPI setup file as `Setup_User.h`.

### 1) Install/use this setup in your TFT_eSPI library

TFT_eSPI does not automatically read `Setup_User.h` from the project root. You must apply it to your TFT_eSPI installation.

1. Open `Setup_User.h` (in this project).
2. Copy its contents (or replace the file) into your TFT_eSPI library folder:
   - `Documents/Arduino/libraries/TFT_eSPI/User_Setup.h`

### 2) Select the correct display driver (critical)

Your CYD boards come in multiple display controller variants. In `Setup_User.h`, choose exactly ONE driver:

- `ILI9341_DRIVER` (v1 original, 1× Micro-USB)
- `ILI9341_2_DRIVER` (v1 alternative controller, 1× Micro-USB)
- `ST7789_2_DRIVER` (v2/v3 newer, USB-C + Micro)

If the screen is blank/white:
- Try `ILI9341_DRIVER` first (then switch to `ILI9341_2_DRIVER` if needed).
- If your board has 2 USB ports (USB-C + Micro), use `ST7789_2_DRIVER`.

### 3) Color order / inversion fixes (if colors look wrong)

In `Setup_User.h`:
- If colors have red/blue swapped, change `TFT_RGB_ORDER` (between `TFT_RGB` and `TFT_BGR`).
- If you use `ST7789_2_DRIVER` and colors look washed/inverted, uncomment `TFT_INVERSION_ON`.

### 4) Gamma tweak in the sketch

`CYDAlbumPlayer.ino` applies a small gamma adjustment intended for the `ILI9341_2` driver:
- `tft.writecommand(0x26); ...`

If you switch your driver to something else (e.g., ST7789), and the colors look off, consider commenting out that gamma block or adjust it.

## Bluetooth pairing/name configuration (required)

You must change the variable that selects the Bluetooth target device:

`BT_SPEAKER_NAME`

In `CYDAlbumPlayer.ino` it is currently:

```cpp
static const char* BT_SPEAKER_NAME = "E6";
```

Set `BT_SPEAKER_NAME` to the **exact name** shown by your Bluetooth speaker/headset (the device that will receive the audio).

This value is used here:
- `a2dp.start(BT_SPEAKER_NAME, btCallback);`

## SD Card layout expected by this project

The code expects:
- Album = a folder under the SD card root (`/`)
- Track files inside each album folder
  - `.mp3`
  - `.wav`

The project ignores a known Windows folder:
- `System Volume Information`

## Touch calibration (optional)

If touch points don’t match the buttons/menu areas, tune the calibration constants in `CYDAlbumPlayer.ino`:
- `TS_MINX`, `TS_MAXX`, `TS_MINY`, `TS_MAXY`

## Libraries used (typical)

You need these dependencies available in Arduino IDE:
- `TFT_eSPI`
- `XPT2046_Touchscreen`
- `ESP32-A2DP` (Phil Schatzmann)
- `ESP8266Audio` (provides `AudioFileSourceSD`, `AudioGeneratorMP3`, `AudioGeneratorWAV`, `AudioOutput`)

## Build & upload

1. Select your ESP32 board in Arduino IDE.
2. Ensure TFT_eSPI is configured using the `Setup_User.h` reference.
3. Set `BT_SPEAKER_NAME` to your Bluetooth device name.
4. Compile and upload `CYDAlbumPlayer.ino`.

If you want, paste the last ~30 lines of your Arduino compile log (especially the first real `error:` if any) and I can help confirm board/driver configuration.

