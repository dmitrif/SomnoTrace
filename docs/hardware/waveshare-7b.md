# Waveshare ESP32-S3-Touch-LCD-7B test build

This board profile targets the **1024x600 7B** model (SKU 31726), not the
800x480 non-B model. It provides a native landscape touch dashboard while
retaining SomnoTrace's web dashboard, AirSense 11 BLE support, EDF generation,
FTP, SMB, and SleepHQ upload paths.

The panel is the primary bedside interface. Its persistent bottom navigation
opens three full-size screens:

- **Home** — live breathing waveform, therapy state, current metrics, elapsed
  time, and one-tap Start/Stop therapy
- **History** — up to 30 newest recorded nights from microSD, revealed seven at
  a time, with a selected-night summary and touch-selectable Flow, SpO2, and
  Leak overviews
- **Manage** — a single settings area with Devices, Connectivity, Display,
  Alerts, Storage, and System sections

Manage supports AirSense 11 and optional O2-ring scan/pair/forget, on-screen
AirSense passkey entry, direct Wi-Fi credentials with a full touch keyboard,
brightness, idle screen timeout, therapy-screen behavior, alert status/test,
storage and upload health, diagnostics, and restart. Infrequent or
high-complexity administration stays in the browser instead of crowding the
bedside interface.

The browser dashboard remains available for detailed zoomable graphs,
integration credentials, OTA updates and other infrequent administration.

## Hardware used

- ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
- 1024x600 RGB565 panel using the ESP32-S3 RGB LCD peripheral
- GT911 touch controller on I2C GPIO8/GPIO9, interrupt GPIO4
- CH32V003 I/O controller at I2C address `0x24`
- Onboard TF card in one-bit SD mode: CLK GPIO12, CMD GPIO11, D0 GPIO13

The board has no compatible onboard alert speaker. Push alerts still work;
audible escalation reports `not supported` on this target.

The existing SomnoTrace brightness setting is preserved, but on this profile
its full slider range is mapped across 1-100% of the 7B backlight range. The
default is 100%, which uses the controller's steady full-on level. Selecting a
lower brightness intentionally uses the board's hardware PWM for bedside
dimming; the on-screen value identifies those levels as PWM.

When therapy is stopped, the display sleeps after five minutes without a touch
by default. **Manage > Display > Screen timeout** can select Never, 1, 5, 15,
or 30 minutes, and the choice persists across restarts. The first touch while
dark only wakes the screen; it does not activate the control underneath.
Therapy, an active alert, and the setup hotspot keep the screen awake. If the
GT911 touch controller is unavailable, automatic and manual screen-off controls
are disabled so this touch-only board cannot lock itself dark.

The therapy modes named **Screen off** and **Always off** still allow a touch to
wake the controls when needed. If their configured policy still calls for a
dark screen, the panel returns to sleep after a one-minute control window.

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

1. Format a reputable 16 GB or 32 GB microSDHC card as one FAT32 volume using
   a Master Boot Record (MBR) partition scheme. Insert it while the board is
   powered off; SomnoTrace detects and mounts it only during the next boot.
2. Hold **BOOT**, connect a USB data cable, then release BOOT. If already
   connected, hold BOOT and tap RESET.
3. In Chrome or Edge, open <https://espressif.github.io/esptool-js/>.
4. Connect to the ESP32-S3 serial port, choose the `-full.bin` above, use flash
   address `0x0`, and program it.
5. Tap RESET after flashing. The 1024x600 SomnoTrace dashboard should appear.
   Open **Manage > System > Diagnostics**; reaching it proves that GT911 input
   reached LVGL, and the panel reports touch and framebuffer health.
6. On a fresh flash, open **Manage > Connectivity**, enter the Wi-Fi SSID and
   password with the on-screen keyboard, then tap **Save changes**. Saving does
   not interrupt an active recording. Tap the same button again when it says
   **Restart now** to confirm the restart; if therapy is running, restart stays
   deferred. The setup hotspot and <http://192.168.4.1> remain available as a
   recovery route.
7. After the board reboots, open <http://somnotrace.local> for advanced setup.
8. Open **Manage > Devices**, tap **Scan** for AirSense 11, choose the machine,
   and tap **Pair**. Enter the four-digit code shown on the AirSense using the
   on-screen keypad, then tap **Confirm code**.
9. Open **History**. It automatically refreshes, selects the newest available
   night, and lazily loads that night's summary and selected overnight channel
   without blocking touch input. **Flow** and **Leak** use AirSense recordings;
   **SpO2** uses a paired O2 Ring recording when one exists. A new night appears
   after the first completed therapy session.

## Quick acceptance checklist

- Entire 1024x600 panel shows the dashboard without shifted or repeated rows.
- **Manage > System > Diagnostics** shows `GT911 detected`, the expected
  1024x600 resolution, sensible non-zero PSRAM/internal RAM and UI stack
  headroom, plus zero RGB synchronization timeouts and touch errors. The boot
  log's `UI tree` line should also leave at least 2 KB of initialization-stack
  headroom.
- Tapping **Manage > Display > Off now**, then touching once, turns the
  backlight off and back on without activating the control under that touch.
- Setting **Screen timeout** to **1 minute** while therapy is stopped turns the
  backlight off after one untouched minute; touching wakes it and starts a new
  minute. Setting it to **Never** keeps the screen awake. Restore the preferred
  timeout after this check.
- All three bottom tabs open distinct full-screen pages and remain easy to tap.
- Home's primary Start/Stop therapy action responds in one tap and does not
  open another screen.
- Ten consecutive Start/Stop cycles neither reboot the board nor produce an SD
  write error. After the final stop, **History** opens without a manual refresh.
- **Manage > Display** brightness changes immediately and survives a restart.
- Wi-Fi credentials can be entered and saved on the panel; the device restarts
  only after the separate confirmation and reconnects to that network.
- An AirSense scan runs without freezing tab navigation; pairing accepts the
  four-digit code entirely on the touchscreen.
- **History** remains responsive while reading the card, lists newest nights
  first, selects the newest night automatically, loads seven more on request up
  to 30, and switches among distinct Flow, SpO2, and Leak overviews for the
  selected night.
- After a therapy cycle, repeatedly switching among **History**, every
  **Manage** section, and the three bottom tabs for several minutes causes no
  restart, progressive slowdown, or SD read failure.
- During therapy, the setup hotspot is refused so the board cannot accidentally
  disconnect from the AirSense and interrupt recording.
- The status bar changes to `Wi-Fi OK` after setup.
- With the board powered off, inserting the card and then booting changes the
  top status badge to `SD OK`; the web status API also reports storage
  available. A card inserted while SomnoTrace is already running requires a
  restart before it can be detected.
- AirSense pairing changes the top badge to `AirSense OK`.
- During therapy, the breathing graph moves smoothly while pressure, leak,
  respiratory rate, flow limitation, and runtime update; touch navigation must
  remain responsive during graph redraws.

## Capture logs if bring-up fails

Keep the browser flasher's console open after reset, or use a serial monitor at
115200 baud. Save the boot log from `SomnoTrace ... starting up` through the
first error. Especially useful tags are `board_7b`, `display_7b`, `sd_storage`,
`netprov`, and `as11_ble`.

Hardware validation is still required on a physical 7B. The source build
validates interfaces and memory layout but cannot prove panel timing, touch
orientation, or TF signal integrity without the board.
