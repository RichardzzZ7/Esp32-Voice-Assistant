// inventory.c - 简易库存链表与持久化接口（依赖 storage.c）

#include "inventory.h"
#include "storage.h"
#include "parser.h"
#include "cJSON.h"
#include "esp_log.h"
#include "sync.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "inventory";
static inventory_item_t *g_head = NULL;
static const char *INV_PATH = "/spiffs/inventory.json";

// Maximum days we consider as a reasonable shelf life to
// avoid absurd values from cloud parsing (e.g., year 3000).
#define MAX_SHELF_LIFE_DAYS 365

// Simple id generator (timestamp + counter)
static int s_id_counter = 0;
const char *inventory_generate_id(void)
{
    static char buf[32];
    time_t now = time(NULL);
    s_id_counter = (s_id_counter + 1) & 0xFFFF;
    // Use standard long for portability and safety, assuming time_t fits in long or we just use lower bits
    snprintf(buf, sizeof(buf), "%ld_%04x", (long)now, s_id_counter);
    return buf;
}

// Map category to a default shelf life (days)
static int category_default_days(const char *category)
{
    if (!category) return 7;
    if (strstr(category, "牛奶") || strstr(category, "乳")) return 7;
    if (strstr(category, "肉") || strstr(category, "鸡") || strstr(category, "鱼")) return 3;
    if (strstr(category, "蔬") || strstr(category, "果")) return 5;
    if (strstr(category, "熟食")) return 2;
    if (strstr(category, "冷冻") || strstr(category, "冰")) return 30;
    return 7;
}

// compute calculated_expiry_date and remaining_days; return remaining_days
int inventory_compute_expiry(inventory_item_t *item)
{
    if (!item) return -1;
    if (item->added_time == 0) item->added_time = time(NULL);
    
    // If calculated_expiry_date is already set (e.g. from LLM), use it to reverse-calc shelf life if needed
    if (item->calculated_expiry_date > 0) {
        if (item->default_shelf_life_days <= 0) {
             item->default_shelf_life_days = (int)((item->calculated_expiry_date - item->added_time) / (24*3600));
        }
    } else {
        // Otherwise calculate from shelf life
        if (item->default_shelf_life_days <= 0) {
            item->default_shelf_life_days = category_default_days(item->category);
        }
        item->calculated_expiry_date = item->added_time + (int64_t)item->default_shelf_life_days * 24 * 3600;
    }

    time_t now = time(NULL);
    double diff_sec = difftime(item->calculated_expiry_date, now);
    int days;
    if (diff_sec <= 0) {
        days = 0;
    } else {
        // 向上取整到整天，比如剩余 2.1 天显示为 3 天
        days = (int)ceil(diff_sec / (24.0 * 3600.0));
    }
    if (days < 0) days = 0;
    if (days > MAX_SHELF_LIFE_DAYS) {
        days = MAX_SHELF_LIFE_DAYS;
    }
    item->remaining_days = days;
    return item->remaining_days;
}

void inventory_init(void)
{
    inventory_load();
}

int inventory_add_item(const inventory_item_t *item)
{
    if (!item) return -1;
    inventory_item_t *n = calloc(1, sizeof(inventory_item_t));
    if (!n) return -1;
    memcpy(n, item, sizeof(inventory_item_t));
    // ensure id
    if (n->item_id[0] == '\0') {
        strncpy(n->item_id, inventory_generate_id(), sizeof(n->item_id)-1);
    }
    if (n->added_time == 0) n->added_time = time(NULL);
    if (n->default_shelf_life_days == 0) n->default_shelf_life_days = category_default_days(n->category);
    n->last_notified_remaining_days = -1;
    inventory_compute_expiry(n);
    n->next = g_head;
    g_head = n;
    ESP_LOGI(TAG, "Added item: %s qty:%d %s loc:%s remaining:%d", n->name, n->quantity, n->unit, n->location, n->remaining_days);
    inventory_save();
    // enqueue sync event
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "item_id", n->item_id);
    cJSON_AddStringToObject(ev, "action", "add_item");
    cJSON_AddStringToObject(ev, "name", n->name);
    cJSON_AddNumberToObject(ev, "quantity", n->quantity);
    cJSON_AddStringToObject(ev, "location", n->location);
    char *s = cJSON_PrintUnformatted(ev);
    if (s) { sync_enqueue_event("add_item", s); free(s); }
    cJSON_Delete(ev);
    return 0;
}

int inventory_add_item_from_text(const char *text)
{
    inventory_item_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (parse_add_command(text, &tmp) == 0) {
        if (tmp.added_time == 0) tmp.added_time = time(NULL);
        if (tmp.default_shelf_life_days == 0) tmp.default_shelf_life_days = category_default_days(tmp.category);
        return inventory_add_item(&tmp);
    }
    ESP_LOGW(TAG, "parse failed for text: %s", text);
    return -1;
}

void inventory_save(void)
{
    cJSON *arr = cJSON_CreateArray();
    inventory_item_t *it = g_head;
    while (it) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "item_id", it->item_id);
        cJSON_AddStringToObject(o, "name", it->name);
        cJSON_AddStringToObject(o, "category", it->category);
        cJSON_AddStringToObject(o, "location", it->location);
        cJSON_AddStringToObject(o, "unit", it->unit);
        cJSON_AddNumberToObject(o, "quantity", it->quantity);
        cJSON_AddNumberToObject(o, "added_time", (double)it->added_time);
        cJSON_AddNumberToObject(o, "default_shelf_life_days", it->default_shelf_life_days);
        cJSON_AddNumberToObject(o, "calculated_expiry_date", (double)it->calculated_expiry_date);
        cJSON_AddNumberToObject(o, "remaining_days", it->remaining_days);
        cJSON_AddNumberToObject(o, "last_notified_remaining_days", it->last_notified_remaining_days);
        cJSON_AddStringToObject(o, "notes", it->notes);
        cJSON_AddStringToObject(o, "photo_url", it->photo_url);
        cJSON_AddItemToArray(arr, o);
        it = it->next;
    }
    char *s = cJSON_PrintUnformatted(arr);
    if (s) {
        storage_write_file(INV_PATH, s);
        free(s);
    }
    cJSON_Delete(arr);
}

void inventory_load(void)
{
    char *s = storage_read_file(INV_PATH);
    if (!s) return;
    cJSON *arr = cJSON_Parse(s);
    free(s);
    if (!arr) return;
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    size_t n = cJSON_GetArraySize(arr);
    for (size_t i = 0; i < n; ++i) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) continue;
        inventory_item_t *it = calloc(1, sizeof(inventory_item_t));
        cJSON *v;
        v = cJSON_GetObjectItem(o, "item_id"); if (v && cJSON_IsString(v)) strncpy(it->item_id, v->valuestring, sizeof(it->item_id)-1);
        v = cJSON_GetObjectItem(o, "name"); if (v && cJSON_IsString(v)) strncpy(it->name, v->valuestring, sizeof(it->name)-1);
        v = cJSON_GetObjectItem(o, "category"); if (v && cJSON_IsString(v)) strncpy(it->category, v->valuestring, sizeof(it->category)-1);
        v = cJSON_GetObjectItem(o, "location"); if (v && cJSON_IsString(v)) strncpy(it->location, v->valuestring, sizeof(it->location)-1);
        v = cJSON_GetObjectItem(o, "unit"); if (v && cJSON_IsString(v)) strncpy(it->unit, v->valuestring, sizeof(it->unit)-1);
        v = cJSON_GetObjectItem(o, "quantity"); if (v && cJSON_IsNumber(v)) it->quantity = v->valueint;
        v = cJSON_GetObjectItem(o, "added_time"); if (v && cJSON_IsNumber(v)) it->added_time = (int64_t)v->valuedouble;
        v = cJSON_GetObjectItem(o, "default_shelf_life_days"); if (v && cJSON_IsNumber(v)) it->default_shelf_life_days = v->valueint;
        v = cJSON_GetObjectItem(o, "calculated_expiry_date"); if (v && cJSON_IsNumber(v)) it->calculated_expiry_date = (int64_t)v->valuedouble;
        v = cJSON_GetObjectItem(o, "remaining_days"); if (v && cJSON_IsNumber(v)) it->remaining_days = v->valueint;
        v = cJSON_GetObjectItem(o, "last_notified_remaining_days"); if (v && cJSON_IsNumber(v)) it->last_notified_remaining_days = v->valueint; else it->last_notified_remaining_days = -1;
        v = cJSON_GetObjectItem(o, "notes"); if (v && cJSON_IsString(v)) strncpy(it->notes, v->valuestring, sizeof(it->notes)-1);
        v = cJSON_GetObjectItem(o, "photo_url"); if (v && cJSON_IsString(v)) strncpy(it->photo_url, v->valuestring, sizeof(it->photo_url)-1);
        // ensure computed fields
        inventory_compute_expiry(it);
        it->next = g_head;
        g_head = it;
    }
    cJSON_Delete(arr);
}

void inventory_print_all(void)
{
    inventory_item_t *it = g_head;
    while (it) {
        ESP_LOGI(TAG, "Item: %s qty:%d %s loc:%s added:%lld expiry:%lld remaining:%d days", it->name, it->quantity, it->unit, it->location, (long long)it->added_time, (long long)it->calculated_expiry_date, it->remaining_days);
        it = it->next;
    }
}

// Build array of pointers to items, sorted by remaining_days ascending
int inventory_list_items(inventory_item_t ***out_items)
{
    if (!out_items) return -1;
    int count = 0;
    inventory_item_t *it = g_head;
    while (it) { count++; it = it->next; }
    if (count == 0) { *out_items = NULL; return 0; }
    inventory_item_t **arr = calloc(count, sizeof(inventory_item_t*));
    if (!arr) return -1;
    int i = 0;
    it = g_head;
    while (it && i < count) { arr[i++] = it; it = it->next; }

    // sort by remaining_days ascending
    int cmp(const void *a, const void *b) {
        inventory_item_t *ia = *(inventory_item_t **)a;
        inventory_item_t *ib = *(inventory_item_t **)b;
        return ia->remaining_days - ib->remaining_days;
    }
    qsort(arr, count, sizeof(inventory_item_t*), cmp);
    *out_items = arr;
    return count;
}

void inventory_free_list(inventory_item_t **items)
{
    if (items) free(items);
}

void inventory_mark_notified(inventory_item_t *item, int remaining_days)
{
    if (!item) return;
    item->last_notified_remaining_days = remaining_days;
    // persist change
    inventory_save();
    // enqueue notify event
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "item_id", item->item_id);
    cJSON_AddStringToObject(ev, "action", "notified");
    cJSON_AddNumberToObject(ev, "remaining_days", remaining_days);
    char *s = cJSON_PrintUnformatted(ev);
    if (s) { sync_enqueue_event("notified", s); free(s); }
    cJSON_Delete(ev);
}

int inventory_remove_item(const char *name, int quantity)
{
    if (!name || quantity <= 0) return -1;
    
    inventory_item_t *curr = g_head;
    inventory_item_t *prev = NULL;
    
    while (curr) {
        // Simple substring match or exact match? Let's try strstr for flexibility
        if (strstr(curr->name, name) != NULL) {
            ESP_LOGI(TAG, "Found item to remove: %s (qty: %d)", curr->name, curr->quantity);
            
            if (curr->quantity > quantity) {
                curr->quantity -= quantity;
                ESP_LOGI(TAG, "Decreased quantity to %d", curr->quantity);
            } else {
                // Remove entire item
                if (prev) {
                    prev->next = curr->next;
                } else {
                    g_head = curr->next;
                }
                ESP_LOGI(TAG, "Removed item completely");
                free(curr);
                // Don't access curr after free
                curr = NULL; 
            }
            
            inventory_save();
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    
    ESP_LOGW(TAG, "Item not found for removal: %s", name);
    return -1;
}

void inventory_clear_all(void)
{
    inventory_item_t *curr = g_head;
    while (curr) {
        inventory_item_t *next = curr->next;
        free(curr);
        curr = next;
    }
    g_head = NULL;
    inventory_save(); // Save empty list
    ESP_LOGI(TAG, "Inventory cleared");
}

