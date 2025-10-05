/*
 * Copyright (c) 2018, Sensirion AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Sensirion AG nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sensirion_uart_hal.h"
#include "sensirion_common.h"
#include "sensirion_config.h"
#include "sensirion_uart_portdescriptor.h"

static const char *READ_TAG = "SPS30_HAL_READ";
static const char *WRITE_TAG = "SPS30_HAL_WRITE";
#define SPS30_UART_PORT   UART_NUM_2
#define SPS30_BAUD_RATE   115200

static void hexdump(const char* tag, const uint8_t *buf, size_t len) 
{
    char line[128];
    for (size_t off = 0; off < len; off += 16) {
        int n = 0;
        size_t end = off + 16; if (end > len) end = len;
        for (size_t i = off; i < end; ++i) n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[i]);
        ESP_LOGI(tag, "%d: %s", len, line);
    }
}
/*
 * INSTRUCTIONS
 * ============
 *
 * Implement all functions where they are marked with TODO: implement
 * Follow the function specification in the comments.
 */

/**
 * sensirion_uart_hal_select_port() - select the UART port index to use
 *                                THE IMPLEMENTATION IS OPTIONAL ON SINGLE-PORT
 *                                SETUPS (only one SPS30)
 *
 * Return:      0 on success, an error code otherwise
 */
int16_t sensirion_uart_hal_select_port(uint8_t port) {
    /* TODO: implement */
    return NOT_IMPLEMENTED_ERROR;
}

/**
 * sensirion_uart_hal_init() - initialize UART
 *
 * Return:      0 on success, an error code otherwise
 */
int16_t sensirion_uart_hal_init(UartDescr port) 
{
    printf("in sensirion_uart_hal_init\n");
    const uart_config_t uart_config = 
    {
        .baud_rate = SPS30_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(SPS30_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SPS30_UART_PORT, CONFIG_UART_TX_GPIO, CONFIG_UART_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SPS30_UART_PORT, 256, 0, 0, NULL, 0));

    return ESP_OK;
}

/**
 * sensirion_uart_hal_free() - release UART resources
 *
 * Return:      0 on success, an error code otherwise
 */
int16_t sensirion_uart_hal_free() 
{
    /* TODO: implement */
    return NOT_IMPLEMENTED_ERROR;
}

/**
 * sensirion_uart_hal_tx() - transmit data over UART
 *
 * @data_len:   number of bytes to send
 * @data:       data to send
 * Return:      Number of bytes sent or a negative error code
 */
int16_t sensirion_uart_hal_tx(uint16_t data_len, const uint8_t* data) 
{
    int numBytes = 0;
    numBytes = uart_write_bytes(SPS30_UART_PORT, (const char*)data, data_len);
    //hexdump(WRITE_TAG, data, data_len);
    return numBytes;
}

/**
 * sensirion_uart_hal_rx() - receive data over UART
 *
 * @data_len:   max number of bytes to receive
 * @data:       Memory where received data is stored
 * Return:      Number of bytes received or a negative error code
 */
int16_t sensirion_uart_hal_rx(uint16_t max_data_len, uint8_t* data) 
{
    int length = 0;
    ESP_ERROR_CHECK(uart_get_buffered_data_len(SPS30_UART_PORT, (size_t*)&length));
    //ESP_LOGI(READ_TAG, "rx buffer len: %d", length);

    int len = uart_read_bytes(SPS30_UART_PORT, data, max_data_len, pdMS_TO_TICKS(100));
    if(len > 0)
    {
        //uart_flush(SPS30_UART_PORT);
        //hexdump(READ_TAG, data, len);
        return len;
    }
    else
    {
        ESP_LOGI(READ_TAG, "No data read");
        return -1;
    }
}

/**
 * Sleep for a given number of microseconds. The function should delay the
 * execution for at least the given time, but may also sleep longer.
 *
 * Despite the unit, a <10 millisecond precision is sufficient.
 *
 * @param useconds the sleep time in microseconds
 */
void sensirion_uart_hal_sleep_usec(uint32_t useconds) 
{
    vTaskDelay(pdMS_TO_TICKS(useconds / 1000));
}
