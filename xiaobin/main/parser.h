// parser.h - 简单命令文本解析（槽位抽取）
#ifndef _PARSER_H_
#define _PARSER_H_

#include "inventory.h"

// 解析“放入...数量...位置...时间”类命令，成功返回0并填充 out。
int parse_add_command(const char *text, inventory_item_t *out);

#endif // _PARSER_H_
