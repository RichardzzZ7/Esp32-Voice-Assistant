#include "cloud_llm.h"
// #include "cloud_asr.h" // No longer needed for LLM if using IAM Auth
#include "recipe_config.h"
#include "inventory.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

static const char *TAG = "cloud_llm";

// IAM Access Key for Qianfan LLM
#define QIANFAN_ACCESS_KEY "ALTAK-qV035uKpslFPqXnfHWzFd"
#define QIANFAN_SECRET_KEY "8017c9c36f6a9555e4b9b6f4b0898b787c71c7a9"

// Helper for HMAC-SHA256
static void hmac_sha256(const char *key, size_t key_len, const char *data, size_t data_len, unsigned char *output) {
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, key_len);
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, data_len);
    mbedtls_md_hmac_finish(&ctx, output);
    mbedtls_md_free(&ctx);
}

// Hex encode helper
static void hex_encode(const unsigned char *input, size_t len, char *output) {
    for (size_t i = 0; i < len; i++) {
        sprintf(output + (i * 2), "%02x", input[i]);
    }
    output[len * 2] = 0;
}

// Simple URL Encode for timestamp (only encodes :)
static void url_encode_timestamp(const char *input, char *output) {
    while (*input) {
        if (*input == ':') {
            strcpy(output, "%3A");
            output += 3;
        } else {
            *output = *input;
            output++;
        }
        input++;
    }
    *output = 0;
}

// Generate BCE IAM Signature
// Returns malloc'd string for Authorization header
static char *generate_bce_signature(const char *method, const char *uri, const char *host, const char *timestamp) {
    // 1. Auth String Prefix
    // bce-auth-v1/{accessKeyId}/{timestamp}/{expirationPeriodInSeconds}
    char auth_prefix[128];
    snprintf(auth_prefix, sizeof(auth_prefix), "bce-auth-v1/%s/%s/1800", QIANFAN_ACCESS_KEY, timestamp);

    // 2. Signing Key
    // HMAC-SHA256(SecretAccessKey, AuthStringPrefix)
    unsigned char signing_key[32];
    hmac_sha256(QIANFAN_SECRET_KEY, strlen(QIANFAN_SECRET_KEY), auth_prefix, strlen(auth_prefix), signing_key);
    
    char signing_key_hex[65];
    hex_encode(signing_key, 32, signing_key_hex);

    // 3. Canonical Request
    // Method + \n + URI + \n + Query + \n + Headers
    
    // Encode timestamp for the canonical header value
    char encoded_timestamp[64];
    url_encode_timestamp(timestamp, encoded_timestamp);

    // Headers must be sorted: host, x-bce-date
    // Format: header_name:header_value\n...
    // Values must be URL encoded if they contain special chars (like :)
    // Note: CanonicalHeaders must end with a newline
    char canonical_request[512];
    snprintf(canonical_request, sizeof(canonical_request), 
             "%s\n%s\n\nhost:%s\nx-bce-date:%s\n", 
             method, uri, host, encoded_timestamp);

    // 4. Signature
    // HMAC-SHA256(SigningKey, CanonicalRequest)
    unsigned char signature[32];
    // Use raw signing_key bytes, not hex string
    hmac_sha256((const char *)signing_key, 32, canonical_request, strlen(canonical_request), signature);
    
    char signature_hex[65];
    hex_encode(signature, 32, signature_hex);

    // 5. Final Header
    // bce-auth-v1/{accessKeyId}/{timestamp}/{expirationPeriodInSeconds}/{signedHeaders}/{signature}
    char *auth_header = malloc(512);
    if (auth_header) {
        snprintf(auth_header, 512, "%s/host;x-bce-date/%s", auth_prefix, signature_hex);
    }
    return auth_header;
}

// Helper to parse LLM response
static void process_llm_response(const char *json_str, llm_action_t action)
{
    ESP_LOGI(TAG, "LLM Response: %s", json_str);
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse LLM response JSON");
        return;
    }

    // Handle OpenAI-compatible format: choices[0].message.content
    // Also handle Baidu ERNIE format: "result": "..."
    cJSON *result_node = cJSON_GetObjectItem(root, "result");
    if (result_node && cJSON_IsString(result_node)) {
         // Baidu ERNIE response
         ESP_LOGI(TAG, "Baidu ERNIE Result: %s", result_node->valuestring);
         // The content itself should be a JSON string (as requested in prompt)
         char *content_str = result_node->valuestring;
         char *start = strchr(content_str, '{');
         char *end = strrchr(content_str, '}');
         if (start && end && end > start) {
             char *item_json_str = strndup(start, end - start + 1);
             ESP_LOGI(TAG, "Extracted Item JSON: %s", item_json_str);
             
             cJSON *item_json = cJSON_Parse(item_json_str);
             if (item_json) {
                 // ... (Same parsing logic as before) ...
                 cJSON *name = cJSON_GetObjectItem(item_json, "name");
                 cJSON *qty = cJSON_GetObjectItem(item_json, "quantity");
                 
                 if (action == LLM_ACTION_REMOVE) {
                     if (name && name->valuestring && qty) {
                         inventory_remove_item(name->valuestring, qty->valueint);
                     }
                 } else {
                     // ADD
                     inventory_item_t item;
                     memset(&item, 0, sizeof(item));
                     
                     cJSON *cat = cJSON_GetObjectItem(item_json, "category");
                     cJSON *unit = cJSON_GetObjectItem(item_json, "unit");
                     cJSON *loc = cJSON_GetObjectItem(item_json, "location");
                     cJSON *exp = cJSON_GetObjectItem(item_json, "expiry_date");
                     cJSON *shelf = cJSON_GetObjectItem(item_json, "shelf_life_days");

                     if (name && name->valuestring) strncpy(item.name, name->valuestring, sizeof(item.name)-1);
                     if (cat && cat->valuestring) strncpy(item.category, cat->valuestring, sizeof(item.category)-1);
                     if (qty) item.quantity = qty->valueint;
                     if (unit && unit->valuestring) strncpy(item.unit, unit->valuestring, sizeof(item.unit)-1);
                     if (loc && loc->valuestring) strncpy(item.location, loc->valuestring, sizeof(item.location)-1);
                     
                     // Handle expiry date parsing (YYYY-MM-DD)
                     if (exp && exp->valuestring) {
                         struct tm tmv = {0};
                         if (sscanf(exp->valuestring, "%d-%d-%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
                             tmv.tm_year -= 1900;
                             tmv.tm_mon -= 1;
                             item.calculated_expiry_date = mktime(&tmv);
                             // Calculate remaining days
                             time_t now = time(NULL);
                             item.default_shelf_life_days = (item.calculated_expiry_date - now) / (24 * 3600);
                         }
                     } else if (shelf && shelf->valueint > 0) {
                         // Fallback: use shelf_life_days if expiry_date is missing
                         item.default_shelf_life_days = shelf->valueint;
                         item.calculated_expiry_date = time(NULL) + (int64_t)item.default_shelf_life_days * 24 * 3600;
                     }

                     // Add to inventory
                     item.added_time = time(NULL);
                     if (item.quantity <= 0) item.quantity = 1;
                     inventory_add_item(&item);
                     ESP_LOGI(TAG, "Item added via Cloud LLM: %s", item.name);
                 }
                 cJSON_Delete(item_json);
             }
             free(item_json_str);
         }
         cJSON_Delete(root);
         return;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_IsArray(choices)) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(choice, "message");
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content)) {
            // The content itself should be a JSON string (as requested in prompt)
            // We need to find the JSON block in the content
            char *start = strchr(content->valuestring, '{');
            char *end = strrchr(content->valuestring, '}');
            if (start && end && end > start) {
                char *item_json_str = strndup(start, end - start + 1);
                ESP_LOGI(TAG, "Extracted Item JSON: %s", item_json_str);
                
                cJSON *item_json = cJSON_Parse(item_json_str);
                if (item_json) {
                    cJSON *name = cJSON_GetObjectItem(item_json, "name");
                    cJSON *qty = cJSON_GetObjectItem(item_json, "quantity");
                    
                    if (action == LLM_ACTION_REMOVE) {
                        if (name && name->valuestring && qty) {
                            inventory_remove_item(name->valuestring, qty->valueint);
                        }
                    } else {
                        // ADD
                        inventory_item_t item;
                        memset(&item, 0, sizeof(item));
                        
                        cJSON *cat = cJSON_GetObjectItem(item_json, "category");
                        cJSON *unit = cJSON_GetObjectItem(item_json, "unit");
                        cJSON *loc = cJSON_GetObjectItem(item_json, "location");
                        cJSON *exp = cJSON_GetObjectItem(item_json, "expiry_date");
                        cJSON *shelf = cJSON_GetObjectItem(item_json, "shelf_life_days");

                        if (name && name->valuestring) strncpy(item.name, name->valuestring, sizeof(item.name)-1);
                        if (cat && cat->valuestring) strncpy(item.category, cat->valuestring, sizeof(item.category)-1);
                        if (qty) item.quantity = qty->valueint;
                        if (unit && unit->valuestring) strncpy(item.unit, unit->valuestring, sizeof(item.unit)-1);
                        if (loc && loc->valuestring) strncpy(item.location, loc->valuestring, sizeof(item.location)-1);
                        
                        // Handle expiry date parsing (YYYY-MM-DD)
                        if (exp && exp->valuestring) {
                            struct tm tmv = {0};
                            if (sscanf(exp->valuestring, "%d-%d-%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
                                tmv.tm_year -= 1900;
                                tmv.tm_mon -= 1;
                                item.calculated_expiry_date = mktime(&tmv);
                                time_t now = time(NULL);
                                item.default_shelf_life_days = (item.calculated_expiry_date - now) / (24 * 3600);
                            }
                        } else if (shelf && shelf->valueint > 0) {
                            item.default_shelf_life_days = shelf->valueint;
                            item.calculated_expiry_date = time(NULL) + (int64_t)item.default_shelf_life_days * 24 * 3600;
                        }

                        item.added_time = time(NULL);
                        if (item.quantity <= 0) item.quantity = 1;
                        inventory_add_item(&item);
                        ESP_LOGI(TAG, "Item added via Cloud LLM: %s", item.name);
                    }
                    cJSON_Delete(item_json);
                }
                free(item_json_str);
            }
        }
    }
    cJSON_Delete(root);
}

bool cloud_llm_parse_inventory(const char *text, llm_action_t action)
{
    if (!text) return false;
    bool success = false;
    ESP_LOGI(TAG, "Requesting Cloud LLM parsing for: %s (Action: %d)", text, action);

    // Use Baidu ERNIE-Speed-128K with IAM Auth
    const char *host = "aip.baidubce.com";
    const char *uri = "/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/ernie-speed-128k";
    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", host, uri);

    // Prepare Timestamp for Signature
    time_t now = time(NULL);
    struct tm timeinfo = {0};
    gmtime_r(&now, &timeinfo); // UTC time
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    // Generate Signature
    char *auth_header = generate_bce_signature("POST", uri, host, timestamp);
    if (!auth_header) {
        ESP_LOGE(TAG, "Failed to generate signature");
        return false;
    }
    ESP_LOGI(TAG, "Auth Header: %s", auth_header);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // Set Headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Host", host);
    esp_http_client_set_header(client, "x-bce-date", timestamp);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    // Build JSON Body for Baidu
    cJSON *root = cJSON_CreateObject();
    
    // Dynamic Date for Prompt (Local Time)
    struct tm local_timeinfo = {0};
    localtime_r(&now, &local_timeinfo);
    char date_str[20];
    if (local_timeinfo.tm_year < (2024 - 1900)) {
        snprintf(date_str, sizeof(date_str), "2025-11-28");
    } else {
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &local_timeinfo);
    }

    char system_prompt[512];
    if (action == LLM_ACTION_REMOVE) {
        snprintf(system_prompt, sizeof(system_prompt), 
             "You are an inventory assistant. User wants to REMOVE items. Extract: name, quantity. Return ONLY JSON. Example: {\"name\":\"apple\",\"quantity\":2}");
    } else {
        snprintf(system_prompt, sizeof(system_prompt), 
             "You are an inventory assistant. Extract: name, category, quantity, unit, expiry_date (YYYY-MM-DD), shelf_life_days (int), location. Today is %s. Return ONLY JSON. Example: {\"name\":\"milk\",\"quantity\":1,\"unit\":\"box\",\"expiry_date\":\"2025-12-01\"}", 
             date_str);
    }

    // Baidu uses "messages" array just like OpenAI
    cJSON *messages = cJSON_CreateArray();
    
    // Note: Baidu ERNIE sometimes prefers system prompt in "system" field (newer API) or just as first user message.
    // Let's try the standard "system" role if supported, or prepend to user.
    // ERNIE-Speed supports "system" field in root, NOT in messages array usually.
    cJSON_AddStringToObject(root, "system", system_prompt);

    cJSON *msg_user = cJSON_CreateObject();
    cJSON_AddStringToObject(msg_user, "role", "user");
    cJSON_AddStringToObject(msg_user, "content", text);
    cJSON_AddItemToArray(messages, msg_user);
    
    cJSON_AddItemToObject(root, "messages", messages);
    
    char *post_data = cJSON_PrintUnformatted(root);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    // Execute
    // Use blocking open/write/read for simplicity
    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err == ESP_OK) {
        int wlen = esp_http_client_write(client, post_data, strlen(post_data));
        if (wlen < 0) {
            ESP_LOGE(TAG, "Write failed");
        }
        
        int content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
        } else {
            int read_len;
            char *buffer = malloc(4096); // Allocate 4KB buffer
            if (buffer) {
                read_len = esp_http_client_read_response(client, buffer, 4096);
                if (read_len >= 0) {
                    buffer[read_len] = 0; // Null terminate
                    ESP_LOGI(TAG, "HTTP Response: %s", buffer);
                    process_llm_response(buffer, action);
                    success = true;
                }
                free(buffer);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP connection failed: %s", esp_err_to_name(err));
    }
    
    // Cleanup
    cJSON_Delete(root);
    free(post_data);
    free(auth_header); // Free signature
    esp_http_client_cleanup(client);
    
    return success;
}
