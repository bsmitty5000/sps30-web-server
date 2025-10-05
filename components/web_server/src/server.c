#include <stdio.h>
#include <sys/time.h>
#include <string.h>
#include <fcntl.h>
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "nvs.h"
#include "cJSON.h"
#include "server.h"
#include "alarm.h"

static const char *TAG = "web_server";

#define SERVER_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

typedef struct sunrise_server_context 
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
    int64_t s_alarm_epoch_ms;
    alarm_handle_t s_alarm_timer;
} sunrise_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

static esp_err_t send_file(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    sunrise_server_context_t *_context = (sunrise_server_context_t *)req->user_ctx;
    strlcpy(filepath, _context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') 
    {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } 
    else 
    {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) 
    {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);
    
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char *chunk = _context->scratch;
    ssize_t read_bytes;
    do 
    {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) 
        {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } 
        else if (read_bytes > 0) 
        {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) 
            {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);

    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---------- Alarm scheduling ---------- */

// static void alarm_cb_trampoline(void *arg) 
// {
//     ESP_LOGI(TAG, "Alarm fired at epoch_ms=%lld", (long long)now_ms());
//     s_alarm_epoch_ms = -1;
//     if (s_alarm_cb) s_alarm_cb(s_alarm_ctx);
// }

// static void cancel_alarm_timer(void) 
// {
//     if (s_alarm_timer) 
//     {
//         esp_timer_stop(s_alarm_timer);
//         esp_timer_delete(s_alarm_timer);
//         s_alarm_timer = NULL;
//     }
// }

// static esp_err_t schedule_alarm_at(int64_t epoch_ms) 
// {
//     cancel_alarm_timer();
//     int64_t delta = epoch_ms - now_ms();
//     if (delta < 100) delta = 100; // clamp
//     esp_timer_create_args_t args = 
//     {
//         .callback = alarm_cb_trampoline,
//         .arg = NULL,
//         .dispatch_method = ESP_TIMER_TASK,
//         .name = "sunrise_alarm"
//     };
//     ESP_ERROR_CHECK(esp_timer_create(&args, &s_alarm_timer));
//     ESP_ERROR_CHECK(esp_timer_start_once(s_alarm_timer, (uint64_t)delta * 1000ULL));
//     s_alarm_epoch_ms = epoch_ms;
//     ESP_LOGI(TAG, "Alarm scheduled in %lld ms (at %lld)", (long long)delta, (long long)epoch_ms);
//     return ESP_OK;
// }

/* ---------- API handlers ---------- */

// static esp_err_t ping_get(httpd_req_t *req) 
// {
//     httpd_resp_set_type(req, "text/plain; charset=utf-8");
//     httpd_resp_set_hdr(req, "Cache-Control", "no-store");
//     httpd_resp_sendstr(req, "pong");
//     return ESP_OK;
// }

static esp_err_t alarm_get(httpd_req_t *req) 
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    alarm_handle_t ah = ((sunrise_server_context_t *)(req->user_ctx))->s_alarm_timer;

    cJSON_AddNumberToObject(root, "hours", ah->hour);
    cJSON_AddNumberToObject(root, "minutes", ah->min);
    cJSON_AddNumberToObject(root, "sunrise", ah->sunrise_minutes);
    cJSON_AddBoolToObject(root, "enabled", ah->enabled);

    const char *ah_info = cJSON_Print(root);
    httpd_resp_sendstr(req, ah_info);
    free((void *)ah_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t alarm_put(httpd_req_t *req) 
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((sunrise_server_context_t *)(req->user_ctx))->scratch;
    alarm_handle_t ah = ((sunrise_server_context_t *)(req->user_ctx))->s_alarm_timer;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) 
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf); 
    cJSON *hours_json = cJSON_GetObjectItem(root, "hours");
    int hours = (hours_json != NULL) ? hours_json->valueint : -1;
    
    cJSON *minutes_json = cJSON_GetObjectItem(root, "minutes");
    int minutes = (minutes_json != NULL) ? minutes_json->valueint : -1;
    
    cJSON *sunrise_json = cJSON_GetObjectItem(root, "sunrise");
    int sunrise = (sunrise_json != NULL) ? sunrise_json->valueint : -1;
    cJSON_Delete(root);

    if (hours < 0 || minutes < 0 || sunrise < 0) 
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid alarm value");
        return ESP_FAIL;
    }

    alarm_update(ah, hours, minutes, sunrise, ah->brightness_pct, ah->cool_balance, ah->enabled);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t enable_put(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((sunrise_server_context_t *)(req->user_ctx))->scratch;
    alarm_handle_t ah = ((sunrise_server_context_t *)(req->user_ctx))->s_alarm_timer;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE)
    {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) 
    {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf); 
    cJSON *en_json = cJSON_GetObjectItem(root, "enabled");
    bool valid = false;
    if (en_json && cJSON_IsBool(en_json)) 
    {
        valid = true;
        bool enabled = cJSON_IsTrue(en_json);
        alarm_set_enabled(ah, enabled);
    }
    cJSON_Delete(root);

    if (!valid) 
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enable value");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// static esp_err_t alarm_delete(httpd_req_t *req) 
// {
//     cancel_alarm_timer();
//     s_alarm_epoch_ms = -1;
//     httpd_resp_set_type(req, "application/json; charset=utf-8");
//     httpd_resp_sendstr(req, "{\"ok\":true}");
//     return ESP_OK;
// }

esp_err_t web_server_start(alarm_handle_t cb, const char *base_path) 
{
    SERVER_CHECK(base_path, "wrong base path", err);
    sunrise_server_context_t *sunrise_context = calloc(1, sizeof(sunrise_server_context_t));
    SERVER_CHECK(sunrise_context, "No memory for sunrise server context", err);
    strlcpy(sunrise_context->base_path, base_path, sizeof(sunrise_context->base_path));

    alarm_update_from_nvs(cb);

    sunrise_context->s_alarm_timer = cb;

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    SERVER_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    // API
    // httpd_uri_t ping = { .uri="/api/ping", .method=HTTP_GET, .handler=ping_get };
    // httpd_uri_t aget = { .uri="/api/alarm", .method=HTTP_GET, .handler=alarm_get };
    // httpd_uri_t apost= { .uri="/api/alarm", .method=HTTP_POST, .handler=alarm_post };
    // httpd_uri_t adel = { .uri="/api/alarm", .method=HTTP_DELETE, .handler=alarm_delete };
    // httpd_register_uri_handler(s_httpd, &ping);
    // httpd_register_uri_handler(s_httpd, &aget);
    // httpd_register_uri_handler(s_httpd, &apost);
    // httpd_register_uri_handler(s_httpd, &adel);
    
    // httpd_uri_t system_ping_get_uri = 
    // {
    //     .uri = "/api/ping",
    //     .method = HTTP_GET,
    //     .handler = system_ping_get_handler,
    //     .user_ctx = sunrise_context
    // };
    // httpd_register_uri_handler(server, &system_ping_get_uri);

    /* URI handler for simple post */
    httpd_uri_t alarm_put_uri = 
    {
        .uri = "/api/v1/alarm",
        .method = HTTP_PUT,
        .handler = alarm_put,
        .user_ctx = sunrise_context
    };
    httpd_register_uri_handler(server, &alarm_put_uri);
    
    httpd_uri_t alarm_get_uri = 
    {
        .uri = "/api/v1/alarm",
        .method = HTTP_GET,
        .handler = alarm_get,
        .user_ctx = sunrise_context
    };
    httpd_register_uri_handler(server, &alarm_get_uri);

    httpd_uri_t enabled_put_uri = 
    {
        .uri = "/api/v1/enable",
        .method = HTTP_PUT,
        .handler = enable_put,
        .user_ctx = sunrise_context
    };
    httpd_register_uri_handler(server, &enabled_put_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = 
    {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = send_file,
        .user_ctx = sunrise_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    return ESP_OK;
err_start:
    free(sunrise_context);
err:
    return ESP_FAIL;
}
