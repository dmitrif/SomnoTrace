# Waveshare ESP32-S3-Touch-LCD-7B test build

This board profile targets the **1024x600 7B** model (SKU 31726), not the
800x480 non-B model. It provides a native landscape touch dashboard while
retaining SomnoTrace's web dashboard, AirSense 11 BLE support, EDF generation,
FTP, SMB, and SleepHQ upload paths.

The panel is the primary bedside interface. Its persistent bottom navigation
opens five full-size pages:

- **Live** — breathing waveform, current therapy metrics and large actions
- **History** — the newest 12 recorded nights, read directly from microSD,
  with usage, AHI, 95% pressure and 95% leak summaries
- **Devices** — scan, pair and forget an AirSense 11 or supported O2 ring;
  AirSense passkeys are entered with an on-screen numeric keyboard
- **Settings** — brightness, therapy-screen policy, and direct Wi-Fi
  SSID/password entry with an on-screen keyboard
- **System** — live hardware health, memory, frame/touch counters and restart

The browser dashboard remains available for detailed zoomable graphs,
integration credentials, OTA updates and other infrequent administration.

## Hardware used

- ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
- 1024x600 RGB565 panel using the ESP32-S3 RGB LCD peripheral
- GT911 touch controller on I2C GPIO8/GPIO9, interrupt GPIO4
- CH422G I/O expander at I2C address `0x24`
- Onboard TF card in one-bit SD mode: CLK GPIO12, CMD GPIO11, D0 GPIO13

The board has no compatible onboard alert speaker. Push alerts still work;
audible escalation reports `not supported` on this target.

The existing SomnoTrace brightness setting is preserved, but on this profile
its full slider range is mapped across the 7B backlight's usable PWM range;
the default therefore remains readable in daylight without losing a dim
bedside setting.

## Build

Docker is the only host dependency:

```bash
./scripts/build-7b.sh
```

The synthetic host suite also checks the reference pin map, RGB timing,
framebuffer mode, GT911 reset sequence, TF wiring, and board configuration:

```bash
./scripts/test-host.sh
```

The merged image is written to:

```text
dist/somnotrace-waveshare-7b-test-full.bin
```

## First morning test

1. Format a reputable 16 GB or 32 GB microSDHC card as FAT32 and insert it
   while the board is powered off.
2. Hold **BOOT**, connect a USB data cable, then release BOOT. If already
   connected, hold BOOT and tap RESET.
3. In Chrome or Edge, open <https://espressif.github.io/esptool-js/>.
4. Connect to the ESP32-S3 serial port, choose the `-full.bin` above, use flash
   address `0x0`, and program it.
5. Tap RESET after flashing. The 1024x600 SomnoTrace dashboard should appear.
   Tap the **SomnoTrace** title to open the hardware diagnostics panel; the tap
   itself proves that GT911 input reached LVGL.
6. On a fresh flash, tap **Settings**, enter the Wi-Fi SSID and password with
   the on-screen keyboard, then tap **Save & restart**. The setup hotspot and
   <http://192.168.4.1> remain available as a recovery route.
7. After the board reboots, open <http://somnotrace.local> for advanced setup.
8. Tap **Devices** on the display, tap **Scan** under AirSense 11, choose the
   machine, and tap **Pair**. Enter the four-digit code shown on the AirSense
   using the on-screen keypad, then tap **Confirm code**.
9. Tap **History**, then **Refresh history**. A new card will appear after the
   first completed therapy session; selecting it shows the night's summary.

## Quick acceptance checklist

- Entire 1024x600 panel shows the dashboard without shifted or repeated rows.
- Tapping the **SomnoTrace** title opens diagnostics showing `GT911 detected`,
  the expected 1024x600 resolution, sensible non-zero PSRAM/internal RAM and
  UI stack headroom, plus zero RGB synchronization timeouts and touch errors.
- Tapping **Screen off**, then tapping an empty background area, turns the
  backlight off and back on.
- All five bottom tabs open distinct full-screen pages and remain easy to tap.
- The four large Live actions respond without opening a different tab.
- **Settings** brightness changes immediately and survives a restart.
- Wi-Fi credentials can be entered and saved on the panel; the device restarts
  and reconnects to that network.
- An AirSense scan runs without freezing tab navigation; pairing accepts the
  four-digit code entirely on the touchscreen.
- **History** remains responsive while reading the card and lists newest nights
  first.
- During therapy, **Wi-Fi setup** is refused so the board cannot accidentally
  disconnect from the AirSense and interrupt recording.
- The status bar changes to `Wi-Fi OK` after setup.
- Inserting the card changes the top status badge to `SD OK`; the web status
  API also reports storage available.
- AirSense pairing changes the top badge to `AirSense OK`.
- During therapy, the breathing graph moves and pressure, leak, respiratory
  rate, flow limitation, and runtime update.

## Capture logs if bring-up fails

Keep the browser flasher's console open after reset, or use a serial monitor at
115200 baud. Save the boot log from `SomnoTrace ... starting up` through the
first error. Especially useful tags are `board_7b`, `display_7b`, `sd_storage`,
`netprov`, and `as11_ble`.

Hardware validation is still required on a physical 7B. The source build
validates interfaces and memory layout but cannot prove panel timing, touch
orientation, or TF signal integrity without the board.
