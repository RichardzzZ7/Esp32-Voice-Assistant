// storage.c - 基于标准文件 IO 的简单读写，使用 SPIFFS 挂载路径
#include "storage.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "storage";

int storage_write_file(const char *path, const char *buf)
{
    if (!path || !buf) return -1;
    FILE *f = fopen(path, "w");
    if (!f) {
        esp_log_level_set(TAG, ESP_LOG_WARN);
        return -1;
    }
    size_t wr = fwrite(buf, 1, strlen(buf), f);
    fclose(f);
    return (wr == strlen(buf)) ? 0 : -1;
}

char *storage_read_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}
