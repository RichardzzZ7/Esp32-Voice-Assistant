# 智能食材管理（ESP32-S3 语音 + 云端 LLM）

本项目基于乐鑫 ESP32-S3 中文语音识别示例扩展，实现了“语音管理冰箱库存 + 云端大模型辅助”的一体化设备端方案。

## 主要功能

- 语音命令控制（离线唤醒 + 命令词）
  - 唤醒词：沿用乐鑫示例的中文唤醒词（WakeNet），无需联网即可唤醒。
  - 主要命令词（MultiNet）：
    - “测试云端”/`ce shi yun duan`：测试云 ASR + 云 LLM 流程。
    - “放入”/`fang ru`：录音并上传到云 ASR+LLM，解析为“新增库存”。
    - “拿出”/`na chu`：录音并上传到云 ASR+LLM，解析为“减库存/取出物品”。
    - “显示库存”/`xian shi ku cun`：打开库存列表 UI。
    - “清空”/`qing kong`：清空所有库存。
    - “菜谱推荐”/`cai pu tui jian`：基于当前库存向云端 LLM 请求菜谱推荐。

- 云 ASR + 云 LLM 解析库存
  - 音频通过云 ASR 转为文本后，由百度千帆 ERNIE-Speed-128k 模型解析成结构化 JSON：
    - `name`（名称）
    - `category`（类别）
    - `quantity`（数量，int）
    - `unit`（单位，例如“盒”“瓶”“个”）
    - `expiry_date`（保质期日期，YYYY-MM-DD；若用户说“日期未知”，则填“未知”）
    - `shelf_life_days`（建议保质期天数，模型根据食材类型智能补全，>0）
    - `location`（推荐存放区域：冷藏区 / 冷冻室 / 常温储藏区 等）
    - `notes`（备注）
  - 当 `expiry_date` 为“未知”等非日期字符串时，会自动退回使用 `shelf_life_days` 推算过期日期。

- 库存管理与保质期计算
  - 库存数据结构：见 `inventory.h` 中的 `inventory_item_t`，包含名称、类别、数量、单位、位置、添加时间、保质期、剩余天数等。
  - 本地持久化：使用 SPIFFS，将库存保存为 `/spiffs/inventory.json`（JSON 数组）。
  - 保质期与剩余天数逻辑：
    - 优先使用大模型给出的 `shelf_life_days`，上限为 365 天；
    - 若未给出，则按类别映射默认天数（牛奶≈7 天，肉/鸡/鱼≈3 天，蔬果≈5 天，冷冻≈30 天等）；
    - 若给出有效 `expiry_date`，则以此反推或直接计算剩余天数；
    - 用 `difftime` + 向上取整的方式计算剩余天数：
      - 剩余秒数 >0 时按天数向上取整（2.1 天显示为 3 天）；
      - 结果 <=0 统一视为 0 天，表示“今天到期或已过期”。

- 屏幕 UI（LVGL 中文界面）
  - 使用 `font_alipuhui20` 中文字体显示列表（见 `main/ui_inventory.c`）。
  - 列表内容按“剩余天数升序”排序（即最先过期的在最上面）。
  - 每条记录的显示格式为：
    - `名称  数量单位  剩余天数  存放位置`
    - 例如：`鸡蛋  1盒  5天  冷藏区`
  - 剩余天数小于等于阈值（默认 3 天）时，该条目在 UI 中以红色高亮。
  - 当前 UI 只保留一页列表展示，不再显示“上一页/下一页”按钮和页码。

- 通知与提醒（轻量日志 + UI 刷新）
  - `notify` 任务会按固定间隔（默认 12 小时）扫描库存：
    - 对剩余天数 <= 阈值、且上次提醒天数不同的物品，在串口打印日志；
    - 更新 `last_notified_remaining_days` 并刷新 UI 列表。
  - 为避免栈溢出和过多云请求，目前 **不再在通知任务中播放 TTS 语音**，仅做日志和 UI 提示。

- 云端菜谱推荐
  - 通过语音命令“菜谱推荐”触发，由 `cloud_llm_recommend_recipes` 汇总当前库存（名称 + 数量 + 单位），构造提示词并调用百度千帆 ERNIE-Speed-128k：
    - 返回 1~2 个详细菜谱建议；
    - 在串口终端中打印完整菜谱文本，便于在开发阶段调试和查看。
  - `recipe.c` 中预留了基于库存请求菜谱推荐的骨架逻辑，可对接其它接口或在需要时配合 TTS 播报摘要。

- TTS 层（可选）
  - `tts.c` 提供一个统一的 `tts_speak_text` 接口：
    - 优先尝试云 TTS（如科大讯飞 REST API），并将合成的 WAV 缓存在 `/spiffs/tts_cache`；
    - 若云 TTS 不可用，则生成简单 beep 提示音作为回退；
    - 通过 `audio_player` 播放。
  - 目前默认不在“临期提醒”任务中使用 TTS，仅在需要时（例如菜谱摘要）调用，避免占用过多栈和网络资源。

- 离线事件队列与云同步（可选）
  - `sync.*` 模块将新增/删除/提醒等操作封装为事件写入本地队列；
  - 可选对接后台 HTTP 接口 `SYNC_API_URL`，在网络可用时批量上报，构建云端“冰箱资产”视图。

## 代码结构概览

- `main/` - 设备端源码
  - `main.c`：系统启动入口，初始化音频、UI、SPIFFS、库存等。
  - `app_sr.c`：语音前端（AFE）与命令词处理逻辑，负责唤醒和命令分发。
  - `cloud_llm.c` / `cloud_llm.h`：
    - `cloud_llm_parse_inventory`：调用百度千帆解析库存文本为结构化 JSON，并入库；
    - `cloud_llm_recommend_recipes`：基于库存调用千帆生成菜谱建议。
  - `inventory.c` / `inventory.h`：
    - 链表管理库存数据、SPIFFS 持久化、保质期/剩余天数计算、排序等。
  - `parser.*`：本地文本解析辅助（部分路径仍保留，可作为云解析失败时的回退）。
  - `ui_inventory.c` / `ui_inventory.h`：库存列表 UI（LVGL）。
  - `tts.c` / `tts.h`：TTS 抽象层（本地 beep + 云 TTS 调用）。
  - `notify.c` / `notify.h`：库存扫描与临期提醒任务（日志 + UI）。
  - `recipe.c` / `recipe.h`：菜谱推荐逻辑骨架（可使用千帆或其它 LLM）。
  - `sync.c` / `sync.h`：离线事件队列与云同步。
  - 其他：音频驱动、LCD/LVGL 适配、SPIFFS 初始化等。

## 配置说明

- 百度千帆大模型（ERNIE-Speed-128k）
  - 配置位置：`main/cloud_llm.c`
  - 通过 `QIANFAN_BEARER_TOKEN` 宏设置 IAM Bearer Token：
    - 请将示例中的占位 Token 替换成你自己在百度千帆控制台申请的 Token；
    - 接口地址：`https://qianfan.baidubce.com/v2/chat/completions`。

- 云 TTS（可选：科大讯飞）
  - 配置位置：`main/tts_config.h`
  - 需要配置：`APIKey`、`APISecret`、`APPID`，以及可选的 `XFYUN_ROOT_CA` PEM 证书（若启用证书钉扎）。

- 食谱 LLM（可选扩展）
  - 配置位置：`main/recipe_config.h`
  - 可使用已有的讯飞星火/其它兼容接口，也可以直接复用 `cloud_llm.c` 里的千帆接口逻辑。

- 云同步接口（可选）
  - 配置位置：`main/sync_config.h`
  - 将 `SYNC_API_URL` 替换为你自己的后端服务地址即可启用事件上报。

## 编译与烧录（Windows PowerShell）

1. 打开 ESP-IDF PowerShell 环境（以 ESP-IDF v5.1.6 为例）：

```powershell
cd e:\hw\esp\xiaobin
```

2. 根据需要运行 `menuconfig`（可选）：

```powershell
idf.py menuconfig
```

确保启用了以下组件/选项：

- `SPIFFS` 文件系统
- `ESP HTTP client`（`esp_http_client`）
- `mbedtls` / `ESP-TLS`（用于 HTTPS 和云端通信）
- LVGL 显示驱动与字体（已在 CMakeLists 中启用 `LV_FONT_FMT_TXT_LARGE=1`）

3. 构建固件：

```powershell
idf.py build
```

4. 烧录并打开串口监视（将 `COM4` 换成你实际的端口）：

```powershell
idf.py -p COM4 flash monitor
```

## 使用示例

- 启动与初始化
  - 上电后，设备会初始化音频、LVGL、SPIFFS，并从 `/spiffs/inventory.json` 载入历史库存。
  - 串口会打印诸如 `inventory: Added item ...`、`cloud_llm: HTTP Response ...` 等日志。

- 语音添加食材
  - 唤醒后说：“放入一盒鸡蛋，放冷藏，保质期大概一周。”
  - 设备：
    - 录音 -> 云 ASR -> 文本 -> 千帆 LLM 解析；
    - 生成 JSON：`{"name":"鸡蛋","quantity":1,"unit":"盒","location":"冷藏区","shelf_life_days":5,...}`；
    - 写入库存并在 UI 中出现类似：`鸡蛋  1盒  5天  冷藏区`。

- 语音取出食材
  - 唤醒后说：“拿出两瓶牛奶。”
  - 设备会解析为减库存操作，更新对应条目数量。

- 查看库存
  - 唤醒后说：“显示库存。”
  - 屏幕显示按剩余天数排序的列表，即将过期的物品在最上面，并用红色高亮。

- 菜谱推荐
  - 唤醒后说：“菜谱推荐。”
  - 设备会根据当前所有库存生成描述，调用千帆 LLM 获取 1~2 个菜谱；
  - 完整菜谱文本会打印在串口终端，便于查看与调试。

## 调试与注意事项

- 时间与时区
  - 保质期计算依赖 RTC 时间；请确保设备时间尽量准确（可通过 NTP 或手动设置）。
  - 剩余天数使用向上取整，因此“2.1 天”会显示为“3 天”。

- 云端调用与网络
  - 云 ASR 与千帆 LLM 均依赖外网连接，请确保 Wi-Fi 配置正确；
  - 若网络异常，云解析失败时不会添加条目，可通过日志定位问题。

- 栈与任务
  - 所有耗时的 HTTP/LLM 请求（ASR、千帆 LLM、菜谱推荐）均在独立任务中执行，避免阻塞语音前端（AFE）；
  - 通知任务目前仅做轻量操作（日志 + UI），不启用云 TTS，以避免栈溢出问题。

## 后续可扩展方向

- UI 详情页：点击某条库存进入详情，展示完整保质期、备注、历史通知记录等。
- 条目编辑：在设备端支持修改数量、单位、位置或备注。
- 更丰富的菜谱交互：将菜谱结果保存到本地并通过 UI 分页浏览，或结合 TTS 分步播报烹饪步骤。
- 手机/小程序联动：利用 `sync` 模块的事件流，将库存和临期提醒推送到手机 App 或小程序。


