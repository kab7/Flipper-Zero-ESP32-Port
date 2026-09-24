#!/usr/bin/env python3
"""Validate CI images and make a LilyGO T-Embed CC1101 dual-boot release."""

import argparse
import hashlib
from pathlib import Path
import struct
from zipfile import ZIP_DEFLATED, ZipFile


FLASH_SIZE = 0x1000000
PARTITIONS = {
    "nvs": (0x9000, 0x6000),
    "phy_init": (0xF000, 0x1000),
    "ota_0": (0x10000, 0x5F0000),
    "bruce": (0x600000, 0x5F0000),
    "otadata": (0xBF0000, 0x2000),
    "coredump": (0xBF2000, 0x20000),
    "spiffs": (0xC20000, 0x3E0000),
}
IMAGES = (
    ("bootloader.bin", 0x0000, 0x8000),
    ("partition-table.bin", 0x8000, 0x1000),
    ("furi_esp32.bin", 0x10000, 0x5F0000),
    ("bruce.bin", 0x600000, 0x5F0000),
    ("otadata-empty.bin", 0xBF0000, 0x2000),
)


def check_partition_table(data: bytes) -> None:
    actual = {}
    for position in range(0, len(data), 32):
        entry = data[position : position + 32]
        if len(entry) < 32:
            break
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xEBEB:
            break  # ESP-IDF partition-table MD5 entry
        if magic != 0x50AA:
            raise ValueError(f"Invalid partition-table entry at {position:#x}")
        _, _, _, offset, size, name, _ = struct.unpack("<HBBII16sI", entry)
        actual[name.split(b"\0", 1)[0].decode("ascii")] = (offset, size)
    if actual != PARTITIONS:
        raise ValueError(f"Unexpected partition layout: {actual!r}")


def package(flipper: Path, bruce: Path, output: Path) -> None:
    sources = {
        "bootloader.bin": flipper / "bootloader" / "bootloader.bin",
        "partition-table.bin": flipper / "partition_table" / "partition-table.bin",
        "furi_esp32.bin": flipper / "furi_esp32.bin",
        "bruce.bin": bruce,
    }
    contents = {name: path.read_bytes() for name, path in sources.items()}
    contents["otadata-empty.bin"] = b"\xff" * 0x2000
    check_partition_table(contents["partition-table.bin"])

    for name, offset, maximum in IMAGES:
        data = contents[name]
        if not data or len(data) > maximum or offset + len(data) > FLASH_SIZE:
            raise ValueError(f"{name} does not fit its flash region")
        if name in ("bootloader.bin", "furi_esp32.bin", "bruce.bin") and data[0] != 0xE9:
            raise ValueError(f"{name} is not an ESP image")

    output.mkdir(parents=True, exist_ok=True)
    for name, data in contents.items():
        (output / name).write_bytes(data)

    install = """LilyGO T-Embed CC1101 dual boot (ESP32-S3, 16 MB)

Use the web flasher for the easiest installation. For a command-line install,
connect the board over USB and run from this directory:

esptool --chip esp32s3 --port PORT --baud 460800 write_flash \\
  --flash_mode dio --flash_freq 80m --flash_size 16MB \\
  0x0000 bootloader.bin 0x8000 partition-table.bin \\
  0x10000 furi_esp32.bin 0x600000 bruce.bin \\
  0xBF0000 otadata-empty.bin

Replace PORT with your serial port. Do not use the upstream single-app flasher
or normal OTA update with this dual-boot layout. This command preserves NVS and
the Bruce filesystem, but resets the active boot slot so Flipper starts first.
The release has not yet been tested on a physical T-Embed CC1101.
"""
    (output / "INSTALL.txt").write_text(install)
    bundle = output / "lilygo-t-embed-cc1101-dualboot.zip"
    with ZipFile(bundle, "w", compression=ZIP_DEFLATED) as archive:
        for name in contents:
            archive.write(output / name, name)
        archive.write(output / "INSTALL.txt", "INSTALL.txt")

    files = [*contents, "INSTALL.txt", bundle.name]
    checksums = "".join(
        f"{hashlib.sha256((output / name).read_bytes()).hexdigest()}  {name}\n"
        for name in files
    )
    (output / "SHA256SUMS").write_text(checksums)
    print(f"Packaged {len(files)} files in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--flipper", type=Path, required=True)
    parser.add_argument("--bruce", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.flipper, args.bruce, args.output)
