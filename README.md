# 智能食材管理（设备端）

这是基于现有 ESP32 语音识别例程扩展的智能食材管理设备端实现。功能包括：

- 语音添加物品（名称、数量、位置、可选保质期/日期）
- 本地库存保存（SPIFFS，JSON）
- 自动保质期计算与剩余天数
- 屏幕 UI：按剩余天数排序、分页与高亮临近过期项（LVGL）
- 提醒与通知：本地 TTS 播报临近过期物品
- 食谱推荐骨架：当 >=2 个物品临近过期时调用云端（讯飞星火）或本地回退生成简单建议
- 本地/云 TTS 切换：本地占位合成或调用讯飞在线 TTS（HTTPS）并缓存
- 离线队列与同步：本地事件队列会持久化并尝试同步到云端后端

项目结构（主要文件）
- `main/` - 设备端源码
  - `main.c` - 启动流程
  - `app_sr.c` - 语音识别入口（复用原示例）
  - `inventory.*` - 库存数据模型与持久化
  - `parser.*` - 语音文本槽位抽取
  - `ui_inventory.*` - LVGL 列表界面（分页/高亮）
  - `tts.*` - TTS 层（本地占位 + 讯飞云端调用）
  - `notify.*` - 提醒/通知任务
  - `recipe.*` - 食谱推荐（云端调用骨架 + 本地回退）
  - `sync.*` - 离线队列与云同步

配置（需要在代码中设置/确认）
- `main/tts_config.h`：讯飞 TTS 的 `APIKey`、`APISecret`、`APPID`，和 `XFYUN_ROOT_CA`（可选证书钉扎 PEM）
- `main/recipe_config.h`：讯飞星火 Spark 接口地址与凭证（已填写你提供的值）
- `main/sync_config.h`：后端同步接口 `SYNC_API_URL`，请替换为你的后端地址

编译与烧录（Windows PowerShell）
1. 打开 ESP-IDF PowerShell 环境（请先安装并配置 ESP-IDF）：

```powershell
cd e:\hw\esp\xiaobin
```

2. 检查并配置 menuconfig（可选）：

```powershell
idf.py menuconfig
```

确保启用了需要的组件：`mbedtls`, `esp_http_client`, `SPIFFS` 等。

3. 构建固件：

```powershell
idf.py build
```

4. 烧录并打开日志（替换端口为你的设备端口，例如 `COM3`）：

```powershell
idf.py -p COM3 flash monitor
```

运行提示与测试
- 启动后设备会挂载 SPIFFS，并在串口日志打印初始化信息。
- 使用语音唤醒并说“放入牛奶 2 瓶 冷藏 保质期7天”之类句子，系统会解析并保存条目。
- 当有物品剩余天数 <= 阈值（默认 3 天）时，设备会用 TTS 语音播报并在 UI 中高亮显示该条目。
- 当检测到 >=2 个临近过期物品，会向讯飞星火发送 recipe 请求（若网络可用），并将建议保存到 `/spiffs/recipe_last.json`。

调试与注意事项
- 如果使用云 TTS/LLM，请确保设备可访问 Internet 并且 `tts_config.h` / `recipe_config.h` 中的凭证正确。
- 若启用了证书钉扎，请把 PEM 放入 `XFYUN_ROOT_CA` 常量（`tts_config.h`）；否则 ESP-IDF 将使用系统 CA 存储。
- `sync` 模块的 `SYNC_API_URL` 为占位，请替换为你的后端实现地址以启用同步功能。

下一步
- 可继续完善：UI 详情页、删除/修改条目、TTS 音频缓存管理、推送到手机的云端通知。


