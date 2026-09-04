# Waveshare ESP32-S3-Touch-LCD-7B profile

This board profile targets the **1024x600 7B** model (SKU 31726), not the
800x480 non-B model. It provides a native landscape touch dashboard while
retaining SomnoTrace's web dashboard, AirSense 11 BLE support, EDF generation,
FTP, SMB, and SleepHQ upload paths.

The panel is the primary bedside interface. The 7B interaction model is pure
touch; normal operation does not require GPIO buttons. Its persistent bottom
navigation opens three full-size screens:

- **Home** — live breathing waveform, therapy state, current metrics, elapsed
  time, and one-tap Start/Stop therapy
- **History** — a seven-row viewport over the complete microSD night index,
  selected-night summary, calendar/night navigation, eight available signal
  views, respiratory-event markers, exact visible-window statistics, cursor,
  pan and zoom
- **Manage** — a persistent left rail with Devices, Connectivity, Alerts,
  Uploads, Storage, System, Logs and Advanced destinations

Manage supports AirSense 11 and optional O2-ring scan/pair/forget, on-screen
AirSense passkey entry, direct Wi-Fi credentials with a full touch keyboard,
brightness, idle screen timeout, therapy-screen behavior, alert status/test,
storage and upload health, retained logs, diagnostics, and restart. Brightness
and the other display controls are merged into **System**; there is no separate
Display destination. Only the selected Manage detail tree is allocated, and
Logs is constructed lazily when first opened, keeping the boot-time LVGL tree
and repeated navigation bounded.

The panel is not a claim of complete browser parity. Integration credentials,
notification schedules and delivery configuration, OTA installation, card
formatting, reset-all, file browsing/regeneration/deletion and other
high-impact administration remain in the browser.

## First-run setup

An incomplete new installation opens a dedicated Welcome/checklist/Ready
surface before the normal shell. It is resumable from durable state and has six
steps: Wi-Fi, Time & clock, AirSense, microSD card, Alerts and Uploads. The
AirSense flow tells the user to start pairing on the machine first and accepts
the actual four-digit code shown by the machine.

There is no generic **Skip setup** action. Wi-Fi, time, AirSense, Alerts and
Uploads can each be deferred explicitly. A missing or unreadable card instead
requires **Continue without recording**, so the receipt cannot imply that new
nights will be saved. Existing installations are reconciled from durable facts
and are not sent through setup merely because this firmware introduced the
checklist.

## Card-backed History

History automatically loads on entry, selects the newest available night and
pages seven rows at a time across the complete card index. Seven is a viewport
size, not a retention limit. Available signal controls are:

- Breathing / Flow (L/s)
- Pressure, with the EPR companion when present (cmH₂O)
- Leak (L/min)
- Flow limit
- Snore
- SpO₂ (%)
- Pulse (bpm)
- Motion

Unavailable channels stay explicitly unavailable rather than being fabricated.
The graph includes an OA/CA/H/generic-apnea/RERA marker lane and a touch cursor.
Statistics are computed from source samples in the selected visible time
window—not from the 480 display bins. Flow reports absolute P50/P95/P99.5;
Pressure, Leak, Flow limit and Snore report P50/P95/P99.5; SpO₂ reports minimum,
P5, P0.5 and time below 88%; Pulse reports minimum/median/maximum. Motion is
selectable when its source provides usable samples, but its statistic fields
remain unavailable rather than being invented.

**Fit** returns to the whole night. Zoom steps through a night-quarter, 90-,
22-, 10- and 5-minute windows, with pan constrained to the night. Initial and
new-night aggregation is cancellable. Zoom and pan use a serialized background
microSD reread, keep the last resolved graph visible beneath determinate
progress, and publish the new graph atomically. Flow range reads prefer raw
25 Hz samples for windows of 22 minutes or less and for any window no larger
than one-quarter of the night. If every contributing session cannot provide
raw samples, the UI truthfully identifies the 1 Hz min/max envelope fallback.

## Native Logs

Logs is a lazy ten-row viewport over the retained in-memory ring, not a 2,048-row
LVGL list. Newest lines appear first. Pause/search establishes a stable anchor
while collection continues, the UI reports the exact number of newer lines,
and dismissing search or jumping to newest does not silently resume. Level
filters include Debug, vertical swipes page older/newer, **Clear** affects RAM
only, and **Save to card** reports determinate progress or a truthful failure.

## Hardware used

- ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
- 1024x600 RGB565 panel using the ESP32-S3 RGB LCD peripheral
- GT911 touch controller on I2C GPIO8/GPIO9, interrupt GPIO4
- CH32V003 I/O controller at I2C address `0x24`
- Onboard TF card in one-bit SD mode: CLK GPIO12, CMD GPIO11, D0 GPIO13

The board has no compatible onboard alert speaker. Push alerts still work;
audible escalation reports `not supported` on this target.

## RGB scanout

The accepted production configuration scans at **30.85 MHz** with two RGB565
framebuffers in PSRAM and a DMA bounce buffer of **10 full lines**
(`1024 * 10` pixels). Physical testing found this combination free of the prior
shimmer and tearing while preserving touch responsiveness. The 10-line buffer
is 20 KiB in RGB565 and fits within the configured 64 KiB data cache.

The **18 MHz** clock remains accepted only as a diagnostic A/B fallback. It is
not the boot default and should not be documented or deployed as the normal
7B operating mode.

The existing SomnoTrace brightness setting is preserved, but on this profile
its full slider range is mapped across 1-100% of the 7B backlight range. The
default is 100%, which uses the controller's steady full-on level. Selecting a
lower brightness intentionally uses the board's hardware PWM for bedside
dimming; the on-screen value identifies those levels as PWM.

When therapy is stopped, the display sleeps after five minutes without a touch
by default. **Manage > System > Screen timeout** can select Never, 1, 5, 15, or
30 minutes, and the choice persists across restarts. Sleeping turns off the
backlight rather than merely drawing black. The first touch while dark only
wakes the screen; it does not activate the control underneath. Therapy, an
active alert, and the setup hotspot keep the screen awake. If the GT911 touch
controller is unavailable, automatic and manual screen-off controls are
disabled so this touch-only board cannot lock itself dark.

The therapy modes named **Screen off** and **Off except alerts** still allow a
touch to wake the controls when needed. If their configured policy still calls
for a dark screen, the panel returns to sleep after a one-minute control window.

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
5. Tap RESET after flashing. A new installation should open the full-screen
   first-run Welcome surface. Tap **Start setup** and connect Wi-Fi using the
   scan list or hidden-network entry and on-screen keyboard.
6. Set the time zone, then follow the machine-first AirSense instructions:
   enable pairing on the AirSense, scan/select it on SomnoTrace, and enter the
   four-digit code shown on the machine. Starting from SomnoTrace before the
   AirSense is in pairing mode is expected to fail.
7. Confirm that the inserted card reports ready. Alerts and Uploads can be
   configured or explicitly deferred. Finish at the Ready receipt; do not use
   **Continue without recording** when the card is expected to hold the night's
   data.
8. In the normal shell, open **Manage > System > Diagnostics**. Reaching it
   proves that GT911 input reached LVGL, and the panel reports touch,
   framebuffer, heap and stack health.
9. Open **Manage > Connectivity** to verify the saved network. Later credential
   changes can be saved without interrupting recording; any required restart is
   a separate confirmation and remains deferred during therapy. The setup
   hotspot and <http://192.168.4.1> remain recovery routes.
10. Open <http://somnotrace.local> for the advanced features that remain
    browser-only.
11. After a completed therapy session, open **History**. It loads automatically,
    selects the newest night and resolves its summary, available signals,
    events and selected graph without blocking touch input.

## Quick acceptance checklist

- Entire 1024x600 panel shows the dashboard without shifted or repeated rows.
- **Manage > System > Diagnostics** shows `GT911 detected`, the expected
  1024x600 resolution, sensible non-zero PSRAM/internal RAM and UI stack
  headroom, plus zero RGB synchronization timeouts and touch errors. The boot
  log's `UI tree` line should also leave at least 2 KB of initialization-stack
  headroom.
- A fresh install shows the six-step first-run checklist; after completion its
  Ready receipt leads to Home, and a restart does not reopen setup.
- Tapping **Manage > System > Off now**, then touching once, turns the
  backlight off and back on without activating the control under that touch.
- Setting **System > Screen timeout** to **1 minute** while therapy is stopped
  turns the backlight off after one untouched minute; touching wakes it and
  starts a new minute. Setting it to **Never** keeps the screen awake. Restore
  the preferred timeout after this check.
- All three bottom tabs open distinct full-screen pages and remain easy to tap.
- Home's primary Start/Stop therapy action responds in one tap and does not
  open another screen.
- Ten consecutive Start/Stop cycles neither reboot the board nor produce an SD
  write error. After the final stop, **History** opens without a manual refresh.
- **Manage > System** brightness changes immediately and survives a restart.
- Wi-Fi credentials can be entered and saved on the panel; the device restarts
  only after the separate confirmation and reconnects to that network.
- An AirSense scan runs without freezing tab navigation; pairing accepts the
  four-digit code entirely on the touchscreen.
- **History** remains responsive while reading the card, lists newest nights
  first, selects the newest automatically and navigates beyond the first seven
  rows across the complete card index. Every source-backed signal among Flow,
  Pressure, Leak, Flow limit, Snore, SpO₂, Pulse and Motion can be selected.
- Fit, zoom, pan and the touch cursor update the graph, event-marker lane and
  visible-window statistics coherently. Flow reports raw 25 Hz or the explicit
  1 Hz fallback according to the selected range and available recordings.
- Opening **Manage > Logs** creates the ten-row viewer on demand. Pause/search,
  level filters and rapid vertical paging remain responsive, and leaving Logs
  releases its detail tree after any active worker completes.
- After a therapy cycle, repeatedly switching among **History**, every
  **Manage** destination, and the three bottom tabs for several minutes causes no
  restart, progressive slowdown, or SD read failure.
- During therapy, the setup hotspot is refused so the board cannot accidentally
  disconnect from the AirSense and interrupt recording.
- The Wi-Fi status dot becomes healthy after setup.
- With the board powered off, inserting the card and then booting changes the
  top **Card** status to healthy; the web status API also reports storage
  available. A card inserted while SomnoTrace is already running requires a
  restart before it can be detected.
- AirSense pairing changes the top **AirSense** status to healthy.
- During therapy, the breathing graph moves smoothly while pressure, leak,
  respiratory rate, flow limitation, and runtime update; touch navigation must
  remain responsive during graph redraws.

## Capture logs if bring-up fails

Keep the browser flasher's console open after reset, or use a serial monitor at
115200 baud. Save the boot log from `SomnoTrace ... starting up` through the
first error. Especially useful tags are `board_7b`, `display_7b`, `sd_storage`,
`netprov`, and `as11_ble`.

The 30.85 MHz/two-framebuffer/ten-line configuration is the accepted physical
7B baseline. A source or QEMU build still cannot prove the touch orientation,
power integrity or TF signal integrity of a particular board, cable, supply or
card; use the checklist on each new hardware unit.
