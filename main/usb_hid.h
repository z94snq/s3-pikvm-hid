#pragma once

#include <stdbool.h>
#include <stdint.h>

// Build USB descriptors for the active PROTO_OUT1_* outputs and start TinyUSB.
// Returns false if nothing is enabled or the driver failed to start.
bool usb_hid_init(uint8_t outputs);

// Call often (at least every millisecond): retries pending reports, tracks online state
void usb_hid_task(void);
