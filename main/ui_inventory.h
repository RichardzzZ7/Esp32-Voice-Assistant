// ui_inventory.h - 最小化的 UI 接口占位
#ifndef _UI_INVENTORY_H_
#define _UI_INVENTORY_H_

void ui_inventory_init(void);
void ui_inventory_refresh(void);
void ui_inventory_show(void);
void ui_inventory_next_page(void);
void ui_inventory_prev_page(void);

// 在当前容器上显示简要菜谱文本（多行字符串）
void ui_recipe_show_text(const char *text);

#endif // _UI_INVENTORY_H_
