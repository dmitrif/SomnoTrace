# ESP32-S3 QEMU UI preview

This profile runs SomnoTrace as ESP32-S3 firmware in Espressif QEMU and renders
the native 1024x600 LVGL dashboard through the virtual RGB565 panel. It is a UI
preview: AirSense, O2, Wi-Fi and microSD values are deterministic simulated
data. Mouse clicks and drags emulate the board's single-point touch controller;
the 7B product UI is pure touch and does not depend on GPIO navigation buttons.

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
automatically. Manage contains eight destinations: Devices, Connectivity,
Alerts, Uploads, Storage, System, Logs and Advanced. Display controls are
merged into System. Click buttons and settings controls, or drag where the
physical screen accepts a gesture. The cursor remains visible. Leave the
terminal open and use Control-C to stop the emulator.

System emulates brightness, therapy display behavior, the persisted Screen
timeout choices (Never, 1, 5, 15, or 30 minutes), and **Off now**. The preview
starts therapy in its demo state, so stop therapy before testing automatic idle
sleep. Sleep disables the backlight as well as the visible UI. A dark preview
consumes the first mouse press to wake, just as the physical touch panel does.

## First-run setup preview

The real 7B lazily opens a resumable Welcome/checklist/Ready flow when durable
setup state is incomplete. Its six steps are Wi-Fi, Time & clock, AirSense,
microSD card, Alerts and Uploads. There is no whole-setup Skip action: the five
skippable steps use their own **Skip for now**, while a missing card requires
the explicit **Continue without recording** choice.

QEMU normally starts from deterministic completed setup state so UI captures
reach Home. Open the first-run surface through its preview hotspot or capture
its first Wi-Fi state directly:

```sh
./scripts/capture-qemu-ui.py --interaction-state setup-wifi
```

The preview exercises the actual setup presentation and state transitions, but
network scans, pairing, clock changes and card checks remain simulated.

Run the non-graphical firmware boot check with:

```sh
./scripts/test-qemu-ui.sh
```

Run the graphical host-pointer-to-LVGL integration check with:

```sh
./scripts/test-qemu-touch.py
```

Stress the reusable Manage detail host through 64 real touch transitions and
record object-count plus heap/PSRAM bounds with:

```sh
./scripts/test-qemu-manage-lifecycle.py
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
The two System frames show the aligned brightness/timeout controls and the open
timeout menu:

- `system-display-controls.ppm`
- `system-display-timeout-open.ppm`

`logs.ppm` proves that entering Logs lazily constructs the retained viewer
instead of keeping it in the boot-time object tree.

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

Use repeated `--interaction-state` options to run only the states under review
without also opening the primary-screen captures, for example:

```sh
./scripts/capture-qemu-ui.py --interaction-state system-display-controls \
  --interaction-state connectivity-password-revealed
```

Every output frame uses a separate QEMU process. The capture helper keeps each
synthetic press down until LVGL reports that it sampled the touch, avoiding
fixed-delay misses during a slow redraw.

QEMU validates the ESP32-S3, FreeRTOS, LVGL, native 1024x600 geometry and screen
layout. It provides an 8 MB quad-PSRAM model for the same usable capacity as
the board. Espressif's stock RGB device is capped at 800x600, which is why the
setup script builds the small pinned width patch rather than stretching an
800-pixel preview.

It does not validate physical scanout timing. The board build uses the accepted
30.85 MHz production pixel clock, two PSRAM framebuffers and a ten-line DMA
bounce buffer. The available 18 MHz setting is diagnostic fallback only.

It translates one host pointer into the same LVGL input path used by the GT911;
it does not emulate the GT911's I2C protocol or multitouch. It also does not
emulate the Waveshare RGB timings, CH32V003 I/O controller, SDMMC or Bluetooth
radio.
Pairing controls are interactive but BLE operations remain physical-board
tests. History uses a seven-row viewport over the complete simulated card
index. It exposes all eight signal controls—Flow, Pressure, Leak, Flow limit,
Snore, SpO₂, Pulse and
Motion—while disabling channels absent from the selected night. The graph has
an event-marker lane, touch cursor, source-derived visible-window statistics,
Fit/pan, and stepped night-quarter/90/22/10/5-minute zoom. Initial or new-night
aggregation can be cancelled; zoom and pan reread the card asynchronously,
retain the last resolved graph under progress, and publish the new graph
atomically when complete.

Logs is also lazy. It keeps ten visible rows backed by the retained ring rather
than creating thousands of LVGL objects, and supports pause/resume, tag/message
search, level filters, swipe paging, jump-to-newest, RAM-only clear and a card
snapshot with progress. QEMU exercises those interactions with simulated log
and card state. Browser-only administration still includes integration
credentials, OTA, card formatting and destructive recording operations; QEMU
does not imply parity for those features.
