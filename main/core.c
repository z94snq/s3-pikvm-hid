/*
 * PiKVM serial HID protocol core.
 *
 * Behaviour follows kvmd/hid/pico/src/main.c and ph_cmds.c (USB outputs only).
 * Part of KVMD - The main PiKVM daemon.
 * Copyright (C) 2018-2024  Maxim Devaev <mdevaev@gmail.com>
 * Licensed under the GNU General Public License v3 or later.
 */
#include "core.h"

#include "proto.h"
#include "keymap.h"
#include "hid.h"

static uint8_t s_active = 0;          // PROTO_OUT1_* bits currently in use
static bool s_reset_required = false;
static uint8_t s_prev_code = PROTO_RESP_NONE;

#define IS_KBD_USB          ((s_active & PROTO_OUT1_KBD_MASK) == PROTO_OUT1_KBD_USB)
#define MOUSE_MODE          (s_active & PROTO_OUT1_MOUSE_MASK)
#define IS_MOUSE_USB_ABS    (MOUSE_MODE == PROTO_OUT1_MOUSE_USB_ABS)
#define IS_MOUSE_USB_REL    (MOUSE_MODE == PROTO_OUT1_MOUSE_USB_REL)
#define IS_MOUSE_USB        (IS_MOUSE_USB_ABS || IS_MOUSE_USB_REL)


void core_init(uint8_t default_outputs) {
	int stored = outputs_storage_read();
	uint8_t out = (stored < 0 ? default_outputs : (uint8_t)stored);

	// Only USB keyboard and USB abs/rel mouse exist on this board
	uint8_t kbd = out & PROTO_OUT1_KBD_MASK;
	uint8_t mouse = out & PROTO_OUT1_MOUSE_MASK;
	if (kbd != PROTO_OUT1_KBD_USB) {
		kbd = 0;
	}
	if (mouse != PROTO_OUT1_MOUSE_USB_ABS && mouse != PROTO_OUT1_MOUSE_USB_REL) {
		mouse = 0;
	}
	s_active = kbd | mouse;
	outputs_storage_write(s_active);
	s_reset_required = false;
	s_prev_code = PROTO_RESP_NONE;
}

uint8_t core_outputs(void) {
	return s_active;
}

bool core_reset_required(void) {
	return s_reset_required;
}

// Changing an output changes the USB descriptors, so the board must reboot (like Pico HID)
static void _set_output(uint8_t mask, uint8_t value) {
	value &= mask;
	if (mask == PROTO_OUT1_KBD_MASK) {
		if (value != 0 && value != PROTO_OUT1_KBD_USB) {
			return; // PS/2 is not supported here
		}
	} else {
		if (value != 0 && value != PROTO_OUT1_MOUSE_USB_ABS && value != PROTO_OUT1_MOUSE_USB_REL) {
			return; // PS/2 and Win98 modes are not supported here
		}
	}
	const uint8_t next = (s_active & ~mask) | value;
	if (next != s_active) {
		outputs_storage_write(next);
		s_reset_required = true;
	}
}

static void _cmd_key(const uint8_t *a) {
	const uint8_t usage = keymap_usb(a[0]);
	if (usage > 0 && IS_KBD_USB) {
		hid_kbd_key(usage, a[1] != 0);
	}
}

static void _cmd_mouse_button(const uint8_t *a) {
	if (!IS_MOUSE_USB) {
		return;
	}
#	define HANDLE(x_byte, x_name, x_button) { \
		if (a[x_byte] & PROTO_MOUSE_##x_name##_SELECT) { \
			hid_mouse_button(x_button, !!(a[x_byte] & PROTO_MOUSE_##x_name##_STATE)); \
		} \
	}
	HANDLE(0, LEFT, HID_MOUSE_LEFT);
	HANDLE(0, RIGHT, HID_MOUSE_RIGHT);
	HANDLE(0, MIDDLE, HID_MOUSE_MIDDLE);
	HANDLE(1, BACK, HID_MOUSE_BACK);
	HANDLE(1, FORWARD, HID_MOUSE_FORWARD);
#	undef HANDLE
}

static void _cmd_mouse_abs(const uint8_t *a) {
	if (IS_MOUSE_USB_ABS) {
		const int16_t x = (int16_t)(((uint16_t)a[0] << 8) | a[1]);
		const int16_t y = (int16_t)(((uint16_t)a[2] << 8) | a[3]);
		hid_mouse_abs(x, y);
	}
}

static void _cmd_mouse_rel(const uint8_t *a) {
	if (IS_MOUSE_USB_REL) {
		hid_mouse_rel((int8_t)a[0], (int8_t)a[1]);
	}
}

static void _cmd_mouse_wheel(const uint8_t *a) {
	if (IS_MOUSE_USB) {
		hid_mouse_wheel((int8_t)a[0], (int8_t)a[1]);
	}
}

static uint8_t _handle_request(const uint8_t *req) {
	const uint16_t crc = ((uint16_t)req[6] << 8) | req[7];
	if (req[0] != PROTO_MAGIC || proto_crc16(req, 6) != crc) {
		return PROTO_RESP_CRC_ERROR;
	}
	const uint8_t *args = req + 2;
	switch (req[1]) {
		case PROTO_CMD_PING:            return PROTO_PONG_OK;
		case PROTO_CMD_SET_KBD:         _set_output(PROTO_OUT1_KBD_MASK, args[0]); return PROTO_PONG_OK;
		case PROTO_CMD_SET_MOUSE:       _set_output(PROTO_OUT1_MOUSE_MASK, args[0]); return PROTO_PONG_OK;
		case PROTO_CMD_SET_CONNECTED:   return PROTO_PONG_OK; // Arduino AUM only
		case PROTO_CMD_CLEAR_HID:       hid_clear(); return PROTO_PONG_OK;
		case PROTO_CMD_KBD_KEY:         _cmd_key(args); return PROTO_PONG_OK;
		case PROTO_CMD_MOUSE_BUTTON:    _cmd_mouse_button(args); return PROTO_PONG_OK;
		case PROTO_CMD_MOUSE_ABS:       _cmd_mouse_abs(args); return PROTO_PONG_OK;
		case PROTO_CMD_MOUSE_REL:       _cmd_mouse_rel(args); return PROTO_PONG_OK;
		case PROTO_CMD_MOUSE_WHEEL:     _cmd_mouse_wheel(args); return PROTO_PONG_OK;
		case PROTO_CMD_REPEAT:          return 0;
		default:                        return PROTO_RESP_INVALID_ERROR;
	}
}

static void _make_response(uint8_t code, uint8_t *resp) {
	if (code == 0) {
		code = s_prev_code; // REPEAT: resend the last code
	} else {
		s_prev_code = code;
	}

	for (int i = 0; i < 8; ++i) {
		resp[i] = 0;
	}
	resp[0] = PROTO_MAGIC_RESP;

	if (code & PROTO_PONG_OK) {
		resp[1] = PROTO_PONG_OK;
		if (s_reset_required) {
			resp[1] |= PROTO_PONG_RESET_REQUIRED;
		}
		if (IS_KBD_USB) {
			const uint8_t leds = hid_kbd_leds();
			resp[1] |= (leds & 0x01) ? PROTO_PONG_NUM : 0;
			resp[1] |= (leds & 0x02) ? PROTO_PONG_CAPS : 0;
			resp[1] |= (leds & 0x04) ? PROTO_PONG_SCROLL : 0;
			resp[1] |= hid_kbd_online() ? 0 : PROTO_PONG_KBD_OFFLINE;
		}
		if (IS_MOUSE_USB) {
			resp[1] |= hid_mouse_online() ? 0 : PROTO_PONG_MOUSE_OFFLINE;
		}
		resp[2] = PROTO_OUT1_DYNAMIC | s_active;
		resp[3] = PROTO_OUT2_HAS_USB;
	} else {
		resp[1] = code;
	}

	const uint16_t crc = proto_crc16(resp, 6);
	resp[6] = (uint8_t)(crc >> 8);
	resp[7] = (uint8_t)(crc & 0xFF);
}

void core_handle_request(const uint8_t *req, uint8_t *resp) {
	_make_response(_handle_request(req), resp);
}

void core_timeout_response(uint8_t *resp) {
	_make_response(PROTO_RESP_TIMEOUT_ERROR, resp);
}
