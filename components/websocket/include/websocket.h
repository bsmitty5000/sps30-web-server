#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the websocket-based server and mount SPIFFS (base_path="/spiffs", label="storage"). 
 *  Registers static file handlers and websocket 
 *  @param base_path Path to hosted files
 */
esp_err_t websocket_server_start(const char *base_path);

#ifdef __cplusplus
}
#endif
