# Waveshare S3 RLCD-4.2 代码运行与回调流程图

> 配合源码阅读使用。所有行号对应当前仓库 `main/boards/waveshare-s3-rlcd-4.2/` 下的实际位置。
> 本文档使用 Mermaid 8.x 兼容语法（GitHub / VSCode / Typora 均可渲染）。

---

## 0. 阅读路线（先看这个）

按以下顺序读源码，理解曲线最平滑：

| # | 文件 | 看什么 |
|---|---|---|
| 1 | `main/main.cc` | 30 行的入口，看 `app_main()` |
| 2 | `waveshare-s3-rlcd-4.2.cc` 构造函数（行 733） | 板子是怎么"装配"出来的 |
| 3 | `InitializeButtons()`（行 93） | 最简单的回调示例（lambda 捕获 `this`） |
| 4 | `InitializeTools()`（行 257） | **重点**。看 `AddTool()` 的 lambda 签名 |
| 5 | `managers/pomodoro_manager.cc` | 一个完整状态机 + FreeRTOS task 写法 |
| 6 | `data_update_task.cc` | 独立 task + `DisplayLockGuard` 跨线程更新 LVGL |
| 7 | `main/application.cc` | 主循环，和板级代码完全解耦 |

**架构精髓**：板子只负责**注册回调**，不主动驱动主循环。Application 跑事件循环，事件（按键 / MCP 工具调用 / 音频帧）触发时自然调到你在板子构造期挂上去的 lambda。
所以加新功能 = 加 manager + 在 `InitializeTools()` 多注册几个 `AddTool()`。

---

## 1. 整体启动流程（开机 → 进入主循环）

```mermaid
graph TD
    A[ESP-IDF 上电] --> B[app_main main/main.cc:15]
    B --> B1[nvs_flash_init]
    B --> C[Application::GetInstance 单例]
    C --> D[app.Initialize application.cc:77]
    D --> D1[Board::GetInstance 触发 DECLARE_BOARD]
    D1 --> E[CustomBoard 构造函数 行733]

    E --> E1[InitializeI2c I2C_NUM_0]
    E --> E2[InitializeSensors SHTC3 + PCF85063]
    E --> E3[InitializeSdcard 挂 /sdcard]
    E --> E4[InitializeButtons 注册按键回调]
    E --> E5[InitializeTools 注册 11 个 MCP 工具]
    E --> E6[InitializeLcdDisplay SPI + LVGL + DataUpdateTask]

    D --> D2[InitializeProtocol WebSocket / MQTT]
    D --> D3[McpServer 启动]
    D --> D4[OTA 检查]

    D --> F[app.Run 主事件循环 永不返回]
    F --> F1[音频管线 录音 / 播放]
    F --> F2[协议收发]
    F --> F3[设备状态机]
```

**对照看：**
- `main/main.cc:15` `extern "C" void app_main()`
- `waveshare-s3-rlcd-4.2.cc:733` `CustomBoard::CustomBoard()` —— 6 个 `Initialize*()` 顺序调用
- `waveshare-s3-rlcd-4.2.cc:771` `DECLARE_BOARD(CustomBoard)` —— 把这个类注册成全局 Board

---

## 2. 按键回调链（USER & BOOT）

```mermaid
graph LR
    K1[GPIO0 BOOT] --> Q[Button 事件分发 IRAM ISR + 队列]
    K2[GPIO18 USER] --> Q

    Q -->|click| C1[boot OnClick 行95]
    Q -->|click| C2[user OnClick 行106]
    Q -->|double| C3[user OnDoubleClick 行114]
    Q -->|long| C4[user OnLongPress 行120]

    C1 --> R1{设备状态}
    R1 -->|Starting| R1a[EnterWifiConfigMode]
    R1 -->|其他| R1b[ToggleChatState 开始或打断对话]

    C2 --> R2[CycleDisplayMode 切页 weather music pomodoro]
    C3 --> R3[RefreshAllData NTP 同步加 UI 提示]
    C4 --> R4[ShowSystemInfo 鱼咬尾循环动画 行128]
```

**对照看：** `waveshare-s3-rlcd-4.2.cc:93` `InitializeButtons()` —— 4 个 lambda 全在这里。

---

## 3. AI 调用 MCP 工具的完整链（**最核心的交互**）

以"开始番茄钟 25 分钟"为例，从用户开口到屏幕切页的全过程：

```mermaid
sequenceDiagram
    participant U as 用户语音
    participant Mic as ES7210 麦克风
    participant App as Application
    participant Proto as WebSocket
    participant Cloud as 云端 LLM
    participant MCP as McpServer 设备侧
    participant CB as CustomBoard lambda
    participant PM as PomodoroManager
    participant UI as CustomLcdDisplay
    participant SD as SdcardManager

    U->>Mic: 开始番茄钟
    Mic->>App: PCM 流
    App->>Proto: 上行音频
    Proto->>Cloud: ASR + LLM
    Cloud-->>Proto: tools/call self.pomodoro.start
    Proto->>MCP: DispatchToolCall
    MCP->>CB: 触发已注册 lambda

    CB->>CB: 解析参数 + 边界裁剪
    CB->>PM: pomo.start(25, true)
    PM->>PM: 状态 IDLE 转 FOCUSING
    PM->>SD: 读 /sdcard/white-noise/*.mp3
    PM->>App: 通过音频管线播放
    CB->>UI: SwitchToPomodoroPage
    CB-->>MCP: ReturnValue 字符串
    MCP-->>Cloud: 工具结果
    Cloud-->>Proto: TTS 番茄钟已启动
    Proto->>App: 下行音频
    App->>U: 喇叭播报
```

**关键机制：**

```cpp
// AddTool 的签名（伪代码）
mcp_server.AddTool(
    name,                                  // "self.pomodoro.start"
    description,                           // 给 AI 看的中英文用法说明
    PropertyList({...}),                   // 参数 schema (类型/范围/默认值)
    [this](const PropertyList& p) -> ReturnValue { ... }  // 回调
);
```

云端通过 MCP 协议下发 `tools/call`，`McpServer` 根据 name 查表直接调那个 lambda；
lambda 里再操作板载 manager（PomodoroManager / WeatherManager / Memo NVS / Display）。

**11 个工具一览：**

| Tool name | 行号 | 触发的回调动作 |
|---|---|---|
| `self.system.info` | 261 | 读 heap/PSRAM/CPU/电池/WiFi → 返回字符串 |
| `self.weather.update` | 313 | `WeatherManager::set()` → 刷新天气区 |
| `self.disp.network` | 343 | `EnterWifiConfigMode()` |
| `self.disp.switch` | 350 | `display_->SwitchToXxxPage()` |
| `self.pomodoro.start` | 394 | `PomodoroManager::start()` + 切番茄钟页 |
| `self.pomodoro.stop` | 434 | `PomodoroManager::stop()` + 切回天气页 |
| `self.pomodoro.status` | 452 | 返回剩余时间字符串 |
| `self.pomodoro.pause` | 472 | 暂停 / 恢复 |
| `self.memo.add` | 492 | NVS 写入 + UI 刷新（带 HH:MM 校验） |
| `self.memo.list` | 553 | NVS 读取 → 返回带序号列表 |
| `self.memo.done` | 587 | 按 index 删除 |
| `self.memo.clear` | 636 | 清空 |

---

## 4. UI 后台刷新任务（FreeRTOS Task）

```mermaid
graph TD
    Init[xTaskCreate DataUpdateTask data_update_task.cc:51 stack=8192 prio=2] --> T0[task 入口 vTaskDelay 3s 等系统稳定]
    T0 --> Loop[while 1]
    Loop --> S1[读 SHTC3 温湿度]
    Loop --> S2[读 PCF85063 或 NTP 时间]
    Loop --> S3[读电池 ADC]
    Loop --> S4[查 WiFi 状态]
    Loop --> S5[扫描备忘 HHMM 触发]

    S1 --> Diff{超过阈值 温度0.2 湿度1}
    Diff -->|是| Push[DisplayLockGuard 加锁 lv_label_set_text]
    Diff -->|否| Skip[跳过]

    Push --> Sleep
    S5 --> Sleep
    Skip --> Sleep[vTaskDelay delay_ms 活跃 1s 省电 5s]
    Sleep --> Active{5 分钟内有 NotifyUserActivity}
    Active -->|是| FastDelay[delay 1000ms]
    Active -->|否| SlowDelay[delay 5000ms]
    FastDelay --> Loop
    SlowDelay --> Loop

    Sys[ShowSystemInfo 按下] -.SetShowingSystemInfo true.-> Pause[task 暂停 UI 写入 避免锁竞争和 watchdog]
```

**为什么要加 `DisplayLockGuard`？**
LVGL 不是线程安全的——主循环里有 LVGL tick task，`DataUpdateTask` 是另一个 FreeRTOS task。
两边都要写 widget 时必须先拿同一把互斥锁，否则会造成渲染崩溃 / heap 损坏。

**为什么 `ShowSystemInfo` 要 `SetShowingSystemInfo(true)`？**
长按时主循环正在跑"鱼咬尾"动画（每 30ms 设一次 Y 坐标）。如果此时 `DataUpdateTask` 还在抢锁写文字，
锁等待会触发 task watchdog（默认 5s）。所以临时停掉 task 对 chat_label 的写入。

---

## 5. 回调注册总览（"谁回调谁"一张图记住）

```mermaid
graph LR
    E1[GPIO 中断] --> R1[Button 库]
    E2[WebSocket 帧 JSON-RPC] --> R2[McpServer]
    E3[FreeRTOS Tick] --> R3[LVGL 定时器]
    E4[DataUpdateTask 加锁直调] --> R3

    R1 -->|click double long| L1[lambda ToggleChatState CycleDisplayMode 等]
    R2 -->|tools call name| L2[lambda 调 Manager 方法]
    R3 -->|tick| L3[UI 重绘]

    L1 --> D1[PomodoroManager]
    L2 --> D1
    L1 --> D2[WeatherManager]
    L2 --> D2
    L1 --> D3[SensorManager]
    L2 --> D3
    L1 --> D4[SdcardManager]
    L2 --> D4
    L1 --> D5[CustomLcdDisplay]
    L2 --> D5
    L1 --> D6[Settings/NVS]
    L2 --> D6
```

---

## 6. 数据流总览（Sensor / Weather / Memo / Audio）

```mermaid
graph LR
    I1[SHTC3 温湿度] --> SM[SensorManager]
    I2[PCF85063 RTC] --> SM
    I3[NTP server] --> SM
    SM --> I2
    I4[云端 AI weather.update] --> WM[WeatherManager]
    I5[云端 AI memo.add] --> Memo[NVS namespace memo]
    I7[GPIO4 ADC 电池] --> BAT[BatterygetPercent 10次平均加EMA加抛物线]
    I6[ES7210 麦克风] --> AC[BoxAudioCodec + AEC]

    SM --> UI[CustomLcdDisplay LVGL]
    WM --> UI
    Memo --> UI
    BAT --> UI
    AC --> APP[Application 状态机]
    APP --> SPK[ES8311 转 MAX98357A 喇叭]
```

**关键点：**
- 天气**不**由设备主动拉取，由云端 AI 通过 `self.weather.update` 回写——这是这个板子最有意思的设计：把"取数据"的活完全外包给 LLM 的工具链。
- NTP 同步成功后会反向写到 PCF85063，断电也保留时间。
- 备忘录持久化在 NVS，重启不丢。

---

## 7. 番茄钟状态机（PomodoroManager 内部）

```mermaid
stateDiagram
    [*] --> IDLE
    IDLE --> FOCUSING: start
    FOCUSING --> PAUSED: pause
    PAUSED --> FOCUSING: pause again
    FOCUSING --> BREAKING: 倒计时结束
    BREAKING --> IDLE: 休息结束
    FOCUSING --> IDLE: stop
    PAUSED --> IDLE: stop
    BREAKING --> IDLE: stop
```

> FOCUSING 状态下：独立 FreeRTOS task 每秒 tick，播放 SD 卡白噪音。

**对照看：** `managers/pomodoro_manager.cc` 的 `start() / stop() / pause() / TimerTask()`。

---

## 8. 配网流程（WiFi 凭据从无到有）

```mermaid
graph TD
    Boot[设备开机] --> Chk{NVS 里有 wifi.ssid}
    Chk -->|否| Cfg[进入配网模式 EnterWifiConfigMode]
    Chk -->|是| Conn[WifiStation 连接]

    Cfg --> M1{menuconfig 选哪种}
    M1 -->|BluFi| BF[蓝牙广播 Xiaozhi-Blufi EspBlufi App 写入 SSID 和 PSK]
    M1 -->|Hotspot| HS[开 AP Xiaozhi-XXXX 浏览器访问 192.168.4.1 网页表单]

    BF --> Save[写 NVS namespace=wifi]
    HS --> Save
    Save --> Reboot[重启] --> Conn

    Conn --> OK{连上了}
    OK -->|否| Cfg
    OK -->|是| App[进入 Application 主流程]

    UserAct[BOOT 单击启动期 或 self.disp.network 工具] --> Cfg
```

---

## 9. 关键数据结构 / 类层次

```mermaid
classDiagram
    class Board {
        +GetInstance()
        +GetAudioCodec()
        +GetDisplay()
        +GetBatteryLevel()
    }
    class WifiBoard {
        +EnterWifiConfigMode()
    }
    class CustomBoard {
        -i2c_bus_
        -boot_button_
        -user_button_
        -display_
        +InitializeI2c()
        +InitializeSensors()
        +InitializeButtons()
        +InitializeTools()
        +ShowSystemInfo()
        +RefreshAllData()
    }
    Board <|-- WifiBoard
    WifiBoard <|-- CustomBoard

    class Application {
        +Initialize()
        +Run()
        +ToggleChatState()
        +GetDeviceState()
    }

    class McpServer {
        +AddTool(name, desc, props, lambda)
        +DispatchToolCall(name, args)
    }

    class CustomLcdDisplay {
        +CycleDisplayMode()
        +SwitchToWeatherPage()
        +SwitchToMusicPage()
        +SwitchToPomodoroPage()
        +NotifyUserActivity()
        +SetShowingSystemInfo()
    }

    class PomodoroManager {
        +start(minutes, white_noise)
        +stop()
        +pause()
        +getState()
        +getRemainingTimeStr()
    }

    CustomBoard --> CustomLcdDisplay
    CustomBoard --> McpServer
    Application --> Board
    Application --> McpServer
    CustomBoard --> PomodoroManager
```

---

## 10. 速查：常用文件 / 关键行号

| 我想看…… | 去哪 |
|---|---|
| 入口 `app_main` | `main/main.cc:15` |
| 板子构造装配 | `waveshare-s3-rlcd-4.2.cc:733` |
| 全局注册 | `waveshare-s3-rlcd-4.2.cc:771` `DECLARE_BOARD` |
| 按键回调 | `waveshare-s3-rlcd-4.2.cc:93` `InitializeButtons()` |
| MCP 工具注册 | `waveshare-s3-rlcd-4.2.cc:257` `InitializeTools()` |
| 系统信息长按动画 | `waveshare-s3-rlcd-4.2.cc:128` `ShowSystemInfo()` |
| 电池电量算法 | `waveshare-s3-rlcd-4.2.cc:704` `BatterygetPercent()` |
| UI 后台 task | `data_update_task.cc:51` `xTaskCreate` |
| LVGL 布局 | `custom_lcd_display.cc` |
| 反射屏 SPI 驱动 | `rlcd_driver.cc` |
| 番茄钟状态机 | `managers/pomodoro_manager.cc` |
| 温湿度 + RTC + NTP | `managers/sensor_manager.cc` |
| Kconfig 注册 | `main/Kconfig.projbuild:320` |
| CMake 路由 | `main/CMakeLists.txt:336` |
| 主循环 | `main/application.cc:181` `Application::Run()` |

---

## 11. 完整调用链总览（三条执行线）

整个系统运行时有 **三条并行执行线**，共同驱动 UI 显示：

### 11.1 启动流程（主任务）

```mermaid
graph TB
    ROM[ROM Bootloader] --> SPL[2nd Bootloader]
    SPL --> FREERTOS[FreeRTOS 创建 main_task]
    FREERTOS --> APP_MAIN["app_main() - main.cc:15"]
    APP_MAIN --> NVS["nvs_flash_init() - main.cc:18"]
    NVS --> APP_GET["Application::GetInstance() - main.cc:27"]
    APP_GET --> APP_INIT["app.Initialize() - main.cc:28"]
    APP_INIT --> BOARD_GET["Board::GetInstance() - board.h:create_board()"]
    BOARD_GET --> BOARD_CTOR["CustomBoard() - waveshare-s3-rlcd-4.2.cc:1037"]
    BOARD_CTOR --> B1["InitializeI2c() - :1038 I2C总线+4设备"]
    B1 --> B2["InitializeSensors() - :1039 SHTC3+PCF85063"]
    B2 --> B3["InitializeSdcard() - :1040 挂载/sdcard"]
    B3 --> B4["InitializeButtons() - :1041 BOOT+USER按键"]
    B4 --> B5["InitializeTools() - :1042 12个MCP工具"]
    B5 --> B6["InitializeLcdDisplay() - :1043"]
    B6 --> LCD_NEW["new CustomLcdDisplay() - custom_lcd_display.cc:构造函数"]
    LCD_NEW --> RLCD_INIT["rlcd_->RLCD_Init() - rlcd_driver.cc:151"]
    RLCD_INIT --> LVGL_INIT["LvglDisplay::Initialize() - lvgl_display.cc"]
    LVGL_INIT --> SETUP_UI["SetupUI() - custom_lcd_display.cc 创建3个页面"]
    SETUP_UI --> START_DT{"StartDataUpdateTask() - data_update_task.cc:42"}
    LVGL_INIT --> START_LV{"esp_lvgl_port 自动创建 LVGL Task"}
    START_DT -.-> DT_TASK["DataUpdateTask() 优先级2 - data_update_task.cc:54"]
    START_LV -.-> LV_TASK["lvgl_port_task() 优先级4 - esp_lvgl_port库"]
    BOARD_CTOR --> APP_RUN["Application::Run() - application.cc:181"]
    APP_RUN --> EVENT_LOOP["主事件循环 xEventGroupWaitBits - 永不返回"]

    style DT_TASK fill:#c8e6c9,stroke:#388E3C
    style LV_TASK fill:#ffe0b2,stroke:#F57C00
    style EVENT_LOOP fill:#bbdefb,stroke:#1976D2
```

### 11.2 主任务事件循环（优先级 10）

```mermaid
graph TB
    WAIT["xEventGroupWaitBits - 阻塞等待"] --> EVENTS["事件分发"]
    EVENTS --> E1["NETWORK_CONNECTED - 连接AI服务器"]
    EVENTS --> E2["WAKE_WORD_DETECTED - 开始语音识别"]
    EVENTS --> E3["SCHEDULE - 执行排队的lambda"]
    EVENTS --> E4["STATE_CHANGED - 更新UI状态"]
    EVENTS --> E5["SEND_AUDIO - 发送音频数据"]
    EVENTS --> E6["TOGGLE_CHAT - 开始/停止对话"]
    E1 --> BACK["处理完毕 回到等待"]
    E2 --> BACK
    E3 --> BACK
    E4 --> BACK
    E5 --> BACK
    E6 --> BACK
    BACK -.-> WAIT

    E3 --> LVGL_CTRL["写入LVGL控件树 - 加锁"]
    E4 --> LVGL_CTRL

    style WAIT fill:#bbdefb,stroke:#1976D2
    style LVGL_CTRL fill:#ffe0b2,stroke:#F57C00
```

### 11.3 DataUpdateTask 循环（优先级 2）

```mermaid
graph TB
    ENTRY[DataUpdateTask入口 - data_update_task.cc:54] --> DELAY[vTaskDelay 3s 等系统启动]
    DELAY --> LOOP[while 1 主循环]
    LOOP --> NTP[NTP时间同步 - 首次+24h校准+指数退避]
    NTP --> TIME[获取当前时间 + 时间跳变保护]
    TIME --> CLOCK[时钟UI更新 - 每分钟: HH:MM/星期/日期]
    CLOCK --> MEMO[备忘闹钟检查 - HH:MM匹配则Alert - 锁外执行]
    MEMO --> SENSOR[温湿度读取 - SHTC3 变化大于0.2度才更新]
    SENSOR --> WEATHER[天气显示刷新 - WeatherManager缓存]
    WEATHER --> BATTERY[电池状态 - ADC采样10s + 图标 + 低电量弹窗]
    BATTERY --> WIFI[WiFi图标更新 - 状态变化时]
    WIFI --> AI[AI状态更新 - 表情+状态文字]
    AI --> POMO[番茄钟UI - 倒计时+进度条 - 运行时每秒]
    POMO --> SLEEP[省电检测 - 5min无活动则降频]
    SLEEP --> VDELAY[vTaskDelay 正常1s/省电5s]
    VDELAY -.->|循环| LOOP

    CLOCK -->|lv_label_set_text + DisplayLockGuard| LVGL[写入LVGL控件树]
    SENSOR -->|lv_label_set_text| LVGL
    WIFI -->|lv_image_set_src| LVGL
    AI -->|lv_label_set_text| LVGL

    style ENTRY fill:#c8e6c9,stroke:#388E3C
    style LVGL fill:#ffe0b2,stroke:#F57C00
```

### 11.4 LVGL Task 渲染循环（优先级 4）

```mermaid
graph TB
    ENTRY[lvgl_port_task - esp_lvgl_port库内部] --> LOOP[while 1]
    LOOP --> TICK[lv_timer_handler - 每约50ms]
    TICK --> DIRTY[检查脏标记 - 哪些控件需要重绘]
    DIRTY --> RENDER[局部重绘 - 只渲染变化区域到RGB565绘图缓冲234KB]
    RENDER --> FLUSH[flush_cb回调 - custom_lcd_display.cc:37]
    FLUSH --> LUT[RGB565转1bit LUT - 彩色转黑白]
    LUT --> DMA[SPI DMA整帧发送 - DispBuffer 15KB到屏幕]
    DMA --> DONE[lv_display_flush_ready - 通知LVGL发送完毕]
    DONE -.->|约50ms后| LOOP

    style ENTRY fill:#ffe0b2,stroke:#F57C00
    style FLUSH fill:#fff3e0,stroke:#FF9800
```

### 11.5 三条线的交互关系

```mermaid
graph LR
    M1[Schedule lambda] -->|lock| L1
    M2[SetChatMessage] -->|lock| L1
    M3[SetEmotion] -->|lock| L1
    D1[Clock/Date] -->|lock| L1
    D2[Sensor/Weather] -->|lock| L1
    D3[WiFi/Battery] -->|lock| L1
    D4[AI Status] -->|lock| L1
    L1[LVGL Widget Tree] --> R1[Dirty Check]
    R1 --> R2[Redraw RGB565]
    R2 --> R3[flush_cb 1bit LUT]
    R3 --> R4[SPI DMA to Screen]

    style M1 fill:#bbdefb,stroke:#1976D2
    style M2 fill:#bbdefb,stroke:#1976D2
    style M3 fill:#bbdefb,stroke:#1976D2
    style D1 fill:#c8e6c9,stroke:#388E3C
    style D2 fill:#c8e6c9,stroke:#388E3C
    style D3 fill:#c8e6c9,stroke:#388E3C
    style D4 fill:#c8e6c9,stroke:#388E3C
    style L1 fill:#f5f5f5,stroke:#999
    style R1 fill:#ffe0b2,stroke:#F57C00
    style R2 fill:#ffe0b2,stroke:#F57C00
    style R3 fill:#ffe0b2,stroke:#F57C00
    style R4 fill:#ffe0b2,stroke:#F57C00
```

> 蓝色 = 主任务(prio 10)，绿色 = DataUpdateTask(prio 2)，橙色 = LVGL Task(prio 4)，灰色 = LVGL 控件树(内存)

### 三条执行线总结

| 执行线 | 来源文件 | 创建方式 | 优先级 | 职责 | 循环间隔 |
|---|---|---|---|---|---|
| **主任务** | `application.cc:181` | FreeRTOS 默认 main_task | 10 (最高) | AI 对话、网络、语音、事件分发 | 事件驱动(阻塞等待) |
| **DataUpdateTask** | `data_update_task.cc:54` | `xTaskCreate` (板子构造函数中) | 2 (低) | NTP/时钟/天气/传感器/电池/AI状态 | 正常 1s / 省电 5s |
| **LVGL Task** | esp_lvgl_port 库内部 | `esp_lvgl_port_init` | 4 (中) | 脏区检测 → 局部重绘 → flush → SPI | ~50ms |

### 数据流向

```
主任务 (Application)                    DataUpdateTask
    │                                       │
    │ Schedule(SetChatMessage/             │ lv_label_set_text(时钟)
    │          SetEmotion/...)             │ lv_image_set_src(WiFi图标)
    │                                       │ lv_label_set_text(温湿度)
    │                                       │
    ▼                                       ▼
┌─────────────────────────────────────────────┐
│           LVGL 控件树 (内存中)                │
│  ┌─────────┐  ┌─────────┐  ┌──────────┐    │
│  │ 天气页   │  │ 音乐页   │  │ 番茄钟页  │    │
│  │(当前可见) │  │(隐藏)    │  │(隐藏)     │    │
│  └─────────┘  └─────────┘  └──────────┘    │
│                   ↓ 脏标记                   │
└─────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────┐
│      LVGL Task (每 ~50ms)                    │
│                                              │
│  脏区检测 → 局部重绘(RGB565) → flush_cb      │
│                    ↓                         │
│  RGB565 → 1-bit LUT → DispBuffer(15KB)      │
│                    ↓                         │
│           SPI DMA → RLCD 屏幕                │
└─────────────────────────────────────────────┘
```

### 线程安全机制

三条线并发访问 LVGL 控件，通过 **DisplayLockGuard** (互斥锁) 保证安全：

```
DataUpdateTask:                    LVGL Task:                 主任务(Schedule):
    │                                  │                          │
    ├─ lock(lvgl_mutex)                │                          │
    ├─ lv_label_set_text(...)          │ (被阻塞)                  │
    ├─ unlock(lvgl_mutex)              │                          │
    │                                  ├─ lock(lvgl_mutex)        │
    │                                  ├─ lv_timer_handler()      │
    │                                  ├─ → flush_cb → SPI        │
    │                                  ├─ unlock(lvgl_mutex)      │
    │                                  │                          ├─ lock(lvgl_mutex)
    │                                  │                          ├─ SetChatMessage(...)
    │                                  │                          ├─ unlock(lvgl_mutex)
```

> **关键点**：任何时刻只有一条线能操作 LVGL 控件。LVGL 本身不是线程安全的，全靠这把锁。

---

**最后更新：** 2026-04-21
