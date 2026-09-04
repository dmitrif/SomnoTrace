#!/usr/bin/env python3
"""Boot the QEMU UI and verify an absolute host click reaches LVGL."""

import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def qmp_command(stream, execute, arguments=None):
    payload = {"execute": execute}
    if arguments is not None:
        payload["arguments"] = arguments
    stream.write(json.dumps(payload).encode() + b"\r\n")
    while True:
        reply = json.loads(stream.readline())
        if "error" in reply:
            raise RuntimeError(reply["error"])
        if "return" in reply:
            return reply["return"]


def log_character_offset(log_path):
    """Return an offset compatible with decoded serial-log text slices."""
    if not log_path.exists():
        return 0
    return len(log_path.read_text(errors="replace"))


def wait_for_log(process, log_path, phrase, timeout, start_offset=0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        if log_path.exists():
            log = log_path.read_text(errors="replace")
            for line in log[start_offset:].splitlines():
                if phrase in line:
                    return line.strip()
        time.sleep(0.1)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise TimeoutError(f"QEMU did not log {phrase!r}\n{log}")


def tap(process, log_path, stream, pixel_x, pixel_y):
    """Send one touch and hold it until LVGL has sampled the press edge."""
    scaled_x = round(pixel_x * 32767 / 1023)
    scaled_y = round(pixel_y * 32767 / 599)
    start_offset = log_character_offset(log_path)
    qmp_command(stream, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": scaled_x}},
        {"type": "abs", "data": {"axis": "y", "value": scaled_y}},
        {"type": "btn", "data": {"button": "left", "down": True}},
    ]})
    try:
        observed = wait_for_log(
            process, log_path, "emulated touch at", 3,
            start_offset=start_offset,
        )
        time.sleep(0.05)
    finally:
        qmp_command(stream, "input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
    # Give LVGL a polling interval to observe release before another press.
    time.sleep(0.08)
    return observed, start_offset


def main():
    qemu = subprocess.check_output(
        [ROOT / "scripts/setup-qemu-macos.sh"], text=True
    ).strip()
    flash = ROOT / "build-qemu/qemu_flash.bin"
    efuse = ROOT / "build-qemu/qemu_efuse.bin"
    if not flash.exists() or not efuse.exists():
        subprocess.run([ROOT / "scripts/build-qemu.sh"], check=True)

    with tempfile.TemporaryDirectory(prefix="somnotrace-touch-") as temporary:
        qmp_socket = Path(temporary) / "qmp.sock"
        serial_log = Path(temporary) / "serial.log"
        command = [
            qemu,
            "-M", "esp32s3",
            "-m", "8M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
            "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth",
            "-display", "sdl,show-cursor=on",
            "-qmp", f"unix:{qmp_socket},server=on,wait=off",
            "-serial", f"file:{serial_log}",
            "-monitor", "none",
        ]
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_for_log(process, serial_log, "interactive UI preview ready", 12)
            wait_for_log(process, serial_log, "QEMU UI first frame published", 15)
            client = socket.socket(socket.AF_UNIX)
            client.connect(str(qmp_socket))
            stream = client.makefile("rwb", buffering=0)
            json.loads(stream.readline())
            qmp_command(stream, "qmp_capabilities")

            # Centre of the History pill in the custom three-screen navigation.
            x = round(512 * 32767 / 1023)
            y = round(559 * 32767 / 599)
            click_started = time.monotonic()
            qmp_command(stream, "input-send-event", {"events": [
                {"type": "abs", "data": {"axis": "x", "value": x}},
                {"type": "abs", "data": {"axis": "y", "value": y}},
                {"type": "btn", "data": {"button": "left", "down": True}},
            ]})
            # Read only the position register here. Reading the adjacent
            # status register would itself consume the emulator's one-shot
            # short-click latch before the guest gets a chance to poll it.
            pressed_position = qmp_command(stream, "human-monitor-command", {
                "command-line": "xp /1wx 0x2100001c",
            })
            print(f"Touch position while pressed: {pressed_position.strip()}")
            qmp_command(stream, "input-send-event", {"events": [
                {"type": "btn", "data": {"button": "left", "down": False}},
            ]})
            observed = wait_for_log(process, serial_log, "emulated touch at", 3)
            selected = wait_for_log(
                process, serial_log, "emulated touch selected page 1", 3
            )
            registers = qmp_command(stream, "human-monitor-command", {
                "command-line": "xp /2wx 0x2100001c",
            })
            print(f"Touch registers after synthetic click: {registers.strip()}")
            click_latency = time.monotonic() - click_started
            if click_latency > 1.5:
                raise AssertionError(
                    f"QEMU navigation click took {click_latency:.2f}s"
                )

            # Exercise the complete manual-sleep interaction from the current
            # History page. Destination controls react on touch-down; command
            # controls still require a press/release gesture.
            _, home_offset = tap(process, serial_log, stream, 330, 559)
            home_selected = wait_for_log(
                process, serial_log, "emulated touch selected page 0", 3,
                start_offset=home_offset,
            )

            # QEMU boots with therapy running. Stop it through the same Home
            # command used on hardware, then allow its worker to publish the
            # stopped state before entering Manage.
            tap(process, serial_log, stream, 858, 458)
            time.sleep(0.5)

            _, manage_offset = tap(process, serial_log, stream, 694, 559)
            manage_selected = wait_for_log(
                process, serial_log, "emulated touch selected page 2", 3,
                start_offset=manage_offset,
            )
            tap(process, serial_log, stream, 130, 256)  # Display rail row
            time.sleep(0.2)

            _, off_offset = tap(process, serial_log, stream, 898, 449)
            backlight_off = wait_for_log(
                process, serial_log, "backlight off", 3,
                start_offset=off_offset,
            )

            # The wake layer must consume this first press over History. If it
            # leaks through, the page-selection log will appear in this exact
            # post-sleep interval and fail the smoke test.
            _, wake_offset = tap(process, serial_log, stream, 512, 559)
            backlight_on = wait_for_log(
                process, serial_log, "backlight on", 3,
                start_offset=wake_offset,
            )
            time.sleep(0.25)
            wake_end = log_character_offset(serial_log)
            wake_log = serial_log.read_text(errors="replace")[wake_offset:wake_end]
            if "emulated touch selected page 1" in wake_log:
                raise AssertionError(
                    "first touch after screen-off selected History instead of "
                    "only waking the display"
                )

            _, second_history_offset = tap(
                process, serial_log, stream, 512, 559
            )
            history_after_wake = wait_for_log(
                process, serial_log, "emulated touch selected page 1", 3,
                start_offset=second_history_offset,
            )
            print(
                f"QEMU touch smoke test passed: {observed}; {selected}; "
                f"{registers.strip()}; {click_latency:.2f}s; "
                f"{home_selected}; {manage_selected}; {backlight_off}; "
                f"{backlight_on}; wake-only first touch; {history_after_wake}"
            )
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
