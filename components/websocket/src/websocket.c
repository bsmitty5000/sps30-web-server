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
#include "cJSON.h"
#include "websocket.h"

static const char *TAG = "websocket";

#define WEBSOCKET_CHECK(a, str, goto_tag, ...)                                              \
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

typedef struct websocket_context 
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
    int ws_fd;
} websocket_context_t;

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

    websocket_context_t *_context = (websocket_context_t *)req->user_ctx;
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

esp_err_t websocket_server_start(const char *base_path) 
{
    WEBSOCKET_CHECK(base_path, "wrong base path", err);
    websocket_context_t *_context = calloc(1, sizeof(websocket_context_t));
    WEBSOCKET_CHECK(_context, "No memory for sunrise server context", err);
    strlcpy(_context->base_path, base_path, sizeof(_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    WEBSOCKET_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

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
    //     .user_ctx = _context
    // };
    // httpd_register_uri_handler(server, &system_ping_get_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = 
    {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = send_file,
        .user_ctx = _context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    return ESP_OK;
err_start:
    free(_context);
err:
    return ESP_FAIL;
}
