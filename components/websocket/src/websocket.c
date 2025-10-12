#include <stdio.h>
#include <sys/time.h>
#include <string.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#define MAX_WEBSOCKET_CLIENTS 5

typedef struct 
{
    int fd;
} ws_client_t;

typedef struct websocket_context 
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
    ws_client_t clients[MAX_WEBSOCKET_CLIENTS];
    httpd_handle_t server;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
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
                ESP_LOGE(TAG, "Failed to send file : %s", filepath);
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

/**
 * @brief Sends a JSON response back to a specific client.
 *
 * Used to send ACK/NACK messages for client actions.
 *
 * @param req The HTTP request object, used to identify the client.
 * @param action The original action this response is for (e.g., "registerClient").
 * @param status The result of the action ("success" or "error").
 * @param message A descriptive message.
 */
static void send_response_to_client(httpd_req_t *req, const char *action, const char *status, const char *message) 
{
    cJSON *response_root = cJSON_CreateObject();
    cJSON_AddStringToObject(response_root, "response_for", action);
    cJSON_AddStringToObject(response_root, "status", status);
    cJSON_AddStringToObject(response_root, "message", message);

    const char *response_str = cJSON_PrintUnformatted(response_root);
    if (response_str) 
    {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)response_str;
        ws_pkt.len = strlen(response_str);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        // Use httpd_ws_send_frame to send back to the specific client
        httpd_ws_send_frame(req, &ws_pkt);
    }

    cJSON_Delete(response_root);
    free((void*)response_str);
}

/**
 * @brief Adds a new client's file descriptor to the list of clients.
 *
 * @param new_fd The file descriptor of the new client.
 * @return esp_err_t ESP_OK on success, ESP_FAIL if the client list is full.
 */
static esp_err_t add_client(websocket_context_t* _context, int new_fd) 
{
    if (xSemaphoreTake(_context->lock, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
            if (_context->clients[i].fd == 0) { // Find an empty slot
                _context->clients[i].fd = new_fd;
                ESP_LOGI(TAG, "Client connected, fd=%d", new_fd);
                xSemaphoreGive(_context->lock);
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "Client list full, connection rejected for fd=%d", new_fd);
        xSemaphoreGive(_context->lock);
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

/**
 * @brief Removes a client's file descriptor from the list.
 *
 * @param fd_to_remove The file descriptor of the client to remove.
 */
static void remove_client(websocket_context_t* _context, int fd_to_remove) 
{
    if (xSemaphoreTake(_context->lock, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
            if (_context->clients[i].fd == fd_to_remove) {
                _context->clients[i].fd = 0; // Mark slot as empty
                ESP_LOGI(TAG, "Client disconnected, fd=%d", fd_to_remove);
                break;
            }
        }
        xSemaphoreGive(_context->lock);
    }
}

/**
 * @brief Task to broadcast dummy data to all connected WebSocket clients.
 *
 * Runs in a loop, creates a JSON payload, and sends it to every client
 * in the client list every 2 seconds.
 *
 * @param pvParameters context.
 */
void broadcast_task(void *pvParameters) 
{
    websocket_context_t *_context = (websocket_context_t*)pvParameters;
    int sentCount = 0;

    for (;;) 
    {
        // Create JSON payload
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "randomNumber", esp_random() % 100);
        cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000);
        cJSON_AddStringToObject(root, "status", "OK");
        const char *json_string = cJSON_PrintUnformatted(root);

        if (json_string == NULL) 
        {
            ESP_LOGE(TAG, "Failed to print cJSON");
            cJSON_Delete(root);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Prepare WebSocket frame
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)json_string;
        ws_pkt.len = strlen(json_string);
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        // Iterate over clients and send data
        if (xSemaphoreTake(_context->lock, portMAX_DELAY) == pdTRUE) 
        {
            for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++)
            {
                if (_context->clients[i].fd != 0) 
                {
                    sentCount++;
                    // Use httpd_ws_send_frame_async for non-blocking sends
                    httpd_ws_send_frame_async(_context->server, _context->clients[i].fd, &ws_pkt);
                }
            }
            xSemaphoreGive(_context->lock);

            if(sentCount == 0)
            {
                ESP_LOGI(TAG, "No clients yet!");
            }
            else
            {
                ESP_LOGI(TAG, "Sent %s to %d clients!", json_string, sentCount);
            }
            sentCount = 0;
        }

        // Clean up
        cJSON_Delete(root);
        free((void*)json_string);

        // Wait for the next broadcast
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

//     if (req->method == HTTP_GET) {
//         ESP_LOGI(TAG, "Handshake done, new connection was opened");
//         return ESP_OK;
//     }

//     websocket_context_t *_context = (websocket_context_t *)req->user_ctx;

//     int client_fd = httpd_req_to_sockfd(req);
//     if (add_client(_context, client_fd) != ESP_OK) 
//     {
//         // If we can't add the client, close the connection
//         httpd_sess_trigger_close(req->handle, client_fd);
//         return ESP_OK;
//     }

static esp_err_t ws_handler(httpd_req_t *req) 
{
    if (req->method == HTTP_GET) 
    {
        ESP_LOGI(TAG, "Handshake done, new connection was opened");
        return ESP_OK;
    }

    websocket_context_t *_context = (websocket_context_t *)req->user_ctx;

    int client_fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) 
    {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        remove_client(_context, client_fd);
        return ret;
    }
    
    if (ws_pkt.len > 0) 
    {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            remove_client(_context, client_fd);
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            remove_client(_context, client_fd);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);

        // JSON parsing
        cJSON *root = cJSON_Parse((const char *)ws_pkt.payload);
        if (root) {
            cJSON *action_item = cJSON_GetObjectItem(root, "action");
            if (action_item && cJSON_IsString(action_item)) 
            {
                const char *action_str = action_item->valuestring;
                ESP_LOGI(TAG, "Received action: %s from fd: %d", action_str, client_fd);

                if (strcmp(action_str, "registerClient") == 0) 
                {
                    if (add_client(_context, client_fd) == ESP_OK) 
                    {
                        send_response_to_client(req, "registerClient", "success", "Client registered successfully.");
                    } else 
                    {
                        send_response_to_client(req, "registerClient", "error", "Client list is full.");
                    }
                } else if (strcmp(action_str, "closeConnection") == 0) 
                {
                    remove_client(_context, client_fd);
                    send_response_to_client(req, "closeConnection", "success", "Connection will be closed.");
                    // The session will be closed after this handler returns
                    httpd_sess_trigger_close(req->handle, client_fd);
                } else 
                {
                    send_response_to_client(req, action_str, "error", "Unknown action.");
                }
            }
            cJSON_Delete(root);
        } else 
        {
             send_response_to_client(req, "parse", "error", "Invalid JSON format.");
        }
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) 
    {
        remove_client(_context, client_fd);
    }
    
    free(buf);
    return ESP_OK;
}

esp_err_t websocket_server_start(const char *base_path) 
{
    WEBSOCKET_CHECK(base_path, "wrong base path", err);
    websocket_context_t *_context = calloc(1, sizeof(websocket_context_t));
    WEBSOCKET_CHECK(_context, "No memory for sunrise server context", err);
    strlcpy(_context->base_path, base_path, sizeof(_context->base_path));
    
    _context->lock = xSemaphoreCreateMutex();
    
    WEBSOCKET_CHECK(_context->lock, "xSemaphoreCreateMutex failed", err_start);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    WEBSOCKET_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);
    _context->server = server;

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

        
    // Register WebSocket handler
    httpd_uri_t ws_uri = 
    {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = _context,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = 
    {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = send_file,
        .user_ctx = _context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    BaseType_t ok = xTaskCreatePinnedToCore(
        broadcast_task, "broadcast_task", 4096, _context, 5, &_context->task, tskNO_AFFINITY);
        

    if (ok != pdPASS) 
    {
        ESP_LOGE(TAG, "failed to create task");
        xSemaphoreTake(_context->lock, portMAX_DELAY);
        _context->task = NULL;
        xSemaphoreGive(_context->lock);
        goto err;
    }

    return ESP_OK;
err_start:
    free(_context);
err:
    return ESP_FAIL;
}
