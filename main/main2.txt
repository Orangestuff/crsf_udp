#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

// --- Hardware Setup ---
#define CRSF_UART_PORT UART_NUM_2
#define CRSF_RX_PIN    17   // Make sure this matches your S3 setup
#define CRSF_TX_PIN    16   
#define UART_BUF_SIZE  1024

static const char *TAG = "CRSF_PARSER";

uint16_t rc_channels[16];

// --- Official TBS CRSF CRC8 Table ---
static const uint8_t crc8tab[256] = {
    0x00, 0xD5, 0x7F, 0xAA, 0xFE, 0x2B, 0x81, 0x54, 0x29, 0xFC, 0x56, 0x83, 0xD7, 0x02, 0xA8, 0x7D,
    0x52, 0x87, 0x2D, 0xF8, 0xAC, 0x79, 0xD3, 0x06, 0x7B, 0xAE, 0x04, 0xD1, 0x85, 0x50, 0xFA, 0x2F,
    0xA4, 0x71, 0xDB, 0x0E, 0x5A, 0x8F, 0x25, 0xF0, 0x8D, 0x58, 0xF2, 0x27, 0x73, 0xA6, 0x0C, 0xD9,
    0xF6, 0x23, 0x89, 0x5C, 0x08, 0xDD, 0x77, 0xA2, 0xDF, 0x0A, 0xA0, 0x75, 0x21, 0xF4, 0x5E, 0x8B,
    0x9D, 0x48, 0xE2, 0x37, 0x63, 0xB6, 0x1C, 0xC9, 0xB4, 0x61, 0xCB, 0x1E, 0x4A, 0x9F, 0x35, 0xE0,
    0xCF, 0x1A, 0xB0, 0x65, 0x31, 0xE4, 0x4E, 0x9B, 0xE6, 0x33, 0x99, 0x4C, 0x18, 0xCD, 0x67, 0xB2,
    0x39, 0xEC, 0x46, 0x93, 0xC7, 0x12, 0xB8, 0x6D, 0x10, 0xC5, 0x6F, 0xBA, 0xEE, 0x3B, 0x91, 0x44,
    0x6B, 0xBE, 0x14, 0xC1, 0x95, 0x40, 0xEA, 0x3F, 0x42, 0x97, 0x3D, 0xE8, 0xBC, 0x69, 0xC3, 0x16,
    0xEF, 0x3A, 0x90, 0x45, 0x11, 0xC4, 0x6E, 0xBB, 0xC6, 0x13, 0xB9, 0x6C, 0x38, 0xED, 0x47, 0x92,
    0xBD, 0x68, 0xC2, 0x17, 0x43, 0x96, 0x3C, 0xE9, 0x94, 0x41, 0xEB, 0x3E, 0x6A, 0xBF, 0x15, 0xC0,
    0x4B, 0x9E, 0x34, 0xE1, 0xB5, 0x60, 0xCA, 0x1F, 0x62, 0xB7, 0x1D, 0xC8, 0x9C, 0x49, 0xE3, 0x36,
    0x19, 0xCC, 0x66, 0xB3, 0xE7, 0x32, 0x98, 0x4D, 0x30, 0xE5, 0x4F, 0x9A, 0xCE, 0x1B, 0xB1, 0x64,
    0x72, 0xA7, 0x0D, 0xD8, 0x8C, 0x59, 0xF3, 0x26, 0x5B, 0x8E, 0x24, 0xF1, 0xA5, 0x70, 0xDA, 0x0F,
    0x20, 0xF5, 0x5F, 0x8A, 0xDE, 0x0B, 0xA1, 0x74, 0x09, 0xDC, 0x76, 0xA3, 0xF7, 0x22, 0x88, 0x5D,
    0xD6, 0x03, 0xA9, 0x7C, 0x28, 0xFD, 0x57, 0x82, 0xFF, 0x2A, 0x80, 0x55, 0x01, 0xD4, 0x7E, 0xAB,
    0x84, 0x51, 0xFB, 0x2E, 0x7A, 0xAF, 0x05, 0xD0, 0xAD, 0x78, 0xD2, 0x07, 0x53, 0x86, 0x2C, 0xF9
};

// Calculates CRC according to spec (Type + Payload)
uint8_t crsf_crc8(const uint8_t *ptr, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc = crc8tab[crc ^ *ptr++];
    }
    return crc;
}

// 11-bit Unpacking Magic
void unpack_crsf_channels(const uint8_t* p) {
    rc_channels[0] = ((p[1] & 0x07) << 8) | p[0];
    rc_channels[1] = ((p[2] & 0x3F) << 5) | (p[1] >> 3);
    rc_channels[2] = ((p[4] & 0x01) << 10) | (p[3] << 2) | (p[2] >> 6);
    rc_channels[3] = ((p[5] & 0x0F) << 7) | (p[4] >> 1);
    rc_channels[4] = ((p[6] & 0x7F) << 4) | (p[5] >> 4);
}

// Process a validated frame
void process_crsf_packet(uint8_t *frame) {
    uint8_t frame_type = frame[2];

    if (frame_type == 0x16) { // CRSF_TYPE_RC_CHANNELS
        unpack_crsf_channels(&frame[3]);
        
       /* // Print to monitor at 10Hz to prevent spam
        static TickType_t last_print = 0;
        if (xTaskGetTickCount() - last_print >= pdMS_TO_TICKS(100)) {
            ESP_LOGI(TAG, "CH1 (A/E): %04d | CH2 (A/E): %04d | CH3 (Thr): %04d | CH4 (Rud): %04d", 
                     rc_channels[0], rc_channels[1], rc_channels[2], rc_channels[3]);
            last_print = xTaskGetTickCount();
        }
        */
    }
}

// Robust Sliding Window Task
void crsf_rx_task(void *arg) {
    uint8_t window[256];
    uint16_t window_len = 0;
    uint8_t uart_data[128];

    ESP_LOGI(TAG, "CRSF Sliding Window Parser Started");

    while (1) {
        int rx_bytes = uart_read_bytes(CRSF_UART_PORT, uart_data, sizeof(uart_data), 10 / portTICK_PERIOD_MS);
        
        if (rx_bytes > 0) {
            // 1. Add new bytes to the end of our window
            for (int i = 0; i < rx_bytes; i++) {
                if (window_len < sizeof(window)) {
                    window[window_len++] = uart_data[i];
                }
            }
            
            // 2. Parse the window
            while (window_len >= 4) { 
                // Check if the first byte is a valid Sync (FC, TX Module, or Radio)
                uint8_t sync = window[0];
                if (sync != 0xC8 && sync != 0xEE && sync != 0xEA) {
                    memmove(window, window + 1, window_len - 1); // Slide window by 1
                    window_len--;
                    continue;
                }
                
                uint8_t frame_len = window[1];
                if (frame_len < 2 || frame_len > 62) {
                    memmove(window, window + 1, window_len - 1); // Fake sync, slide by 1
                    window_len--;
                    continue;
                }
                
                uint8_t total_frame_size = frame_len + 2; // Sync + Len + [Type + Payload + CRC]
                
                if (window_len < total_frame_size) {
                    break; // Wait for the rest of the frame to arrive
                }
                
                // 3. We have a full frame candidate. Verify CRC!
                // CRC covers Type + Payload (which is frame_len - 1 bytes starting at index 2)
                uint8_t calculated_crc = crsf_crc8(&window[2], frame_len - 1);
                uint8_t received_crc = window[total_frame_size - 1];
                
                if (calculated_crc == received_crc) {
                    // VALID FRAME! Send it to be processed
                    process_crsf_packet(window);
                    
                    // Clear this frame out of the window
                    memmove(window, window + total_frame_size, window_len - total_frame_size);
                    window_len -= total_frame_size;
                } else {
                    // CRC Failed. The sync byte was a fluke. Slide by 1 and resume searching.
                    memmove(window, window + 1, window_len - 1);
                    window_len--;
                }
            }
        }
    }
}

void app_main(void) {
    uart_config_t uart_config = {
        .baud_rate = 400000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(CRSF_UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CRSF_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CRSF_UART_PORT, CRSF_TX_PIN, CRSF_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // --- ADD THIS LINE TO INVERT THE RX SIGNAL ---
    ESP_ERROR_CHECK(uart_set_line_inverse(CRSF_UART_PORT, UART_SIGNAL_RXD_INV));
    // ---------------------------------------------

    xTaskCreate(crsf_rx_task, "crsf_rx", 4096, NULL, 10, NULL);
}