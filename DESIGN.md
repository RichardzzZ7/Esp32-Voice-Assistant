# 设计文档（设备端概览）

## 目标
实现一个基于 ESP32 的智能食材管理设备，支持语音添加物品、本地持久化、保质期计算、提醒/通知、食谱推荐和云同步能力。

## 总体架构
- 设备端（Edge）
  - 语音采集与唤醒：使用现有 AFE/多网络模型（原示例保留）
  - 本地槽位抽取：`parser.c` 做初步意图解析，减少不必要的网络调用
  - 数据存储：SPIFFS 保存 `inventory.json`、`sync_queue.json`、`recipe_last.json` 等
  - UI：`ui_inventory.c` 使用 LVGL 显示按剩余保质期排序的列表，支持分页与高亮
  - TTS：`tts.c` 支持本地占位音频与云端（讯飞）调用并缓存
  - 通知：`notify.c` 周期性扫描并使用 TTS 提醒用户
  - 食谱推荐：`recipe.c` 调用云端 LLM（讯飞星火）获取建议，失败则本地回退
  - 同步：`sync.c` 管理离线队列并向后端同步事件

- 云端（建议/可选）
  - ASR：百度 ASR（设备端也可上传音频）
  - LLM：讯飞星火（生成食谱、对话管理）
  - TTS：讯飞（生成合成音频供设备播放/推送）
  - 后端服务：API 网关、用户管理、通知推送、持久化数据库（Postgres / MySQL）

## 数据模型（设备端 `inventory_item_t`）
- item_id
- name
- category
- quantity
- unit
- location
- added_time (UTC epoch)
- default_shelf_life_days
- calculated_expiry_date
- remaining_days
- notes / photo_url
- last_notified_remaining_days

## 关键算法与策略
- 保质期计算：根据 `default_shelf_life_days` + `added_time` 计算 `calculated_expiry_date`；如果用户提供显式生产/失效日期，优先使用用户输入。
- 提醒阈值：默认 3 天，可配置；当 `remaining_days <= threshold` 时触发提醒并标记已通知以避免重复。
- 优先级（后续）：可用 f(remaining_days, quantity, 用户标签) 计算排序权重。
- 食谱推荐：当发现 >=2 个临近过期食材时触发 recipe 请求；请求由 `recipe.c` 发起并用 TTS 播报摘要。

## 接口
- 本地文件：
  - `/spiffs/inventory.json`：库存持久化
  - `/spiffs/sync_queue.json`：离线同步队列
  - `/spiffs/recipe_last.json`：最后一次推荐结果

- 同步 API：`SYNC_API_URL`（设备向后端发送 `add_item` / `notified` 事件）

## 安全与隐私
- 设备可在“隐私模式”下禁用云调用，完全本地工作（本地槽位解析 + 本地 TTS 占位）。
- 当启用云服务时，请妥善保管 APIKey/APISecret，并优先使用 HTTPS + 证书钉扎以防中间人攻击。

## 可扩展点（后续迭代）
- UI：添加详情页、图片展示、手动操作（删除/修改/标记已用）
- 通知：支持多通道（TTS、屏幕、手机推送）和多档阈值设置
- 同步：批处理、幂等性、冲突解决策略
- LLM：在云端构建食谱服务并缓存常见食谱以减少延迟
