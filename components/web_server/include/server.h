#pragma once
#include "esp_err.h"
#include "alarm.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the HTTP server and mount SPIFFS (base_path="/spiffs", label="storage"). 
 *  Registers static file handlers and REST API.
 *  @param cb   Called when the alarm fires
 *  @param base_path Path to hosted files
 */
esp_err_t web_server_start(alarm_handle_t cb, const char *base_path);

/** Query the currently scheduled alarm epoch (ms), or -1 if none. */
int64_t web_server_get_alarm_epoch_ms(void);

/** Cancel the current alarm (if any). */
void web_server_cancel_alarm(void);

#ifdef __cplusplus
}
#endif
