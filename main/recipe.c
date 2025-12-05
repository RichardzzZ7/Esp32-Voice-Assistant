// recipe.c - request recipe suggestions from cloud LLM or generate local suggestion
#include "recipe.h"
#include "recipe_config.h"
#include "tts.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CLOUD_RECIPE_ENABLED
#include "esp_http_client.h"
#include "mbedtls/base64.h"
#include "mbedtls/md5.h"
#include <time.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "recipe";

typedef struct { inventory_item_t **items; int count; } task_arg_t;
// forward declarations for helper functions used by recipe_task
static void save_suggestion(const char *json);
static char *local_generate_recipe(inventory_item_t **items, int count);
#if CLOUD_RECIPE_ENABLED
static char *cloud_request_recipe(inventory_item_t **items, int count);
#endif

static void recipe_task(void *arg)
{
    task_arg_t *ta = (task_arg_t*)arg;
    inventory_item_t **its = ta->items; int c = ta->count;
    char *result = NULL;
#if CLOUD_RECIPE_ENABLED
    result = cloud_request_recipe(its, c);
#endif
    if (!result) {
        result = local_generate_recipe(its, c);
    }
    if (result) {
        save_suggestion(result);
        // speak short summary (trim to 200 chars)
        char summary[256];
        strncpy(summary, result, sizeof(summary)-1);
        summary[sizeof(summary)-1] = '\0';
        tts_speak_text(summary, true);
        free(result);
    }
    free(its);
    free(ta);
    vTaskDelete(NULL);
}

// write suggestion to SPIFFS file
static void save_suggestion(const char *json)
{
    FILE *f = fopen("/spiffs/recipe_last.json", "w");
    if (!f) return;
    fwrite(json, 1, strlen(json), f);
    fclose(f);
}

// simple local recipe generator
static char *local_generate_recipe(inventory_item_t **items, int count)
{
    // build simple suggestion text
    char *buf = malloc(1024);
    if (!buf) return NULL;
    strcpy(buf, "建议：\n");
    strcat(buf, "以下是基于临近过期食材的简单做法：\n");
    strcat(buf, "食材：");
    for (int i = 0; i < count; ++i) {
        strcat(buf, items[i]->name);
        if (i != count-1) strcat(buf, "、");
    }
    strcat(buf, "。\n做法：\n1. 将食材清洗切好；\n2. 快速翻炒或煮汤，加入常用调味即可。\n预计用时：30 分钟。\n");
    return buf;
}

#if CLOUD_RECIPE_ENABLED
// perform HTTP request to recipe LLM endpoint
static char *cloud_request_recipe(inventory_item_t **items, int count)
{
    // build simple prompt JSON with ingredient names
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; ++i) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(items[i]->name));
    }
    cJSON_AddItemToObject(root, "ingredients", arr);
    cJSON_AddStringToObject(root, "instruction", "请基于这些食材生成一个优先使用这些食材的食谱，输出步骤、用时、难度、可替代材料，返回json格式。");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return NULL;

    esp_http_client_config_t config = {
        .url = RECIPE_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(body); return NULL; }

    // Build Spark chat-compatible request payload
    // Create JSON: {"model":"spark-x1.5","messages":[{"role":"user","content":"..."}]}
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", "spark-x1.5");
    cJSON *msgs = cJSON_CreateArray();
    // build ingredient list string
    char ingreds[512] = {0};
    for (int i = 0; i < count; ++i) {
        strcat(ingreds, items[i]->name);
        if (i != count-1) strcat(ingreds, ", ");
    }
    char prompt[1024];
    snprintf(prompt, sizeof(prompt), "请基于以下食材（%s）生成一个优先使用这些食材的菜谱，要求输出：菜名、步骤、用时、难度、替代材料，返回 JSON 格式。", ingreds);
    cJSON *m0 = cJSON_CreateObject();
    cJSON_AddStringToObject(m0, "role", "user");
    cJSON_AddStringToObject(m0, "content", prompt);
    cJSON_AddItemToArray(msgs, m0);
    cJSON_AddItemToObject(req, "messages", msgs);
    char *req_body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_body) { esp_http_client_cleanup(client); free(body); return NULL; }

    // Set headers. Use provided credentials in recipe_config.h
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Appid", RECIPE_APPID);
    esp_http_client_set_header(client, "X-API-Key", RECIPE_API_KEY);
    esp_http_client_set_header(client, "X-API-Secret", RECIPE_API_SECRET);

    esp_http_client_set_post_field(client, req_body, strlen(req_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            // read response dynamically
            (void)esp_http_client_fetch_headers(client);
            int chunk = 1024;
            char *resp = malloc(chunk);
            if (!resp) { esp_http_client_cleanup(client); free(req_body); free(body); return NULL; }
            int total = 0;
            int read_len = 0;
            while ((read_len = esp_http_client_read(client, resp + total, chunk - total)) > 0) {
                total += read_len;
                if (total + 256 >= chunk) {
                    chunk *= 2;
                    char *n = realloc(resp, chunk);
                    if (!n) break;
                    resp = n;
                }
            }
            resp[total] = '\0';
            esp_http_client_cleanup(client);
            free(req_body);
            free(body);
            // try to extract content from JSON (choices[0].message.content or choices[0].content)
            cJSON *rj = cJSON_Parse(resp);
            if (rj) {
                cJSON *choices = cJSON_GetObjectItem(rj, "choices");
                if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                    cJSON *first = cJSON_GetArrayItem(choices, 0);
                    cJSON *msg = cJSON_GetObjectItem(first, "message");
                    cJSON *cont = NULL;
                    if (msg) cont = cJSON_GetObjectItem(msg, "content");
                    if (!cont) cont = cJSON_GetObjectItem(first, "content");
                    if (cont && cJSON_IsString(cont)) {
                        char *out = strdup(cont->valuestring);
                        cJSON_Delete(rj);
                        free(resp);
                        return out;
                    }
                }
                cJSON_Delete(rj);
            }
            // fallback: return raw body
            return resp;
        } else {
            ESP_LOGW(TAG, "Recipe API returned status %d", status);
        }
    } else {
        ESP_LOGW(TAG, "Recipe API request failed: %d", err);
    }
    esp_http_client_cleanup(client);
    free(req_body);
    free(body);
    return NULL;
}
#endif

void recipe_request_for_items(inventory_item_t **items, int count)
{
    if (!items || count <= 0) return;
    // run in background
    inventory_item_t **items_copy = calloc(count + 1, sizeof(inventory_item_t*));
    if (!items_copy) return;
    for (int i = 0; i < count; ++i) items_copy[i] = items[i];
    items_copy[count] = NULL; // terminator for simple scanning in task

    task_arg_t *targ = malloc(sizeof(task_arg_t));
    if (!targ) { free(items_copy); return; }
    targ->items = items_copy; targ->count = count;

    // create a FreeRTOS task to perform the recipe request
    xTaskCreatePinnedToCore(recipe_task, "recipe_task", 8*1024, (void*)targ, 5, NULL, 1);
}
