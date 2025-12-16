
#include "app_sr.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "esp32_s3_szp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_process_sdkconfig.h"
#include "audio_player.h"
#include "app_ui.h"
#include "inventory.h"
#include "ui_inventory.h"
#include "cloud_llm.h"
#include "cloud_asr.h"

#include "esp_task_wdt.h"

static const char *TAG = "app_sr";

// Audio recording buffer for Cloud ASR
#define RECORD_TIME_SEC 3
#define RECORD_SAMPLE_RATE 16000
#define RECORD_BUFFER_SIZE (RECORD_TIME_SEC * RECORD_SAMPLE_RATE * sizeof(int16_t))
static int16_t *g_record_buffer = NULL;
static int g_record_offset = 0;
static bool g_is_recording = false;
static volatile bool g_is_processing = false;
static TaskHandle_t s_process_task_handle = NULL;
static llm_action_t g_current_action = LLM_ACTION_ADD;

srmodel_list_t *models = NULL;
static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;

int detect_flag = 0;
static volatile int task_flag = 0;

// Background task to run cloud recipe recommendation without blocking AFE/multinet loop
static void llm_recipe_task(void *arg)
{
    (void)arg;
    cloud_llm_recommend_recipes();
    vTaskDelete(NULL);
}

void process_audio_task(void *arg) {
    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Do the heavy lifting
        if (g_record_buffer) {
             char *text = cloud_asr_send_audio(g_record_buffer, g_record_offset);
             if (text) {
                 ESP_LOGI(TAG, "ASR: %s", text);
                 cloud_llm_parse_inventory(text, g_current_action);
                 ui_inventory_refresh();
                 free(text);
             }
             free(g_record_buffer);
             g_record_buffer = NULL;
        }
        
        // Done
        g_is_processing = false;
    }
}

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;  // 获取参数
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data); // 获取帧长度
    int nch = afe_handle->get_channel_num(afe_data); // 获取声道数
    int feed_channel = bsp_get_feed_channel(); // 获取ADC输入通道数
    assert(nch <= feed_channel);
    int16_t *i2s_buff = heap_caps_malloc(audio_chunksize * sizeof(int16_t) * feed_channel, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM); // 分配获取I2S数据的缓存大小
    assert(i2s_buff);

    while (task_flag) {
        bsp_get_feed_data(false, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);  // 获取I2S数据

        afe_handle->feed(afe_data, i2s_buff); // 把获取到的I2S数据输入给afe_data
    }
    if (i2s_buff) {
        free(i2s_buff);
        i2s_buff = NULL;
    }
    vTaskDelete(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;  // 接收参数
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);  // 获取fetch帧长度
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE); // 初始化命令词模型
    printf("multinet:%s\n", mn_name); // 打印命令词模型名称
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);  // 设置唤醒后等待事件 6000代表6000毫秒
    esp_mn_commands_clear(); // 清除当前的命令词列表
    esp_mn_commands_add(1, "ce shi yun duan"); // 测试云端 (Test Cloud)
    esp_mn_commands_add(2, "fang ru"); // 放入 (Trigger for demo)
    esp_mn_commands_add(3, "na chu"); // 拿出
    esp_mn_commands_add(4, "xian shi ku cun"); // 显示库存
    esp_mn_commands_add(5, "qing kong"); // 清空
    esp_mn_commands_add(6, "cai pu tui jian"); // 菜谱推荐
    esp_mn_commands_add(7, "fan hui"); // 返回
    esp_mn_commands_update(); // 更新命令词
    int mu_chunksize = multinet->get_samp_chunksize(model_data);  // 获取samp帧长度
    assert(mu_chunksize == afe_chunksize);

    // 打印所有的命令
    multinet->print_active_speech_commands(model_data);
    printf("------------detect start------------\n");

    bool was_processing = false;
    while (task_flag) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); // 获取模型输出结果
        if (!res || res->ret_value == ESP_FAIL) {
            printf("fetch error!\n");
            break;
        }

        // Handle Recording Logic
        if (g_is_recording) {
            if (g_record_buffer && res->data) {
                int chunk_bytes = res->data_size; // data_size is in bytes
                if (g_record_offset + chunk_bytes < RECORD_BUFFER_SIZE) {
                    memcpy((char*)g_record_buffer + g_record_offset, res->data, chunk_bytes);
                    g_record_offset += chunk_bytes;
                } else {
                    // Buffer full, stop recording and process
                    printf("Recording finished (buffer full). Processing...\n");
                    g_is_recording = false;
                    g_is_processing = true;
                    was_processing = true;
                    xTaskNotifyGive(s_process_task_handle);
                }
            }
            continue; // Skip multinet detection while recording
        }

        if (g_is_processing) {
             // Just drain the buffer to prevent "rb_out slow"
             continue;
        }
        
        if (was_processing) {
             // Transition from Processing -> Idle
             was_processing = false;
             afe_handle->enable_wakenet(afe_data);
             detect_flag = 0;
             ai_gui_out();
             printf("-----------awaits to be waken up-----------\n");
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            printf("WAKEWORD DETECTED\n");
	        multinet->clean(model_data);  // clean all status of multinet
        } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {  // 检测到唤醒词
            // play_voice = -1;
            afe_handle->disable_wakenet(afe_data);  // 关闭唤醒词识别
            detect_flag = 1; // 标记已检测到唤醒词
            ai_gui_in(); // AI人出现
            printf("AFE_FETCH_CHANNEL_VERIFIED, channel index: %d\n", res->trigger_channel_id);
        }

        if (detect_flag == 1) {
            esp_mn_state_t mn_state = multinet->detect(model_data, res->data); // 检测命令词

            if (mn_state == ESP_MN_STATE_DETECTING) {
                continue;
            }

            if (mn_state == ESP_MN_STATE_DETECTED) { // 已检测到命令词
                esp_mn_results_t *mn_result = multinet->get_results(model_data); // 获取检测词结果
                for (int i = 0; i < mn_result->num; i++) { // 打印获取到的命令词
                    printf("TOP %d, command_id: %d, phrase_id: %d, string:%s prob: %f\n", 
                    i+1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->string, mn_result->prob[i]);
                }
                // 根据命令词 执行相应动作
                switch (mn_result->command_id[0])
                {
                    case 1: // ce shi yun duan
                        printf("Starting Voice Recording for Cloud ASR...\n");
                        // Allocate buffer
                        g_record_buffer = heap_caps_malloc(RECORD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
                        if (g_record_buffer) {
                            g_record_offset = 0;
                            g_is_recording = true;
                            printf("Recording... Speak now!\n");
                        } else {
                            printf("Failed to allocate record buffer!\n");
                        }
                        break;
                    case 2: // fang ru
                        printf("Starting Voice Recording for Cloud ASR (ADD)...\n");
                        g_current_action = LLM_ACTION_ADD;
                        // Allocate buffer
                        g_record_buffer = heap_caps_malloc(RECORD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
                        if (g_record_buffer) {
                            g_record_offset = 0;
                            g_is_recording = true;
                            printf("Recording... Speak now!\n");
                        } else {
                            printf("Failed to allocate record buffer!\n");
                        }
                        break;
                    case 3: // na chu
                        printf("Starting Voice Recording for Cloud ASR (REMOVE)...\n");
                        g_current_action = LLM_ACTION_REMOVE;
                        // Allocate buffer
                        g_record_buffer = heap_caps_malloc(RECORD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
                        if (g_record_buffer) {
                            g_record_offset = 0;
                            g_is_recording = true;
                            printf("Recording... Speak now!\n");
                        } else {
                            printf("Failed to allocate record buffer!\n");
                        }
                        break;
                    case 4: // xian shi ku cun
                        printf("Showing Inventory...\n");
                        inventory_print_all();
                        ui_inventory_refresh();
                        // Reset state to idle
                        afe_handle->enable_wakenet(afe_data);
                        detect_flag = 0;
                        ai_gui_out();
                        printf("\n-----------awaits to be waken up-----------\n");
                        continue;
                    case 5: // qing kong
                        printf("Clearing Inventory...\n");
                        inventory_clear_all();
                        ui_inventory_refresh();
                        // Reset state to idle
                        afe_handle->enable_wakenet(afe_data);
                        detect_flag = 0;
                        ai_gui_out();
                        printf("\n-----------awaits to be waken up-----------\n");
                        continue;

                    case 6: // cai pu tui jian
                        printf("Recommending Recipes...\n");
                        // Run recipe recommendation in background to avoid blocking
                        // the AFE/multinet detection loop and causing rb_out slow.
                        xTaskCreatePinnedToCore(llm_recipe_task, "llm_recipe", 8*1024, NULL, 5, NULL, 1);
                        // Reset state to idle
                        afe_handle->enable_wakenet(afe_data);
                        detect_flag = 0;
                        ai_gui_out();
                        printf("\n-----------awaits to be waken up-----------\n");
                        continue;

                    case 7: // fan hui
                        printf("Return to home screen...\n");
                        // 回到初始界面：当前设计为库存列表
                        ui_inventory_refresh();
                        // Reset state to idle
                        afe_handle->enable_wakenet(afe_data);
                        detect_flag = 0;
                        ai_gui_out();
                        printf("\n-----------awaits to be waken up-----------\n");
                        continue;

                    default:
                        break;
                }
                printf("\n-----------listening-----------\n");
            }

            if (mn_state == ESP_MN_STATE_TIMEOUT) { // 达到最大检测命令词时间
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                // 超时但未匹配到有效命令词的语音内容直接丢弃，不再用于入库
                if (mn_result && mn_result->string[0] != '\0') {
                    printf("timeout, discard string:%s\n", mn_result->string);
                } else {
                    printf("timeout, no valid command detected\n");
                }
                afe_handle->enable_wakenet(afe_data);  // 重新打开唤醒词识别
                detect_flag = 0; // 清除标记
                printf("\n-----------awaits to be waken up-----------\n");
                ai_gui_out(); // AI人退出
                continue;
            }
        }
    }
    if (model_data) {
        multinet->destroy(model_data);
        model_data = NULL;
    }
    printf("detect exit\n");
    vTaskDelete(NULL);
}

void app_sr_init(void)
{
    models = esp_srmodel_init("model"); // 获取模型 名称“model”和分区表中装载模型的名称一致

    afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;  // 先配置afe句柄 随后才可以调用afe接口
    afe_config_t afe_config = AFE_CONFIG_DEFAULT(); // 配置afe

    afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL); // 配置唤醒模型 必须在create_from_config之前配置
    afe_data = afe_handle->create_from_config(&afe_config); // 创建afe_data
    ESP_LOGI(TAG, "wakenet:%s", afe_config.wakenet_model_name); // 打印唤醒名称

    task_flag = 1;
    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void*)afe_data, 5, NULL, 1); 
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&process_audio_task, "process_audio", 8 * 1024, NULL, 5, &s_process_task_handle, 1);
}

