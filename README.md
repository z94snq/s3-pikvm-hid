# S3 PiKVM HID

ESP32-S3 firmware that acts as PiKVM's keyboard and mouse, with no GPIO wiring.
It speaks the same serial protocol as PiKVM's official Pico HID, so kvmd's
standard `serial` HID plugin drives it unchanged.

```
PiKVM (kvmd) ──USB──► [S3 "UART/COM" port] ─ USB-UART bridge ─ UART0 ─┐
                                                                      │ firmware
Target PC    ◄──USB── [S3 "USB" native port] ◄── keyboard + mouse ────┘
```

What works: USB keyboard (all keys kvmd knows, Caps/Num/Scroll Lock LEDs reported
back to the web UI), absolute mouse, relative boot-protocol mouse (for BIOSes),
wheel, back/forward buttons, switching mouse mode from the web UI, remote wakeup.

Not supported: PS/2, Win98 mouse mode, virtual CD-ROM/flash drive.

## Hardware

Any ESP32-S3 board with **two USB-C ports**, where one is the native USB
(often labelled `USB`) and the other goes through a USB-UART chip
(CP2102, CH340/CH343; often labelled `UART` or `COM`). The ESP32-S3-DevKitC-1
and most clones are like this. The defaults assume that chip is wired to UART0
(GPIO43/44); change it in menuconfig if your board differs.

Tested on a **Freenove ESP32-S3-WROOM** board (CH343 bridge, `1a86:55d3`,
shows up as `/dev/ttyACM0`) with PiKVM OS on a Raspberry Pi 5: keyboard,
absolute and relative mouse, mode switching, and BIOS/UEFI setup all work.
Other boards should work the same way but haven't been tried.

## Build and flash

You need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) 5.1 or newer.

```sh
cd s3-pikvm-hid
idf.py set-target esp32s3
idf.py menuconfig        # optional: "S3 PiKVM HID" menu
idf.py build
idf.py -p /dev/ttyACM0 flash    # use the UART/COM port (CP210x/CH340 boards: /dev/ttyUSB0)
```

The first build downloads `esp_tinyusb` and `tinyusb` from the Espressif component registry.

After flashing, the board is silent on the serial port. Logs are disabled on
purpose, since that port belongs to kvmd.

## Test from a PC first

This checks the firmware without involving the Pi:

1. Plug the S3's **UART** port into your PC and its **USB** port into a target.
   The target can be the same PC.
2. Open a text editor on the target.
3. Run:

```sh
pip install pyserial
python3 tools/s3hid_test.py /dev/ttyACM0      # CP210x/CH340: /dev/ttyUSB0, Windows: COM5, macOS: /dev/cu.usbserial-*
```

It should print `PING -> OK keyboard=usb mouse=usb (absolute)`, type
"Hello from ESP32-S3.", move the pointer, and scroll.

If PING fails, check the port name and that you used the UART port.
If PING works but nothing types, check the native USB cable (it must carry data).

## Set up PiKVM

1. Plug the S3's **UART** port into a Pi 5 USB port and its **USB** port into the target.

2. Find the bridge chip's IDs:

   ```sh
   lsusb
   # e.g. "10c4:ea60 Silicon Labs CP210x"  or  "1a86:55d3 QinHeng CH343"
   ```

3. Make the filesystem writable and add a udev rule (replace the IDs with yours):

   ```sh
   rw
   cat > /etc/udev/rules.d/99-kvmd-extra.rules << 'EOF'
   SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", SYMLINK+="kvmd-hid"
   EOF
   ```

4. Add this to `/etc/kvmd/override.yaml` (merge it if the file already has a `kvmd:` section):

   ```yaml
   kvmd:
       hid:
           type: serial
           device: /dev/kvmd-hid
           reset_pin: -1      # no reset wire (kvmd defaults to GPIO4 otherwise)
           reset_self: true   # the S3 reboots itself after a mode change
   ```

5. Apply and lock the filesystem again:

   ```sh
   udevadm control --reload && udevadm trigger --action=add --subsystem-match=tty
   udevadm settle
   ls -l /dev/kvmd-hid          # should point to ttyACM0 (CH343) or ttyUSB0 (CP210x/CH340)
   systemctl restart kvmd
   ro
   ```

6. Open the PiKVM web UI. The keyboard and mouse indicators should turn online.

## Mouse modes

- **Absolute** (default): the pointer goes exactly where you point. Works in
  modern OSes and most UEFI setups.
- **Relative**: a standard boot-protocol mouse. Use it if your BIOS ignores the
  absolute mouse.

Switch modes in the web UI's System menu. The S3 reboots for about a second
and keeps the mode until it loses power. To change the mode it starts with,
use `idf.py menuconfig` → S3 PiKVM HID → Mouse mode after power-on.

## Troubleshooting

**HID flips offline/online when kvmd starts.** Some boards' auto-reset circuit
resets the S3 when the serial port is opened. kvmd retries, so it usually
recovers within a second or two. If it keeps happening, check your board's
schematic for the DTR/RTS → EN/GPIO0 transistors; a few boards have a jumper
to disable them. The Freenove ESP32-S3-WROOM (CH343) does not reset: kvmd
raises DTR and RTS together, which its auto-reset circuit ignores.

**Keyboard works in the OS but not in the BIOS.** The keyboard is already a boot
keyboard. If only the mouse is missing, switch to relative mode.

**`/dev/kvmd-hid` doesn't appear.** Re-check the IDs in the udev rule against
`lsusb`, then run `udevadm trigger` again.

**Back-powering.** With both ports connected, the board gets 5V from the Pi and
the target. DevKitC boards have diodes on both inputs; check your clone's
schematic before leaving it connected long-term.

## Developing

`main/core.c` holds the whole protocol and has no ESP-IDF dependencies, so it
can be tested on a PC against kvmd's own encoder:

```sh
git clone --depth 1 https://github.com/pikvm/kvmd
pip install evdev setproctitle gpiod
gcc -std=c11 -Wall -Wextra -O2 -o hosttest test/host_main.c main/core.c
python3 test/test_core.py ./kvmd ./hosttest
```

The tests cover all 126 keys, mouse buttons, both mouse modes, mode switching,
and error handling. If kvmd changes its key table, refresh `main/keymap.h` from
kvmd's `hid/pico/src/ph_usb_keymap.h` and rerun the tests.

| File | Purpose |
| --- | --- |
| `main/core.c` | Protocol: parse requests, dispatch, build responses |
| `main/usb_hid.c` | TinyUSB descriptors, HID reports, retries, online detection |
| `main/main.c` | UART loop, 100 ms partial-request timeout, reboot on mode change |
| `main/outputs_esp.c` | Keeps the selected mode in RTC memory across reboots |
| `main/keymap.h`, `main/proto.h` | Key table and protocol constants from kvmd |
| `tools/s3hid_test.py` | Standalone PC test |
| `test/` | Host tests of the protocol core |

## License

GPLv3, because the protocol handling, report descriptors and key table are
ported from [kvmd](https://github.com/pikvm/kvmd)'s Pico HID firmware
(Copyright © 2018-2024 Maxim Devaev).
