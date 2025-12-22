#include "cloud_llm.h"
// #include "cloud_asr.h" // No longer needed for LLM if using IAM Auth
#include "recipe_config.h"
#include "inventory.h"
#include "ui_inventory.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

static const char *TAG = "cloud_llm";

// IAM Bearer Token for Qianfan LLM (V2 API)
#define QIANFAN_BEARER_TOKEN "bce-v3/ALTAK-qV035uKpslFPqXnfHWzFd/8017c9c36f6a9555e4b9b6f4b0898b787c71c7a9"

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
                     cJSON *notes = cJSON_GetObjectItem(item_json, "notes");
                     cJSON *exp = cJSON_GetObjectItem(item_json, "expiry_date");
                     cJSON *shelf = cJSON_GetObjectItem(item_json, "shelf_life_days");

                     if (name && name->valuestring) strncpy(item.name, name->valuestring, sizeof(item.name)-1);
                     if (cat && cat->valuestring) strncpy(item.category, cat->valuestring, sizeof(item.category)-1);
                     if (qty) item.quantity = qty->valueint;
                     if (unit && unit->valuestring) strncpy(item.unit, unit->valuestring, sizeof(item.unit)-1);
                     if (loc && loc->valuestring) strncpy(item.location, loc->valuestring, sizeof(item.location)-1);
                     if (notes && notes->valuestring) strncpy(item.notes, notes->valuestring, sizeof(item.notes)-1);
                     
                     // Use shelf_life_days when provided
                     if (shelf && cJSON_IsNumber(shelf) && shelf->valueint > 0) {
                         item.default_shelf_life_days = shelf->valueint;
                     }

                     // Handle expiry date parsing (YYYY-MM-DD) when it's a non-empty string
                     if (exp && cJSON_IsString(exp) && exp->valuestring && exp->valuestring[0] != '\0') {
                         struct tm tmv = {0};
                         if (sscanf(exp->valuestring, "%d-%d-%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
                             tmv.tm_year -= 1900;
                             tmv.tm_mon -= 1;
                             item.calculated_expiry_date = mktime(&tmv);
                         }
                     }

                     // If expiry_date is missing or parsing failed but we have shelf_life_days,
                     // derive a reasonable expiry date from shelf_life_days.
                     if (item.calculated_expiry_date == 0 && item.default_shelf_life_days > 0) {
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
                        cJSON *notes = cJSON_GetObjectItem(item_json, "notes");
                        cJSON *exp = cJSON_GetObjectItem(item_json, "expiry_date");
                        cJSON *shelf = cJSON_GetObjectItem(item_json, "shelf_life_days");

                        if (name && name->valuestring) strncpy(item.name, name->valuestring, sizeof(item.name)-1);
                        if (cat && cat->valuestring) strncpy(item.category, cat->valuestring, sizeof(item.category)-1);
                        if (qty) item.quantity = qty->valueint;
                        if (unit && unit->valuestring) strncpy(item.unit, unit->valuestring, sizeof(item.unit)-1);
                        if (loc && loc->valuestring) strncpy(item.location, loc->valuestring, sizeof(item.location)-1);
                        if (notes && notes->valuestring) strncpy(item.notes, notes->valuestring, sizeof(item.notes)-1);
                        
                        // Use shelf_life_days when provided
                        if (shelf && cJSON_IsNumber(shelf) && shelf->valueint > 0) {
                            item.default_shelf_life_days = shelf->valueint;
                        }

                        // Handle expiry date parsing (YYYY-MM-DD) when it's a non-empty and valid string
                        if (exp && cJSON_IsString(exp) && exp->valuestring && exp->valuestring[0] != '\0') {
                            const char *exp_str = exp->valuestring;
                            // Treat "未知" / "不详" / "unknown" 等为未知日期，交给下方 shelf_life_days 逻辑
                            if (strcmp(exp_str, "未知") != 0 && strcmp(exp_str, "不详") != 0 &&
                                strcasecmp(exp_str, "unknown") != 0 && strcasecmp(exp_str, "unk") != 0) {
                                struct tm tmv = {0};
                                if (sscanf(exp_str, "%d-%d-%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday) == 3) {
                                    tmv.tm_year -= 1900;
                                    tmv.tm_mon -= 1;
                                    item.calculated_expiry_date = mktime(&tmv);
                                }
                            }
                        }

                        // If expiry_date is missing or parsing failed but we have shelf_life_days,
                        // derive a reasonable expiry date from shelf_life_days.
                        if (item.calculated_expiry_date == 0 && item.default_shelf_life_days > 0) {
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

    // Use Baidu Qianfan V2 API with Bearer Token
    const char *url = "https://qianfan.baidubce.com/v2/chat/completions";

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        // 减小 HTTP buffer 占用的内部内存，降低 TLS 内存压力
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // Set Headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", QIANFAN_BEARER_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    // Build JSON Body for Baidu
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "ernie-speed-128k");
    
    // Dynamic Date for Prompt (Local Time)
    time_t now = time(NULL);
    struct tm local_timeinfo = {0};
    localtime_r(&now, &local_timeinfo);
    char date_str[20];
    if (local_timeinfo.tm_year < (2024 - 1900)) {
        snprintf(date_str, sizeof(date_str), "2025-11-28");
    } else {
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &local_timeinfo);
    }

    char system_prompt[2048];
    if (action == LLM_ACTION_REMOVE) {
        snprintf(system_prompt, sizeof(system_prompt),
            "你是一个冰箱库存管理助手。用户希望移除一些物品。请从用户语音中抽取: name(名称), quantity(数量)。只返回 JSON，不要包含其他文字。示例: {\"name\":\"苹果\",\"quantity\":2}");
    } else {
        snprintf(system_prompt, sizeof(system_prompt),
            "你是一个冰箱库存管理助手。请从用户语音中抽取以下字段: name(名称), category(类别), quantity(数量), unit(单位), expiry_date(保质期, YYYY-MM-DD), shelf_life_days(保质期天数, int), location(推荐存放区域), notes(备注)。"
            "今天是 %s。如果用户没有明确说出具体保质期天数, 需要你根据食材类型和常见保存习惯智能推荐一个合理的 shelf_life_days(>0), 例如: 牛奶/酸奶≈7天, 生肉≈2-3天, 冷冻食品≈30天, 常温零食≈30天。"
            "location 字段如果用户没有明确说出存放位置, 需要你根据食材类型智能推荐, 例如: 牛奶/熟食/鸡蛋→冷藏区, 冷冻食品→冷冻室, 罐头/零食→常温储藏区。务必给出非空的 location 字符串。"
            "只返回 JSON, 不要包含其他文字。示例: {\"name\":\"牛奶\",\"category\":\"乳制品\",\"quantity\":1,\"unit\":\"盒\",\"expiry_date\":\"2025-12-01\",\"shelf_life_days\":7,\"location\":\"冷藏区\",\"notes\":\"脱脂\"}",
            date_str);
    }

    // Baidu uses "messages" array just like OpenAI
    cJSON *messages = cJSON_CreateArray();
    
    // Combine system prompt and user text into a single user message to ensure it is respected
    // Some Baidu models/endpoints ignore the 'system' field or separate system messages.
    char *combined_content = heap_caps_malloc(strlen(system_prompt) + strlen(text) + 50,
                                              MALLOC_CAP_SPIRAM);
    if (combined_content) {
        sprintf(combined_content, "%s\n\nUser Input: %s", system_prompt, text);
        
        cJSON *msg_user = cJSON_CreateObject();
        cJSON_AddStringToObject(msg_user, "role", "user");
        cJSON_AddStringToObject(msg_user, "content", combined_content);
        cJSON_AddItemToArray(messages, msg_user);
        
        free(combined_content);
    } else {
        // Fallback if malloc fails
        cJSON *msg_user = cJSON_CreateObject();
        cJSON_AddStringToObject(msg_user, "role", "user");
        cJSON_AddStringToObject(msg_user, "content", text);
        cJSON_AddItemToArray(messages, msg_user);
    }
    
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
            char *buffer = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM); // Allocate 4KB buffer in PSRAM
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

bool cloud_llm_recommend_recipes(void)
{
    ESP_LOGI(TAG, "Requesting Recipe Recommendation...");

    // 1. Get Inventory
    inventory_item_t **items = NULL;
    int count = inventory_list_items(&items);
    if (count <= 0) {
        ESP_LOGW(TAG, "Inventory is empty, cannot recommend recipes.");
        printf("Inventory is empty. Please add items first.\n");
        return false;
    }

    // 2. Build Inventory String
    char *inv_str = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!inv_str) {
        inventory_free_list(items);
        return false;
    }
    strcpy(inv_str, "Current Inventory: ");
    for (int i = 0; i < count; i++) {
        char item_buf[128];
        snprintf(item_buf, sizeof(item_buf), "%s (%d %s), ", items[i]->name, items[i]->quantity, items[i]->unit);
        if (strlen(inv_str) + strlen(item_buf) < 2047) {
            strcat(inv_str, item_buf);
        }
    }
    inventory_free_list(items);

    // 3. Prepare LLM Request
    const char *url = "https://qianfan.baidubce.com/v2/chat/completions";
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000, // Longer timeout for recipe generation
        // 略微减小 buffer，避免占用过多内部内存
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", QIANFAN_BEARER_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth_header);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "ernie-speed-128k");

    // 提示词：用简体中文，中餐优先，推荐 3 道常见家常菜，突出食材和主要步骤，控制在约 350 字以内
    const char *system_prompt =
        "你是一个专业中文厨师助手，请根据提供的冰箱库存，优先推荐中国人日常三餐常吃的中餐家常菜"
        "（如炒菜、炖菜、煲汤、主食等），避免奇怪或少见的搭配。"
        "请只输出纯文本，最多推荐 3 道菜，不要任何前后解释，也不要 Markdown。"
        "每道菜单独一行，格式类似："
        "1. 菜名 - 食材：食材1, 食材2, 食材3；步骤：用一两句话概括主要烹饪方法，语言自然口语化。"
        "整体字数尽量控制在 350 字以内，不需要详细到精确克数或时间。";
    
    cJSON *messages = cJSON_CreateArray();
    char *combined_content = heap_caps_malloc(strlen(system_prompt) + strlen(inv_str) + 50,
                                              MALLOC_CAP_SPIRAM);
    if (combined_content) {
        sprintf(combined_content, "%s\n\n%s", system_prompt, inv_str);
        cJSON *msg_user = cJSON_CreateObject();
        cJSON_AddStringToObject(msg_user, "role", "user");
        cJSON_AddStringToObject(msg_user, "content", combined_content);
        cJSON_AddItemToArray(messages, msg_user);
        free(combined_content);
    }
    cJSON_AddItemToObject(root, "messages", messages);
    free(inv_str);

    char *post_data = cJSON_PrintUnformatted(root);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    // 4. Execute
    bool success = false;
    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err == ESP_OK) {
        esp_http_client_write(client, post_data, strlen(post_data));
        if (esp_http_client_fetch_headers(client) >= 0) {
            char *buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM); // Larger buffer for recipe in PSRAM
            if (buffer) {
                int read_len = esp_http_client_read_response(client, buffer, 8192);
                if (read_len >= 0) {
                    buffer[read_len] = 0;
                    // Parse and print content，同时在 UI 上显示简要菜谱
                    cJSON *resp = cJSON_Parse(buffer);
                    if (resp) {
                        char *recipe_text = NULL;

                        cJSON *result = cJSON_GetObjectItem(resp, "result");
                        if (result && cJSON_IsString(result) && result->valuestring) {
                            recipe_text = strdup(result->valuestring);
                        } else {
                            // Try OpenAI format
                            cJSON *choices = cJSON_GetObjectItem(resp, "choices");
                            if (choices && cJSON_IsArray(choices)) {
                                cJSON *first = cJSON_GetArrayItem(choices, 0);
                                cJSON *msg = cJSON_GetObjectItem(first, "message");
                                cJSON *content = cJSON_GetObjectItem(msg, "content");
                                if (content && cJSON_IsString(content) && content->valuestring) {
                                    recipe_text = strdup(content->valuestring);
                                }
                            }
                        }

                        if (recipe_text) {
                            // 为安全起见，截断到适中的长度，避免 UI 过长
                            char short_buf[512];
                            size_t len = strlen(recipe_text);
                            if (len >= sizeof(short_buf)) {
                                len = sizeof(short_buf) - 1;
                            }
                            memcpy(short_buf, recipe_text, len);
                            short_buf[len] = '\0';

                            printf("\n=== Recipe Recommendation ===\n%s\n=============================\n", short_buf);
                            ui_recipe_show_text(short_buf);

                            free(recipe_text);
                        }

                        cJSON_Delete(resp);
                    }
                    success = true;
                }
                free(buffer);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP Failed: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    free(post_data);
    esp_http_client_cleanup(client);
    return success;
}
