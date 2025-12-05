#ifndef _CLOUD_ASR_H_
#define _CLOUD_ASR_H_

#include <stdint.h>
#include <stddef.h>

// 初始化 ASR (获取 Token 等)
void cloud_asr_init(void);

// 发送 PCM 音频数据到云端进行识别
// audio_data: 16bit 16kHz 单声道 PCM 数据
// len: 数据字节长度
// 返回: 识别到的文本字符串 (需要调用者 free) 或 NULL
char* cloud_asr_send_audio(const int16_t *audio_data, int len);

#endif // _CLOUD_ASR_H_
