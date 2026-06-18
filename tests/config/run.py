#!/usr/bin/env python3
"""Config-validation tests for the BLE host components.

Three checks, all driven by `esphome config` (schema + final-validation only — no
C++ compile, no hardware, no D-Bus):

  1. Every shipped example in examples/ validates (minus *.local.yaml / secrets).
  2. Every fixture in tests/config/fixtures/valid/ validates.
  3. Every fixture in tests/config/fixtures/invalid/ is REJECTED — and, when the
     fixture's first line is `# EXPECT_ERROR: <substr>`, the rejection message
     must contain <substr> (case-insensitive) so we know the *right* validator
     fired, not some unrelated typo.

Zero third-party deps beyond esphome itself, so CI runs it with just
`pip install esphome`. Exits non-zero if any check fails.

Usage:
    python3 tests/config/run.py            # uses `esphome` on PATH
    ESPHOME=/path/to/esphome python3 tests/config/run.py
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
EXAMPLES = REPO / "examples"
ESPHOME = os.environ.get("ESPHOME", "esphome")


def _expect_error_substr(path: Path) -> str | None:
    """Return the EXPECT_ERROR substring from a fixture's first line, if any."""
    try:
        first = path.read_text().splitlines()[0]
    except (OSError, IndexError):
        return None
    marker = "# EXPECT_ERROR:"
    if first.startswith(marker):
        return first[len(marker):].strip().lower()
    return None


def _run_config(path: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [ESPHOME, "-s", "name_add_mac_suffix", "false", "config", str(path)],
        capture_output=True,
        text=True,
    )
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    failures: list[str] = []
    n = 0

    # 1 + 2: configs that must validate.
    valid_paths = sorted((FIXTURES / "valid").glob("*.yaml"))
    example_paths = [
        p
        for p in sorted(EXAMPLES.glob("*.yaml"))
        if not p.name.endswith(".local.yaml") and p.name != "secrets.yaml"
    ]
    for path in example_paths + valid_paths:
        n += 1
        rc, out = _run_config(path)
        rel = path.relative_to(REPO)
        if rc != 0:
            failures.append(f"VALID config rejected: {rel}\n{_tail(out)}")
            print(f"FAIL  {rel}  (expected valid, got rc={rc})")
        else:
            print(f"ok    {rel}")

    # 3: configs that must be rejected (and for the right reason).
    for path in sorted((FIXTURES / "invalid").glob("*.yaml")):
        n += 1
        rc, out = _run_config(path)
        rel = path.relative_to(REPO)
        want = _expect_error_substr(path)
        if rc == 0:
            failures.append(f"INVALID config accepted: {rel} (expected rejection)")
            print(f"FAIL  {rel}  (expected rejection, but config was accepted)")
        elif want and want not in out.lower():
            failures.append(
                f"INVALID config {rel} rejected, but message lacks '{want}'\n{_tail(out)}"
            )
            print(f"FAIL  {rel}  (rejected, but not for '{want}')")
        else:
            why = f" [matched '{want}']" if want else ""
            print(f"ok    {rel}  (correctly rejected){why}")

    print(f"\nconfig: {n} fixtures, {len(failures)} failures")
    if failures:
        print("\n=== FAILURES ===")
        for f in failures:
            print("-", f)
        return 1
    print("ALL CONFIG TESTS PASSED")
    return 0


def _tail(text: str, lines: int = 8) -> str:
    return "\n".join(text.strip().splitlines()[-lines:])


if __name__ == "__main__":
    sys.exit(main())
