#!/usr/bin/env python3
"""
Feeds requests built by kvmd's own encoder (kvmd/plugins/hid/_mcu/proto.py)
into the firmware's protocol core compiled for the PC (test/host_main.c),
and checks responses with kvmd's own check_response().

Usage:
    gcc -std=c11 -Wall -Wextra -O2 -o hosttest test/host_main.c main/core.c
    python3 test/test_core.py /path/to/kvmd ./hosttest
"""

import collections.abc
import os
import subprocess
import sys
import typing

# kvmd targets Python 3.13+, which allows Generator[X]; shim it for older Pythons
typing.Generator = collections.abc.Generator  # type: ignore
typing.AsyncGenerator = collections.abc.AsyncGenerator  # type: ignore

KVMD_DIR, HARNESS = sys.argv[1], sys.argv[2]
sys.path.insert(0, KVMD_DIR)

from evdev import ecodes  # noqa: E402
from kvmd.keyboard.mappings import KEYMAP  # noqa: E402
from kvmd.plugins.hid._mcu import proto  # noqa: E402

KBD_USB, ABS, REL = 0x01, 0x08, 0x10
FAILS = 0


def check(cond: bool, what: str) -> None:
    global FAILS
    if not cond:
        FAILS += 1
        print("FAIL:", what)


def run(reqs: list[bytes], env: dict | None = None, default: int | None = None) -> tuple[list[bytes], list[str]]:
    args = [HARNESS] + ([str(default)] if default is not None else [])
    p = subprocess.run(args, input=b"".join(reqs), capture_output=True, env={**os.environ, **(env or {})})
    out = p.stdout
    resps = [out[i:i + 8] for i in range(0, len(out), 8)]
    return resps, p.stderr.decode().splitlines()


def calls(log: list[str]) -> list[str]:
    return [line for line in log if not line.startswith(("init", "store"))]


# 1. Ping: valid pong, USB keyboard + absolute mouse by default
(r,), log = run([proto.REQUEST_PING])
check(proto.check_response(r), "ping CRC")
check(r[0] == 0x34 and r[1] == 0x80, f"ping pong {r.hex()}")
check(proto.get_active_keyboard(r[2]) == "usb", "active keyboard usb")
check(proto.get_active_mouse(r[2]) == "usb", "active mouse usb (absolute)")
check(r[2] & 0x80 and r[3] == 0x01, "dynamic + has_usb")

# 2. LEDs: HID Num(1)+Caps(2) -> pong NUM(4)+CAPS(1)
(r,), _ = run([proto.REQUEST_PING], env={"MOCK_LEDS": "3"})
check(r[1] == 0x80 | 0x04 | 0x01, f"leds {r.hex()}")

# 3. Offline flags
(r,), _ = run([proto.REQUEST_PING], env={"MOCK_OFFLINE": "1"})
check(r[1] & 0x08 and r[1] & 0x10, f"offline flags {r.hex()}")

# 4. Every key kvmd knows maps to kvmd's own USB usage
reqs, expected = [], []
for code, key in KEYMAP.items():
    for state in (True, False):
        reqs.append(proto.KeyEvent(code, state).make_request())
        # kvmd stores modifiers as report bitmasks; the MCU table uses usages 0xE0..0xE7
        usage = (0xE0 + key.usb.code.bit_length() - 1) if key.usb.is_mod else key.usb.code
        expected.append(f"key {usage} {int(state)}")
resps, log = run(reqs)
check(all(proto.check_response(x) and x[1] & 0x80 for x in resps), "key responses")
check(calls(log) == expected, "keymap matches kvmd for all keys")
print(f"checked {len(KEYMAP)} keys")

# 5. Mouse buttons
buttons = [(ecodes.BTN_LEFT, 1), (ecodes.BTN_RIGHT, 2), (ecodes.BTN_MIDDLE, 4), (ecodes.BTN_BACK, 8), (ecodes.BTN_FORWARD, 16)]
reqs, expected = [], []
for btn, bit in buttons:
    for state in (True, False):
        reqs.append(proto.MouseButtonEvent(btn, state).make_request())
        expected.append(f"button {bit} {int(state)}")
_, log = run(reqs)
check(calls(log) == expected, f"buttons {calls(log)}")

# 6. Absolute moves (extremes), wheel; relative ignored in absolute mode
reqs = [
    proto.MouseMoveEvent(-32768, 32767).make_request(),
    proto.MouseMoveEvent(0, -1).make_request(),
    proto.MouseWheelEvent(0, -5).make_request(),
    proto.MouseRelativeEvent(10, 10).make_request(),
]
_, log = run(reqs)
check(calls(log) == ["abs -32768 32767", "abs 0 -1", "wheel 0 -5"], f"abs mode {calls(log)}")

# 7. Switch to relative mouse: store + reset flag, then reboot into relative mode
(r,), log = run([proto.SetMouseOutputEvent("usb_rel").make_request(), proto.REQUEST_PING])
check(proto.check_response(r) and r[1] & 0x40, f"reset required {r.hex()}")
check(f"store {KBD_USB | REL}" in log and "reset" in log, f"stored rel {log}")
resps, log = run([
    proto.REQUEST_PING,
    proto.MouseRelativeEvent(-127, 127).make_request(),
    proto.MouseMoveEvent(5, 5).make_request(),
], env={"MOCK_STORED": str(KBD_USB | REL)})
check(proto.get_active_mouse(resps[0][2]) == "usb_rel", "rel after reboot")
check(not resps[0][1] & 0x40, "no reset flag after reboot")
check(calls(log) == ["rel -127 127"], f"rel mode {calls(log)}")

# 8. Unsupported or unchanged outputs don't reboot
for ev in [proto.SetKeyboardOutputEvent("ps2"), proto.SetMouseOutputEvent("ps2"),
           proto.SetMouseOutputEvent("usb_win98"), proto.SetMouseOutputEvent("usb")]:
    (r,), log = run([ev.make_request()])
    check(not r[1] & 0x40 and "reset" not in log, f"no reset for {ev}")

# 9. Disabling the keyboard
(r,), log = run([proto.SetKeyboardOutputEvent("disabled").make_request()])
check(r[1] & 0x40 and f"store {ABS}" in log, f"kbd disable {log}")
(r,), log = run([proto.REQUEST_PING, proto.KeyEvent(30, True).make_request()][:1], env={"MOCK_STORED": str(ABS)})
check(proto.get_active_keyboard(r[2]) == "disabled", "keyboard disabled after reboot")

# 10. Errors, repeat, clear, SET_CONNECTED
bad = bytearray(proto.REQUEST_PING)
bad[3] ^= 0xFF
unknown = proto._make_request(b"\x7f\x00\x00\x00\x00")
resps, log = run([bytes(bad), proto.REQUEST_REPEAT, unknown, proto.ClearEvent().make_request(),
                  proto.SetConnectedEvent(True).make_request(), proto.REQUEST_REPEAT])
codes = [x[1] for x in resps]
check(all(proto.check_response(x) for x in resps), "error responses have valid CRC")
check(codes[0] == 0x40 and codes[1] == 0x40, f"crc error + repeat {codes}")
check(codes[2] == 0x45, f"invalid command {codes}")
check(codes[3] & 0x80 and "clear" in log, "clear")
check(codes[4] & 0x80 and codes[5] & 0x80, "set_connected + repeat pong")

print("ALL PASSED" if FAILS == 0 else f"{FAILS} FAILED")
sys.exit(1 if FAILS else 0)
