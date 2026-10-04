/*
 * ESP32-S3 HID for PiKVM.
 *
 * kvmd (serial HID plugin) -> USB-UART bridge port -> UART -> this firmware
 * this firmware -> native USB port -> target computer (keyboard + mouse)
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "proto.h"
#include "core.h"
#include "usb_hid.h"

#define UART_PORT       ((uart_port_t)CONFIG_S3HID_UART_NUM)
#define REQ_TIMEOUT_US  100000  // Drop a partial request after 100 ms, like Pico HID

#if CONFIG_S3HID_DEFAULT_MOUSE_REL
#	define DEFAULT_MOUSE PROTO_OUT1_MOUSE_USB_REL
#else
#	define DEFAULT_MOUSE PROTO_OUT1_MOUSE_USB_ABS
#endif

static void _uart_init(void) {
	const uart_config_t cfg = {
		.baud_rate = CONFIG_S3HID_UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 512, 512, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
	ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
		CONFIG_S3HID_UART_TX_PIN, CONFIG_S3HID_UART_RX_PIN,
		UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void _send(const uint8_t *resp) {
	uart_write_bytes(UART_PORT, resp, 8);
	if (core_reset_required()) {
		// Outputs changed: reboot so the new USB descriptors take effect
		uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
		vTaskDelay(pdMS_TO_TICKS(100));
		esp_restart();
	}
}

void app_main(void) {
	core_init(PROTO_OUT1_KBD_USB | DEFAULT_MOUSE);
	usb_hid_init(core_outputs());
	_uart_init();

	uint8_t buf[8];
	uint8_t resp[8];
	unsigned index = 0;
	int64_t last_ts = 0;

	while (true) {
		uint8_t byte;
		// Short wait so usb_hid_task() still runs every ~1 ms (needs CONFIG_FREERTOS_HZ=1000)
		if (uart_read_bytes(UART_PORT, &byte, 1, 1) == 1) {
			buf[index] = byte;
			if (index == 7) {
				core_handle_request(buf, resp);
				_send(resp);
				index = 0;
			} else {
				last_ts = esp_timer_get_time();
				++index;
			}
		} else if (index > 0 && esp_timer_get_time() - last_ts > REQ_TIMEOUT_US) {
			core_timeout_response(resp);
			_send(resp);
			index = 0;
		}
		usb_hid_task();
	}
}
