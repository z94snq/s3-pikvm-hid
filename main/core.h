#pragma once

#include <stdbool.h>
#include <stdint.h>

// Load the stored outputs (or use default_outputs), keeping only modes this board supports
void core_init(uint8_t default_outputs);

// Active PROTO_OUT1_* bits; decides which USB interfaces to expose
uint8_t core_outputs(void);

// Handle one 8-byte request and fill an 8-byte response
void core_handle_request(const uint8_t *req, uint8_t *resp);

// Response for a partial request that timed out
void core_timeout_response(uint8_t *resp);

// True after kvmd changed an output; the board must reboot after sending the response
bool core_reset_required(void);
