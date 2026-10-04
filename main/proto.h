/*
 * PiKVM serial HID protocol constants.
 *
 * Ported from kvmd/hid/pico/src/ph_proto.h and ph_tools.h.
 * Part of KVMD - The main PiKVM daemon.
 * Copyright (C) 2018-2024  Maxim Devaev <mdevaev@gmail.com>
 * Licensed under the GNU General Public License v3 or later.
 *
 * Every request from kvmd is 8 bytes:
 *   [0x33] [cmd] [arg0] [arg1] [arg2] [arg3] [crc_hi] [crc_lo]
 * Every response is 8 bytes:
 *   [0x34] [code/pong] [outputs1] [outputs2] [0] [0] [crc_hi] [crc_lo]
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define PROTO_MAGIC                 ((uint8_t)0x33)
#define PROTO_MAGIC_RESP            ((uint8_t)0x34)

#define PROTO_RESP_NONE             ((uint8_t)0x24)
#define PROTO_RESP_CRC_ERROR        ((uint8_t)0x40)
#define PROTO_RESP_INVALID_ERROR    ((uint8_t)0x45)
#define PROTO_RESP_TIMEOUT_ERROR    ((uint8_t)0x48)

// Complex response flags (byte 1)
#define PROTO_PONG_OK               ((uint8_t)0x80)
#define PROTO_PONG_CAPS             ((uint8_t)0x01)
#define PROTO_PONG_SCROLL           ((uint8_t)0x02)
#define PROTO_PONG_NUM              ((uint8_t)0x04)
#define PROTO_PONG_KBD_OFFLINE      ((uint8_t)0x08)
#define PROTO_PONG_MOUSE_OFFLINE    ((uint8_t)0x10)
#define PROTO_PONG_RESET_REQUIRED   ((uint8_t)0x40)

// Active outputs (byte 2), also used as SET_KBD / SET_MOUSE argument
#define PROTO_OUT1_DYNAMIC          ((uint8_t)0x80)
#define PROTO_OUT1_KBD_MASK         ((uint8_t)0x07)
#define PROTO_OUT1_KBD_USB          ((uint8_t)0x01)
#define PROTO_OUT1_KBD_PS2          ((uint8_t)0x03)
#define PROTO_OUT1_MOUSE_MASK       ((uint8_t)0x38)
#define PROTO_OUT1_MOUSE_USB_ABS    ((uint8_t)0x08)
#define PROTO_OUT1_MOUSE_USB_REL    ((uint8_t)0x10)
#define PROTO_OUT1_MOUSE_PS2        ((uint8_t)0x18)
#define PROTO_OUT1_MOUSE_USB_W98    ((uint8_t)0x20)

// Available outputs (byte 3)
#define PROTO_OUT2_CONNECTABLE      ((uint8_t)0x80)
#define PROTO_OUT2_CONNECTED        ((uint8_t)0x40)
#define PROTO_OUT2_HAS_USB          ((uint8_t)0x01)
#define PROTO_OUT2_HAS_PS2          ((uint8_t)0x02)
#define PROTO_OUT2_HAS_USB_W98      ((uint8_t)0x04)

// Commands
#define PROTO_CMD_PING              ((uint8_t)0x01)
#define PROTO_CMD_REPEAT            ((uint8_t)0x02)
#define PROTO_CMD_SET_KBD           ((uint8_t)0x03)
#define PROTO_CMD_SET_MOUSE         ((uint8_t)0x04)
#define PROTO_CMD_SET_CONNECTED     ((uint8_t)0x05)
#define PROTO_CMD_CLEAR_HID         ((uint8_t)0x10)
#define PROTO_CMD_KBD_KEY           ((uint8_t)0x11)
#define PROTO_CMD_MOUSE_ABS         ((uint8_t)0x12)
#define PROTO_CMD_MOUSE_BUTTON      ((uint8_t)0x13)
#define PROTO_CMD_MOUSE_WHEEL       ((uint8_t)0x14)
#define PROTO_CMD_MOUSE_REL         ((uint8_t)0x15)

// Mouse button select/state bits (arg0: left/right/middle, arg1: back/forward)
#define PROTO_MOUSE_LEFT_SELECT     ((uint8_t)0x80)
#define PROTO_MOUSE_LEFT_STATE      ((uint8_t)0x08)
#define PROTO_MOUSE_RIGHT_SELECT    ((uint8_t)0x40)
#define PROTO_MOUSE_RIGHT_STATE     ((uint8_t)0x04)
#define PROTO_MOUSE_MIDDLE_SELECT   ((uint8_t)0x20)
#define PROTO_MOUSE_MIDDLE_STATE    ((uint8_t)0x02)
#define PROTO_MOUSE_BACK_SELECT     ((uint8_t)0x80)
#define PROTO_MOUSE_BACK_STATE      ((uint8_t)0x08)
#define PROTO_MOUSE_FORWARD_SELECT  ((uint8_t)0x40)
#define PROTO_MOUSE_FORWARD_STATE   ((uint8_t)0x04)

// CRC-16/MODBUS, same as kvmd's bitbang.make_crc16()
static inline uint16_t proto_crc16(const uint8_t *buf, size_t len) {
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < len; ++i) {
		crc ^= buf[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
		}
	}
	return crc;
}
