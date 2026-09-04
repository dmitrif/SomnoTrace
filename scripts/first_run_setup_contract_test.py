#!/usr/bin/env python3
"""Static integration contracts for first-run setup persistence."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main/first_run_setup.h").read_text(encoding="utf-8")
MODEL = (ROOT / "main/first_run_setup_model.c").read_text(encoding="utf-8")
SERVICE = (ROOT / "main/first_run_setup.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
HOST = (ROOT / "scripts/test-host.sh").read_text(encoding="utf-8")


def require(text: str, pattern: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"missing first-run setup contract: {description}")


for name in ("WIFI", "TIME", "AIRSENSE", "CARD", "ALERTS", "UPLOADS"):
    require(HEADER, rf"FIRST_RUN_SETUP_STEP_{name}", f"named {name} step")

require(HEADER, r"FIRST_RUN_SETUP_SCHEMA_VERSION\s+1U", "schema version")
require(HEADER, r"completed_mask", "completed-step mask")
require(HEADER, r"skipped_mask", "skipped-step mask")
require(HEADER, r"continue_without_recording", "explicit no-recording state")
require(HEADER, r"first_run_setup_snapshot\s*\(", "snapshot API")
require(HEADER, r"first_run_setup_update\s*\(", "transition API")
require(HEADER, r"first_run_setup_reset\s*\(", "reset API")
require(HEADER, r"first_run_setup_reconcile\s*\(", "reconciliation API")
assert "skip_all" not in HEADER.lower() and "skip_setup" not in HEADER.lower(), (
    "a generic whole-setup skip API must not be introduced"
)

require(
    MODEL,
    r"FIRST_RUN_SETUP_UPDATE_SKIP:.*?first_run_setup_step_can_skip\(step\)",
    "skip is validated per named step",
)
require(
    MODEL,
    r"case FIRST_RUN_SETUP_STEP_CARD:.*?return false;",
    "Card cannot be marked skipped",
)
require(
    MODEL,
    r"!had_persisted_state\s*&&\s*observed->established_installation",
    "legacy auto-resolution only applies without persisted progress",
)
require(
    MODEL,
    r"step == FIRST_RUN_SETUP_STEP_CARD.*?continue_without_recording = true",
    "legacy missing Card uses explicit no-recording resolution",
)

require(SERVICE, r'#define NVS_KEY_SCHEMA\s+"schema"', "stable schema key")
require(SERVICE, r'#define NVS_KEY_STATE\s+"state"', "stable state key")
require(
    SERVICE,
    r"nvs_set_u16\s*\(.*?FIRST_RUN_SETUP_SCHEMA_VERSION.*?"
    r"nvs_set_blob\s*\(.*?nvs_commit",
    "schema and state are committed together",
)
require(
    SERVICE,
    r"do_save_nvs.*?const first_run_setup_state_t state\s*=.*?nvs_open",
    "writer callback copies input before opening NVS",
)
require(
    SERVICE,
    r"nvs_close\(h\).*?\*\(first_run_setup_state_t \*\)arg = decoded",
    "reader callback copies output only after closing NVS",
)
require(
    SERVICE,
    r"first_run_setup_load\(void\).*?nvs_writer_init\(\).*?"
    r"nvs_writer_run\(do_load_nvs",
    "load initializes the proxy before any possible PSRAM-stack call",
)
require(
    SERVICE,
    r"nvs_writer_run\(do_load_nvs,\s*&s_nvs_work\)",
    "loads run through the internal-stack NVS worker",
)
require(
    SERVICE,
    r"nvs_writer_run\(do_save_nvs,\s*&s_nvs_work\)",
    "writes run through the internal-stack NVS worker",
)
require(
    SERVICE,
    r"nvs_writer_run\(do_reset_nvs,\s*NULL\)",
    "reset runs through the internal-stack NVS worker",
)

for source in ("first_run_setup.c", "first_run_setup_model.c"):
    assert f'"{source}"' in CMAKE, f"{source} is not registered in CMake"
require(HOST, r"first_run_setup_test\.c.*?first_run_setup_model\.c", "host unit test")
require(HOST, r"python3 scripts/first_run_setup_contract_test\.py", "contract test")

print("first-run setup persistence contract passed")
