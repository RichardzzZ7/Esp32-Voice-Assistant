// tts.c - simple local TTS cache + placeholder synth (sine beep) when cloud not available
#include "tts.h"
#include "tts_config.h"
#include "esp_log.h"
#include "audio_player.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>

#if CLOUD_TTS_ENABLED
#include "esp_http_client.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"
#include "esp_system.h"
#include <time.h>
#endif

static const char *TAG = "tts";
static const char *CACHE_DIR = "/spiffs/tts_cache";

static unsigned long djb2_hash(const char *str)
{
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c;
    return hash;
}

void tts_init(void)
{
    // create cache dir if not exist
    struct stat st = {0};
    if (stat(CACHE_DIR, &st) == -1) {
        mkdir(CACHE_DIR, 0777);
    }
}

// Create a short WAV file with a beep tone (mono 16-bit 16kHz)
static int create_beep_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int sample_rate = 16000;
    int duration_ms = 600;
    int samples = sample_rate * duration_ms / 1000;
    int byte_rate = sample_rate * 2; // 16-bit mono
    int data_size = samples * 2;

    // RIFF header
    fwrite("RIFF",1,4,f);
    int32_t chunk_size = 36 + data_size;
    fwrite(&chunk_size,4,1,f);
    fwrite("WAVEfmt ",1,8,f);
    int32_t subchunk1 = 16;
    fwrite(&subchunk1,4,1,f);
    int16_t audio_format = 1;
    fwrite(&audio_format,2,1,f);
    int16_t num_channels = 1;
    fwrite(&num_channels,2,1,f);
    fwrite(&sample_rate,4,1,f);
    fwrite(&byte_rate,4,1,f);
    int16_t block_align = 2;
    fwrite(&block_align,2,1,f);
    int16_t bits_per_sample = 16;
    fwrite(&bits_per_sample,2,1,f);
    fwrite("data",1,4,f);
    fwrite(&data_size,4,1,f);

    // generate sine tone
    double freq = 700.0;
    for (int i = 0; i < samples; ++i) {
        double t = (double)i / sample_rate;
        double env = 1.0;
        double val = sin(2.0 * M_PI * freq * t) * 0.5 * env;
        int16_t s = (int16_t)(val * 32767);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}

int tts_speak_text(const char *text, bool cache)
{
    if (!text) return -1;
    // compute cache filename
    unsigned long h = djb2_hash(text);
    char path[256];
    snprintf(path, sizeof(path), "%s/%08lx.wav", CACHE_DIR, h);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
#if CLOUD_TTS_ENABLED
        // Try cloud TTS (讯飞 REST API)
        ESP_LOGI(TAG, "Attempting cloud TTS for text");
        const char *param_json = "{\"aue\":\"raw\",\"auf\":\"audio/L16;rate=16000\",\"voice_name\":\"xiaoyan\",\"speed\":\"50\",\"volume\":\"50\",\"pitch\":\"50\",\"engine_type\":\"intp65\"}";
        size_t param_b64_len = 0;
        // get required buffer length
        if (mbedtls_base64_encode(NULL, 0, &param_b64_len, (const unsigned char*)param_json, strlen(param_json)) == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
            unsigned char *param_b64 = malloc(param_b64_len + 1);
            if (param_b64) {
                if (mbedtls_base64_encode(param_b64, param_b64_len, &param_b64_len, (const unsigned char*)param_json, strlen(param_json)) == 0) {
                    param_b64[param_b64_len] = '\0';
                    char curtime[32];
                    snprintf(curtime, sizeof(curtime), "%lld", (long long)time(NULL));

                    // checksum = md5(APISecret + curtime + param_b64)  (use APISecret per provided credentials)
                    char checksum_input[1024];
                    snprintf(checksum_input, sizeof(checksum_input), "%s%s%s", XFYUN_API_SECRET, curtime, (char*)param_b64);
                    unsigned char md5sum[16];
                    // Use mbedtls_md5_ret when available, otherwise fall back to mbedtls_md5
        #if defined(MBEDTLS_MD5_RET)
                    mbedtls_md5_ret((const unsigned char*)checksum_input, strlen(checksum_input), md5sum);
        #else
                    mbedtls_md5((const unsigned char*)checksum_input, strlen(checksum_input), md5sum);
        #endif
                    char checksum_hex[33];
                    for (int i = 0; i < 16; ++i) sprintf(checksum_hex + i*2, "%02x", md5sum[i]);

                    esp_http_client_config_t config = {
                        .url = XFYUN_TTS_URL,
                        .method = HTTP_METHOD_POST,
                        .timeout_ms = 10000,
                    };
                    // If a root CA PEM was provided in config, use it for cert pinning
                    if (sizeof(XFYUN_ROOT_CA) > 1 && XFYUN_ROOT_CA[0] != '\0') {
                        config.cert_pem = XFYUN_ROOT_CA;
                    }
                    esp_http_client_handle_t client = esp_http_client_init(&config);
                    if (client) {
                        esp_http_client_set_header(client, "X-Appid", XFYUN_APPID);
                        esp_http_client_set_header(client, "X-CurTime", curtime);
                        esp_http_client_set_header(client, "X-Param", (char*)param_b64);
                        esp_http_client_set_header(client, "X-CheckSum", checksum_hex);
                        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded; charset=utf-8");

                        // prepare body: urlencoded text
                        char body[1024];
                        snprintf(body, sizeof(body), "text=%s", text);
                        esp_http_client_set_post_field(client, body, strlen(body));

                        esp_err_t err = esp_http_client_perform(client);
                        if (err == ESP_OK) {
                            int status = esp_http_client_get_status_code(client);
                            if (status == 200) {
                                FILE *out = fopen(path, "wb");
                                if (out) {
                                    int read_len = 0;
                                    char buffer[1024];
                                    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
                                        fwrite(buffer, 1, read_len, out);
                                    }
                                    fclose(out);
                                }
                            } else {
                                ESP_LOGW(TAG, "Cloud TTS returned HTTP %d", status);
                            }
                        } else {
                            ESP_LOGW(TAG, "Cloud TTS request failed: %d", err);
                        }
                        esp_http_client_cleanup(client);
                    }
                }
                free(param_b64);
            }
        }
#endif
        // if cloud synthesis didn't produce file, fallback to local beep
        // not cached: create placeholder beep wav and save
        if (create_beep_wav(path) != 0) {
            ESP_LOGW(TAG, "failed create beep wav");
            return -1;
        }
        if (cache) {
            // already created at path
        }
        fp = fopen(path, "rb");
        if (!fp) return -1;
    }

    // Play via audio_player
    ESP_LOGI(TAG, "TTS play: %s", text);
    audio_player_play(fp);
    // audio_player likely takes ownership of FILE*, do not fclose here
    return 0;
}
