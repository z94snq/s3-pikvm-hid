# Project: S3 PiKVM HID

ESP32-S3 firmware that acts as PiKVM's keyboard and mouse. It is a port of
PiKVM's official Pico HID firmware (kvmd/hid/pico) to the ESP32-S3, speaking
the same serial protocol so kvmd's standard `serial` HID plugin works unchanged.

## Hardware setup
- Raspberry Pi 5 running PiKVM OS (official Pi 5 image for HDMI-USB capture)
- Elgato 4K S capture card (USB 3, UVC, MJPEG) for video
- Freenove ESP32-S3-WROOM board (8 MB PSRAM) with two USB-C ports:
  - "UART" port = WCH CH343 USB-UART bridge (USB ID 1a86:55d3) wired to UART0
    (GPIO43 TX, GPIO44 RX) → plugs into the Pi. Shows up as /dev/ttyACM0, not ttyUSB0.
  - "USB" port = native USB OTG → plugs into the target PC, appears as keyboard + mouse
- No GPIO wiring between Pi and S3, no Y-splitter, no reset wire

## Status
- Builds cleanly (0 warnings) with ESP-IDF v5.3.4 + esp_tinyusb ^2.0
- Protocol core passes host tests against kvmd's own encoder (all 126 keys, mouse modes, errors)
- 2026-10-04: works on real hardware from a plain Pi 5 (Raspberry Pi OS): PING OK,
  tools/s3hid_test.py typed and moved the mouse on the target. Opening the port
  with DTR/RTS low did not reset the board.
- Not yet tested under kvmd on PiKVM OS.

## Architecture
- `main/core.c`: whole protocol (parse 8-byte request, dispatch, build 8-byte response).
  Must stay free of ESP-IDF includes so it can be tested on a PC.
- `main/usb_hid.c`: TinyUSB descriptors, HID reports, retry of pending reports, online detection
- `main/main.c`: UART read loop, 100 ms partial-request timeout, esp_restart() after a mode change
- `main/outputs_esp.c`: selected keyboard/mouse mode kept in RTC_NOINIT memory across reboots
- `main/keymap.h`, `main/proto.h`: copied from kvmd (do not hand-edit the key table)
- `test/`: host harness + test_core.py using kvmd's real proto.py
- `tools/s3hid_test.py`: PC-side test that pings, types, moves the mouse over the serial port

## Protocol facts (verified from kvmd source, don't change without re-reading it)
- Request: [0x33][cmd][4 args][crc16 BE], Response: [0x34][code][out1][out2][0][0][crc16 BE]
- CRC is CRC-16/MODBUS (poly 0xA001, init 0xFFFF)
- 115200 baud, 8N1
- Changing keyboard/mouse output sets RESET_REQUIRED (0x40) in the pong, then the board reboots
- KBD_OFFLINE / MOUSE_OFFLINE are set when the target hasn't enumerated the device
  or has suspended it (same rule as Pico HID)
- Source of truth: https://github.com/pikvm/kvmd, files hid/pico/src/*.c and
  kvmd/plugins/hid/_mcu/proto.py, kvmd/plugins/hid/serial.py.
  When unsure about protocol behavior, read those files instead of guessing.

## PiKVM config (on the Pi)
```yaml
kvmd:
    hid:
        type: serial
        device: /dev/kvmd-hid
        reset_pin: -1      # must be explicit; kvmd defaults to GPIO4
        reset_self: true
```
Plus a udev rule symlinking the bridge chip's tty to /dev/kvmd-hid (see README;
for the Freenove board use idVendor 1a86, idProduct 55d3).
PiKVM's root filesystem is read-only: run `rw` before editing, `ro` after.

## Rules
- UART0 belongs to kvmd. Never enable the console, logs, or printf on it
  (sdkconfig.defaults sets CONFIG_ESP_CONSOLE_NONE). For debug logs, use UART1
  on spare GPIOs with a separate USB-UART adapter, behind a Kconfig option that is off by default.
- Keep CONFIG_FREERTOS_HZ=1000; the main loop relies on 1-tick UART reads.
- After any change to core.c, keymap.h or proto.h, run the host tests:
```sh
  gcc -std=c11 -Wall -Wextra -O2 -o hosttest test/host_main.c main/core.c
  python3 test/test_core.py /path/to/kvmd ./hosttest
```
  (needs `pip install evdev setproctitle gpiod` in a venv, and a clone of pikvm/kvmd)
- After any change, run `idf.py build` and fix all warnings.
- License is GPLv3 (derived from kvmd). Keep attribution headers.
- Don't add features (PS/2, mass storage, Wi-Fi) unless I ask. Reliability first.

## Build / flash
```sh
. ~/esp/esp-idf/export.sh        # ESP-IDF v5.3.4; on Linux also run once:
                                 #   python3 ~/esp/esp-idf/tools/idf_tools.py install cmake
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash     # auto-reset into download mode works on the Freenove board
python3 tools/s3hid_test.py /dev/ttyACM0
```

## Next tasks, in order
1. ~~Flash and run tools/s3hid_test.py from a PC.~~ Done 2026-10-04.
2. Move it to the Pi 5 running PiKVM OS: udev rule, override.yaml, check `journalctl -u kvmd`.
3. Test in BIOS/UEFI on the target (keyboard, relative mouse mode).
4. Check whether kvmd opening the serial port resets the board (DTR/RTS auto-reset) and fix it if so.
