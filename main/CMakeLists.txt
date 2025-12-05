idf_component_register(SRCS "cloud_asr.c" "cloud_llm.c" "img_bilibili120.c" "app_sr.c" "esp32_s3_szp.c" "main.c" "app_ui.c" "inventory.c" "storage.c" "parser.c" "ui_inventory.c" "tts.c" "notify.c" "recipe.c" "sync.c" "wifi.c"
                    INCLUDE_DIRS ".")

# Prevent LVGL macros from placing data into IRAM for this build
add_compile_definitions(LV_ATTRIBUTE_FAST_MEM=)

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
