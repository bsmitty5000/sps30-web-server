# Multi-Protocol Air Quality Sensor Implementation Plan

> **For Claude:** Use `${SUPERPOWERS_SKILLS_ROOT}/skills/collaboration/executing-plans/SKILL.md` to implement this plan task-by-task.

**Goal:** Transform ESP32+SPS30 into a professional multi-protocol air quality sensor supporting WebSockets, MQTT (with Home Assistant Discovery), REST API, and Prometheus metrics.

**Architecture:** Event-driven system using ESP-IDF's event loop. Sensor task publishes data events at 1Hz; protocol tasks (WebSocket, MQTT, HTTP) subscribe and format data for their respective channels. All protocols share access to sensor data via events - no direct coupling.

**Tech Stack:** ESP-IDF v5.5.1, FreeRTOS, esp_http_server, esp-mqtt, Sensirion SPS30 UART driver

---

## Task 1: Create Event-Driven Core Infrastructure

**Goal:** Establish the event loop foundation that all protocols will use to communicate.

**Files:**
- Create: `main/sensor_events.h`
- Create: `main/sensor_events.c`
- Modify: `main/CMakeLists.txt`

**Step 1: Define event structures in sensor_events.h**

Create `main/sensor_events.h`:

```c
#pragma once

#include "esp_event.h"
#include <stdbool.h>
#include <stdint.h>

// Event base declarations
ESP_EVENT_DECLARE_BASE(SENSOR_EVENT);
ESP_EVENT_DECLARE_BASE(COMMAND_EVENT);

// Sensor event IDs
typedef enum {
    SENSOR_DATA_READY,      // New sensor reading available
    SENSOR_STATUS_CHANGE,   // Sensor status changed
    SENSOR_ERROR            // Sensor error occurred
} sensor_event_id_t;

// Command event IDs
typedef enum {
    CMD_FAN_CLEAN,         // Trigger fan cleaning
    CMD_SLEEP,             // Enter sleep mode
    CMD_WAKE              // Exit sleep mode
} command_event_id_t;

// Sensor status enumeration
typedef enum {
    SENSOR_OK = 0,
    SENSOR_COMM_ERROR = 1,
    SENSOR_NOT_READY = 2,
    SENSOR_FAN_CLEANING = 3,
    SENSOR_SLEEPING = 4
} sensor_status_t;

// Sensor data structure (posted with SENSOR_DATA_READY)
typedef struct {
    float pm1_0;              // PM1.0 mass concentration (µg/m³)
    float pm2_5;              // PM2.5 mass concentration (µg/m³)
    float pm4_0;              // PM4.0 mass concentration (µg/m³)
    float pm10;               // PM10 mass concentration (µg/m³)
    float nc0_5;              // Number concentration 0.5µm (#/cm³)
    float nc1_0;              // Number concentration 1.0µm (#/cm³)
    float nc2_5;              // Number concentration 2.5µm (#/cm³)
    float nc4_0;              // Number concentration 4.0µm (#/cm³)
    float nc10;               // Number concentration 10µm (#/cm³)
    float typical_size;       // Typical particle size (µm)
    int64_t timestamp_ms;     // Timestamp when read (esp_timer_get_time() / 1000)
    sensor_status_t status;   // Current sensor status
} sensor_data_t;

// Command data structures
typedef struct {
    bool enabled;  // true = sleep, false = wake
} sleep_command_t;

// Initialize the event loop
esp_err_t sensor_events_init(void);
```

**Step 2: Implement event base definitions**

Create `main/sensor_events.c`:

```c
#include "sensor_events.h"
#include "esp_log.h"

static const char *TAG = "sensor_events";

// Define event bases
ESP_EVENT_DEFINE_BASE(SENSOR_EVENT);
ESP_EVENT_DEFINE_BASE(COMMAND_EVENT);

esp_err_t sensor_events_init(void) {
    ESP_LOGI(TAG, "Initializing sensor event loop");

    // Create default event loop if not already created
    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_ERR_INVALID_STATE) {
        // Already created, this is fine
        ESP_LOGI(TAG, "Event loop already exists");
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Event loop created successfully");
    return ESP_OK;
}
```

**Step 3: Add to build system**

Modify `main/CMakeLists.txt` - add `sensor_events.c` to SRCS list:

```cmake
idf_component_register(SRCS "main.c" "sensor_events.c"
                    INCLUDE_DIRS "."
                    REQUIRES sps30 websocket)
```

**Step 4: Initialize event loop in main**

Modify `main/main.c` - add include and initialization:

After existing includes, add:
```c
#include "sensor_events.h"
```

In `app_main()`, before any other initialization, add:
```c
// Initialize event loop
ESP_ERROR_CHECK(sensor_events_init());
ESP_LOGI(TAG, "Event system initialized");
```

**Step 5: Build and verify**

```bash
idf.py build
```

Expected: Clean build with no errors. Should see new object files for sensor_events.c.

**Step 6: Commit**

```bash
git add main/sensor_events.h main/sensor_events.c main/CMakeLists.txt main/main.c
git commit -m "feat: add event-driven infrastructure for sensor/protocol communication

- Define SENSOR_EVENT and COMMAND_EVENT bases
- Create sensor_data_t structure for readings
- Initialize default event loop in app_main"
```

---

## Task 2: Refactor Sensor Task to Publish Events

**Goal:** Convert existing sensor reading code to publish events instead of direct data passing.

**Files:**
- Create: `main/sensor_task.h`
- Create: `main/sensor_task.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

**Step 1: Define sensor task interface**

Create `main/sensor_task.h`:

```c
#pragma once

#include "esp_err.h"

/**
 * Start the sensor reading task
 * Initializes SPS30 and begins publishing sensor events at 1Hz
 */
esp_err_t sensor_task_start(void);

/**
 * Get the last sensor reading (for REST API queries)
 * Thread-safe access to most recent data
 */
esp_err_t sensor_task_get_latest(sensor_data_t *out_data);
```

**Step 2: Implement sensor task with event publishing**

Create `main/sensor_task.c`:

```c
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
```

**Step 3: Update build system**

Modify `main/CMakeLists.txt` - add sensor_task.c to SRCS:

```cmake
idf_component_register(SRCS "main.c" "sensor_events.c" "sensor_task.c"
                    INCLUDE_DIRS "."
                    REQUIRES sps30 websocket)
```

**Step 4: Start sensor task from main**

Modify `main/main.c`:

Add include:
```c
#include "sensor_task.h"
```

In `app_main()`, after event loop init but before starting websocket server, add:
```c
// Start sensor task
ESP_ERROR_CHECK(sensor_task_start());
ESP_LOGI(TAG, "Sensor task started");
```

**Step 5: Build and verify**

```bash
idf.py build
```

Expected: Clean build. New object files for sensor_task.c.

**Step 6: Commit**

```bash
git add main/sensor_task.h main/sensor_task.c main/CMakeLists.txt main/main.c
git commit -m "feat: implement event-driven sensor task

- Create sensor task that publishes SENSOR_DATA_READY events at 1Hz
- Add command handlers for fan cleaning and sleep/wake
- Thread-safe access to latest reading for REST API
- Auto-retry on sensor init failure"
```

---

## Task 3: Adapt WebSocket to Subscribe to Events

**Goal:** Modify existing WebSocket broadcast to consume sensor events instead of generating dummy data.

**Files:**
- Modify: `components/websocket/websocket_server.c`

**Step 1: Add sensor event includes**

In `components/websocket/websocket_server.c`, add at top with other includes:

```c
#include "sensor_events.h"
```

**Step 2: Remove dummy data generation, subscribe to events**

Find the `broadcast_task()` function. Replace the entire function with:

```c
static void broadcast_task(void *arg) {
    ESP_LOGI(TAG, "Broadcast task started");

    while (1) {
        // Wait for next broadcast interval
        vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));

        // Note: We could subscribe to SENSOR_DATA_READY events instead,
        // but using a timer + get_latest() is simpler and avoids event handler complexity
        // This maintains the existing broadcast timing behavior

        sensor_data_t data;
        esp_err_t ret = sensor_task_get_latest(&data);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get sensor data");
            continue;
        }

        // Create JSON with actual sensor data
        char json_buffer[512];
        int len = snprintf(json_buffer, sizeof(json_buffer),
            "{"
            "\"pm1_0\":%.2f,"
            "\"pm2_5\":%.2f,"
            "\"pm4_0\":%.2f,"
            "\"pm10\":%.2f,"
            "\"nc0_5\":%.2f,"
            "\"nc1_0\":%.2f,"
            "\"nc2_5\":%.2f,"
            "\"nc4_0\":%.2f,"
            "\"nc10\":%.2f,"
            "\"typical_particle_size\":%.2f,"
            "\"uptime\":%lld,"
            "\"status\":\"%s\""
            "}",
            data.pm1_0, data.pm2_5, data.pm4_0, data.pm10,
            data.nc0_5, data.nc1_0, data.nc2_5, data.nc4_0, data.nc10,
            data.typical_size,
            data.timestamp_ms,
            (data.status == SENSOR_OK) ? "OK" :
            (data.status == SENSOR_COMM_ERROR) ? "COMM_ERROR" :
            (data.status == SENSOR_FAN_CLEANING) ? "CLEANING" :
            (data.status == SENSOR_SLEEPING) ? "SLEEPING" : "NOT_READY"
        );

        if (len < 0 || len >= sizeof(json_buffer)) {
            ESP_LOGE(TAG, "JSON buffer too small");
            continue;
        }

        // Broadcast to all clients
        broadcast_to_all_clients(json_buffer);
    }
}
```

**Step 3: Update CMakeLists to add dependency**

Modify `components/websocket/CMakeLists.txt` to add dependency on main component:

```cmake
idf_component_register(SRCS "websocket_server.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_http_server esp_timer
                    REQUIRES main)
```

Note: This creates a dependency where websocket component depends on main (for sensor_task.h and sensor_events.h). In ESP-IDF, this is acceptable for application-level components.

**Step 4: Build and verify**

```bash
idf.py build
```

Expected: Clean build.

**Step 5: Commit**

```bash
git add components/websocket/websocket_server.c components/websocket/CMakeLists.txt
git commit -m "feat: adapt WebSocket to use real sensor data

- Replace dummy random data with actual SPS30 readings
- Get latest data from sensor task via sensor_task_get_latest()
- Include sensor status in broadcast JSON"
```

---

## Task 4: Add WebSocket Command Handling

**Goal:** Enable WebSocket clients to trigger fan cleaning and sleep mode.

**Files:**
- Modify: `components/websocket/websocket_server.c`

**Step 1: Add command parsing to WebSocket receive handler**

In `components/websocket/websocket_server.c`, find the `handle_websocket_message()` function (or wherever WS messages are parsed).

Add command handling for `fanClean` and `sleep`:

```c
// After existing registerClient/closeConnection handling, add:

else if (strcmp(action, "fanClean") == 0) {
    ESP_LOGI(TAG, "Client requested fan cleaning");

    // Post fan clean command event
    esp_event_post(COMMAND_EVENT, CMD_FAN_CLEAN, NULL, 0, portMAX_DELAY);

    // Send response
    char response[256];
    snprintf(response, sizeof(response),
        "{\"response_for\":\"fanClean\",\"status\":\"success\","
        "\"message\":\"Fan cleaning started (takes ~10 seconds)\"}");

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)response;
    ws_pkt.len = strlen(response);

    httpd_ws_send_frame(req, &ws_pkt);

} else if (strcmp(action, "sleep") == 0) {
    // Parse enabled field
    cJSON *enabled_json = cJSON_GetObjectItem(root, "enabled");
    bool enabled = cJSON_IsTrue(enabled_json);

    ESP_LOGI(TAG, "Client requested sleep mode: %s", enabled ? "enabled" : "disabled");

    // Post sleep command event
    sleep_command_t cmd = { .enabled = enabled };
    esp_event_post(COMMAND_EVENT, CMD_SLEEP, &cmd, sizeof(cmd), portMAX_DELAY);

    // Send response
    char response[256];
    snprintf(response, sizeof(response),
        "{\"response_for\":\"sleep\",\"status\":\"success\","
        "\"message\":\"Sleep mode %s\"}",
        enabled ? "enabled" : "disabled");

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)response;
    ws_pkt.len = strlen(response);

    httpd_ws_send_frame(req, &ws_pkt);
}
```

**Step 2: Build and verify**

```bash
idf.py build
```

Expected: Clean build.

**Step 3: Commit**

```bash
git add components/websocket/websocket_server.c
git commit -m "feat: add WebSocket command handling for fan clean and sleep

- Parse fanClean and sleep actions from WebSocket messages
- Post command events to trigger sensor task actions
- Send confirmation responses to client"
```

---

## Task 5: Create MQTT Protocol Component

**Goal:** Add MQTT support with Home Assistant Discovery.

**Files:**
- Create: `components/mqtt_protocol/include/mqtt_protocol.h`
- Create: `components/mqtt_protocol/mqtt_protocol.c`
- Create: `components/mqtt_protocol/CMakeLists.txt`
- Create: `components/mqtt_protocol/Kconfig`

**Step 1: Create component structure**

```bash
mkdir -p components/mqtt_protocol/include
```

**Step 2: Define MQTT protocol interface**

Create `components/mqtt_protocol/include/mqtt_protocol.h`:

```c
#pragma once

#include "esp_err.h"

/**
 * Start MQTT protocol
 * Connects to broker, publishes Home Assistant discovery, subscribes to commands
 */
esp_err_t mqtt_protocol_start(void);

/**
 * Stop MQTT protocol
 */
esp_err_t mqtt_protocol_stop(void);
```

**Step 3: Implement MQTT with Home Assistant Discovery**

Create `components/mqtt_protocol/mqtt_protocol.c`:

```c
#include "mqtt_protocol.h"
#include "sensor_events.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_protocol";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Device ID (MAC-based)
static char device_id[20] = {0};
static char device_name[50] = {0};

// Topic buffers
static char state_topic[128] = {0};
static char fan_clean_cmd_topic[128] = {0};
static char sleep_cmd_topic[128] = {0};

/**
 * Generate device ID from MAC address
 */
static void generate_device_id(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "sps30_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(device_name, sizeof(device_name), "SPS30 Air Quality %02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

/**
 * Publish Home Assistant discovery config for a sensor
 */
static void publish_sensor_discovery(const char *sensor_id, const char *name,
                                     const char *unit, const char *device_class,
                                     const char *value_template) {
    char topic[256];
    char payload[1024];

    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s/%s/config", device_id, sensor_id);

    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"value_template\":\"%s\","
        "\"unit_of_measurement\":\"%s\","
        "\"device_class\":\"%s\","
        "\"state_class\":\"measurement\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"model\":\"ESP32-SPS30\","
            "\"manufacturer\":\"Custom\","
            "\"sw_version\":\"1.0.0\""
        "}"
        "}",
        name, device_id, sensor_id, state_topic, value_template,
        unit, device_class, device_id, device_name
    );

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "Published discovery for %s, msg_id=%d", sensor_id, msg_id);
}

/**
 * Publish Home Assistant discovery config for a button
 */
static void publish_button_discovery(const char *button_id, const char *name,
                                     const char *command_topic) {
    char topic[256];
    char payload[1024];

    snprintf(topic, sizeof(topic),
             "homeassistant/button/%s/%s/config", device_id, button_id);

    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"command_topic\":\"%s\","
        "\"payload_press\":\"PRESS\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"model\":\"ESP32-SPS30\","
            "\"manufacturer\":\"Custom\""
        "}"
        "}",
        name, device_id, button_id, command_topic,
        device_id, device_name
    );

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "Published button discovery for %s, msg_id=%d", button_id, msg_id);
}

/**
 * Publish Home Assistant discovery config for a switch
 */
static void publish_switch_discovery(const char *switch_id, const char *name,
                                     const char *command_topic, const char *state_topic_suffix) {
    char topic[256];
    char payload[1024];
    char state_topic_full[256];

    snprintf(topic, sizeof(topic),
             "homeassistant/switch/%s/%s/config", device_id, switch_id);

    snprintf(state_topic_full, sizeof(state_topic_full),
             "homeassistant/switch/%s/%s/state", device_id, switch_id);

    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"model\":\"ESP32-SPS30\","
            "\"manufacturer\":\"Custom\""
        "}"
        "}",
        name, device_id, switch_id, command_topic, state_topic_full,
        device_id, device_name
    );

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "Published switch discovery for %s, msg_id=%d", switch_id, msg_id);
}

/**
 * Publish all Home Assistant discovery messages
 */
static void publish_ha_discovery(void) {
    ESP_LOGI(TAG, "Publishing Home Assistant discovery messages");

    // Publish sensor discoveries (10 sensors)
    publish_sensor_discovery("pm1_0", "PM1.0", "µg/m³", "pm1",
                            "{{ value_json.pm1_0 }}");

    publish_sensor_discovery("pm2_5", "PM2.5", "µg/m³", "pm25",
                            "{{ value_json.pm2_5 }}");

    publish_sensor_discovery("pm4_0", "PM4.0", "µg/m³", "",
                            "{{ value_json.pm4_0 }}");

    publish_sensor_discovery("pm10", "PM10", "µg/m³", "pm10",
                            "{{ value_json.pm10 }}");

    publish_sensor_discovery("nc0_5", "NC0.5", "#/cm³", "",
                            "{{ value_json.nc0_5 }}");

    publish_sensor_discovery("nc1_0", "NC1.0", "#/cm³", "",
                            "{{ value_json.nc1_0 }}");

    publish_sensor_discovery("nc2_5", "NC2.5", "#/cm³", "",
                            "{{ value_json.nc2_5 }}");

    publish_sensor_discovery("nc4_0", "NC4.0", "#/cm³", "",
                            "{{ value_json.nc4_0 }}");

    publish_sensor_discovery("nc10", "NC10", "#/cm³", "",
                            "{{ value_json.nc10 }}");

    publish_sensor_discovery("typical_size", "Typical Particle Size", "µm", "",
                            "{{ value_json.typical_size }}");

    // Publish button discovery (fan clean)
    publish_button_discovery("fan_clean", "Fan Cleaning", fan_clean_cmd_topic);

    // Publish switch discovery (sleep mode)
    publish_switch_discovery("sleep", "Sleep Mode", sleep_cmd_topic, "sleep_state");

    ESP_LOGI(TAG, "Home Assistant discovery published");
}

/**
 * Publish sensor data to MQTT state topic
 */
static void publish_sensor_data(sensor_data_t *data) {
    if (!mqtt_connected) {
        return;
    }

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{"
        "\"pm1_0\":%.2f,"
        "\"pm2_5\":%.2f,"
        "\"pm4_0\":%.2f,"
        "\"pm10\":%.2f,"
        "\"nc0_5\":%.2f,"
        "\"nc1_0\":%.2f,"
        "\"nc2_5\":%.2f,"
        "\"nc4_0\":%.2f,"
        "\"nc10\":%.2f,"
        "\"typical_size\":%.2f,"
        "\"timestamp\":%lld,"
        "\"status\":\"%s\""
        "}",
        data->pm1_0, data->pm2_5, data->pm4_0, data->pm10,
        data->nc0_5, data->nc1_0, data->nc2_5, data->nc4_0, data->nc10,
        data->typical_size, data->timestamp_ms,
        (data->status == SENSOR_OK) ? "OK" :
        (data->status == SENSOR_COMM_ERROR) ? "COMM_ERROR" :
        (data->status == SENSOR_FAN_CLEANING) ? "CLEANING" :
        (data->status == SENSOR_SLEEPING) ? "SLEEPING" : "NOT_READY"
    );

    esp_mqtt_client_publish(mqtt_client, state_topic, payload, 0, 0, 0);
}

/**
 * Event handler for sensor data ready
 */
static void sensor_data_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    sensor_data_t *data = (sensor_data_t *)event_data;
    publish_sensor_data(data);
}

/**
 * Handle MQTT data events
 */
static void mqtt_data_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

            // Handle fan clean command
            if (strncmp(event->topic, fan_clean_cmd_topic, event->topic_len) == 0) {
                if (strncmp(event->data, "PRESS", event->data_len) == 0) {
                    ESP_LOGI(TAG, "Fan clean button pressed via MQTT");
                    esp_event_post(COMMAND_EVENT, CMD_FAN_CLEAN, NULL, 0, portMAX_DELAY);
                }
            }
            // Handle sleep command
            else if (strncmp(event->topic, sleep_cmd_topic, event->topic_len) == 0) {
                sleep_command_t cmd;
                if (strncmp(event->data, "ON", event->data_len) == 0) {
                    cmd.enabled = true;
                    ESP_LOGI(TAG, "Sleep mode enabled via MQTT");
                } else {
                    cmd.enabled = false;
                    ESP_LOGI(TAG, "Sleep mode disabled via MQTT");
                }
                esp_event_post(COMMAND_EVENT, CMD_SLEEP, &cmd, sizeof(cmd), portMAX_DELAY);

                // Publish state back
                char state_topic_sleep[256];
                snprintf(state_topic_sleep, sizeof(state_topic_sleep),
                        "homeassistant/switch/%s/sleep/state", device_id);
                esp_mqtt_client_publish(mqtt_client, state_topic_sleep,
                                       cmd.enabled ? "ON" : "OFF", 0, 1, 0);
            }
            break;

        default:
            break;
    }
}

/**
 * MQTT event handler
 */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            mqtt_connected = true;

            // Subscribe to command topics
            esp_mqtt_client_subscribe(mqtt_client, fan_clean_cmd_topic, 0);
            esp_mqtt_client_subscribe(mqtt_client, sleep_cmd_topic, 0);
            ESP_LOGI(TAG, "Subscribed to command topics");

            // Publish Home Assistant discovery
            publish_ha_discovery();

            // Register for sensor events
            esp_event_handler_register(SENSOR_EVENT, SENSOR_DATA_READY, sensor_data_handler, NULL);

            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_connected = false;

            // Unregister sensor event handler
            esp_event_handler_unregister(SENSOR_EVENT, SENSOR_DATA_READY, sensor_data_handler);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            mqtt_data_handler(arg, base, event_id, event_data);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %d", event_id);
            break;
    }
}

esp_err_t mqtt_protocol_start(void) {
    ESP_LOGI(TAG, "Starting MQTT protocol");

    // Generate device ID
    generate_device_id();

    // Build topic names
    snprintf(state_topic, sizeof(state_topic),
             "homeassistant/sensor/%s/state", device_id);
    snprintf(fan_clean_cmd_topic, sizeof(fan_clean_cmd_topic),
             "homeassistant/button/%s/fan_clean/set", device_id);
    snprintf(sleep_cmd_topic, sizeof(sleep_cmd_topic),
             "homeassistant/switch/%s/sleep/set", device_id);

    ESP_LOGI(TAG, "Device ID: %s", device_id);
    ESP_LOGI(TAG, "Device Name: %s", device_name);
    ESP_LOGI(TAG, "State topic: %s", state_topic);

    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    // Register event handler
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // Start MQTT client
    esp_err_t ret = esp_mqtt_client_start(mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

esp_err_t mqtt_protocol_stop(void) {
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
    return ESP_OK;
}
```

**Step 4: Create CMakeLists.txt**

Create `components/mqtt_protocol/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "mqtt_protocol.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt main)
```

**Step 5: Create Kconfig for MQTT settings**

Create `components/mqtt_protocol/Kconfig`:

```kconfig
menu "MQTT Protocol Configuration"

    config MQTT_BROKER_URI
        string "MQTT Broker URI"
        default "mqtt://192.168.1.100:1883"
        help
            URI of the MQTT broker (e.g., mqtt://192.168.1.100:1883)

    config MQTT_USERNAME
        string "MQTT Username"
        default ""
        help
            Username for MQTT authentication (leave empty if no auth)

    config MQTT_PASSWORD
        string "MQTT Password"
        default ""
        help
            Password for MQTT authentication (leave empty if no auth)

endmenu
```

**Step 6: Build and verify**

```bash
idf.py build
```

Expected: Clean build with new mqtt_protocol component.

**Step 7: Commit**

```bash
git add components/mqtt_protocol/
git commit -m "feat: add MQTT protocol with Home Assistant Discovery

- Publish sensor data to MQTT state topic at 1Hz
- Auto-discovery of 10 sensor entities in Home Assistant
- Button entity for fan cleaning
- Switch entity for sleep mode
- Subscribe to command topics and post command events
- Configurable broker URI and credentials via menuconfig"
```

---

## Task 6: Integrate MQTT Protocol into Main

**Goal:** Start MQTT protocol from main application.

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

**Step 1: Add MQTT dependency**

Modify `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" "sensor_events.c" "sensor_task.c"
                    INCLUDE_DIRS "."
                    REQUIRES sps30 websocket mqtt_protocol)
```

**Step 2: Start MQTT protocol**

Modify `main/main.c`:

Add include:
```c
#include "mqtt_protocol.h"
```

In `app_main()`, after sensor task start, add:
```c
// Start MQTT protocol
ESP_ERROR_CHECK(mqtt_protocol_start());
ESP_LOGI(TAG, "MQTT protocol started");
```

**Step 3: Build and verify**

```bash
idf.py build
```

Expected: Clean build.

**Step 4: Commit**

```bash
git add main/CMakeLists.txt main/main.c
git commit -m "feat: integrate MQTT protocol into application startup"
```

---

## Task 7: Create REST API Component

**Goal:** Add REST API endpoints for sensor info, status, and control.

**Files:**
- Create: `components/rest_api/include/rest_api.h`
- Create: `components/rest_api/rest_api.c`
- Create: `components/rest_api/CMakeLists.txt`

**Step 1: Create component structure**

```bash
mkdir -p components/rest_api/include
```

**Step 2: Define REST API interface**

Create `components/rest_api/include/rest_api.h`:

```c
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Register REST API endpoints with HTTP server
 *
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t rest_api_register_handlers(httpd_handle_t server);
```

**Step 3: Implement REST API endpoints**

Create `components/rest_api/rest_api.c`:

```c
#include "rest_api.h"
#include "sensor_events.h"
#include "sensor_task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "rest_api";

/**
 * GET /api/info - Device capabilities and metadata
 */
static esp_err_t api_info_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /api/info");

    // Get MAC address for device ID
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[20];
    snprintf(device_id, sizeof(device_id), "esp32_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Build JSON response
    cJSON *root = cJSON_CreateObject();

    // Device info
    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "name", "SPS30 Air Quality Sensor");
    cJSON_AddStringToObject(device, "model", "ESP32-SPS30");
    cJSON_AddStringToObject(device, "manufacturer", "Custom");
    cJSON_AddStringToObject(device, "sw_version", "1.0.0");
    cJSON_AddStringToObject(device, "id", device_id);
    cJSON_AddItemToObject(root, "device", device);

    // Sensors array
    cJSON *sensors = cJSON_CreateArray();

    const char *sensor_defs[][4] = {
        {"pm1_0", "PM1.0", "µg/m³", "pm1"},
        {"pm2_5", "PM2.5", "µg/m³", "pm25"},
        {"pm4_0", "PM4.0", "µg/m³", ""},
        {"pm10", "PM10", "µg/m³", "pm10"},
        {"nc0_5", "NC0.5", "#/cm³", ""},
        {"nc1_0", "NC1.0", "#/cm³", ""},
        {"nc2_5", "NC2.5", "#/cm³", ""},
        {"nc4_0", "NC4.0", "#/cm³", ""},
        {"nc10", "NC10", "#/cm³", ""},
        {"typical_size", "Typical Particle Size", "µm", ""}
    };

    for (int i = 0; i < 10; i++) {
        cJSON *sensor = cJSON_CreateObject();
        cJSON_AddStringToObject(sensor, "id", sensor_defs[i][0]);
        cJSON_AddStringToObject(sensor, "name", sensor_defs[i][1]);
        cJSON_AddStringToObject(sensor, "unit", sensor_defs[i][2]);
        if (strlen(sensor_defs[i][3]) > 0) {
            cJSON_AddStringToObject(sensor, "device_class", sensor_defs[i][3]);
        }
        cJSON_AddStringToObject(sensor, "state_class", "measurement");
        cJSON_AddItemToArray(sensors, sensor);
    }
    cJSON_AddItemToObject(root, "sensors", sensors);

    // Controls array
    cJSON *controls = cJSON_CreateArray();

    cJSON *fan_clean = cJSON_CreateObject();
    cJSON_AddStringToObject(fan_clean, "id", "fan_clean");
    cJSON_AddStringToObject(fan_clean, "name", "Start Fan Cleaning");
    cJSON_AddStringToObject(fan_clean, "type", "button");
    cJSON_AddItemToArray(controls, fan_clean);

    cJSON *sleep = cJSON_CreateObject();
    cJSON_AddStringToObject(sleep, "id", "sleep");
    cJSON_AddStringToObject(sleep, "name", "Sleep Mode");
    cJSON_AddStringToObject(sleep, "type", "switch");
    cJSON_AddItemToArray(controls, sleep);

    cJSON_AddItemToObject(root, "controls", controls);

    // Protocols array
    cJSON *protocols = cJSON_CreateArray();
    cJSON_AddItemToArray(protocols, cJSON_CreateString("websocket"));
    cJSON_AddItemToArray(protocols, cJSON_CreateString("mqtt"));
    cJSON_AddItemToArray(protocols, cJSON_CreateString("rest"));
    cJSON_AddItemToArray(protocols, cJSON_CreateString("prometheus"));
    cJSON_AddItemToObject(root, "protocols", protocols);

    // Convert to string and send
    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * GET /api/status - Current sensor reading and status
 */
static esp_err_t api_status_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /api/status");

    sensor_data_t data;
    esp_err_t ret = sensor_task_get_latest(&data);

    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Failed to get sensor data\"}");
        return ESP_OK;
    }

    // Build JSON response
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "pm1_0", data.pm1_0);
    cJSON_AddNumberToObject(root, "pm2_5", data.pm2_5);
    cJSON_AddNumberToObject(root, "pm4_0", data.pm4_0);
    cJSON_AddNumberToObject(root, "pm10", data.pm10);
    cJSON_AddNumberToObject(root, "nc0_5", data.nc0_5);
    cJSON_AddNumberToObject(root, "nc1_0", data.nc1_0);
    cJSON_AddNumberToObject(root, "nc2_5", data.nc2_5);
    cJSON_AddNumberToObject(root, "nc4_0", data.nc4_0);
    cJSON_AddNumberToObject(root, "nc10", data.nc10);
    cJSON_AddNumberToObject(root, "typical_size", data.typical_size);
    cJSON_AddNumberToObject(root, "timestamp_ms", data.timestamp_ms);

    const char *status_str =
        (data.status == SENSOR_OK) ? "OK" :
        (data.status == SENSOR_COMM_ERROR) ? "COMM_ERROR" :
        (data.status == SENSOR_FAN_CLEANING) ? "CLEANING" :
        (data.status == SENSOR_SLEEPING) ? "SLEEPING" : "NOT_READY";
    cJSON_AddStringToObject(root, "status", status_str);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * POST /api/control/fan_clean - Trigger fan cleaning
 */
static esp_err_t api_fan_clean_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "POST /api/control/fan_clean");

    // Post fan clean command
    esp_event_post(COMMAND_EVENT, CMD_FAN_CLEAN, NULL, 0, portMAX_DELAY);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Fan cleaning started\"}");

    return ESP_OK;
}

/**
 * POST /api/control/sleep - Control sleep mode
 */
static esp_err_t api_sleep_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "POST /api/control/sleep");

    // Read request body
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing request body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *enabled_json = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(enabled_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'enabled' field");
        return ESP_FAIL;
    }

    sleep_command_t cmd = { .enabled = cJSON_IsTrue(enabled_json) };
    cJSON_Delete(root);

    // Post sleep command
    esp_event_post(COMMAND_EVENT, CMD_SLEEP, &cmd, sizeof(cmd), portMAX_DELAY);

    httpd_resp_set_type(req, "application/json");
    char response[128];
    snprintf(response, sizeof(response),
             "{\"status\":\"success\",\"message\":\"Sleep mode %s\"}",
             cmd.enabled ? "enabled" : "disabled");
    httpd_resp_sendstr(req, response);

    return ESP_OK;
}

/**
 * GET /api/diagnostics - System diagnostics
 */
static esp_err_t api_diagnostics_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /api/diagnostics");

    // Get WiFi info
    wifi_ap_record_t ap_info;
    esp_wifi_sta_get_ap_info(&ap_info);

    // Get IP address
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    // Get sensor data
    sensor_data_t data;
    esp_err_t sensor_ret = sensor_task_get_latest(&data);

    // Build JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", true);
    cJSON_AddNumberToObject(wifi, "rssi", ap_info.rssi);
    cJSON_AddStringToObject(wifi, "ip", ip_str);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON *sensor = cJSON_CreateObject();
    if (sensor_ret == ESP_OK) {
        cJSON_AddStringToObject(sensor, "status",
            (data.status == SENSOR_OK) ? "OK" :
            (data.status == SENSOR_COMM_ERROR) ? "COMM_ERROR" :
            (data.status == SENSOR_FAN_CLEANING) ? "CLEANING" :
            (data.status == SENSOR_SLEEPING) ? "SLEEPING" : "NOT_READY");
        cJSON_AddNumberToObject(sensor, "last_read_ms", data.timestamp_ms);
    } else {
        cJSON_AddStringToObject(sensor, "status", "UNAVAILABLE");
    }
    cJSON_AddItemToObject(root, "sensor", sensor);

    cJSON *heap = cJSON_CreateObject();
    cJSON_AddNumberToObject(heap, "free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(heap, "min_free", esp_get_minimum_free_heap_size());
    cJSON_AddItemToObject(root, "heap", heap);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

esp_err_t rest_api_register_handlers(httpd_handle_t server) {
    ESP_LOGI(TAG, "Registering REST API handlers");

    // GET /api/info
    httpd_uri_t info_uri = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = api_info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &info_uri);

    // GET /api/status
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_uri);

    // GET /api/readings/latest (alias for /api/status)
    httpd_uri_t latest_uri = {
        .uri = "/api/readings/latest",
        .method = HTTP_GET,
        .handler = api_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &latest_uri);

    // POST /api/control/fan_clean
    httpd_uri_t fan_clean_uri = {
        .uri = "/api/control/fan_clean",
        .method = HTTP_POST,
        .handler = api_fan_clean_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &fan_clean_uri);

    // POST /api/control/sleep
    httpd_uri_t sleep_uri = {
        .uri = "/api/control/sleep",
        .method = HTTP_POST,
        .handler = api_sleep_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &sleep_uri);

    // GET /api/diagnostics
    httpd_uri_t diag_uri = {
        .uri = "/api/diagnostics",
        .method = HTTP_GET,
        .handler = api_diagnostics_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &diag_uri);

    ESP_LOGI(TAG, "REST API handlers registered");
    return ESP_OK;
}
```

**Step 4: Create CMakeLists.txt**

Create `components/rest_api/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "rest_api.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server main json)
```

**Step 5: Build and verify**

```bash
idf.py build
```

Expected: Clean build with new rest_api component.

**Step 6: Commit**

```bash
git add components/rest_api/
git commit -m "feat: add REST API endpoints

- GET /api/info - device capabilities and metadata
- GET /api/status - current sensor readings
- GET /api/readings/latest - alias for status
- POST /api/control/fan_clean - trigger fan cleaning
- POST /api/control/sleep - control sleep mode
- GET /api/diagnostics - system health info"
```

---

## Task 8: Add Prometheus Metrics Endpoint

**Goal:** Add `/metrics` endpoint in Prometheus text format.

**Files:**
- Modify: `components/rest_api/rest_api.c`

**Step 1: Implement Prometheus metrics handler**

Add to `components/rest_api/rest_api.c`:

```c
/**
 * GET /metrics - Prometheus metrics endpoint
 */
static esp_err_t metrics_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "GET /metrics");

    sensor_data_t data;
    esp_err_t ret = sensor_task_get_latest(&data);

    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "# Sensor unavailable\n");
        return ESP_OK;
    }

    // Build Prometheus text format
    char metrics[2048];
    int offset = 0;

    // Helper macro for adding metrics
    #define ADD_METRIC(help, type, name, value) \
        offset += snprintf(metrics + offset, sizeof(metrics) - offset, \
                          "# HELP " name " " help "\n" \
                          "# TYPE " name " " type "\n" \
                          name " %.2f\n", value)

    ADD_METRIC("PM1.0 mass concentration in micrograms per cubic meter",
               "gauge", "sps30_pm1_0", data.pm1_0);

    ADD_METRIC("PM2.5 mass concentration in micrograms per cubic meter",
               "gauge", "sps30_pm2_5", data.pm2_5);

    ADD_METRIC("PM4.0 mass concentration in micrograms per cubic meter",
               "gauge", "sps30_pm4_0", data.pm4_0);

    ADD_METRIC("PM10 mass concentration in micrograms per cubic meter",
               "gauge", "sps30_pm10", data.pm10);

    ADD_METRIC("Number concentration 0.5um in particles per cubic centimeter",
               "gauge", "sps30_nc0_5", data.nc0_5);

    ADD_METRIC("Number concentration 1.0um in particles per cubic centimeter",
               "gauge", "sps30_nc1_0", data.nc1_0);

    ADD_METRIC("Number concentration 2.5um in particles per cubic centimeter",
               "gauge", "sps30_nc2_5", data.nc2_5);

    ADD_METRIC("Number concentration 4.0um in particles per cubic centimeter",
               "gauge", "sps30_nc4_0", data.nc4_0);

    ADD_METRIC("Number concentration 10um in particles per cubic centimeter",
               "gauge", "sps30_nc10", data.nc10);

    ADD_METRIC("Typical particle size in micrometers",
               "gauge", "sps30_typical_size", data.typical_size);

    // Add sensor status as numeric
    offset += snprintf(metrics + offset, sizeof(metrics) - offset,
                      "# HELP sps30_sensor_status Sensor status (0=OK, 1=error, 2=not_ready, 3=cleaning, 4=sleeping)\n"
                      "# TYPE sps30_sensor_status gauge\n"
                      "sps30_sensor_status %d\n", (int)data.status);

    #undef ADD_METRIC

    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    httpd_resp_sendstr(req, metrics);

    return ESP_OK;
}
```

**Step 2: Register metrics endpoint**

In `rest_api_register_handlers()`, add before the final return:

```c
// GET /metrics (Prometheus)
httpd_uri_t metrics_uri = {
    .uri = "/metrics",
    .method = HTTP_GET,
    .handler = metrics_handler,
    .user_ctx = NULL
};
httpd_register_uri_handler(server, &metrics_uri);
```

**Step 3: Build and verify**

```bash
idf.py build
```

Expected: Clean build.

**Step 4: Commit**

```bash
git add components/rest_api/rest_api.c
git commit -m "feat: add Prometheus metrics endpoint at /metrics

- Expose all sensor readings in Prometheus text format
- Include sensor status as numeric gauge
- Compatible with Prometheus, Grafana, and other monitoring tools"
```

---

## Task 9: Integrate REST API into WebSocket Server

**Goal:** Register REST API handlers with the existing HTTP server.

**Files:**
- Modify: `components/websocket/include/websocket_server.h`
- Modify: `components/websocket/websocket_server.c`
- Modify: `components/websocket/CMakeLists.txt`

**Step 1: Expose HTTP server handle**

Modify `components/websocket/include/websocket_server.h`:

Add after existing function declarations:

```c
/**
 * Get the HTTP server handle
 * Allows other components to register additional handlers
 */
httpd_handle_t websocket_server_get_handle(void);
```

**Step 2: Implement getter and store server handle**

Modify `components/websocket/websocket_server.c`:

Add global variable at top (with other static variables):
```c
static httpd_handle_t server = NULL;
```

Find where `httpd_start()` is called and save the handle:
```c
// Replace: httpd_handle_t server = NULL;
// With: (server is now global, declared at top of file)

esp_err_t ret = httpd_start(&server, &config);
```

Add getter function at end of file:
```c
httpd_handle_t websocket_server_get_handle(void) {
    return server;
}
```

**Step 3: Add REST API dependency**

Modify `components/websocket/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "websocket_server.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_http_server esp_timer
                    REQUIRES main rest_api)
```

**Step 4: Register REST API handlers after server start**

Modify `components/websocket/websocket_server.c`:

Add include at top:
```c
#include "rest_api.h"
```

In `websocket_server_start()`, after `httpd_start()` succeeds, add:
```c
// Register REST API handlers
ESP_LOGI(TAG, "Registering REST API handlers");
rest_api_register_handlers(server);
```

**Step 5: Build and verify**

```bash
idf.py build
```

Expected: Clean build.

**Step 6: Commit**

```bash
git add components/websocket/include/websocket_server.h components/websocket/websocket_server.c components/websocket/CMakeLists.txt
git commit -m "feat: integrate REST API with HTTP server

- Expose HTTP server handle for component access
- Register REST API handlers at server startup
- All REST endpoints now available on same server as WebSocket"
```

---

## Task 10: Update Web UI with Controls

**Goal:** Add fan cleaning and sleep controls to web interface using REST API.

**Files:**
- Modify: `www/index.html`
- Modify: `www/app.js`
- Modify: `www/style.css`

**Step 1: Add control UI elements**

Modify `www/index.html`, add after existing controls section:

```html
<div class="controls">
    <button id="connect-btn" onclick="connectToServer()">Connect</button>
    <button id="disconnect-btn" onclick="disconnectFromServer()" disabled>Disconnect</button>
</div>

<!-- Add new sensor controls section -->
<div class="sensor-controls">
    <h2>Sensor Controls</h2>
    <button id="fan-clean-btn" onclick="triggerFanClean()" class="control-btn">
        Start Fan Cleaning
    </button>
    <div class="sleep-control">
        <label class="switch">
            <input type="checkbox" id="sleep-toggle" onchange="toggleSleep()">
            <span class="slider"></span>
        </label>
        <span>Sleep Mode</span>
    </div>
    <div id="control-status" class="control-status"></div>
</div>
```

**Step 2: Add control functions to JavaScript**

Modify `www/app.js`, add these functions at the end:

```javascript
// Trigger fan cleaning via REST API
async function triggerFanClean() {
    const btn = document.getElementById('fan-clean-btn');
    const status = document.getElementById('control-status');

    btn.disabled = true;
    btn.textContent = 'Cleaning...';
    status.textContent = 'Starting fan cleaning (takes ~10 seconds)...';
    status.className = 'control-status info';

    try {
        const response = await fetch('/api/control/fan_clean', {
            method: 'POST'
        });

        const data = await response.json();

        if (response.ok) {
            status.textContent = data.message;
            status.className = 'control-status success';

            // Re-enable after 10 seconds
            setTimeout(() => {
                btn.disabled = false;
                btn.textContent = 'Start Fan Cleaning';
                status.textContent = '';
            }, 10000);
        } else {
            throw new Error(data.error || 'Failed to start fan cleaning');
        }
    } catch (error) {
        status.textContent = 'Error: ' + error.message;
        status.className = 'control-status error';
        btn.disabled = false;
        btn.textContent = 'Start Fan Cleaning';
    }
}

// Toggle sleep mode via REST API
async function toggleSleep() {
    const toggle = document.getElementById('sleep-toggle');
    const status = document.getElementById('control-status');
    const enabled = toggle.checked;

    status.textContent = `${enabled ? 'Enabling' : 'Disabling'} sleep mode...`;
    status.className = 'control-status info';

    try {
        const response = await fetch('/api/control/sleep', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ enabled: enabled })
        });

        const data = await response.json();

        if (response.ok) {
            status.textContent = data.message;
            status.className = 'control-status success';
            setTimeout(() => { status.textContent = ''; }, 3000);
        } else {
            throw new Error(data.error || 'Failed to toggle sleep mode');
        }
    } catch (error) {
        status.textContent = 'Error: ' + error.message;
        status.className = 'control-status error';
        // Revert toggle on error
        toggle.checked = !enabled;
    }
}

// Fetch device info on page load
async function loadDeviceInfo() {
    try {
        const response = await fetch('/api/info');
        const data = await response.json();
        console.log('Device info:', data);
        // Could display this in UI if desired
    } catch (error) {
        console.error('Failed to load device info:', error);
    }
}

// Call on page load
window.addEventListener('load', () => {
    loadDeviceInfo();
});
```

**Step 3: Add CSS styling for controls**

Modify `www/style.css`, add at the end:

```css
/* Sensor controls */
.sensor-controls {
    background: white;
    padding: 20px;
    border-radius: 8px;
    box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    margin: 20px auto;
    max-width: 500px;
}

.sensor-controls h2 {
    margin-top: 0;
    margin-bottom: 15px;
    font-size: 1.2em;
}

.control-btn {
    width: 100%;
    padding: 12px;
    margin-bottom: 15px;
    background: #4CAF50;
    color: white;
    border: none;
    border-radius: 4px;
    font-size: 1em;
    cursor: pointer;
    transition: background 0.3s;
}

.control-btn:hover:not(:disabled) {
    background: #45a049;
}

.control-btn:disabled {
    background: #ccc;
    cursor: not-allowed;
}

.sleep-control {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 10px;
}

/* Toggle switch */
.switch {
    position: relative;
    display: inline-block;
    width: 50px;
    height: 24px;
}

.switch input {
    opacity: 0;
    width: 0;
    height: 0;
}

.slider {
    position: absolute;
    cursor: pointer;
    top: 0;
    left: 0;
    right: 0;
    bottom: 0;
    background-color: #ccc;
    transition: 0.4s;
    border-radius: 24px;
}

.slider:before {
    position: absolute;
    content: "";
    height: 18px;
    width: 18px;
    left: 3px;
    bottom: 3px;
    background-color: white;
    transition: 0.4s;
    border-radius: 50%;
}

input:checked + .slider {
    background-color: #2196F3;
}

input:checked + .slider:before {
    transform: translateX(26px);
}

.control-status {
    padding: 10px;
    border-radius: 4px;
    margin-top: 10px;
    text-align: center;
}

.control-status.success {
    background: #d4edda;
    color: #155724;
}

.control-status.error {
    background: #f8d7da;
    color: #721c24;
}

.control-status.info {
    background: #d1ecf1;
    color: #0c5460;
}
```

**Step 4: Build and verify**

```bash
idf.py build
```

Expected: Clean build. Web assets compressed and included.

**Step 5: Commit**

```bash
git add www/index.html www/app.js www/style.css
git commit -m "feat: add sensor control UI to web interface

- Add fan cleaning button with REST API integration
- Add sleep mode toggle switch
- Display control status feedback
- Fetch and log device info on page load"
```

---

## Task 11: Update Documentation

**Goal:** Update README and CLAUDE.md to reflect new multi-protocol architecture.

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`

**Step 1: Update README with features and setup**

Modify `README.md`:

```markdown
# ESP32 SPS30 Multi-Protocol Air Quality Sensor

Professional multi-protocol air quality monitoring system using ESP32 and Sensirion SPS30 particulate matter sensor.

## Features

- **Multi-Protocol Support:**
  - WebSocket - Real-time browser visualization
  - MQTT - Home Assistant auto-discovery integration
  - REST API - Direct HTTP queries and control
  - Prometheus - Metrics endpoint for monitoring tools

- **Self-Describing:**
  - Home Assistant auto-discovery (10 sensor entities + controls)
  - REST API exposes device capabilities and metadata
  - Supports multiple concurrent protocols

- **Sensor Control:**
  - Fan cleaning trigger (via WebSocket, MQTT, REST)
  - Sleep/wake mode
  - Real-time status reporting

- **Professional Architecture:**
  - Event-driven ESP-IDF design
  - Thread-safe sensor data access
  - Auto-reconnect and error recovery
  - Comprehensive diagnostics endpoint

## Hardware Requirements

- ESP32 development board
- Sensirion SPS30 particulate matter sensor
- USB cable for programming

### Wiring

Connect SPS30 to ESP32 UART (see `components/sps30/src/sensirion_uart_hal.c` for pin config):
- SPS30 TX → ESP32 RX (default GPIO 16)
- SPS30 RX → ESP32 TX (default GPIO 17)
- SPS30 VDD → 5V
- SPS30 GND → GND

## Software Setup

### Prerequisites

- ESP-IDF v5.5.1 or later
- Home Assistant (optional, for MQTT integration)
- Mosquitto or other MQTT broker (optional)

### Configuration

```bash
# Configure WiFi and MQTT settings
idf.py menuconfig

# Important settings:
# - Example Connection Configuration → WiFi SSID/Password
# - MQTT Protocol Configuration → Broker URI, username, password
```

### Build and Flash

```bash
# Build
idf.py build

# Flash to ESP32
idf.py flash monitor
```

## Usage

### Web Interface

1. Device advertises as `sps30.local` via mDNS
2. Open `http://sps30.local/` in browser
3. Click "Connect" to start real-time WebSocket stream
4. Use controls to trigger fan cleaning or sleep mode

### Home Assistant Integration

1. Configure MQTT broker in `idf.py menuconfig`
2. Flash firmware and power on device
3. In Home Assistant, device auto-discovers with 10 sensors + 2 controls
4. Add sensors to dashboard, create automations

### REST API

**Get device info:**
```bash
curl http://sps30.local/api/info
```

**Get current readings:**
```bash
curl http://sps30.local/api/status
```

**Trigger fan cleaning:**
```bash
curl -X POST http://sps30.local/api/control/fan_clean
```

**Enable sleep mode:**
```bash
curl -X POST http://sps30.local/api/control/sleep \
  -H "Content-Type: application/json" \
  -d '{"enabled": true}'
```

**System diagnostics:**
```bash
curl http://sps30.local/api/diagnostics
```

### Prometheus Metrics

Scrape metrics endpoint:
```bash
curl http://sps30.local/metrics
```

Configure Prometheus to scrape `sps30.local:80/metrics` at desired interval.

## Architecture

Event-driven system built on ESP-IDF:

- **Sensor Task** - Reads SPS30 at 1Hz, publishes events
- **Protocol Tasks** - WebSocket, MQTT, HTTP all subscribe to sensor events
- **Command Events** - Protocols post commands, sensor task handles them
- **Thread-Safe** - Mutex-protected access to latest readings

See `CLAUDE.md` for detailed architecture documentation.

## License

MIT License - see LICENSE file
```

**Step 2: Update CLAUDE.md architecture section**

Modify `CLAUDE.md` - replace the "Architecture" section with:

```markdown
## Architecture

### Event-Driven Multi-Protocol System

**Core Design:** ESP-IDF event loop coordinates all components. Sensor task publishes data events; protocol tasks subscribe and format data for their channels.

### Component Structure

**main/** - Application core
- `main.c` - Entry point, initializes all subsystems
- `sensor_events.h/c` - Event base definitions (SENSOR_EVENT, COMMAND_EVENT)
- `sensor_task.h/c` - SPS30 reading task, publishes events at 1Hz

**components/sps30/** - SPS30 sensor driver
- Wraps Sensirion UART driver (git submodule)
- `src/sensirion_uart_hal.c` - ESP32 UART HAL implementation

**components/websocket/** - WebSocket protocol
- HTTP server with WebSocket endpoint at `/ws`
- Subscribes to sensor events, broadcasts JSON to clients
- Handles WebSocket commands (fan clean, sleep)
- Serves static web assets from SPIFFS

**components/mqtt_protocol/** - MQTT protocol
- Home Assistant MQTT Discovery integration
- Publishes sensor data to state topic
- Subscribes to command topics (fan clean, sleep)
- Auto-reconnect on broker disconnect

**components/rest_api/** - REST API protocol
- Device info endpoint (`/api/info`)
- Current readings (`/api/status`, `/api/readings/latest`)
- Control endpoints (`/api/control/fan_clean`, `/api/control/sleep`)
- Diagnostics (`/api/diagnostics`)
- Prometheus metrics (`/metrics`)

### Data Flow

```
SPS30 Sensor (1Hz)
       ↓
Sensor Task reads → Posts SENSOR_DATA_READY event
                           ↓
       ┌───────────────────┼───────────────────┐
       ↓                   ↓                   ↓
WebSocket Handler    MQTT Handler      REST API cache
  (broadcasts)        (publishes)       (get_latest)
       ↓                   ↓                   ↓
   Browsers          Home Assistant    Prometheus/curl
```

### Command Flow

```
User Action (Web UI, HA, REST)
       ↓
Protocol posts COMMAND_EVENT (CMD_FAN_CLEAN, CMD_SLEEP)
       ↓
Sensor Task event handler
       ↓
SPS30 driver call (fan_clean, sleep, wake)
       ↓
Posts SENSOR_STATUS_CHANGE event
       ↓
All protocols receive updated status
```
```

**Step 3: Build and verify**

```bash
idf.py build
```

Expected: Clean build.

**Step 4: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: update documentation for multi-protocol architecture

- Add comprehensive README with all protocols
- Document REST API endpoints
- Add Home Assistant setup guide
- Update CLAUDE.md architecture section with event flow diagrams"
```

---

## Task 12: Testing and Verification Plan

**Goal:** Document testing approach and create testing checklist.

**Files:**
- Create: `docs/TESTING.md`

**Step 1: Create testing documentation**

Create `docs/TESTING.md`:

```markdown
# Testing Guide

## Prerequisites

- ESP32 flashed with multi-protocol firmware
- SPS30 connected via UART
- WiFi network configured
- MQTT broker running (for MQTT tests)
- Home Assistant installed (for HA integration tests)

## Unit Testing

### Event System

**Test: Event publishing and subscription**

```c
// Verify events can be posted and received
// Test in sensor_task.c by adding debug logs
ESP_LOGI(TAG, "Posted SENSOR_DATA_READY event");
```

Expected: Log shows event posted, all protocol handlers receive it.

### Sensor Task

**Test: Sensor initialization and reading**

1. Flash and monitor serial output
2. Look for: `Sensor task started`
3. Look for: `SPS30 initialized and measurement started`
4. Look for: `Posted SENSOR_DATA_READY event` every 1 second

Expected: Regular 1Hz sensor readings, no errors.

**Test: Error recovery**

1. Unplug SPS30 UART while running
2. Observe error logs: `Failed to read measurement`
3. Replug SPS30
4. Verify recovery: `Posted SENSOR_DATA_READY event` resumes

Expected: Status changes to COMM_ERROR, recovers when sensor reconnected.

## Protocol Testing

### WebSocket

**Test: Connection and data stream**

1. Open `http://sps30.local/` in browser
2. Click "Connect"
3. Verify: Status shows "Connected"
4. Verify: Charts update with real sensor data every 1 second
5. Verify: Table shows all 10 metrics

Expected: Real-time updates, no disconnections.

**Test: Fan cleaning command**

1. Click "Start Fan Cleaning" button
2. Verify: Button disabled for 10 seconds
3. Verify: Status shows "Cleaning in progress"
4. Monitor serial: Look for `Starting fan cleaning`
5. After 10 seconds: Status returns to "OK"

Expected: Command triggers, status updates, button re-enables.

**Test: Sleep mode command**

1. Toggle "Sleep Mode" switch ON
2. Verify: Serial shows `Entering sleep mode`
3. Verify: Data updates stop (sensor sleeping)
4. Toggle switch OFF
5. Verify: Serial shows `Waking from sleep`
6. Verify: Data updates resume

Expected: Sleep/wake works, data stream pauses/resumes.

### MQTT

**Test: Broker connection**

1. Monitor serial output
2. Look for: `MQTT_EVENT_CONNECTED`
3. Look for: `Subscribed to command topics`
4. Look for: `Home Assistant discovery published`

Expected: Successful connection, all discovery messages sent.

**Test: Home Assistant auto-discovery**

1. Open Home Assistant
2. Navigate to Settings → Devices & Services → MQTT
3. Look for device: "SPS30 Air Quality [MAC]"
4. Click device, verify 10 sensor entities:
   - PM1.0, PM2.5, PM4.0, PM10
   - NC0.5, NC1.0, NC2.5, NC4.0, NC10
   - Typical Particle Size
5. Verify 2 control entities:
   - Fan Cleaning (button)
   - Sleep Mode (switch)

Expected: All entities discovered automatically, no manual config needed.

**Test: Data publishing**

1. Add PM2.5 sensor to HA dashboard
2. Verify: Value updates every 1 second
3. Create history graph
4. Verify: Graph shows real-time trend

Expected: Live data in HA, updates at 1Hz.

**Test: Command from Home Assistant**

1. In HA, click "Fan Cleaning" button
2. Monitor ESP32 serial: `Fan clean button pressed via MQTT`
3. Verify: Sensor status changes to "Cleaning" for 10 seconds
4. Toggle "Sleep Mode" switch in HA
5. Monitor ESP32 serial: `Sleep mode enabled via MQTT`

Expected: Commands from HA trigger sensor actions.

**Test: MQTT broker disconnect/reconnect**

1. Stop MQTT broker (e.g., `systemctl stop mosquitto`)
2. Monitor ESP32: `MQTT_EVENT_DISCONNECTED`
3. Restart broker
4. Monitor ESP32: `MQTT_EVENT_CONNECTED`
5. Verify: Discovery republished, data resumes

Expected: Auto-reconnect, republish discovery.

### REST API

**Test: GET /api/info**

```bash
curl http://sps30.local/api/info | jq
```

Expected: JSON with device info, 10 sensors, 2 controls, 4 protocols.

**Test: GET /api/status**

```bash
curl http://sps30.local/api/status | jq
```

Expected: JSON with current readings, all 10 metrics, status "OK".

**Test: POST /api/control/fan_clean**

```bash
curl -X POST http://sps30.local/api/control/fan_clean
```

Monitor serial: `POST /api/control/fan_clean`

Expected: `{"status":"success","message":"Fan cleaning started"}`

**Test: POST /api/control/sleep**

```bash
curl -X POST http://sps30.local/api/control/sleep \
  -H "Content-Type: application/json" \
  -d '{"enabled": true}'
```

Expected: `{"status":"success","message":"Sleep mode enabled"}`

Verify sensor stops publishing data.

**Test: GET /api/diagnostics**

```bash
curl http://sps30.local/api/diagnostics | jq
```

Expected: JSON with uptime, WiFi info, sensor status, heap stats.

### Prometheus

**Test: GET /metrics**

```bash
curl http://sps30.local/metrics
```

Expected: Prometheus text format output:
```
# HELP sps30_pm2_5 PM2.5 mass concentration in micrograms per cubic meter
# TYPE sps30_pm2_5 gauge
sps30_pm2_5 5.80
...
```

**Test: Grafana scraping**

1. Configure Prometheus to scrape `sps30.local:80/metrics`
2. Verify metrics appear in Prometheus targets
3. Create Grafana dashboard with PM2.5 graph
4. Verify real-time data

Expected: Metrics scraped successfully, graphed in Grafana.

## Integration Testing

### Multiple Simultaneous Clients

**Test: WebSocket + MQTT + REST simultaneously**

1. Open web browser connected via WebSocket
2. Have Home Assistant connected via MQTT
3. Run curl loop querying REST API every 5 seconds
4. Verify all three receive data
5. Monitor heap usage in `/api/diagnostics`

Expected: All protocols work simultaneously, no memory leaks.

### Error Scenarios

**Test: WiFi disconnect**

1. Disable WiFi AP while ESP32 running
2. Monitor serial: WiFi disconnect event
3. Re-enable WiFi AP
4. Verify: ESP32 reconnects, all protocols resume

Expected: Auto-reconnect, protocols recover.

**Test: Sensor communication error**

1. Disconnect SPS30 TX line (simulate comm failure)
2. Verify all protocols report status "COMM_ERROR"
3. Reconnect TX line
4. Verify recovery to "OK" status

Expected: Error detected and reported, auto-recovery.

## Performance Testing

**Test: Long-term stability**

1. Run device for 24 hours
2. Check `/api/diagnostics` periodically
3. Monitor heap free and min_free
4. Check for task watchdog resets

Expected: No memory leaks, no crashes, stable operation.

**Test: Throughput**

1. Connect 5 WebSocket clients simultaneously
2. Have MQTT publishing at 1Hz
3. Run REST API query every second
4. Monitor system performance

Expected: All clients receive data, no dropped messages.

## Acceptance Criteria

- [ ] All four protocols working (WebSocket, MQTT, REST, Prometheus)
- [ ] Home Assistant auto-discovery functional
- [ ] All 10 sensor metrics reported correctly
- [ ] Fan cleaning command works from all interfaces
- [ ] Sleep mode works from all interfaces
- [ ] Error recovery tested and working
- [ ] WiFi reconnect tested and working
- [ ] 24-hour stability test passed
- [ ] Web UI polished and responsive
- [ ] Documentation complete and accurate
```

**Step 2: Commit**

```bash
git add docs/TESTING.md
git commit -m "docs: add comprehensive testing guide

- Unit tests for event system and sensor task
- Protocol-specific test procedures (WebSocket, MQTT, REST, Prometheus)
- Home Assistant integration testing
- Error scenario testing
- Performance and stability testing
- Acceptance criteria checklist"
```

---

## Deployment Checklist

Once all tasks are complete, verify the following before considering the project "done":

### Build and Flash
- [ ] `idf.py build` completes without errors
- [ ] `idf.py flash monitor` successfully flashes and boots
- [ ] WiFi connects and mDNS advertises `sps30.local`

### Protocols
- [ ] WebSocket: Browser connects, receives data, controls work
- [ ] MQTT: Connects to broker, publishes data, HA discovery works
- [ ] REST API: All endpoints respond correctly
- [ ] Prometheus: `/metrics` returns valid format

### Controls
- [ ] Fan cleaning triggers from all interfaces
- [ ] Sleep mode works from all interfaces
- [ ] Status updates correctly during operations

### Home Assistant
- [ ] Device auto-discovers in HA
- [ ] All 10 sensors appear
- [ ] Button and switch entities work
- [ ] Data updates in real-time

### Error Handling
- [ ] Sensor disconnect/reconnect works
- [ ] WiFi disconnect/reconnect works
- [ ] MQTT broker offline/online works

### Documentation
- [ ] README.md accurate and complete
- [ ] CLAUDE.md reflects current architecture
- [ ] API documented
- [ ] Testing guide available

### Code Quality
- [ ] No hardcoded secrets (WiFi/MQTT in menuconfig)
- [ ] ESP-IDF conventions followed
- [ ] Proper error checking
- [ ] Logging appropriate (ESP_LOGI/E/W)

### Polish
- [ ] Web UI looks professional
- [ ] No console errors in browser
- [ ] Responsive design works on mobile
- [ ] Version tagged (v1.0.0)

---

## Execution Notes

**Estimated Time:** 8-12 hours for full implementation

**Dependencies:**
- Tasks 1-2 must complete before Task 3
- Task 5 must complete before Task 6
- Task 7-8 must complete before Task 9
- Task 9 must complete before Task 10
- Task 11-12 can be done in parallel with final testing

**Testing Strategy:**
- Build and test after each task commit
- Flash to device after Tasks 3, 6, 9, 10 to verify incremental progress
- Full integration testing after Task 10

**Potential Issues:**
- MQTT library version compatibility - check ESP-IDF documentation
- WebSocket buffer sizes - may need tuning for large JSON payloads
- Heap usage with all protocols active - monitor via diagnostics endpoint

---

**End of Implementation Plan**
