# ESP32-S3 QEMU UI preview

This profile runs SomnoTrace as ESP32-S3 firmware in Espressif QEMU and renders
the native 1024x600 LVGL dashboard through the virtual RGB565 panel. It is a UI
preview: AirSense, O2, Wi-Fi and microSD values are deterministic simulated
data, and the five tabs rotate automatically every eight seconds.

On Apple Silicon or Intel macOS, from the repository root:

```sh
./scripts/run-qemu-ui.sh --build
```

The first run installs any missing Homebrew build libraries, downloads the
pinned Espressif QEMU source, applies SomnoTrace's one-line 1024-pixel RGB
device patch, and builds it in `~/Library/Caches/SomnoTrace`. Expect roughly
one to three minutes on a recent Mac. Firmware and emulator builds are cached;
later runs can omit `--build` and normally open in a few seconds.

The QEMU window is deliberately hands-off for now. It cycles through Live,
History, Devices, Settings and System every eight seconds so every 7-inch
layout is inspectable without emulated touch. The macOS cursor remains visible
when the window is clicked. If QEMU captures keyboard or mouse input, press
Control-Option-G to release it. Leave the terminal open and use Control-C to
stop it.

Run the non-graphical firmware boot check with:

```sh
./scripts/test-qemu-ui.sh
```

QEMU validates the ESP32-S3, FreeRTOS, LVGL, native 1024x600 geometry and screen
layout. It provides an 8 MB quad-PSRAM model for the same usable capacity as
the board. Espressif's stock RGB device is capped at 800x600, which is why the
setup script builds the small pinned width patch rather than stretching an
800-pixel preview.

It does not emulate the Waveshare RGB timings, CH422G expander, GT911 touch,
SDMMC or Bluetooth radio. Pairing buttons and saved settings are visible but
do not operate in this preview; those remain physical-board tests.
