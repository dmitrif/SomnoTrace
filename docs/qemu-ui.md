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

The Display section also emulates the persisted Screen timeout choices (Never,
1, 5, 15, or 30 minutes) and **Off now**. The preview starts therapy in its demo
state, so stop therapy before testing automatic idle sleep. A dark preview
consumes the first mouse press to wake, just as the physical touch panel does.

Run the non-graphical firmware boot check with:

```sh
./scripts/test-qemu-ui.sh
```

Run the graphical host-pointer-to-LVGL integration check with:

```sh
./scripts/test-qemu-touch.py
```

Capture the native framebuffer for Home, History, and Manage without QEMU
window chrome with:

```sh
./scripts/capture-qemu-ui.py
```

The capture tool uses QMP `screendump` to read the guest console surface, so
the resulting images contain no macOS/QEMU window chrome or pointer cursor. It
writes three validated 1024x600 PPM images to `build-qemu/captures`. Pass a
different output directory as the first argument, or add `--build` to rebuild
the firmware before capturing. By default it waits 3.5 seconds for the boot
notice to clear; `--settle-seconds` can override that when capturing a
transient state intentionally.

Add `--representative` to capture the same acceptance states as the handoff:
stopped Home, the newest selected History night, and Manage's System section.
Use repeated `--screen` options to limit the primary captures, for example
`--screen home --screen history`.

Add `--interaction-states` to capture fresh-boot interaction frames.
`devices.ppm` shows therapy stopped with Manage's default Devices section open.
The three Connectivity frames show the password keyboard after typing a sample
value, then the revealed value, then the remasked value:

- `connectivity-password-keyboard.ppm`
- `connectivity-password-revealed.ppm`
- `connectivity-password-remasked.ppm`

The flag adds these frames to the default primary captures, or to any primary
captures selected with `--screen`:

```sh
./scripts/capture-qemu-ui.py --screen manage --interaction-states
```

Every output frame uses a separate QEMU process. The capture helper keeps each
synthetic press down until LVGL reports that it sampled the touch, avoiding
fixed-delay misses during a slow redraw.

QEMU validates the ESP32-S3, FreeRTOS, LVGL, native 1024x600 geometry and screen
layout. It provides an 8 MB quad-PSRAM model for the same usable capacity as
the board. Espressif's stock RGB device is capped at 800x600, which is why the
setup script builds the small pinned width patch rather than stretching an
800-pixel preview.

It translates one host pointer into the same LVGL input path used by the GT911;
it does not emulate the GT911's I2C protocol or multitouch. It also does not
emulate the Waveshare RGB timings, CH422G expander, SDMMC or Bluetooth radio.
Pairing controls are interactive but BLE operations remain physical-board
tests. The bedside History screen presents a bounded night list, nightly
metrics and a selectable overnight channel when the stored data is available;
Manage includes everyday device, network, display, alert, storage and system
controls. Deeper multi-channel review and server administration remain in the
browser dashboard.
