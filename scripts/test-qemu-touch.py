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


def wait_for_log(process, log_path, phrase, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited with status {process.returncode}")
        if log_path.exists():
            for line in log_path.read_text(errors="replace").splitlines():
                if phrase in line:
                    return line.strip()
        time.sleep(0.1)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise TimeoutError(f"QEMU did not log {phrase!r}\n{log}")


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
            client = socket.socket(socket.AF_UNIX)
            client.connect(str(qmp_socket))
            stream = client.makefile("rwb", buffering=0)
            json.loads(stream.readline())
            qmp_command(stream, "qmp_capabilities")

            # Centre of the History pill in the custom three-screen navigation.
            x = round(512 * 32767 / 1023)
            y = round(559 * 32767 / 599)
            qmp_command(stream, "input-send-event", {"events": [
                {"type": "abs", "data": {"axis": "x", "value": x}},
                {"type": "abs", "data": {"axis": "y", "value": y}},
                {"type": "btn", "data": {"button": "left", "down": True}},
            ]})
            pressed_registers = qmp_command(stream, "human-monitor-command", {
                "command-line": "xp /2wx 0x2100001c",
            })
            print(f"Touch registers while pressed: {pressed_registers.strip()}")
            time.sleep(0.3)
            qmp_command(stream, "input-send-event", {"events": [
                {"type": "btn", "data": {"button": "left", "down": False}},
            ]})
            registers = qmp_command(stream, "human-monitor-command", {
                "command-line": "xp /2wx 0x2100001c",
            })
            print(f"Touch registers after synthetic click: {registers.strip()}")
            observed = wait_for_log(process, serial_log, "emulated touch at", 3)
            selected = wait_for_log(
                process, serial_log, "emulated touch selected page 1", 3
            )
            print(
                f"QEMU touch smoke test passed: {observed}; {selected}; "
                f"{registers.strip()}"
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
