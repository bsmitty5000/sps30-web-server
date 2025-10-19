#include "sensor_task.h"
#include "sensor_events.h"
#include "sps30.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sensor_task";

// Shared sensor data (protected by mutex)
static sensor_data_t latest_reading = {0};
static SemaphoreHandle_t data_mutex = NULL;

// Sensor state
static sensor_status_t current_status = SENSOR_NOT_READY;
static bool sensor_initialized = false;

// Read interval (1 second = 1000ms)
#define SENSOR_READ_INTERVAL_MS 1000
#define SENSOR_INIT_RETRY_MS 5000

/**
 * Initialize SPS30 sensor with retry logic
 */
static esp_err_t init_sensor(void) {
    ESP_LOGI(TAG, "Initializing SPS30 sensor");

    int16_t ret = sps30_probe();
    if (ret != 0) {
        ESP_LOGE(TAG, "SPS30 probe failed: %d", ret);
        return ESP_FAIL;
    }

    ret = sps30_start_measurement();
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start measurement: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SPS30 initialized and measurement started");
    return ESP_OK;
}

/**
 * Read sensor and publish event
 */
static void read_and_publish(void) {
    struct sps30_measurement m;
    int16_t ret = sps30_read_measurement(&m);

    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to read measurement: %d", ret);

        // Update status if changed
        if (current_status != SENSOR_COMM_ERROR) {
            current_status = SENSOR_COMM_ERROR;

            // Publish status change event
            esp_event_post(SENSOR_EVENT, SENSOR_STATUS_CHANGE,
                          &current_status, sizeof(current_status), portMAX_DELAY);
        }
        return;
    }

    // Update status to OK if it was error
    if (current_status != SENSOR_OK) {
        current_status = SENSOR_OK;
        esp_event_post(SENSOR_EVENT, SENSOR_STATUS_CHANGE,
                      &current_status, sizeof(current_status), portMAX_DELAY);
    }

    // Build sensor data structure
    sensor_data_t data = {
        .pm1_0 = m.mc_1p0,
        .pm2_5 = m.mc_2p5,
        .pm4_0 = m.mc_4p0,
        .pm10 = m.mc_10p0,
        .nc0_5 = m.nc_0p5,
        .nc1_0 = m.nc_1p0,
        .nc2_5 = m.nc_2p5,
        .nc4_0 = m.nc_4p0,
        .nc10 = m.nc_10p0,
        .typical_size = m.typical_particle_size,
        .timestamp_ms = esp_timer_get_time() / 1000,
        .status = current_status
    };

    // Update latest reading (thread-safe)
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        latest_reading = data;
        xSemaphoreGive(data_mutex);
    }

    // Publish event to all subscribers
    esp_event_post(SENSOR_EVENT, SENSOR_DATA_READY, &data, sizeof(data), 0);
}

/**
 * Handle fan cleaning command
 */
static void handle_fan_clean(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    ESP_LOGI(TAG, "Starting fan cleaning");

    // Update status
    current_status = SENSOR_FAN_CLEANING;
    esp_event_post(SENSOR_EVENT, SENSOR_STATUS_CHANGE,
                  &current_status, sizeof(current_status), portMAX_DELAY);

    // Trigger fan cleaning
    int16_t ret = sps30_start_manual_fan_cleaning();
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start fan cleaning: %d", ret);
        current_status = SENSOR_COMM_ERROR;
    } else {
        ESP_LOGI(TAG, "Fan cleaning started (takes ~10 seconds)");
        // Wait for cleaning to complete
        vTaskDelay(pdMS_TO_TICKS(10000));
        current_status = SENSOR_OK;
    }

    // Publish status change
    esp_event_post(SENSOR_EVENT, SENSOR_STATUS_CHANGE,
                  &current_status, sizeof(current_status), portMAX_DELAY);
}

/**
 * Handle sleep/wake commands
 */
static void handle_sleep_cmd(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    sleep_command_t *cmd = (sleep_command_t *)event_data;

    if (cmd->enabled) {
        ESP_LOGI(TAG, "Entering sleep mode");
        sps30_sleep();
        current_status = SENSOR_SLEEPING;
    } else {
        ESP_LOGI(TAG, "Waking from sleep");
        sps30_wake_up();
        current_status = SENSOR_OK;
    }

    esp_event_post(SENSOR_EVENT, SENSOR_STATUS_CHANGE,
                  &current_status, sizeof(current_status), portMAX_DELAY);
}

/**
 * Main sensor task
 */
static void sensor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Sensor task started");

    // Register command handlers
    esp_event_handler_register(COMMAND_EVENT, CMD_FAN_CLEAN, handle_fan_clean, NULL);
    esp_event_handler_register(COMMAND_EVENT, CMD_SLEEP, handle_sleep_cmd, NULL);

    // Initialize sensor with retry
    while (!sensor_initialized) {
        if (init_sensor() == ESP_OK) {
            sensor_initialized = true;
            current_status = SENSOR_OK;
        } else {
            ESP_LOGW(TAG, "Sensor init failed, retrying in %d ms", SENSOR_INIT_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(SENSOR_INIT_RETRY_MS));
        }
    }

    ESP_LOGI(TAG, "Sensor initialized, starting 1Hz read loop");

    // Main read loop
    while (1) {
        if (current_status == SENSOR_OK || current_status == SENSOR_COMM_ERROR) {
            read_and_publish();
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

esp_err_t sensor_task_start(void) {
    // Create mutex for thread-safe data access
    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create data mutex");
        return ESP_FAIL;
    }

    // Create sensor task (8KB stack, priority 5)
    BaseType_t ret = xTaskCreate(sensor_task, "sensor_task", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor task created");
    return ESP_OK;
}

esp_err_t sensor_task_get_latest(sensor_data_t *out_data) {
    if (out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out_data = latest_reading;
        xSemaphoreGive(data_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}