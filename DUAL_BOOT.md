# T-Embed CC1101: Flipper + Bruce

This fork restores the two-firmware layout on the 16 MB LilyGo T-Embed CC1101.
It starts from upstream `main` after the display-sleep and BQ25896 ship-mode
power-off changes. Bruce is pinned to official release `1.16.1`.

## Layout and switching

- Flipper: `ota_0` at `0x10000` (size `0x5F0000`).
- Bruce: `ota_1` at `0x600000` (size `0x5F0000`, label `bruce`).
- NVS and PHY keep the offsets used by upstream v2.0; Bruce's LittleFS gets a
  `spiffs` partition at `0xC20000`.
- In Flipper, open the desktop lock menu and choose **Switch to Bruce**. The
  entry appears when a Bruce app descriptor is present. In Bruce, choose
  **Flipper Zero → Reboot to Flipper** from its main menu.

Normal WiFi/USB OTA firmware updates are disabled in this layout because their
inactive target is Bruce. The firmware refuses such a write before erasing the
slot. To update either firmware, rebuild and flash over USB. Do not use the
upstream full-flash package: it has a different partition table.

## Flash a ready-made build

The [fork's web flasher](https://kab7.github.io/Flipper-Zero-ESP32-Port/) is
for the **16 MB LilyGO T-Embed CC1101 only**. Open it in Chrome or Edge on a
computer, connect the board over USB, select its serial port, and confirm the
full dual-boot install. The page writes the bootloader, partition table,
Flipper, Bruce, and an empty `otadata` selector so Flipper starts first. It
does not erase the entire flash: NVS and the Bruce filesystem are preserved.

The [preview release](https://github.com/kab7/Flipper-Zero-ESP32-Port/releases/tag/v2.0.0-dualboot.2)
also has each binary, a ZIP with all required files and command-line flashing
instructions, and SHA-256 checksums. If the web flasher cannot connect, put
the board into download mode with its BOOT button and retry. Switching from the
updated Flipper to an already installed Bruce and back has been confirmed on one
physical T-Embed. The full `.2` bundle has not yet been tested on hardware;
keep a copy of your existing data.

## Build and flash from source

Install ESP-IDF v5.4.1 and PlatformIO, then run:

```sh
./buildAndFlash_T-Embed.sh --build-only
./buildAndFlash_T-Embed.sh --port /dev/cu.usbmodemXXXX
```

The script downloads the official Bruce `1.16.1` source to
`multi-boot/bruce/`, applies `tools/bruce_multiboot.patch`, gives both builds
the same partition table, builds both images, and flashes them. It erases only
the `otadata` selector after flashing so Flipper boots first; it does not erase
NVS or the SD card. `--skip-bruce` updates Flipper without touching Bruce's
slot. For the first dual-boot installation, build and flash both.

## Experimental battery modes

**Power Off** uses the BQ25896 BATFET ship mode. On the tested T-Embed variant
without external antennas, the board could only be woken by reconnecting USB;
the normal buttons did not wake it. Do not choose this mode if button wake is
required. This hardware-dependent behaviour is not fixed by firmware.

**Deep Sleep** should wake from the encoder/BOOT button. The current test build
holds the CC1101/LED supply and LCD backlight OFF during sleep and enables an
RTC pull-up on the wake button. BLE is unavailable in this mode. Battery drain
on physical hardware has not yet been measured with these changes.

With the screen off and USB disconnected, the running firmware now permits
automatic **Light Sleep** while keeping BLE available. The input service
checks VBUS once per second instead of every 4 ms, and the BLE controller may
sleep between radio events. The stock companion's IR/Sub-GHz controls use the
existing RPC connection; a Sub-GHz one-tap command is now handled as a single
transmission (subject to the existing regional transmit restrictions).

After an unplugged idle period, reconnect USB without rebooting and run
`power sleep` in the Flipper CLI. It reports actual light-sleep entries and
accumulated time since boot. Use `power diag` after waking from Deep Sleep to
see the previous shutdown path and wake cause. For a battery comparison,
record percentage, battery voltage and remaining mAh before and after 24 hours.
These counters and battery readings are diagnostic; they do not replace a
direct current measurement for finding board-level leakage.
