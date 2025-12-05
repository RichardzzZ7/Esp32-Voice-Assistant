// sync.c - simple persistent queue and background sync task
#include "sync.h"
#include "sync_config.h"
#include "storage.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(CONFIG_ESP_HTTP_CLIENT)
#include "esp_http_client.h"
#endif
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "sync";
static const char *QUEUE_PATH = "/spiffs/sync_queue.json";

// read queue as cJSON array; caller must cJSON_Delete
static cJSON *read_queue()
{
    char *s = storage_read_file(QUEUE_PATH);
    if (!s) return cJSON_CreateArray();
    cJSON *arr = cJSON_Parse(s);
    free(s);
    if (!arr) return cJSON_CreateArray();
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return cJSON_CreateArray(); }
    return arr;
}

static int write_queue(cJSON *arr)
{
    if (!arr) return -1;
    char *s = cJSON_PrintUnformatted(arr);
    if (!s) return -1;
    int rc = storage_write_file(QUEUE_PATH, s);
    free(s);
    return rc;
}

int sync_enqueue_event(const char *event_type, const char *payload_json)
{
    if (!event_type || !payload_json) return -1;
    cJSON *arr = read_queue();
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", event_type);
    cJSON *payload = cJSON_Parse(payload_json);
    if (!payload) payload = cJSON_CreateString(payload_json);
    cJSON_AddItemToObject(ev, "payload", payload);
    cJSON_AddNumberToObject(ev, "ts", (double)time(NULL));
    cJSON_AddItemToArray(arr, ev);
    int rc = write_queue(arr);
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "enqueued event %s", event_type);
    return rc;
}

#if defined(CONFIG_ESP_HTTP_CLIENT)
static int post_event_to_server(const char *json, int len)
{
    esp_http_client_config_t config = { .url = SYNC_API_URL };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, len);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return (status >= 200 && status < 300) ? 0 : -1;
}
#else
// If no HTTP client available, always fail so items remain queued
static int post_event_to_server(const char *json, int len)
{
    (void)json; (void)len; return -1;
}
#endif

static void sync_task(void *arg)
{
    (void)arg;
    while (1) {
        cJSON *arr = read_queue();
        int n = cJSON_GetArraySize(arr);
        if (n > 0) {
            cJSON *first = cJSON_GetArrayItem(arr, 0);
            if (first) {
                char *s = cJSON_PrintUnformatted(first);
                if (s) {
                    int rc = post_event_to_server(s, strlen(s));
                    if (rc == 0) {
                        // pop first
                        cJSON_DeleteItemFromArray(arr, 0);
                        write_queue(arr);
                        ESP_LOGI(TAG, "synced one event");
                    } else {
                        ESP_LOGW(TAG, "sync failed, will retry later");
                        free(s);
                        cJSON_Delete(arr);
                        vTaskDelay(pdMS_TO_TICKS(SYNC_POLL_INTERVAL * 1000));
                        continue;
                    }
                    free(s);
                }
            }
        }
        cJSON_Delete(arr);
        vTaskDelay(pdMS_TO_TICKS(SYNC_POLL_INTERVAL * 1000));
    }
}

void sync_init(void)
{
    // ensure queue file exists
    cJSON *arr = read_queue();
    write_queue(arr);
    cJSON_Delete(arr);
    xTaskCreatePinnedToCore(sync_task, "sync", 8*1024, NULL, 5, NULL, 1);
}
