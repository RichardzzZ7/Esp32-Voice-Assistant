#include <stdio.h>
#include "esp32_s3_szp.h"
#include "app_ui.h"
#include "app_sr.h"
#include "inventory.h"
#include "ui_inventory.h"
#include "tts.h"
#include "notify.h"
#include "wifi.h"


void app_main(void)
{
    bsp_i2c_init();  // I2C初始化
    pca9557_init();  // IO扩展芯片初始化
    bsp_lvgl_start(); // 初始化液晶屏lvgl接口

    bsp_spiffs_mount(); // SPIFFS文件系统初始化
    bsp_codec_init(); // 音频初始化
    mp3_player_init(); // MP3播放器初始化

    // 初始化库存与 UI
    inventory_init();
    ui_inventory_init();

    // 初始化 TTS 与提醒
    tts_init();
    // check every 43200s (12 hours), threshold 3 days (adjustable)
    notify_init(43200, 3);

    // Start Wi-Fi (edit wifi_config.h to set SSID/PASSWORD)
    if (!wifi_init_sta()) {
        ESP_LOGW("main", "Wi-Fi failed to start or connect");
    }

    app_sr_init();  // 语音识别初始化   
}
