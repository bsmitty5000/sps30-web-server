#pragma once

#include "esp_event.h"
#include <stdbool.h>
#include <stdint.h>

// Event base declarations
ESP_EVENT_DECLARE_BASE(SENSOR_EVENT);
ESP_EVENT_DECLARE_BASE(COMMAND_EVENT);

// Sensor event IDs
typedef enum 
{
    SENSOR_DATA_READY,      // New sensor reading available
    SENSOR_STATUS_CHANGE,   // Sensor status changed
    SENSOR_ERROR            // Sensor error occurred
} sensor_event_id_t;

// Command event IDs
typedef enum 
{
    CMD_FAN_CLEAN,         // Trigger fan cleaning
    CMD_SLEEP,             // Enter sleep mode
    CMD_WAKE              // Exit sleep mode
} command_event_id_t;

// Sensor status enumeration
typedef enum 
{
    SENSOR_OK = 0,
    SENSOR_COMM_ERROR = 1,
    SENSOR_NOT_READY = 2,
    SENSOR_FAN_CLEANING = 3,
    SENSOR_SLEEPING = 4
} sensor_status_t;

// Sensor data structure (posted with SENSOR_DATA_READY)
typedef struct 
{
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
typedef struct 
{
    bool enabled;  // true = sleep, false = wake
} sleep_command_t;

// Initialize the event loop
esp_err_t sensor_events_init(void);

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