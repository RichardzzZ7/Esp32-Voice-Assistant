// storage.h - 简单文件读写封装（基于 SPIFFS）
#ifndef _STORAGE_H_
#define _STORAGE_H_

int storage_write_file(const char *path, const char *buf);
char *storage_read_file(const char *path); // return malloc'd buffer, caller must free

#endif // _STORAGE_H_
