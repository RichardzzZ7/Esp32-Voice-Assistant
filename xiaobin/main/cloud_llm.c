#include "cloud_llm.h"
#include "recipe_config.h"
#include "inventory.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "cloud_llm";

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
        }
    }
    cJSON_Delete(root);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Data handling
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool cloud_llm_parse_inventory(const char *text, llm_action_t action)
{
    if (!text) return false;
    bool success = false;
    ESP_LOGI(TAG, "Requesting Cloud LLM parsing for: %s (Action: %d)", text, action);

    esp_http_client_config_t config = {
        .url = RECIPE_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // Set Headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // If Spark requires Auth in header:
    // char auth[128]; snprintf(auth, sizeof(auth), "Bearer %s:%s", RECIPE_API_KEY, RECIPE_API_SECRET); // Example
    // esp_http_client_set_header(client, "Authorization", auth); 
    // Note: Spark usually uses a complex signature in URL or Header. 
    // Assuming the user provided URL is a proxy that handles auth or accepts simple key.
    // For now, we'll try adding Authorization: Bearer <API_KEY> as is common for OpenAI-compatible wrappers.
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", RECIPE_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header);

    // Build JSON Body
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "generalv3.5"); // Default Spark model
    
    // Dynamic Date
    time_t now = time(NULL);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    char date_str[20];
    if (timeinfo.tm_year < (2024 - 1900)) {
        // Fallback if time not synced
        snprintf(date_str, sizeof(date_str), "2025-11-28");
    } else {
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);
    }

    char system_prompt[256];
    if (action == LLM_ACTION_REMOVE) {
        snprintf(system_prompt, sizeof(system_prompt), 
             "You are an inventory assistant. User wants to REMOVE items. Extract: name, quantity. Return ONLY JSON.");
    } else {
        snprintf(system_prompt, sizeof(system_prompt), 
             "You are an inventory assistant. Extract: name, category, quantity, unit, expiry_date (YYYY-MM-DD), shelf_life_days (int), location. Today is %s. Return ONLY JSON.", 
             date_str);
    }

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg_sys = cJSON_CreateObject();
    cJSON_AddStringToObject(msg_sys, "role", "system");
    cJSON_AddStringToObject(msg_sys, "content", system_prompt);
    cJSON_AddItemToArray(messages, msg_sys);
    
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
    esp_http_client_cleanup(client);
    
    return success;
}
