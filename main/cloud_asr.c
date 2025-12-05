#include "cloud_asr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cloud_asr";

// 百度语音识别 API 配置 (请替换为您自己的 API Key 和 Secret Key)
// 免费申请地址: https://console.bce.baidu.com/ai/
#define BAIDU_ASR_API_KEY    "hJ1WQoUpmPsV5QhAO6B9CKlE"
#define BAIDU_ASR_SECRET_KEY "Vehcf38MR9pbgEhM4ZeXwqQ5mbJLcpYG"
#define BAIDU_TOKEN_URL      "https://aip.baidubce.com/oauth/2.0/token"
#define BAIDU_ASR_URL        "http://vop.baidu.com/server_api"

static char *g_access_token = NULL;

static esp_err_t _token_http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data) {
                char *buf = (char *)evt->user_data;
                int current_len = strlen(buf);
                // Prevent buffer overflow (assuming 2048 size)
                if (current_len + evt->data_len < 2047) {
                    memcpy(buf + current_len, evt->data, evt->data_len);
                    buf[current_len + evt->data_len] = 0; // Null terminate
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// 获取 Access Token
static void get_access_token(void)
{
    if (g_access_token) return; // 已有 Token

    char url[512];
    snprintf(url, sizeof(url), "%s?grant_type=client_credentials&client_id=%s&client_secret=%s", 
             BAIDU_TOKEN_URL, BAIDU_ASR_API_KEY, BAIDU_ASR_SECRET_KEY);

    // Allocate buffer for response
    char *response_buf = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for token response");
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST, // Baidu supports POST with query params
        .timeout_ms = 10000,
        .event_handler = _token_http_event_handler,
        .user_data = response_buf,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    ESP_LOGI(TAG, "Requesting Token...");
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Token Status: %d, Content-Length: %lld", status_code, esp_http_client_get_content_length(client));
        ESP_LOGI(TAG, "Token Response: %s", response_buf);

        if (status_code == 200) {
            cJSON *root = cJSON_Parse(response_buf);
            if (root) {
                cJSON *token = cJSON_GetObjectItem(root, "access_token");
                if (token && token->valuestring) {
                    g_access_token = strdup(token->valuestring);
                    ESP_LOGI(TAG, "Got Baidu Access Token: %s", g_access_token);
                } else {
                    ESP_LOGE(TAG, "Failed to parse access_token. Check API Key/Secret.");
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON");
            }
        } else {
            ESP_LOGE(TAG, "Token request failed with status %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "Token HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    free(response_buf);
}

void cloud_asr_init(void)
{
    // 检查是否配置了 Key
    if (strcmp(BAIDU_ASR_API_KEY, "REPLACE_WITH_YOUR_API_KEY") == 0) {
        ESP_LOGW(TAG, "Please set BAIDU_ASR_API_KEY in cloud_asr.c");
        return;
    }
    get_access_token();
}

static esp_err_t _asr_http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data) {
                char *buf = (char *)evt->user_data;
                int current_len = strlen(buf);
                // Prevent buffer overflow (assuming 4096 size)
                if (current_len + evt->data_len < 4095) {
                    memcpy(buf + current_len, evt->data, evt->data_len);
                    buf[current_len + evt->data_len] = 0; // Null terminate
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

char* cloud_asr_send_audio(const int16_t *audio_data, int len)
{
    if (!g_access_token) {
        get_access_token();
        if (!g_access_token) {
            ESP_LOGE(TAG, "No access token, cannot perform ASR");
            return NULL;
        }
    }

    ESP_LOGI(TAG, "Sending audio to Cloud ASR, len: %d bytes. Free Heap: %d", len, (int)esp_get_free_heap_size());

    // 1. Base64 Encode Audio
    size_t base64_len = 0;
    mbedtls_base64_encode(NULL, 0, &base64_len, (const unsigned char*)audio_data, len);
    char *base64_buf = heap_caps_malloc(base64_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!base64_buf) {
        ESP_LOGE(TAG, "OOM for base64");
        return NULL;
    }
    mbedtls_base64_encode((unsigned char*)base64_buf, base64_len + 1, &base64_len, (const unsigned char*)audio_data, len);
    base64_buf[base64_len] = 0;

    // 2. Build JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", "pcm");
    cJSON_AddNumberToObject(root, "rate", 16000);
    cJSON_AddNumberToObject(root, "channel", 1);
    cJSON_AddStringToObject(root, "cuid", "esp32-s3-xiaobin");
    cJSON_AddStringToObject(root, "token", g_access_token);
    cJSON_AddNumberToObject(root, "len", len);
    cJSON_AddStringToObject(root, "speech", base64_buf);

    char *post_data = cJSON_PrintUnformatted(root);
    free(base64_buf); // Free base64 buffer early
    cJSON_Delete(root);

    if (!post_data) return NULL;

    // 3. Send HTTP Request
    char *result_text = NULL;
    char *response_buf = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // Allocate buffer for response
    if (!response_buf) {
        free(post_data);
        return NULL;
    }

    esp_http_client_config_t config = {
        .url = BAIDU_ASR_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .event_handler = _asr_http_event_handler,
        .user_data = response_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "ASR Status: %d, Len: %lld", status_code, esp_http_client_get_content_length(client));
        
        if (status_code == 200) {
            ESP_LOGI(TAG, "ASR Response: %s", response_buf);
            cJSON *resp = cJSON_Parse(response_buf);
            if (resp) {
                cJSON *res_arr = cJSON_GetObjectItem(resp, "result");
                if (res_arr && cJSON_IsArray(res_arr)) {
                    cJSON *item = cJSON_GetArrayItem(res_arr, 0);
                    if (item && item->valuestring) {
                        result_text = strdup(item->valuestring);
                    }
                } else {
                    cJSON *err_msg = cJSON_GetObjectItem(resp, "err_msg");
                    if (err_msg) ESP_LOGE(TAG, "ASR Error: %s", err_msg->valuestring);
                }
                cJSON_Delete(resp);
            }
        }
    } else {
        ESP_LOGE(TAG, "ASR Connect failed: %s", esp_err_to_name(err));
    }

    free(response_buf);
    free(post_data);
    esp_http_client_cleanup(client);
    return result_text;
}

const char *cloud_asr_get_token(void)
{
    // Ensure token exists (simple check)
    if (!g_access_token) {
        get_access_token();
    }
    return g_access_token;
}

// Helper for cloud_llm.c to get the token
char *get_baidu_access_token(void) {
    if (!g_access_token) {
        get_access_token();
    }
    return g_access_token;
}
