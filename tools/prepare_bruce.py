#!/usr/bin/env python3
"""Prepare a pinned Bruce build without resetting an existing checkout."""

from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
BRUCE = ROOT / "multi-boot" / "bruce"
BRUCE_URL = "https://github.com/BruceDevices/firmware.git"
BRUCE_TAG = "1.16.1"
BRUCE_COMMIT = "ba519c936c87b89c9c667c15b932d7faca5360d5"
PATCH = ROOT / "tools" / "bruce_multiboot.patch"
PARTITIONS = ROOT / "partitions_multiboot.csv"


def git(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", "-C", str(BRUCE), *args], text=True, capture_output=True)


def main() -> int:
    if not PATCH.is_file() or not PARTITIONS.is_file():
        print("Missing Bruce patch or partition table", file=sys.stderr)
        return 1

    if not BRUCE.exists():
        BRUCE.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", BRUCE_TAG, BRUCE_URL, str(BRUCE)],
            check=True,
        )
    elif not (BRUCE / ".git").exists():
        print(f"{BRUCE} exists and is not a Git checkout", file=sys.stderr)
        return 1

    head = git("rev-parse", "HEAD")
    if head.returncode or head.stdout.strip() != BRUCE_COMMIT:
        print(f"Bruce must be at pinned tag {BRUCE_TAG} ({BRUCE_COMMIT})", file=sys.stderr)
        return 1

    reverse = git("apply", "--reverse", "--check", str(PATCH))
    if reverse.returncode:
        check = git("apply", "--check", str(PATCH))
        if check.returncode:
            print("Bruce patch conflicts with this checkout:\n" + check.stderr, file=sys.stderr)
            return 1
        apply = git("apply", str(PATCH))
        if apply.returncode:
            print(apply.stderr, file=sys.stderr)
            return 1

    destination = BRUCE / "custom_16Mb.csv"
    desired = PARTITIONS.read_bytes()
    original = git("show", "HEAD:custom_16Mb.csv")
    if original.returncode:
        print("Cannot read Bruce's original partition table", file=sys.stderr)
        return 1
    if destination.exists() and destination.read_bytes() not in (desired, original.stdout.encode()):
        print(f"Refusing to overwrite modified {destination}", file=sys.stderr)
        return 1
    if not destination.exists() or destination.read_bytes() != desired:
        shutil.copyfile(PARTITIONS, destination)

    # Bruce's ^3.10.3 currently resolves to FastLED 3.10.5, whose `FP` type
    # conflicts with the board's `#define FP 1`. Pin the known pre-regression
    # version without replacing any other PlatformIO settings.
    platformio = BRUCE / "platformio.ini"
    old_dependency = "fastled/FastLED @^3.10.3"
    pinned_dependency = "fastled/FastLED @3.10.3"
    config = platformio.read_text()
    if pinned_dependency not in config:
        if config.count(old_dependency) != 1:
            print(f"Cannot safely pin FastLED in {platformio}", file=sys.stderr)
            return 1
        platformio.write_text(config.replace(old_dependency, pinned_dependency))
    print(f"Bruce {BRUCE_TAG} prepared in {BRUCE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
