// notify.c - periodic scan of inventory and TTS reminders
#include "notify.h"
#include "inventory.h"
#include "tts.h"
#include "ui_inventory.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "notify";
static int g_check_interval = 43200; // seconds
static int g_threshold_days = 3;

static void notify_task(void *arg)
{
    (void)arg;
    while (1) {
        inventory_item_t **items = NULL;
        int count = inventory_list_items(&items);
        if (count > 0 && items) {
            // only do expiry notifications; do NOT trigger recipe suggestions here
            for (int i = 0; i < count; ++i) {
                inventory_item_t *it = items[i];
                if (!it) continue;
                if (it->remaining_days <= g_threshold_days) {
                    if (it->last_notified_remaining_days != it->remaining_days) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "%s 将在 %d 天后过期", it->name, it->remaining_days);
                        ESP_LOGI(TAG, "notify: %s", msg);
                        tts_speak_text(msg, true);
                        inventory_mark_notified(it, it->remaining_days);
                        ui_inventory_refresh();
                    }
                }
            }
        }
        if (items) inventory_free_list(items);
        vTaskDelay(pdMS_TO_TICKS(g_check_interval * 1000));
    }
}

void notify_init(int check_interval_seconds, int threshold_days)
{
    if (check_interval_seconds > 0) g_check_interval = check_interval_seconds;
    if (threshold_days >= 0) g_threshold_days = threshold_days;
    xTaskCreatePinnedToCore(notify_task, "notify", 6*1024, NULL, 5, NULL, 1);
}
