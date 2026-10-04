/*
 * Keeps the selected outputs (keyboard/mouse mode) across the software reset
 * that follows a mode change, like Pico HID does with its watchdog scratch register.
 * RTC_NOINIT memory survives esp_restart() but not a power cycle, so after
 * unplugging the board starts with the default mode from menuconfig again.
 */
#include "esp_attr.h"

#include "proto.h"
#include "hid.h"

static RTC_NOINIT_ATTR uint32_t s_stored;

int outputs_storage_read(void) {
	const uint8_t data[2] = {(uint8_t)(s_stored >> 24), (uint8_t)(s_stored >> 16)};
	const uint16_t crc = (uint16_t)(s_stored & 0xFFFF);
	if (data[0] != PROTO_MAGIC || proto_crc16(data, 2) != crc) {
		return -1;
	}
	return data[1];
}

void outputs_storage_write(uint8_t outputs) {
	const uint8_t data[2] = {PROTO_MAGIC, outputs};
	s_stored = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | proto_crc16(data, 2);
}
