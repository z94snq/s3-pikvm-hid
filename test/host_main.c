/*
 * PC test harness: runs the real protocol core (main/core.c) with a mock HID backend.
 * stdin: 8-byte requests, stdout: 8-byte responses, stderr: one line per HID call.
 * Env: MOCK_LEDS=<n>, MOCK_STORED=<outputs> (simulates RTC memory after a reboot),
 *      MOCK_OFFLINE=1 (report keyboard/mouse offline)
 */
#include <stdio.h>
#include <stdlib.h>

#include "../main/core.h"
#include "../main/hid.h"
#include "../main/proto.h"

static int s_stored = -1;

uint8_t hid_kbd_leds(void) { const char *e = getenv("MOCK_LEDS"); return e ? (uint8_t)atoi(e) : 0; }
bool hid_kbd_online(void) { return getenv("MOCK_OFFLINE") == NULL; }
bool hid_mouse_online(void) { return getenv("MOCK_OFFLINE") == NULL; }
void hid_kbd_key(uint8_t usage, bool pressed) { fprintf(stderr, "key %u %d\n", usage, pressed); }
void hid_mouse_button(uint8_t b, bool pressed) { fprintf(stderr, "button %u %d\n", b, pressed); }
void hid_mouse_abs(int16_t x, int16_t y) { fprintf(stderr, "abs %d %d\n", x, y); }
void hid_mouse_rel(int8_t dx, int8_t dy) { fprintf(stderr, "rel %d %d\n", dx, dy); }
void hid_mouse_wheel(int8_t h, int8_t v) { fprintf(stderr, "wheel %d %d\n", h, v); }
void hid_clear(void) { fprintf(stderr, "clear\n"); }
int outputs_storage_read(void) { return s_stored; }
void outputs_storage_write(uint8_t o) { s_stored = o; fprintf(stderr, "store %u\n", o); }

int main(int argc, char **argv) {
	const char *st = getenv("MOCK_STORED");
	if (st) {
		s_stored = atoi(st);
	}
	core_init(argc > 1 ? (uint8_t)atoi(argv[1]) : (PROTO_OUT1_KBD_USB | PROTO_OUT1_MOUSE_USB_ABS));
	fprintf(stderr, "init %u\n", core_outputs());
	uint8_t req[8], resp[8];
	while (fread(req, 1, 8, stdin) == 8) {
		core_handle_request(req, resp);
		fwrite(resp, 1, 8, stdout);
		fflush(stdout);
		if (core_reset_required()) {
			fprintf(stderr, "reset\n");
			break;
		}
	}
	return 0;
}
