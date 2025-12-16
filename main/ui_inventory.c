// ui_inventory.c - 占位的界面刷新逻辑，实际应使用 LVGL/驱动实现
#include "ui_inventory.h"
#include "inventory.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui_inv";

LV_FONT_DECLARE(font_alipuhui20);

// initialize inventory UI (register pages/buttons as needed)
void ui_inventory_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
}

// UI state
static lv_obj_t *inv_list = NULL;
static inventory_item_t **g_items = NULL;
static int g_item_count = 0;
// 之前示例中有分页逻辑，这里改为单页显示全部条目
static int g_notify_threshold_days = 3; // highlight threshold

static void btn_prev_cb(lv_event_t *e)
{
    ui_inventory_prev_page();
}
static void btn_next_cb(lv_event_t *e)
{
    ui_inventory_next_page();
}

// create or refresh list content from inventory
void ui_inventory_refresh(void)
{
    // free previous items
    if (g_items) { inventory_free_list(g_items); g_items = NULL; g_item_count = 0; }
    int count = inventory_list_items(&g_items);
    if (count < 0) { ESP_LOGW(TAG, "failed to get items"); return; }
    g_item_count = count;
    // update display
    ui_inventory_show();
}

// build the lvgl list for current page
void ui_inventory_show(void)
{
    lvgl_port_lock(0);
    // if existing, delete
    if (inv_list) { lv_obj_clean(inv_list); }
    else {
        inv_list = lv_obj_create(lv_scr_act());
        lv_obj_set_size(inv_list, 320, 200);
        lv_obj_align(inv_list, LV_ALIGN_TOP_MID, 0, 10);
    }

    // 不再做分页，直接显示所有条目
    int start = 0;
    int end = g_item_count;

    // create entries vertically
    int y = 0;
    for (int i = start; i < end; ++i) {
        inventory_item_t *it = g_items[i];
        char buf[128];
        // 显示: 名称  数量单位  剩余天数  存放位置
        // 例如: 鸡蛋  1盒  5天  冷藏区
        snprintf(buf, sizeof(buf), "%s  %d%s  %d天  %s",
                 it->name,
                 it->quantity,
                 it->unit,
                 it->remaining_days,
                 it->location);
        lv_obj_t *label = lv_label_create(inv_list);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_font(label, &font_alipuhui20, LV_STATE_DEFAULT);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10 + y*28);
        // highlight if near expiry
        if (it->remaining_days <= g_notify_threshold_days) {
            lv_obj_set_style_text_color(label, lv_color_make(255, 0, 0), LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_text_color(label, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
        }
        y++;
    }

    lvgl_port_unlock();
}

// 在同一个容器上显示菜谱简要信息
// text 为多行简短字符串（例如两道菜，每道一行或两行）
void ui_recipe_show_text(const char *text)
{
    if (!text) {
        text = "";
    }

    lvgl_port_lock(0);
    // 复用库存列表的容器
    if (inv_list) {
        lv_obj_clean(inv_list);
    } else {
        inv_list = lv_obj_create(lv_scr_act());
        lv_obj_set_size(inv_list, 320, 200);
        lv_obj_align(inv_list, LV_ALIGN_TOP_MID, 0, 10);
    }

    lv_obj_t *label = lv_label_create(inv_list);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 300);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_alipuhui20, LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10);

    lvgl_port_unlock();
}

void ui_inventory_next_page(void)
{
    // 取消分页后，该函数保留为空实现，避免旧代码调用崩溃
    (void)g_items;
}

void ui_inventory_prev_page(void)
{
    // 取消分页后，该函数保留为空实现，避免旧代码调用崩溃
    (void)g_items;
}
