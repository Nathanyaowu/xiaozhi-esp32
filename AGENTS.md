# Agent 约束

## 项目简介

本项目 fork 自 [ZhouhaoJiang/xiaozhi-esp32](https://github.com/ZhouhaoJiang/xiaozhi-esp32)（小智 AI 聊天机器人），针对 **Waveshare ESP32-S3-RLCD-4.2** 开发板进行定制开发。

主要增强功能：
- 股票行情页（新浪财经 API，支持 A 股/港股/美股）
- Web 配置服务（股票自选、刷新间隔、备忘录管理、音量、天气城市、番茄钟）
- 3天天气预报（和风天气 API，自动每24小时拉取）
- 番茄钟（专注/休息模式，长按启动/双击切换，Web 可配时间）
- 开机自定义 BOOT_MESSAGE + 长按 USER 键显示系统信息
- WiFi 图标基于实际连接状态动态更新

## 与上游仓库的关系

```
上游仓库: ZhouhaoJiang/xiaozhi-esp32 (origin)
     ↓ fork
我们的仓库: Nathanyaowu/xiaozhi-esp32
     ↓ 开发分支
feat/rlcd-enhancements （所有定制功能在此分支）
feat/music-playback   （音乐功能 WIP，内存问题待解决）
```

- `main` 分支：保持与上游同步，不做自定义改动
- `feat/rlcd-enhancements` 分支：所有定制开发在此进行
- `feat/music-playback` 分支：音乐搜索与播放功能（因内存限制暂停）
- 同步上游：`git fetch origin && git merge origin/main`（在 main 分支操作后再 rebase 开发分支）

## Git 操作规则

- **所有 push 由用户执行。** Agent 禁止运行 `git push`。
- **每次 `git commit` 前，必须先通知用户** 进行代码 review 和刷机验证。Agent 不得自行提交。

## 工作流程

1. Agent 修改代码，确认编译通过。
2. Agent 通知用户："改动已就绪，请 review 并刷机验证。"
3. 用户 review 代码，烧录固件到设备上验证功能。
4. 用户确认通过 → Agent 提交（或用户手动提交）。
5. 用户执行 push 到远程仓库。

## 技术架构

### 硬件资源约束
- ESP32-S3-WROOM-1-N16R8：16MB Flash + 8MB PSRAM
- 内部 SRAM 可用约 ~85KB（扣除静态分配后）
- 屏幕：400×300 RLCD 单色（黑/白），SPI 40MHz
- DispBuffer 15KB 必须在内部 DMA 内存（PSRAM 需 bounce buffer，音乐播放时碎片化导致分配失败）
- 音频：ES8311 解码 + ES7210 编码 + MAX98357A 功放，I2S 接口

### 内存架构
- `CONFIG_SPIRAM_USE_MALLOC=y`：`malloc()` 可自动 fallback 到 PSRAM
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048`：≤2KB 强制内部 SRAM
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`：内部 SRAM 低于 32KB 时 malloc 转 PSRAM
- `CONFIG_LWIP_MAX_SOCKETS=16`
- `heap_caps_malloc(MALLOC_CAP_8BIT)` 优先内部 SRAM，不受 RESERVE_INTERNAL 控制
- 语音 TTS 复用已有 WebSocket 连接（零额外内存），音乐流播放需新开 TCP+解码器（~75KB 内部 SRAM，当前板子无法承受）

### 屏幕与 UI
- UI 框架：LVGL
- `lv_anim_set_completed_cb`（非 `ready_cb`），`lv_anim_delete`（非 `lv_anim_del`），`lv_coord_t = int32_t`
- `LV_LABEL_LONG_SCROLL` 是持续来回滚动，没有"滚一次停止"内置模式，需手动 `lv_anim_t`
- RLCD SPI 发送失败时跳帧（不 abort），防止 OOM 时设备崩溃

### Web 配置服务
- 文件：`web_config_server.h/cc`（嵌入式 HTML + REST API）
- HTTP Server：`esp_http_server`，WiFi 连接后启动，空闲时零 CPU 开销
- 命名：通用名 `web_config_server`（非 stock_xxx），后续扩展其他设备配置
- 数据校验：JSON 格式、字段类型、code 格式白名单、name 长度限制、market 范围、数量上限
- 页面顺序：系统设置 → 备忘录 → 股票自选

### NVS 存储
- 分区大小 16KB（`0x4000`）
- namespace `stock`, key `list` → JSON 数组 `[{"code":"sh600519","name":"贵州茅台","market":0},...]`
- namespace `stock`, key `interval` → int (5~300, 默认 30)
- namespace `memo`, key `items` → JSON 数组 `[{"t":"15:00","c":"开会","d":"2026-04-28"},...]`
- namespace `weather`, key `city_id` → string (和风天气城市 ID)
- namespace `pomodoro`, key `focus` / `break` → int (1~300 分钟)

### 股票数据获取
- 独立 FreeRTOS task（`StockFetchTask`），与 UI 更新解耦
- 新浪财经 API，A 股/港股名称返回 GBK 编码（项目使用用户配置的 UTF-8 名称，不做转码）
- 科创板 688 开头属于上交所，前缀用 `sh` 不是 `sz`
- `FetchStockData` 是一次 HTTP 请求批量获取所有股票（URL 拼接逗号分隔），要么全部返回要么全部失败，不存在"部分超时"
- HTTP 超时时 `FetchStockData` 返回 0，此时保留上次缓存数据不覆盖（避免显示"停牌"）
- 仅当 `FetchStockData` 返回 > 0（至少有1支有效数据）时才更新缓存
- `GetStockConfigs` 空数组时返回 0（不回退默认股票）
- 美股代码支持下划线/数字/点号，自动转小写

### 天气
- 和风天气 API，每24小时自动拉取一次（3天预报）
- 双击 USER 键不触发天气更新
- Web 端38城市下拉框选择，保存后立即同步拉取
- 显示格式：城市名 + 今/明/后三天天气

### 番茄钟
- 长按 USER 键启动/重置，双击切换专注/休息模式（保持 IDLE 不立即启动）
- 专注结束直接回 IDLE（不自动进入休息模式）
- 无白噪音功能
- Web 可配专注/休息时间（1-300分钟）

### 全局指针桥接
```cpp
// data_update_task.cc
TaskHandle_t g_stock_fetch_task_handle = nullptr;
CustomLcdDisplay* g_display_instance = nullptr;
// web_config_server.cc 通过 extern 访问
```

### 通信协议
- `Protocol::SendText()` 可发送 JSON 文本到服务端
- `Protocol::SendStartListening()` 发送 listen/start 消息
- 支持 WebSocket 和 MQTT+UDP 两种通信协议
- 协议文档：`docs/websocket.md`, `docs/mqtt-udp.md`

### 构建
```bash
cd /home/nathan/rlcd/own/xiaozhi-esp32
source /home/nathan/esp/esp-idf/export.sh > /dev/null 2>&1
idf.py build 2>&1 | tail -20
```

### LSP 已知误报
- `Unknown argument '-mlongcalls'`、`std::string (aka 'int')`、`No matching constructor for 'DisplayLockGuard'` 等错误都是 clangd 无法正确解析 ESP-IDF xtensa 交叉编译环境导致的误报，实际 `idf.py build` 编译正常

## 关键文件

| 文件路径 | 说明 |
|---|---|
| `main/boards/waveshare-s3-rlcd-4.2/web_config_server.h/cc` | Web 配置服务（HTML + REST API） |
| `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h/cc` | 自定义 LCD 显示类 |
| `main/boards/waveshare-s3-rlcd-4.2/rlcd_driver.cc` | RLCD 硬件驱动（SPI + DMA） |
| `main/boards/waveshare-s3-rlcd-4.2/data_update_task.cc` | DataUpdateTask + StockFetchTask + 备忘闹钟 |
| `main/boards/waveshare-s3-rlcd-4.2/stock_data.h/cc` | 股票配置读取（NVS） |
| `main/boards/waveshare-s3-rlcd-4.2/stock_ui.cc` | 股票页 UI（滚动动画、停牌提示） |
| `main/boards/waveshare-s3-rlcd-4.2/music_ui.cc` | 音乐页 UI |
| `main/boards/waveshare-s3-rlcd-4.2/pomodoro_ui.cc` | 番茄钟 UI |
| `main/boards/waveshare-s3-rlcd-4.2/weather_ui.cc` | 天气页（含 IP 显示） |
| `main/boards/waveshare-s3-rlcd-4.2/managers/pomodoro_manager.h/cc` | 番茄钟管理 |
| `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.h/cc` | 和风天气 API |
| `main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc` | MCP 工具定义 + 按钮逻辑 |
| `main/application.cc` | 上游核心（含 PlayMusicFromUrl） |
| `sdkconfig.defaults.esp32s3` | ESP32-S3 共用配置（PSRAM/WiFi/LWIP） |

## 音乐功能现状（feat/music-playback 分支）

### 已实现
- Web 端搜索（酷我旧版 API `search.kuwo.cn/r.s`，无需认证）
- 获取播放 URL（`antiserver.kuwo.cn/anti.s?type=convert_url`，返回 HTTP 明文链接）
- 调用 `Application::PlayMusicFromUrl()` 触发播放

### 未解决问题
音乐流播放需新建完整 HTTP 管道（TCP 连接 + MP3 解码器 + 任务栈），消耗 ~75KB 内部 SRAM。板子总共仅 ~85KB 可用，导致播放时系统近乎 OOM（minimal sram: 51 bytes）。语音 TTS 不存在此问题，因为它复用已有 WebSocket 连接和 OPUS 解码器，零额外内存开销。

### 可能的后续方向
- 服务端转码：让服务器下载 MP3 → 转 OPUS → 通过已有 WebSocket 推送（复用 TTS 通道）
- 缩减 LWIP buffer + task 栈走 PSRAM + 减少解码器内存
- 接受硬件限制，仅保留搜索/展示功能，不做设备端流播放

## 编码注意事项

- `std::string` 中不要使用中文引号 `"` `"`，会被编译器当作字符串终止符
- `WifiStation` 没有 `GetInstance()`，正确用法：`WifiManager::GetInstance()`，需 `#include "wifi_manager.h"`
- POST 数据必须严格校验（防崩溃）
- 备忘录时间校验只检格式不检范围（避免旧数据阻塞新增）
- `ESP_ERROR_CHECK` 仅用于不可恢复的初始化错误，运行时可恢复错误用返回值判断
- alibaba_puhui_16 字体字符集有限，需要完整汉字用 `font_puhui_16_4`（7415 常用汉字）
