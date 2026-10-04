#!/usr/bin/env python3
"""
Talk to the S3 HID from a PC the same way kvmd does, before involving the Pi.

    pip install pyserial
    python3 s3hid_test.py /dev/ttyUSB0          (Linux; macOS: /dev/cu.usbserial-*, Windows: COM5)

Plug the S3's UART/bridge port into this PC and its native USB port into a
target (it can be the same PC: open a text editor and watch it type).
"""

import struct
import sys
import time

import serial


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def request(cmd: bytes) -> bytes:
    assert len(cmd) == 5
    req = b"\x33" + cmd
    return req + struct.pack(">H", crc16(req))


def send(tty: serial.Serial, cmd: bytes) -> bytes:
    tty.reset_input_buffer()  # kvmd also drops stale bytes (e.g. boot messages) first
    tty.write(request(cmd))
    resp = tty.read(8)
    if len(resp) != 8 or resp[0] != 0x34:
        raise RuntimeError(f"bad/no response: {resp.hex() or 'nothing'}")
    if crc16(resp[:6]) != struct.unpack(">H", resp[6:])[0]:
        raise RuntimeError(f"response CRC mismatch: {resp.hex()}")
    return resp


def describe(resp: bytes) -> str:
    code = resp[1]
    if not code & 0x80:
        return {0x40: "CRC error", 0x45: "invalid command", 0x48: "timeout"}.get(code, f"code {code:#x}")
    kbd = {0x00: "disabled", 0x01: "usb"}.get(resp[2] & 0x07, "?")
    mouse = {0x00: "disabled", 0x08: "usb (absolute)", 0x10: "usb_rel"}.get(resp[2] & 0x38, "?")
    flags = [name for bit, name in [(0x01, "CAPS"), (0x04, "NUM"), (0x02, "SCROLL"),
                                    (0x08, "KBD_OFFLINE"), (0x10, "MOUSE_OFFLINE"),
                                    (0x40, "RESET_REQUIRED")] if code & bit]
    return f"OK  keyboard={kbd}  mouse={mouse}  flags={' '.join(flags) or '-'}"


# kvmd MCU key codes (see main/keymap.h)
KEYS = {chr(ord("a") + i): 1 + i for i in range(26)}
KEYS.update({str((i + 1) % 10): 27 + i for i in range(10)})
KEYS.update({"\n": 37, " ": 41, ",": 50, ".": 51})
SHIFT = 78


def key(tty: serial.Serial, code: int, pressed: bool) -> None:
    send(tty, struct.pack(">BBBxx", 0x11, code, int(pressed)))


def type_text(tty: serial.Serial, text: str) -> None:
    for ch in text:
        upper = ch.isupper()
        code = KEYS.get(ch.lower())
        if code is None:
            continue
        if upper:
            key(tty, SHIFT, True)
        key(tty, code, True)
        key(tty, code, False)
        if upper:
            key(tty, SHIFT, False)
        time.sleep(0.02)


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    # dsrdtr/rtscts off and DTR/RTS low: avoids resetting boards with auto-reset circuits
    tty = serial.Serial()
    tty.port, tty.baudrate, tty.timeout = port, 115200, 2.0
    tty.dtr = tty.rts = False
    tty.open()
    time.sleep(0.5)

    print("PING ->", describe(send(tty, b"\x01\x00\x00\x00\x00")))

    print("Typing in 3 seconds: focus a text editor on the target...")
    time.sleep(3)
    type_text(tty, "Hello from ESP32-S3.\n")

    mode = describe(send(tty, b"\x01\x00\x00\x00\x00"))
    if "absolute" in mode:
        print("Moving the pointer to the screen center and around")
        for x, y in [(0, 0), (8000, 0), (8000, 8000), (0, 8000), (0, 0)]:
            send(tty, struct.pack(">Bhh", 0x12, x, y))
            time.sleep(0.3)
    elif "usb_rel" in mode:
        print("Moving the pointer in a square")
        for dx, dy in [(50, 0), (0, 50), (-50, 0), (0, -50)]:
            for _ in range(5):
                send(tty, struct.pack(">Bbbxx", 0x15, dx // 5, dy // 5))
                time.sleep(0.02)

    print("Scrolling down 3 notches")
    for _ in range(3):
        send(tty, struct.pack(">Bxbxx", 0x14, -1))
        time.sleep(0.1)

    send(tty, b"\x10\x00\x00\x00\x00")  # CLEAR_HID: release everything
    print("PING ->", describe(send(tty, b"\x01\x00\x00\x00\x00")))
    print("Done. Press Caps Lock on the target keyboard and run again to see the CAPS flag.")


if __name__ == "__main__":
    main()
