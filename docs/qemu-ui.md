# ESP32-S3 QEMU UI preview

This profile runs SomnoTrace as ESP32-S3 firmware in Espressif QEMU and renders
the native 1024x600 LVGL dashboard through the virtual RGB565 panel. It is a UI
preview: AirSense, O2, Wi-Fi and microSD values are deterministic simulated
data. Mouse clicks and drags emulate the board's single-point touch controller.

On Apple Silicon or Intel macOS, from the repository root:

```sh
./scripts/run-qemu-ui.sh --build
```

The first run installs any missing Homebrew build libraries, downloads the
pinned Espressif QEMU source, applies SomnoTrace's 1024-pixel RGB and host-touch
patches, and builds it in
`~/Library/Caches/SomnoTrace`. Expect roughly one to three minutes on a recent
Mac. Firmware and emulator builds are cached; later runs can omit `--build`
and normally open in a few seconds.

Use the mouse as a finger. The persistent bottom navigation opens Home,
History and Manage; the UI stays on the selected screen rather than cycling
automatically. Manage contains Devices, Connectivity, Display, Alerts, Storage
and System. Click buttons and settings controls, or drag where the physical
screen accepts a gesture. The cursor remains visible. Leave the terminal open
and use Control-C to stop the emulator.

Run the non-graphical firmware boot check with:

```sh
./scripts/test-qemu-ui.sh
```

Run the graphical host-pointer-to-LVGL integration check with:

```sh
./scripts/test-qemu-touch.py
```

QEMU validates the ESP32-S3, FreeRTOS, LVGL, native 1024x600 geometry and screen
layout. It provides an 8 MB quad-PSRAM model for the same usable capacity as
the board. Espressif's stock RGB device is capped at 800x600, which is why the
setup script builds the small pinned width patch rather than stretching an
800-pixel preview.

It translates one host pointer into the same LVGL input path used by the GT911;
it does not emulate the GT911's I2C protocol or multitouch. It also does not
emulate the Waveshare RGB timings, CH422G expander, SDMMC or Bluetooth radio.
Pairing controls are interactive but BLE operations remain physical-board
tests. Detailed traces and advanced administration remain browser-based rather
than being duplicated on the bedside display.
