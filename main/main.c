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
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "sensirion_common.h"
#include "sensirion_uart_hal.h"
#include "sps30_uart.h"
#include "mdns.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "protocol_examples_common.h"
#include "lwip/apps/netbiosns.h"
#include "websocket.h"

int sps30(void);

#define sensirion_hal_sleep_us sensirion_uart_hal_sleep_usec
#define MDNS_INSTANCE "simple sps30 server"

static const char *TAG = "sps30 simple main";

static void initialise_mdns(void)
{
    mdns_init();
    mdns_hostname_set(CONFIG_MDNS_HOST_NAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    mdns_txt_item_t serviceTxtData[] = 
    {
        {"board", "esp32"},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("SimpleSps30-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_WEB_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

static void time_init(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) 
    {
        ESP_LOGE(TAG, "Failed to update system time within 10s timeout");
    }
    setenv("TZ","EST5EDT,M3.2.0/2,M11.1.0/2",1); 
    tzset();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(CONFIG_MDNS_HOST_NAME);

    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(init_fs());
    time_init();
    ESP_ERROR_CHECK(websocket_server_start(CONFIG_WEB_MOUNT_POINT));
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