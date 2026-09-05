#!/usr/bin/env python3
"""Capture chrome-free 1024x600 UI frames from Espressif QEMU via QMP."""

import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
SCREEN_SCENARIOS = {
    "home": (0, (330, 563)),
    "history": (1, (512, 563)),
    "manage": (2, (694, 563)),
}
INTERACTION_SCENARIOS = (
    "history-calendar",
    "history-calendar-selection",
    "setup-wifi",
    "devices",
    "system-display-controls",
    "system-display-timeout-open",
    "logs",
    "connectivity-password-keyboard",
    "connectivity-password-revealed",
    "connectivity-password-remasked",
)
FATAL_MARKERS = (
    "Guru Meditation Error",
    "assert failed",
    "abort() was called",
    "Invalid drawing area",
)


def qmp_command(stream, execute, arguments=None):
    payload = {"execute": execute}
    if arguments is not None:
        payload["arguments"] = arguments
    stream.write(json.dumps(payload).encode() + b"\r\n")
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(f"QMP {execute} failed: {reply['error']}")
        if "return" in reply:
            return reply["return"]


def wait_for_log(process, log_path, phrase, timeout, start_offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        if any(marker in log for marker in FATAL_MARKERS):
            raise RuntimeError("QEMU reported a firmware/display failure")
        if phrase in log[start_offset:]:
            return
        time.sleep(0.1)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise TimeoutError(f"QEMU did not log {phrase!r}\n{log}")


def wait_healthy(process, log_path, duration):
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        if any(marker in log for marker in FATAL_MARKERS):
            raise RuntimeError("QEMU reported a firmware/display failure")
        time.sleep(0.1)


def failure_excerpt(log):
    for marker in FATAL_MARKERS:
        offset = log.find(marker)
        if offset >= 0:
            return log[offset:offset + 4000]
    return log[-4000:]


def connect_qmp(path, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        client = socket.socket(socket.AF_UNIX)
        try:
            client.connect(str(path))
            return client
        except (FileNotFoundError, ConnectionRefusedError):
            client.close()
            time.sleep(0.05)
    raise TimeoutError(f"QMP socket did not become ready: {path}")


def log_character_offset(log_path):
    """Return an offset compatible with wait_for_log's decoded text slices."""
    if not log_path.exists():
        return 0
    return len(log_path.read_text(errors="replace"))


def click(process, log_path, stream, x, y):
    scaled_x = round(x * 32767 / 1023)
    scaled_y = round(y * 32767 / 599)
    log_offset = log_character_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": scaled_x}},
        {"type": "abs", "data": {"axis": "y", "value": scaled_y}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        # Keep the pointer down until the guest has actually sampled it. A
        # fixed wall-clock hold can be missed when a full redraw runs slowly.
        wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=log_offset,
        )
        time.sleep(0.05)
    finally:
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    # Let LVGL sample the release before another scripted press. Page redraws
    # can otherwise make two adjacent clicks look like one held gesture.
    time.sleep(0.08)


def drag(process, log_path, stream, start, end, steps=8):
    """Send one sampled finger drag through the QEMU touch bridge."""
    sx, sy = start
    ex, ey = end
    log_offset = log_character_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {
            "axis": "x", "value": round(sx * 32767 / 1023),
        }},
        {"type": "abs", "data": {
            "axis": "y", "value": round(sy * 32767 / 599),
        }},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=log_offset,
        )
        for step in range(1, steps + 1):
            x = round(sx + (ex - sx) * step / steps)
            y = round(sy + (ey - sy) * step / steps)
            qmp_command(stream, "input-send-event", {"events": [
                {"type": "abs", "data": {
                    "axis": "x", "value": round(x * 32767 / 1023),
                }},
                {"type": "abs", "data": {
                    "axis": "y", "value": round(y * 32767 / 599),
                }},
            ]})
            time.sleep(0.04)
    finally:
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    time.sleep(0.15)


def interaction_sequence(name):
    if name == "history-calendar":
        return (
            # The deterministic 480-bin graph still has to paint before the
            # next emulated press can be sampled reliably on a loaded host.
            ((512, 563), 2.5, "emulated touch selected page 1"),
            ((232, 90), 0.75, None),
        )
    if name == "history-calendar-selection":
        return (
            ((512, 563), 2.5, "emulated touch selected page 1"),
            ((232, 90), 0.5, None),
            # Browse to August, then choose its final recorded night. Calendar
            # must remain selected while only the right detail is replaced.
            ((38, 135), 0.75, None),
            ((36, 433), 1.0, "day=20260831"),
            ((575, 204), 1.0, "signal=1"),
        )
    if name == "setup-wifi":
        return (
            ((66, 31), 0.8, "QEMU setup preview ready"),
        )
    if name == "devices":
        return (
            # Stop therapy first, let its transient notice clear, then open
            # Manage. Devices is the deterministic default Manage section.
            ((858, 458), 3.4, None),
            ((694, 563), 0.35, "emulated touch selected page 2"),
        )
    if name in ("system-display-controls", "system-display-timeout-open"):
        sequence = [
            ((694, 563), 0.35, "emulated touch selected page 2"),
            ((130, 368), 0.35, None),
            (((280, 450), (280, 180)), 0.5, "__drag__"),
        ]
        if name == "system-display-timeout-open":
            sequence.append(((720, 410), 0.5, None))
        return tuple(sequence)
    if name == "logs":
        return (
            ((694, 563), 0.35, "emulated touch selected page 2"),
            ((130, 420), 0.6, "QEMU native Logs pane ready"),
        )
    if name.startswith("connectivity-password-"):
        sequence = [
            ((694, 563), 0.35, "emulated touch selected page 2"),
            ((130, 160), 0.5, None),
            ((620, 396), 0.5, None),
            # Type "hunt" on the actual LVGL keyboard so the acceptance
            # captures exercise password masking rather than an empty hint.
            ((562, 483), 0.12, None),  # h
            ((663, 440), 0.12, None),  # u
            ((663, 526), 0.12, None),  # n
            # Wait past LVGL's brief last-character reveal before capturing
            # the masked state.
            ((462, 440), 1.0, None),   # t
        ]
        if name in ("connectivity-password-revealed",
                    "connectivity-password-remasked"):
            sequence.append(((920, 242), 0.3, None))
        if name == "connectivity-password-remasked":
            sequence.append(((920, 242), 0.3, None))
        return tuple(sequence)
    raise ValueError(f"unknown interaction state: {name}")


def validate_interaction_frame(name, payload):
    if not name.startswith("connectivity-password-"):
        return

    # The handoff's keyboard matrix occupies y=381..584. Check both the first
    # and final key bands so inherited bottom alignment cannot clip a row.
    for label, y1, y2 in (("first", 385, 410), ("last", 555, 582)):
        colours = set()
        for y in range(y1, y2, 3):
            row = y * 1024 * 3
            for x in range(20, 1004, 11):
                offset = row + x * 3
                colours.add(payload[offset:offset + 3])
        if len(colours) < 4:
            raise AssertionError(
                f"keyboard {label} key row is blank or clipped from {name}.ppm"
            )


def bright_samples(payload, bounds, threshold=80, step=2):
    """Count visibly light samples in a framebuffer rectangle."""
    x1, y1, x2, y2 = bounds
    count = 0
    for y in range(y1, y2, step):
        row = y * 1024 * 3
        for x in range(x1, x2, step):
            offset = row + x * 3
            if max(payload[offset:offset + 3]) >= threshold:
                count += 1
    return count


def light_surface_samples(payload, bounds, threshold=180, step=2):
    """Count near-neutral light surface samples, excluding bright accent ink."""
    x1, y1, x2, y2 = bounds
    count = 0
    for y in range(y1, y2, step):
        row = y * 1024 * 3
        for x in range(x1, x2, step):
            offset = row + x * 3
            if min(payload[offset:offset + 3]) >= threshold:
                count += 1
    return count


def validate_persistent_shell(name, payload, representative, interaction):
    """Reject transient screenshots taken before a touch redraw has landed."""
    if name == "setup-wifi":
        # Native setup has its own 60 px header and intentionally no ordinary
        # Home/History/Manage navigation. Require both rail and detail content.
        if bright_samples(payload, (18, 72, 272, 580)) < 180:
            raise AssertionError("first-run setup rail is absent")
        if bright_samples(payload, (292, 72, 1006, 580)) < 300:
            raise AssertionError("first-run setup detail pane is absent")
        return
    anchors = {
        "clock": ((20, 8, 205, 58), 80),
        "status": ((724, 5, 1010, 65), 45),
    }
    # The keyboard is a modal bottom sheet and deliberately covers the shared
    # navigation. Every other frame must retain all three navigation anchors.
    if not name.startswith("connectivity-password-"):
        anchors.update({
            "Home navigation": ((244, 531, 416, 594), 35),
            "History navigation": ((426, 531, 598, 594), 35),
            "Manage navigation": ((608, 531, 780, 594), 35),
        })
    for label, (bounds, minimum) in anchors.items():
        if bright_samples(payload, bounds) < minimum:
            raise AssertionError(f"persistent {label} is absent from {name}.ppm")

    if name == "history" and representative:
        # A selected night is a large inverted capsule; requiring its light
        # surface also proves the row release/click was processed.
        if bright_samples(payload, (27, 137, 326, 203), 120) < 800:
            raise AssertionError("representative History night is not selected")

    if name in ("history-calendar", "history-calendar-selection"):
        # Use the inverted Calendar segment and compact selected-day cell as
        # calendar-specific signatures. A populated List cannot satisfy these
        # checks, and a legacy detail-pane overlay cannot satisfy the graph.
        if light_surface_samples(payload, (164, 72, 300, 110)) < 1000:
            raise AssertionError("History Calendar segment is not selected")
        if light_surface_samples(payload, (20, 72, 156, 110)) > 100:
            raise AssertionError("History List segment remained selected")
        selected_cell = ((60, 192, 100, 234)
                         if name == "history-calendar"
                         else (18, 410, 58, 452))
        if light_surface_samples(payload, selected_cell) < 250:
            raise AssertionError("selected calendar day is not highlighted")
        if light_surface_samples(payload, (27, 137, 304, 203)) > 300:
            raise AssertionError("History List content did not leave the rail")
        if bright_samples(payload, (335, 125, 1000, 500)) < 800:
            raise AssertionError("History calendar covered the night detail")

    if name in (
        "devices", "system-display-controls",
        "system-display-timeout-open", "logs",
    ):
        for index, label in enumerate((
            "Devices", "Connectivity", "Alerts", "Uploads",
            "Storage", "System", "Logs", "Advanced",
        )):
            y1 = 84 + index * 52
            if bright_samples(payload, (35, y1, 225, y1 + 48)) < 20:
                raise AssertionError(f"Manage rail entry {label} is absent")

    if name == "logs":
        # The retained pane is lazy. Requiring detail content proves the Logs
        # rail touch constructed and rendered it rather than capturing the
        # intentionally empty placeholder section.
        if bright_samples(payload, (255, 72, 995, 510)) < 180:
            raise AssertionError("lazy native Logs detail pane is absent")

    if name.startswith("connectivity-password-"):
        # The literal editing frame exposes only the Password row above the
        # sheet, instead of leaving the unrelated Network name field visible.
        if bright_samples(payload, (275, 152, 993, 277)) < 120:
            raise AssertionError("dedicated Password editing row is absent")


def ppm_dimensions(path):
    with path.open("rb") as image:
        tokens = []
        while len(tokens) < 4:
            line = image.readline()
            if not line:
                break
            if line.startswith(b"#"):
                continue
            tokens.extend(line.split())
    if len(tokens) < 4 or tokens[0] != b"P6" or tokens[3] != b"255":
        raise AssertionError(f"unexpected screendump format in {path}")
    return int(tokens[1]), int(tokens[2])


def capture_screen(
    qemu, flash, efuse, output_dir, temporary, name, tab_index, point,
    settle_seconds, representative, interaction=False,
):
    # macOS limits Unix-domain socket paths to roughly one hundred bytes and
    # its per-user temporary directory can already be long. Keep the private
    # QMP filename short even when the descriptive capture name is not.
    qmp_socket = Path(temporary) / f"{name[:8]}.qmp"
    serial_log = Path(temporary) / f"{name}.serial.log"
    command = [
        qemu,
        "-M", "esp32s3",
        "-m", "8M",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-drive", f"file={efuse},if=none,format=raw,id=efuse",
        "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
        "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
        "-nic", "user,model=open_eth",
        # SDL instantiates QEMU's pointer input handler. screendump still reads
        # the guest console surface directly, so the PPM contains no host
        # window border, title bar, or cursor.
        "-display", "sdl,show-cursor=off",
        "-qmp", f"unix:{qmp_socket},server=on,wait=off",
        "-serial", f"file:{serial_log}",
        "-monitor", "none",
    ]
    process = subprocess.Popen(
        command, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    client = None
    try:
        wait_for_log(process, serial_log, "interactive UI preview ready", 15)
        # app_main returns to its simulator loop before LVGL has necessarily
        # finished the initial 1024x600 paint. Never inject a tap into that
        # first render merely because the application banner was printed.
        wait_for_log(process, serial_log, "QEMU UI first frame published", 15)
        client = connect_qmp(qmp_socket)
        stream = client.makefile("rwb", buffering=0)
        greeting = json.loads(stream.readline())
        if "QMP" not in greeting:
            raise RuntimeError(f"unexpected QMP greeting: {greeting}")
        qmp_command(stream, "qmp_capabilities")
        # The QEMU firmware deliberately announces simulated data in a
        # three-second notice. Let that transient state clear so captures show
        # the persistent header and page geometry beneath it.
        wait_healthy(process, serial_log, settle_seconds)

        if interaction:
            for tap, delay, expected_log in interaction_sequence(name):
                log_offset = log_character_offset(serial_log)
                if expected_log == "__drag__":
                    drag(process, serial_log, stream, tap[0], tap[1])
                else:
                    click(process, serial_log, stream, *tap)
                if expected_log:
                    if expected_log == "__drag__":
                        wait_healthy(process, serial_log, delay)
                        continue
                    wait_for_log(
                        process, serial_log, expected_log, 3,
                        start_offset=log_offset,
                    )
                wait_healthy(process, serial_log, delay)
        else:
            # The System acceptance frame is the healthy, therapy-stopped variant.
            # Stop on Home before navigating so the restart affordance is enabled.
            if representative and name == "manage":
                click(process, serial_log, stream, 858, 458)
                wait_healthy(process, serial_log, 3.4)

            # Home is the deterministic boot page. Select either other page through
            # the real pointer bridge before asking QEMU to read the framebuffer.
            if tab_index:
                log_offset = log_character_offset(serial_log)
                click(process, serial_log, stream, *point)
                wait_for_log(
                    process, serial_log,
                    f"emulated touch selected page {tab_index}", 3,
                    start_offset=log_offset,
                )
            if representative:
                # Match the handoff's acceptance frames: stopped Home, first night
                # selected in History, and System selected in Manage.
                if name == "home":
                    click(process, serial_log, stream, 858, 458)
                elif name == "history":
                    click(process, serial_log, stream, 150, 163)
                elif name == "manage":
                    click(process, serial_log, stream, 120, 368)
                # The Home action emits a three-second confirmation notice. The
                # acceptance frame is the persistent stopped state beneath it.
                wait_healthy(process, serial_log, 3.4 if name == "home" else 0.5)
        # Leave at least one complete post-release refresh period before the
        # first attempt. The validation loop below handles slower host loads.
        wait_healthy(process, serial_log, 0.75)
        destination = output_dir / f"{name}.ppm"
        last_validation_error = None
        for attempt in range(8):
            destination.unlink(missing_ok=True)
            qmp_command(stream, "screendump", {"filename": str(destination)})
            deadline = time.monotonic() + 3
            while not destination.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not destination.exists():
                raise TimeoutError(f"QEMU did not create {destination}")
            dimensions = ppm_dimensions(destination)
            if dimensions != (1024, 600):
                raise AssertionError(
                    f"{destination.name} is {dimensions[0]}x{dimensions[1]}, expected 1024x600"
                )
            if destination.stat().st_size < 1024 * 600 * 3:
                raise AssertionError(f"truncated framebuffer capture: {destination}")
            payload = destination.read_bytes()[-1024 * 600 * 3:]
            sampled_colours = {
                payload[offset:offset + 3]
                for offset in range(0, len(payload) - 2, 3 * 997)
            }
            if len(sampled_colours) < 8:
                last_validation_error = AssertionError(
                    f"blank or nearly uniform framebuffer capture: {destination}"
                )
            else:
                try:
                    validate_persistent_shell(
                        name, payload, representative, interaction
                    )
                    validate_interaction_frame(name, payload)
                    last_validation_error = None
                    break
                except AssertionError as error:
                    last_validation_error = error
            if attempt < 7:
                wait_healthy(process, serial_log, 0.4)
        if last_validation_error is not None:
            raise last_validation_error
        print(f"Captured {name}: {destination} (1024x600 PPM)")
    except Exception:
        log = serial_log.read_text(errors="replace") if serial_log.exists() else ""
        if log:
            print(failure_excerpt(log))
        raise
    finally:
        if client is not None:
            client.close()
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output_dir", nargs="?", default=str(ROOT / "build-qemu" / "captures"),
        help="directory for captured 1024x600 PPM frames",
    )
    parser.add_argument("--build", action="store_true", help="rebuild QEMU firmware first")
    parser.add_argument(
        "--settle-seconds", type=float, default=3.5,
        help="time to let boot notices clear before each capture (default: 3.5)",
    )
    parser.add_argument(
        "--representative", action="store_true",
        help="capture the populated/stopped states used by the design handoff",
    )
    parser.add_argument(
        "--screen", action="append", choices=tuple(SCREEN_SCENARIOS),
        help="capture only this screen (repeat for more than one)",
    )
    parser.add_argument(
        "--interaction-states", action="store_true",
        help="also capture every interactive Manage acceptance state",
    )
    parser.add_argument(
        "--interaction-state", action="append", choices=INTERACTION_SCENARIOS,
        help="capture only this interactive Manage state (repeatable)",
    )
    args = parser.parse_args()

    flash = ROOT / "build-qemu/qemu_flash.bin"
    efuse = ROOT / "build-qemu/qemu_efuse.bin"
    if args.build or not flash.exists() or not efuse.exists():
        subprocess.run([ROOT / "scripts/build-qemu.sh"], check=True)
    qemu = subprocess.check_output(
        [ROOT / "scripts/setup-qemu-macos.sh"], text=True
    ).strip()
    output_dir = Path(args.output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="somnotrace-capture-") as temporary:
        # A fresh emulator per frame prevents a synchronous screendump from
        # perturbing the touch edge used to choose the following screen.
        interaction_names = (list(INTERACTION_SCENARIOS)
                             if args.interaction_states
                             else args.interaction_state or [])
        names = (args.screen if args.screen is not None
                 else [] if interaction_names
                 else list(SCREEN_SCENARIOS))
        for name in names:
            tab_index, point = SCREEN_SCENARIOS[name]
            capture_screen(
                qemu, flash, efuse, output_dir, temporary,
                name, tab_index, point, args.settle_seconds,
                args.representative,
            )
        for name in interaction_names:
            capture_screen(
                qemu, flash, efuse, output_dir, temporary,
                name, None, None, args.settle_seconds,
                False, interaction=True,
            )


if __name__ == "__main__":
    main()
