/*
 * Interfaces the protocol core (core.c) needs from the platform.
 * On the ESP32-S3 they are implemented in usb_hid.c and outputs_esp.c;
 * on a PC (test/) they are mocks, so the protocol can be tested without hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Mouse button bits as they appear in the USB HID report
#define HID_MOUSE_LEFT      0x01
#define HID_MOUSE_RIGHT     0x02
#define HID_MOUSE_MIDDLE    0x04
#define HID_MOUSE_BACK      0x08
#define HID_MOUSE_FORWARD   0x10

// ----- USB HID backend -----
uint8_t hid_kbd_leds(void);          // HID LED bitmask: bit0 Num, bit1 Caps, bit2 Scroll
bool hid_kbd_online(void);
bool hid_mouse_online(void);

void hid_kbd_key(uint8_t usage, bool pressed);
void hid_mouse_button(uint8_t button, bool pressed);
void hid_mouse_abs(int16_t x, int16_t y);   // kvmd range -32768..32767
void hid_mouse_rel(int8_t dx, int8_t dy);
void hid_mouse_wheel(int8_t h, int8_t v);
void hid_clear(void);

// ----- Persistent output selection (survives a software reset) -----
// Returns a negative value if nothing valid is stored.
int outputs_storage_read(void);
void outputs_storage_write(uint8_t outputs);
