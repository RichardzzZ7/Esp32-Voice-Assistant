// ui_inventory.c - 占位的界面刷新逻辑，实际应使用 LVGL/驱动实现
#include "ui_inventory.h"
#include "inventory.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui_inv";

// initialize inventory UI (register pages/buttons as needed)
void ui_inventory_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
}

// UI state
static lv_obj_t *inv_list = NULL;
static lv_obj_t *label_page = NULL;
static inventory_item_t **g_items = NULL;
static int g_item_count = 0;
static int g_page = 0;
static int g_page_size = 6;
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
    g_page = 0;
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

    // page label
    if (!label_page) {
        label_page = lv_label_create(lv_scr_act());
        lv_obj_align(label_page, LV_ALIGN_BOTTOM_MID, 0, -30);
    }

    // prev/next buttons
    static lv_obj_t *btn_prev = NULL;
    static lv_obj_t *btn_next = NULL;
    if (!btn_prev) {
        btn_prev = lv_btn_create(lv_scr_act()); lv_obj_set_size(btn_prev, 60, 40);
        lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 10, -10);
        lv_obj_add_event_cb(btn_prev, btn_prev_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lab = lv_label_create(btn_prev); lv_label_set_text_static(lab, "Prev"); lv_obj_center(lab);
    }
    if (!btn_next) {
        btn_next = lv_btn_create(lv_scr_act()); lv_obj_set_size(btn_next, 60, 40);
        lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
        lv_obj_add_event_cb(btn_next, btn_next_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lab2 = lv_label_create(btn_next); lv_label_set_text_static(lab2, "Next"); lv_obj_center(lab2);
    }

    // compute pagination
    int total_pages = (g_item_count + g_page_size - 1) / g_page_size;
    if (total_pages == 0) total_pages = 1;
    if (g_page >= total_pages) g_page = total_pages - 1;

    int start = g_page * g_page_size;
    int end = start + g_page_size; if (end > g_item_count) end = g_item_count;

    // create entries vertically
    int y = 0;
    for (int i = start; i < end; ++i) {
        inventory_item_t *it = g_items[i];
        char buf[128];
        snprintf(buf, sizeof(buf), "%s  %d%s  %s", it->name, it->remaining_days, "天", it->location);
        lv_obj_t *label = lv_label_create(inv_list);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_STATE_DEFAULT);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 10 + y*28);
        // highlight if near expiry
        if (it->remaining_days <= g_notify_threshold_days) {
            lv_obj_set_style_text_color(label, lv_color_make(255, 0, 0), LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_text_color(label, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
        }
        y++;
    }

    // update page label
    char pbuf[32]; snprintf(pbuf, sizeof(pbuf), "%d / %d", g_page+1, total_pages);
    lv_label_set_text(label_page, pbuf);

    lvgl_port_unlock();
}

void ui_inventory_next_page(void)
{
    int total_pages = (g_item_count + g_page_size - 1) / g_page_size;
    if (g_page + 1 < total_pages) { g_page++; ui_inventory_show(); }
}

void ui_inventory_prev_page(void)
{
    if (g_page > 0) { g_page--; ui_inventory_show(); }
}
