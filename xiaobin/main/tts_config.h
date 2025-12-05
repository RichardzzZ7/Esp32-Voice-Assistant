// tts_config.h - 讯飞 TTS 配置（放在设备上会保存 APIKey/Secret）
#ifndef _TTS_CONFIG_H_
#define _TTS_CONFIG_H_

// 若设置为 1 则启用云端 TTS 调用
#define CLOUD_TTS_ENABLED 1

// 以下为用户提供的讯飞认证信息，请妥善保管
#define XFYUN_API_KEY "0d68a07241dc5eb55a972b5f41a721d1"
#define XFYUN_API_SECRET "NGU0MDRmNmRjZWVmNTNmOTliZGMyYTE5"
#define XFYUN_APPID "ba898f75"

// 讯飞 TTS REST endpoint (use HTTPS for secure transport)
#define XFYUN_TTS_URL "https://api.xfyun.cn/v1/service/v1/tts"

// Optional: root CA PEM for certificate pinning. If empty, system CA store is used.
// To enable pinning, replace the empty string with the PEM content, e.g.:
// #define XFYUN_ROOT_CA "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
#define XFYUN_ROOT_CA ""

#endif // _TTS_CONFIG_H_
