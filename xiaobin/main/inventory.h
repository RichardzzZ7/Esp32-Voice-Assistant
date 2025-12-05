// inventory.h - 简易库存数据结构与 API
#ifndef _INVENTORY_H_
#define _INVENTORY_H_

#include <stdint.h>

typedef struct inventory_item_t {
    char item_id[32]; // 唯一标识
    char name[64];
    char category[32]; // 牛奶/肉类/蔬果/熟食/冷冻
    int quantity;
    char unit[16];
    char location[32]; // 中层左/中层右/冷冻室...
    int64_t added_time; // UTC epoch seconds
    int default_shelf_life_days; // 默认保质期（天）
    int64_t calculated_expiry_date; // epoch seconds
    int remaining_days; // 计算值
    char notes[128];
    char photo_url[128];
    int last_notified_remaining_days; // -1 未通知
    struct inventory_item_t *next;
} inventory_item_t;

void inventory_init(void);
int inventory_add_item(const inventory_item_t *item);
int inventory_add_item_from_text(const char *text);
void inventory_save(void);
void inventory_load(void);
void inventory_print_all(void);
int inventory_compute_expiry(inventory_item_t *item);
const char *inventory_generate_id(void);
// 返回动态分配的指针数组（元素为指向内部链表项的指针），caller 需 free() 返回的数组（但不要 free 元素）
int inventory_list_items(inventory_item_t ***out_items);
void inventory_free_list(inventory_item_t **items);
// 通知字段：记录上次被提醒时的 remaining_days，用于避免重复提醒
void inventory_mark_notified(inventory_item_t *item, int remaining_days);

// 移除物品（减少数量或删除）
int inventory_remove_item(const char *name, int quantity);
// 清空所有库存
void inventory_clear_all(void);

#endif // _INVENTORY_H_
