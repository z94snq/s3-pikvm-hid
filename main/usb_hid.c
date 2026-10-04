/*
 * USB HID keyboard + mouse on the ESP32-S3 native USB port.
 *
 * Report descriptors and report logic follow kvmd/hid/pico/src/ph_usb*.c.
 * Part of KVMD - The main PiKVM daemon.
 * Copyright (C) 2018-2024  Maxim Devaev <mdevaev@gmail.com>
 * Licensed under the GNU General Public License v3 or later.
 */
#include "usb_hid.h"

#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#include "proto.h"
#include "hid.h"


// ===== Report descriptors (same as Pico HID) =====
static const uint8_t KBD_DESC[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,         // Generic Desktop / Keyboard / Application
	0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,         // Modifiers E0..E7
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,         // Reserved byte
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08,         // 5 LEDs out
	0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01,         // LED padding
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00,         // 6 keys
	0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00,
	0x2A, 0xFF, 0x00, 0x81, 0x00,
	0xC0,
};

static const uint8_t MOUSE_ABS_DESC[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,         // Generic Desktop / Mouse / Application
	0x09, 0x01, 0xA1, 0x00,                     // Pointer / Physical (needed by Apple Recovery)
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08,         // 8 buttons
	0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31,         // X, Y: 0..32767 absolute
	0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
	0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,         // Wheel -127..127 relative
	0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	0xC0, 0xC0,
};

static const uint8_t MOUSE_REL_DESC[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,         // Generic Desktop / Mouse / Application
	0x09, 0x01, 0xA1, 0x00,                     // Pointer / Physical
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08,         // 8 buttons
	0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, // X, Y, Wheel: -127..127 relative
	0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
	0xC0, 0xC0,
};


// ===== State =====
static int s_kbd_inst = -1;     // TinyUSB HID instance index, -1 if not exposed
static int s_mouse_inst = -1;
static bool s_mouse_abs = true;

static volatile uint8_t s_kbd_leds = 0; // Written from the TinyUSB task
static bool s_kbd_online = true;
static bool s_mouse_online = true;

static uint8_t s_kbd_mods = 0;
static uint8_t s_kbd_keys[6] = {0};
static bool s_kbd_pending = false;

static uint8_t s_mouse_buttons = 0;
static int16_t s_mouse_x = 0;   // kvmd coordinates, -32768..32767
static int16_t s_mouse_y = 0;
static int32_t s_mouse_dx = 0;  // Accumulated, not yet sent
static int32_t s_mouse_dy = 0;
static int32_t s_mouse_wheel = 0;
static bool s_mouse_pending = false;


// ===== USB descriptors =====
static tusb_desc_device_t s_device_desc = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = CONFIG_S3HID_USB_VID,
	.idProduct = CONFIG_S3HID_USB_PID,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

static char s_serial[16];
static const char *s_strings[] = {
	(const char[]){0x09, 0x04},     // 0: English (0x0409)
	CONFIG_S3HID_USB_MANUFACTURER,  // 1
	CONFIG_S3HID_USB_PRODUCT,       // 2
	s_serial,                       // 3: MAC-based serial
};

#define EP_SIZE 16
static uint8_t s_config_desc[TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN];

static void _build_config_desc(uint8_t outputs) {
	size_t offset = TUD_CONFIG_DESC_LEN;
	uint8_t iface = 0;
	uint8_t ep = 0x81;

#	define APPEND(x_proto, x_desc, x_inst) { \
		const uint8_t part[] = {TUD_HID_DESCRIPTOR(iface, 0, x_proto, sizeof(x_desc), ep, EP_SIZE, 1)}; \
		memcpy(s_config_desc + offset, part, sizeof(part)); \
		offset += sizeof(part); \
		x_inst = iface; \
		++iface; \
		++ep; \
	}
	if ((outputs & PROTO_OUT1_KBD_MASK) == PROTO_OUT1_KBD_USB) {
		APPEND(HID_ITF_PROTOCOL_KEYBOARD, KBD_DESC, s_kbd_inst);
	}
	const uint8_t mouse = outputs & PROTO_OUT1_MOUSE_MASK;
	if (mouse == PROTO_OUT1_MOUSE_USB_ABS) {
		s_mouse_abs = true;
		APPEND(HID_ITF_PROTOCOL_NONE, MOUSE_ABS_DESC, s_mouse_inst);
	} else if (mouse == PROTO_OUT1_MOUSE_USB_REL) {
		s_mouse_abs = false;
		APPEND(HID_ITF_PROTOCOL_MOUSE, MOUSE_REL_DESC, s_mouse_inst); // Boot mouse: works in BIOS
	}
#	undef APPEND

	const uint8_t head[] = {TUD_CONFIG_DESCRIPTOR(1, iface, 0, offset, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100)};
	memcpy(s_config_desc, head, sizeof(head));
}

bool usb_hid_init(uint8_t outputs) {
	_build_config_desc(outputs);
	if (s_kbd_inst < 0 && s_mouse_inst < 0) {
		return false; // Everything disabled: don't enumerate at all
	}

	uint8_t mac[6] = {0};
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
	cfg.descriptor.device = &s_device_desc;
	cfg.descriptor.string = s_strings;
	cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
	cfg.descriptor.full_speed_config = s_config_desc;
	return (tinyusb_driver_install(&cfg) == ESP_OK);
}


// ===== Report senders (called from the main task only) =====
static bool _usb_wake_if_suspended(void) {
	if (tud_suspended()) {
		tud_remote_wakeup(); // No-op unless the host enabled remote wakeup
		return true;
	}
	return false;
}

static void _kbd_flush(void) {
	if (!s_kbd_pending || s_kbd_inst < 0) {
		return;
	}
	if (!tud_mounted()) {
		return; // Keep the state; it is sent once the host configures us
	}
	if (_usb_wake_if_suspended()) {
		return;
	}
	if (tud_hid_n_ready(s_kbd_inst)) {
		if (tud_hid_n_keyboard_report(s_kbd_inst, 0, s_kbd_mods, s_kbd_keys)) {
			s_kbd_pending = false;
		}
	}
}

static int8_t _take_s8(int32_t *acc) {
	int32_t v = *acc;
	if (v > 127) {
		v = 127;
	} else if (v < -127) {
		v = -127;
	}
	*acc -= v;
	return (int8_t)v;
}

static void _mouse_flush(void) {
	if (!s_mouse_pending || s_mouse_inst < 0) {
		return;
	}
	if (!tud_mounted()) {
		// Don't replay stale motion after (re)connection
		s_mouse_dx = s_mouse_dy = s_mouse_wheel = 0;
		s_mouse_pending = false;
		return;
	}
	if (_usb_wake_if_suspended()) {
		return;
	}
	if (!tud_hid_n_ready(s_mouse_inst)) {
		return; // Stay pending, retried by usb_hid_task()
	}

	bool sent;
	if (s_mouse_abs) {
		int32_t wheel = s_mouse_wheel;
		struct TU_ATTR_PACKED {
			uint8_t buttons;
			uint16_t x;
			uint16_t y;
			int8_t v;
		} report = {
			s_mouse_buttons,
			(uint16_t)(((int32_t)s_mouse_x + 32768) / 2),
			(uint16_t)(((int32_t)s_mouse_y + 32768) / 2),
			_take_s8(&wheel),
		};
		sent = tud_hid_n_report(s_mouse_inst, 0, &report, sizeof(report));
		if (sent) {
			s_mouse_wheel = wheel;
		}
	} else {
		int32_t dx = s_mouse_dx, dy = s_mouse_dy, wheel = s_mouse_wheel;
		struct TU_ATTR_PACKED {
			uint8_t buttons;
			int8_t x;
			int8_t y;
			int8_t v;
		} report = {s_mouse_buttons, _take_s8(&dx), _take_s8(&dy), _take_s8(&wheel)};
		sent = tud_hid_n_report(s_mouse_inst, 0, &report, sizeof(report));
		if (sent) {
			s_mouse_dx = dx;
			s_mouse_dy = dy;
			s_mouse_wheel = wheel;
		}
	}
	if (sent) {
		// Large accumulated movements are split into several reports
		s_mouse_pending = (s_mouse_dx != 0 || s_mouse_dy != 0 || s_mouse_wheel != 0);
	}
}

static void _mouse_update(void) {
	s_mouse_pending = true;
	_mouse_flush();
}

// Online detection with a 50 ms debounce, as in Pico HID
static void _check_online(int inst, bool *online, bool *prev, int64_t *offline_ts, int64_t now) {
	const bool cur = (tud_ready() && tud_hid_n_ready(inst));
	if (cur) {
		*online = true;
		*offline_ts = 0;
	} else if (*prev) {
		*offline_ts = now;
	} else if (*offline_ts + 50000 < now) {
		*online = false;
	}
	*prev = cur;
}

void usb_hid_task(void) {
	static int64_t next_ts = 0;
	const int64_t now = esp_timer_get_time();
	if (now < next_ts) {
		return;
	}
	next_ts = now + 1000; // Every 1 ms

	if (s_kbd_inst >= 0) {
		static bool prev = true;
		static int64_t offline_ts = 0;
		const bool was_online = s_kbd_online;
		_check_online(s_kbd_inst, &s_kbd_online, &prev, &offline_ts, now);
		if (s_kbd_online && !was_online) {
			s_kbd_pending = true; // Resync key state after a long offline period
		}
		_kbd_flush();
	}
	if (s_mouse_inst >= 0) {
		static bool prev = true;
		static int64_t offline_ts = 0;
		_check_online(s_mouse_inst, &s_mouse_online, &prev, &offline_ts, now);
		_mouse_flush();
	}
}


// ===== hid.h backend =====
uint8_t hid_kbd_leds(void) {
	return s_kbd_leds;
}

bool hid_kbd_online(void) {
	return s_kbd_online;
}

bool hid_mouse_online(void) {
	return s_mouse_online;
}

void hid_kbd_key(uint8_t key, bool state) {
	if (s_kbd_inst < 0) {
		return;
	}
	if (key >= HID_KEY_CONTROL_LEFT && key <= HID_KEY_GUI_RIGHT) { // 0xE0..0xE7: modifiers
		const uint8_t bit = 1 << (key & 0x07);
		if (state) {
			s_kbd_mods |= bit;
		} else {
			s_kbd_mods &= ~bit;
		}
	} else if (state) {
		int free_pos = -1;
		for (int i = 0; i < 6; ++i) {
			if (s_kbd_keys[i] == key) {
				free_pos = -2; // Already pressed
				break;
			} else if (s_kbd_keys[i] == 0 && free_pos == -1) {
				free_pos = i;
			}
		}
		if (free_pos == -2) {
			// Already held: just send the report again, like Pico HID
		} else {
			s_kbd_keys[free_pos >= 0 ? free_pos : 0] = key;
		}
	} else {
		for (int i = 0; i < 6; ++i) {
			if (s_kbd_keys[i] == key) {
				s_kbd_keys[i] = 0;
				break;
			}
		}
	}
	s_kbd_pending = true;
	_kbd_flush();
}

void hid_mouse_button(uint8_t button, bool state) {
	if (state) {
		s_mouse_buttons |= button;
	} else {
		s_mouse_buttons &= ~button;
	}
	_mouse_update();
}

void hid_mouse_abs(int16_t x, int16_t y) {
	s_mouse_x = x;
	s_mouse_y = y;
	_mouse_update();
}

void hid_mouse_rel(int8_t dx, int8_t dy) {
	s_mouse_dx += dx;
	s_mouse_dy += dy;
	_mouse_update();
}

void hid_mouse_wheel(int8_t h, int8_t v) {
	(void)h; // Horizontal scrolling is not supported (BIOS/UEFI compatibility), like Pico HID
	s_mouse_wheel += v;
	_mouse_update();
}

void hid_clear(void) {
	s_kbd_mods = 0;
	memset(s_kbd_keys, 0, sizeof(s_kbd_keys));
	if (s_kbd_inst >= 0) {
		s_kbd_pending = true;
		_kbd_flush();
	}
	s_mouse_buttons = 0;
	s_mouse_dx = s_mouse_dy = s_mouse_wheel = 0;
	if (s_mouse_inst >= 0) {
		_mouse_update();
	}
}


// ===== TinyUSB callbacks (run in the TinyUSB task) =====
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
	if ((int)instance == s_mouse_inst) {
		return (s_mouse_abs ? MOUSE_ABS_DESC : MOUSE_REL_DESC);
	}
	return KBD_DESC;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buf, uint16_t len) {
	(void)instance;
	(void)report_id;
	(void)report_type;
	(void)buf;
	(void)len;
	return 0; // STALL
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buf, uint16_t len) {
	(void)report_id;
	if ((int)instance == s_kbd_inst && report_type == HID_REPORT_TYPE_OUTPUT && len >= 1) {
		s_kbd_leds = buf[0];
	}
}
