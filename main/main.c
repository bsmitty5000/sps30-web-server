/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "sensirion_common.h"
#include "sensirion_uart_hal.h"
#include "sps30_uart.h"

int sps30(void);

#define sensirion_hal_sleep_us sensirion_uart_hal_sleep_usec

void app_main(void)
{
    printf("Hello world!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    sps30();

    fflush(stdout);
}

int sps30(void) 
{
    int16_t error = NO_ERROR;

    // for non linux systems modify the header file sensirion_uart_descriptor.h
    // SERIAL_0 defaults to "/dev/ttyUSB0" for linux user space implementation
    sensirion_uart_hal_init(SERIAL_0);

    error = sps30_stop_measurement();
    if (error != NO_ERROR) {
        printf("error executing sps30_stop_measurement(): %i\n", error);
    }
    int8_t serial_number[32] = {0};
    int8_t product_type[9] = {0};
    error = sps30_read_serial_number(serial_number, 32);
    if (error != NO_ERROR) {
        printf("error executing read_serial_number(): %i\n", error);
        return error;
    }
    printf("serial_number: %s\n", serial_number);
    error = sps30_read_product_type(product_type, 9);
    if (error != NO_ERROR) {
        printf("error executing read_product_type(): %i\n", error);
        return error;
    }
    printf("product_type: %s\n", product_type);
    error = sps30_start_measurement((sps30_output_format)(261));
    if (error != NO_ERROR) {
        printf("error executing start_measurement(): %i\n", error);
        return error;
    }
    uint16_t mc_1p0 = 0;
    uint16_t mc_2p5 = 0;
    uint16_t mc_4p0 = 0;
    uint16_t mc_10p0 = 0;
    uint16_t nc_0p5 = 0;
    uint16_t nc_1p0 = 0;
    uint16_t nc_2p5 = 0;
    uint16_t nc_4p0 = 0;
    uint16_t nc_10p0 = 0;
    uint16_t typical_particle_size = 0;
    uint16_t repetition = 0;
    for (repetition = 0; repetition < 50; repetition++) 
    {
        sensirion_hal_sleep_us(1000000);
        error = sps30_read_measurement_values_uint16(
            &mc_1p0, &mc_2p5, &mc_4p0, &mc_10p0, &nc_0p5, &nc_1p0, &nc_2p5,
            &nc_4p0, &nc_10p0, &typical_particle_size);
        if (error != NO_ERROR) {
            printf("error executing read_measurement_values_uint16(): %i\n",
                   error);
            continue;
        }
        printf("mc_1p0: %u ", mc_1p0);
        printf("mc_2p5: %u ", mc_2p5);
        printf("mc_4p0: %u ", mc_4p0);
        printf("mc_10p0: %u ", mc_10p0);
        printf("nc_0p5: %u ", nc_0p5);
        printf("nc_1p0: %u ", nc_1p0);
        printf("nc_2p5: %u ", nc_2p5);
        printf("nc_4p0: %u ", nc_4p0);
        printf("nc_10p0: %u ", nc_10p0);
        printf("typical_particle_size: %u\n", typical_particle_size);
    }

    error = sps30_stop_measurement();
    if (error != NO_ERROR) {
        return error;
    }
    return NO_ERROR;
}