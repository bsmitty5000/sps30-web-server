# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based web server that reads particulate matter data from a Sensirion SPS30 sensor via UART and streams it to web clients using WebSockets. The device is accessible at `sps30.local` via mDNS. Built using ESP-IDF framework (v5.5.1).

## Build System & Commands

This project uses ESP-IDF's CMake build system with `idf.py` as the primary build tool.

### Configuration
```bash
# Configure WiFi credentials and other settings
idf.py menuconfig
# Important settings to configure:
# - Example Connection Configuration -> WiFi SSID
# - Example Connection Configuration -> WiFi Password
# - CONFIG_MDNS_HOST_NAME (default: "simple-sps30")
# - CONFIG_WEB_MOUNT_POINT (default: "/www")
```

### Building
```bash
# Build the entire project
idf.py build

# Clean build artifacts
idf.py fullclean
```

### Flashing & Monitoring
```bash
# Flash to connected ESP32
idf.py flash

# Monitor serial output
idf.py monitor

# Flash and immediately start monitoring
idf.py flash monitor
```

### Web Assets
Web assets in `www/` (HTML, CSS, JS) are automatically:
1. Copied to `build/web_build/` during build
2. Compressed with gzip via `compress_assets.py`
3. Packaged into SPIFFS partition (`www` partition in partitions.csv)
4. Served with `Content-Encoding: gzip` header

The build process handles this automatically - just modify files in `www/` and rebuild.

## Architecture

### Component Structure

**main/main.c** - Application entry point
- Initializes NVS, network stack, event loop
- Sets up mDNS hostname (`simple-sps30.local`) and NetBIOS name
- Mounts SPIFFS filesystem for web assets
- Configures SNTP time sync (timezone: EST5EDT)
- Initializes and starts SPS30 sensor reading task
- Starts WebSocket server with real-time sensor data broadcasts

**components/websocket/** - WebSocket HTTP server component
- `websocket_server_start()` - Main entry point that starts HTTP server on port 80
- Static file serving with automatic gzip support
- WebSocket endpoint at `/ws` for bidirectional communication
- Client management (max 5 concurrent clients via `MAX_WEBSOCKET_CLIENTS`)
- Broadcast task sends JSON data to all registered clients every 5 seconds
- Client registration protocol: clients send `{"action": "registerClient"}` to subscribe to broadcasts
- Thread-safe client list managed with FreeRTOS mutex
- Uses `httpd_queue_work()` for async broadcast to avoid blocking

**components/sps30/** - SPS30 sensor driver
- Wraps Sensirion's UART-based SPS30 driver (git submodule at `sensirion-uart/`)
- `src/sensirion_uart_hal.c` - ESP32 UART HAL implementation
- Reads particulate matter concentrations (PM1.0, PM2.5, PM4.0, PM10) and particle counts
- Fully integrated with WebSocket broadcast system for real-time sensor data streaming

### Key Dependencies

- **espressif/mdns** - mDNS responder for `.local` hostname resolution
- **protocol_examples_common** - ESP-IDF example WiFi connection helpers
- **esp_http_server** - HTTP/WebSocket server (built into ESP-IDF)
- **spiffs** - File system for web assets

### Partition Layout (partitions.csv)

```
nvs       - 24KB  - Non-volatile storage
phy_init  - 4KB   - WiFi PHY calibration
factory   - 1MB   - Application firmware
www       - 960KB - SPIFFS partition for web assets
```

### WebSocket Communication Protocol

**Client → Server:**
```json
{"action": "registerClient"}       // Subscribe to broadcasts
{"action": "closeConnection"}      // Unsubscribe and disconnect
```

**Server → Client (broadcast every 1s):**
```json
{
  "pm1_0": 2.5,                     // PM1.0 concentration (µg/m³)
  "pm2_5": 5.8,                     // PM2.5 concentration (µg/m³)
  "pm4_0": 7.2,                     // PM4.0 concentration (µg/m³)
  "pm10": 9.1,                      // PM10 concentration (µg/m³)
  "nc0_5": 12.4,                    // Number concentration 0.5µm (#/cm³)
  "nc1_0": 8.7,                     // Number concentration 1.0µm (#/cm³)
  "nc2_5": 3.2,                     // Number concentration 2.5µm (#/cm³)
  "nc4_0": 1.1,                     // Number concentration 4.0µm (#/cm³)
  "nc10": 0.3,                      // Number concentration 10µm (#/cm³)
  "typical_particle_size": 0.85,    // Typical particle size (µm)
  "uptime": 123456,                 // Milliseconds since boot
  "status": "OK"
}
```

**Server → Client (responses):**
```json
{
  "response_for": "registerClient",
  "status": "success",
  "message": "Client registered successfully."
}
```

## Development Notes

### Sensor Integration Status

**✓ Complete and Tested** - The SPS30 sensor is fully integrated and operational:
- Sensor initialization, configuration, and continuous reading implemented
- Real-time sensor data broadcasts via WebSocket every 1 second
- Web interface displays live PM concentrations and particle counts with charts
- End-to-end tested: ESP32 + SPS30 powered up, connected via PC, real-time data verified

### UART Configuration

The SPS30 sensor uses UART. Pin configuration is in `components/sps30/src/sensirion_uart_hal.c`. Default is typically UART2 on ESP32.

### Configuration Values

Key sdkconfig values stored in `sdkconfig.defaults`:
- `CONFIG_PARTITION_TABLE_CUSTOM=y` - Uses custom `partitions.csv`
- `CONFIG_HTTPD_WS_SUPPORT=y` - Enables WebSocket support in HTTP server

Runtime config in `sdkconfig` (gitignored, but currently tracked):
- WiFi credentials are currently hardcoded for development
- Before production, use `idf.py menuconfig` to set credentials properly

### Testing WebSocket

1. Flash and run the firmware
2. Device advertises as `simple-sps30.local`
3. Open web interface at `http://simple-sps30.local/`
4. Click "Connect" button - WebSocket client automatically connects to `/ws` endpoint on same host
5. Client sends registration automatically: `{action: "registerClient"}`
6. Server broadcasts SPS30 sensor data every 1 second to registered clients
7. Charts and table update in real-time with particulate matter readings
