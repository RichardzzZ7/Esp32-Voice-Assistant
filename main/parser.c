// parser.c - 基本中文命令槽位抽取（启发式）
#include "parser.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

// 本实现为启发式解析，目标在设备端做初步槽位抽取以减少误报和网络调用。
// 支持的例句样例（中文或拼音），示例仅对简单句子有效：
// "放入鸡蛋 6 个 在 冰箱 上层 今天" 或 "加入 牛奶 一瓶 冷藏 3天" 或 "放入 牛肉 2 份 失效日期 2025-11-20"

static const char *skip_spaces(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

// 提取第一个连续的中文/字母/数字/汉字串作为词
static int extract_token(const char **p, char *out, int maxlen) {
    const char *s = *p;
    s = skip_spaces(s);
    if (!*s) return 0;
    int i = 0;
    while (*s && i < maxlen-1) {
        if ((unsigned char)*s < 0x80) {
            // ascii: break on spaces or punctuation
            if (isspace((unsigned char)*s) || strchr(",。.，、!?；;:\\/-", *s)) break;
            out[i++] = *s++;
        } else {
            // treat multibyte (likely utf8 chinese) - copy bytes until next ascii or space
            // copy up to 3 bytes as naive approach
            out[i++] = *s++;
            if (*s) { out[i++] = *s++; }
            if (*s) { out[i++] = *s++; }
        }
    }
    out[i] = '\0';
    *p = s;
    return i>0;
}

int parse_add_command(const char *text, inventory_item_t *out)
{
    if (!text || !out) return -1;
    memset(out, 0, sizeof(*out));
    const char *s = text;
    // simple heuristic: look for keywords 放入/加入/存放
    if (strstr(s, "放入") == NULL && strstr(s, "加入") == NULL && strstr(s, "存放") == NULL) {
        // still allow generic sentences
    }

    // Try to find quantity: digits or Chinese numerals (very basic)
    const char *p = s;
    char token[128];
    // find first token as name candidate
    if (!extract_token(&p, token, sizeof(token))) return -1;
    // If token is "放入"/"加入" skip
    if (strstr(token, "放入") || strstr(token, "加入") || strstr(token, "存放")) {
        if (!extract_token(&p, token, sizeof(token))) return -1;
    }
    strncpy(out->name, token, sizeof(out->name)-1);

    // scan remaining text for a number (quantity) or unit or expiry info
    const char *q = p;
    out->quantity = 1;
    while (*q) {
        if (isdigit((unsigned char)*q)) {
            int val = 0;
            while (isdigit((unsigned char)*q)) { val = val*10 + (*q - '0'); q++; }
            out->quantity = val > 0 ? val : 1;
            continue;
        }
        // detect explicit expiry like "失效日期 2025-11-20" or "2025/11/20"
        if (strncmp(q, "失效日期", 12) == 0 || strncmp(q, "失效", 6) == 0) {
            // skip non-digits
            const char *d = q;
            while (*d && !isdigit((unsigned char)*d)) d++;
            int y=0,m=0,dv=0;
            if (sscanf(d, "%d-%d-%d", &y, &m, &dv) == 3) {
                struct tm tmv = {0};
                tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = dv;
                time_t texp = mktime(&tmv);
                out->calculated_expiry_date = (int64_t)texp;
                // if user provides explicit expiry, compute default_shelf_life_days relative to added_time
                out->default_shelf_life_days = (int)((out->calculated_expiry_date - out->added_time) / (24*3600));
            }
        }
        // detect relative shelf life like "3天" or "保质期3天"
        if (strstr(q, "保质期") || strstr(q, "保鲜期")) {
            const char *r = q;
            while (*r && !isdigit((unsigned char)*r)) r++;
            if (isdigit((unsigned char)*r)) {
                int v = atoi(r);
                out->default_shelf_life_days = v;
            }
        }
        // detect date pattern anywhere
        int y=0,mo=0,da=0;
        if (sscanf(q, "%d-%d-%d", &y, &mo, &da) == 3) {
            struct tm tmv = {0};
            tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = da;
            time_t texp = mktime(&tmv);
            out->calculated_expiry_date = (int64_t)texp;
            out->default_shelf_life_days = (int)((out->calculated_expiry_date - out->added_time) / (24*3600));
        }

        q++;
    }

    // Try to extract location with keywords 层/格/箱/冰箱/架
    const char *loc_keywords[] = {"层", "格", "位置", "冰箱", "架", "箱", "上层", "下层", NULL};
    for (int i = 0; loc_keywords[i]; ++i) {
        char *found = strstr(s, loc_keywords[i]);
        if (found) {
            // backward scan to get preceding token as location name
            const char *start = found - 20; if (start < s) start = s;
            const char *t = start;
            char buf[64] = {0}; int bi = 0;
            while (t < found && bi < (int)sizeof(buf)-1) {
                buf[bi++] = *t++;
            }
            buf[bi] = '\0';
            // take last word
            char *last = strrchr(buf, ' ');
            if (last) strncpy(out->location, last+1, sizeof(out->location)-1);
            else strncpy(out->location, buf, sizeof(out->location)-1);
            break;
        }
    }

    // detect basic category keywords
    if (strstr(s, "牛奶") || strstr(s, "牛乳") || strstr(s, "milk")) strncpy(out->category, "牛奶", sizeof(out->category)-1);
    else if (strstr(s, "肉") || strstr(s, "牛肉") || strstr(s, "鸡") || strstr(s, "猪")) strncpy(out->category, "肉类", sizeof(out->category)-1);
    else if (strstr(s, "菜") || strstr(s, "蔬") || strstr(s, "果") || strstr(s, "青菜")) strncpy(out->category, "蔬果", sizeof(out->category)-1);
    else if (strstr(s, "熟食") || strstr(s, "熟")) strncpy(out->category, "熟食", sizeof(out->category)-1);
    else if (strstr(s, "冷冻") || strstr(s, "冰")) strncpy(out->category, "冷冻", sizeof(out->category)-1);

    // default: unit empty, default_shelf_life_days left 0
    return 0;
}
