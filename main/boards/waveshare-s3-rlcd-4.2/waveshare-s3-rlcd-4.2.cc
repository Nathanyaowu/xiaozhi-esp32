#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string>
#include <soc/rtc.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "settings.h"
#include <cJSON.h>
#include <cmath>
#include "lvgl.h"
#include "managers/sensor_manager.h"
#include "managers/sdcard_manager.h"
#include "managers/pomodoro_manager.h"

// 声明小智字体（用于系统信息显示时临时切换字体大小）
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_4);
#include "managers/weather_manager.h"

#define TAG "waveshare_rlcd_4_2"  // ESP_LOG 输出统一前缀，便于在串口日志中过滤本板子的输出

/**
 * @class CustomBoard
 * @brief Waveshare ESP32-S3-RLCD-4.2 开发板的板级实现类
 *
 * 继承关系：Board（抽象接口）← WifiBoard（提供 WiFi/BluFi/Hotspot 配网）← CustomBoard（本类）
 *
 * 职责：
 *   1) 在构造函数中初始化板上所有外设：I2C 总线、传感器、SD 卡、按键、MCP 工具、LCD 显示
 *   2) 提供 GetAudioCodec / GetDisplay / GetBatteryLevel 等覆盖虚函数，供框架统一调用
 *   3) 通过 InitializeTools 注册 11 个 MCP 工具（系统信息、天气、屏幕切换、番茄钟、备忘录）
 *
 * 生命周期：
 *   - 由文件末尾 DECLARE_BOARD(CustomBoard) 宏在 main.cc → Board::GetInstance() 中懒加载实例化
 *   - 全局唯一单例，进程结束前不析构（嵌入式环境无析构需求）
 *
 * 调用入口：
 *   app_main (main.cc) → Application::Initialize → Board::GetInstance() → new CustomBoard()
 */
class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;   // I2C 主总线句柄，被 ES8311/ES7210/SHTC3/PCF85063 共享
    Button boot_button_;                // BOOT 按键（GPIO0），主交互按键
    Button user_button_;                // USER 按键（GPIO18），辅助功能按键
    CustomLcdDisplay *display_;         // 自定义 LCD 显示对象指针，由 InitializeLcdDisplay 创建
    adc_oneshot_unit_handle_t adc1_handle;  // ADC1 单次采样句柄（电池电压检测，已被 BatterygetVoltage 内部静态变量取代）
    adc_cali_handle_t cali_handle;          // ADC 校准句柄（同上）

    /**
     * @brief 校验备忘录时间标签是否为合法的 HH:MM（24 小时制）字符串
     * @param time_str 时间字符串。允许空串（表示"无定时"备忘）
     * @return true=合法或为空；false=格式错误
     * @调用者 InitializeTools 中 self.memo.add 工具的回调（行 509）
     * @回调 无（纯函数，仅做字符串校验）
     * @副作用 无
     */
    bool IsValidMemoTimeLabel(const std::string& time_str) {
        if (time_str.empty()) {
            return true;  // 允许无时间备忘
        }
        if (time_str.size() != 5 || time_str[2] != ':') {
            return false;
        }
        if (time_str[0] < '0' || time_str[0] > '9' ||
            time_str[1] < '0' || time_str[1] > '9' ||
            time_str[3] < '0' || time_str[3] > '9' ||
            time_str[4] < '0' || time_str[4] > '9') {
            return false;
        }
        int hh = (time_str[0] - '0') * 10 + (time_str[1] - '0');
        int mm = (time_str[3] - '0') * 10 + (time_str[4] - '0');
        return (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59);
    }

    /**
     * @brief 初始化 I2C 主总线（ESP32_I2C_HOST，配置见 config.h）
     * @调用者 CustomBoard 构造函数（行 733），是构造序列的第 1 步
     * @回调 ESP-IDF: i2c_new_master_bus
     * @副作用
     *   - 申请 I2C 主总线资源，设置 i2c_bus_ 句柄
     *   - 启用 SDA/SCL 内部上拉
     *   - 失败时 ESP_ERROR_CHECK 触发 abort（这是有意行为：I2C 是后续所有外设的前置条件）
     * @注意 该总线后续被 ES8311(0x18)、ES7210(0x40)、SHTC3(0x70)、PCF85063(0x51) 四个设备复用
     */
    void InitializeI2c() {
        // I2C 总线初始化
        // 这条 I2C 总线被多个设备共享：
        // - ES8311 音频解码器
        // - ES7210 音频编码器
        // - SHTC3 温湿度传感器
        // - PCF85063 RTC 时钟
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;       // 滤除 7 个时钟周期内的毛刺
        i2c_bus_cfg.intr_priority = 0;           // 默认中断优先级
        i2c_bus_cfg.trans_queue_depth = 0;       // 0 = 同步（阻塞）模式
        i2c_bus_cfg.flags.enable_internal_pullup = 1;  // 启用内部上拉（板上若已有外部上拉可设 0）
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    /**
     * @brief 初始化板上所有 I2C 传感器（SHTC3 温湿度 + PCF85063 RTC + NTP 同步）
     * @调用者 CustomBoard 构造函数（行 735），紧接 InitializeI2c 之后
     * @回调 SensorManager::getInstance().init(i2c_bus_)
     * @副作用
     *   - 创建 SensorManager 单例，挂载 SHTC3 / PCF85063 设备到 i2c_bus_
     *   - 启动后台 NTP 同步任务（首次需联网后才生效）
     */
    void InitializeSensors() {
        // 初始化传感器（使用同一条 I2C 总线）
        SensorManager::getInstance().init(i2c_bus_);
        ESP_LOGI(TAG, "传感器初始化完成");
    }

    /**
     * @brief 初始化 SD 卡（SDMMC 4-bit 模式，引脚见 SdcardManager::init）
     * @调用者 CustomBoard 构造函数（行 736）
     * @回调 SdcardManager::getInstance().init()
     * @副作用
     *   - 成功时挂载 /sdcard FAT 文件系统，番茄钟白噪音可用
     *   - 失败时仅打 WARN 日志，不阻塞启动（SD 卡为可选外设）
     */
    void InitializeSdcard() {
        // 初始化 SD 卡（SDMMC 模式，板载 SD 卡槽默认引脚）
        bool ok = SdcardManager::getInstance().init();
        if (ok) {
            ESP_LOGI(TAG, "SD 卡初始化成功");
        } else {
            ESP_LOGW(TAG, "SD 卡初始化失败（可能未插卡），白噪音功能不可用");
        }
    }

    /**
     * @brief 注册 BOOT 与 USER 两个按键的全部回调（单击/双击/长按）
     * @调用者 CustomBoard 构造函数（行 737）
     * @回调
     *   - boot_button_.OnClick → Application::ToggleChatState 或 EnterWifiConfigMode
     *   - user_button_.OnClick → display_->CycleDisplayMode
     *   - user_button_.OnDoubleClick → this->RefreshAllData
     *   - user_button_.OnLongPress → this->ShowSystemInfo
     *   - 每个回调首条都调 display_->NotifyUserActivity 重置自动省电计时
     * @副作用 修改 Button 对象的回调表；按键事件由 Button 内部 GPIO 中断 + 任务派发
     */
    void InitializeButtons() {
        // BOOT 按钮（GPIO0）- 主要交互按键
        boot_button_.OnClick([this]() {
            if (display_) {
                display_->NotifyUserActivity();
                // ShowSystemInfo 滚动动画会占据 chat_status_label_ 并置 showing_system_info_=true，
                // 导致 DataUpdateTask 跳过 AI 状态更新（"聆听中..."等不显示），
                // 用户按 BOOT 键后看不到任何视觉反馈，误以为按键没响应。
                // 因此在进入 ToggleChatState 之前必须先停止滚动、恢复 label 状态。
                if (display_->IsShowingSystemInfo()) {
                    ESP_LOGI(TAG, "BOOT 按下：停止系统信息滚动，恢复之前的显示内容");
                    DisplayLockGuard lock(display_);
                    lv_anim_delete(display_->GetChatStatusLabel(), nullptr);
                    display_->SetShowingSystemInfo(false);
                    lv_obj_t* label = display_->GetChatStatusLabel();
                    if (label) {
                        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
                        lv_obj_align(label, LV_ALIGN_LEFT_MID, 64 + 20, 0);
                        // 恢复进入 ShowSystemInfo 前保存的文字（配网信息、AI对话等）
                        const std::string& saved = display_->GetSavedChatText();
                        lv_label_set_text(label, saved.c_str());
                    }
                }
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // USER 按钮（GPIO18）- 辅助功能按键
        user_button_.OnClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            if (display_) {
                display_->CycleDisplayMode();
            }
            ESP_LOGI(TAG, "USER 按钮单击：切换天气页/音乐页");
        });

        // BOOT 按钮双击：直达股票行情页面（快捷入口）
        boot_button_.OnDoubleClick([this]() {
            if (display_) {
                display_->NotifyUserActivity();
                display_->SwitchToStockPage();
                ESP_LOGI(TAG, "BOOT 双击：切换到股票页");
            }
        });

        user_button_.OnDoubleClick([this]() {
            if (display_) display_->NotifyUserActivity();
            if (display_ && display_->IsPomodoroMode()) {
                // 番茄钟页面双击：切换专注/休息模式（保持IDLE状态）
                auto& pomo = PomodoroManager::getInstance();
                pomo.stop();
                pomo.toggleBreakMode();
                if (pomo.isBreakMode()) {
                    ESP_LOGI("UserButton", "双击切换到休息模式(IDLE)");
                } else {
                    ESP_LOGI("UserButton", "双击切换到专注模式(IDLE)");
                }
            } else {
                RefreshAllData();
            }
        });

        // BOOT 按钮长按：循环切换音量档位 0→34→67→100→0，并播放0.5秒蜂鸣提示
        boot_button_.OnLongPress([this]() {
            if (display_) display_->NotifyUserActivity();

            auto* codec = GetAudioCodec();
            if (!codec) return;

            // 四档音量循环：0 → 34 → 67 → 100 → 0
            static const int kVolumeLevels[] = {0, 34, 67, 100};
            static const int kNumLevels = 4;
            int current = codec->output_volume();

            // 找到当前所在档位，切到下一档
            int next_idx = 0;
            for (int i = 0; i < kNumLevels; i++) {
                if (current <= kVolumeLevels[i]) {
                    next_idx = (i + 1) % kNumLevels;
                    break;
                }
                if (i == kNumLevels - 1) {
                    next_idx = 0;  // 超过100，回到0
                }
            }
            int new_vol = kVolumeLevels[next_idx];
            codec->SetOutputVolume(new_vol);
            ESP_LOGI(TAG, "BOOT 长按：音量切换 %d → %d", current, new_vol);

            // 播放 0.5 秒 800Hz 蜂鸣声
            // 采样率 24000Hz，0.5秒 = 12000 个采样点
            const int sample_rate = 24000;
            const int num_samples = sample_rate / 2;  // 0.5s = 12000 samples
            const int freq_hz = 800;
            std::vector<int16_t> beep_buf(num_samples);
            // 生成正弦波，振幅 0.3（避免太刺耳），9830 ≈ 32767 * 0.3
            for (int i = 0; i < num_samples; i++) {
                float t = (float)i / sample_rate;
                beep_buf[i] = (int16_t)(sinf(2.0f * M_PI * freq_hz * t) * 9830);
            }
            // 静音档临时设到34让用户听到"已静音"提示音
            bool was_muted = (new_vol == 0);
            if (was_muted) {
                codec->SetOutputVolume(34);
            }
            codec->OutputData(beep_buf);
            if (was_muted) {
                codec->SetOutputVolume(0);
            }
        });

        user_button_.OnLongPress([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            // 番茄钟页面：长按启动/重置番茄钟；其他页面：显示系统信息
            if (display_ && display_->IsPomodoroMode()) {
                auto& pomo = PomodoroManager::getInstance();
                if (pomo.getState() == PomodoroManager::IDLE) {
                    Settings pomo_settings("pomodoro", false);
                    int focus_min = pomo_settings.GetInt("focus", 25);
                    int break_min = pomo_settings.GetInt("break", 5);
                    if (pomo.isBreakMode()) {
                        pomo.startBreak(break_min);
                        ESP_LOGI("UserButton", "番茄钟启动休息: %d分钟", break_min);
                    } else {
                        pomo.start(focus_min, false, break_min);
                        ESP_LOGI("UserButton", "番茄钟启动专注: %d分钟, 休息%d分钟", focus_min, break_min);
                    }
                } else {
                    pomo.stop();
                    ESP_LOGI("UserButton", "番茄钟已重置");
                }
            } else {
                ShowSystemInfo();
            }
        });
    }

    // USER 按钮功能实现
    /**
     * @brief 长按 USER 键时在 AI 对话区显示并循环滚动设备系统信息
     * @调用者 user_button_.OnLongPress 回调（行 120）
     * @回调
     *   - esp_get_free_heap_size / heap_caps_get_total_size / heap_caps_get_free_size（内存查询）
     *   - rtc_clk_cpu_freq_get_config（CPU 频率）
     *   - GetBatteryLevel（自身虚函数，读取电池）
     *   - Application::GetDeviceState（WiFi 状态推断）
     *   - esp_timer_get_time（运行时长）
     *   - display_->SetShowingSystemInfo(true)：通知 DataUpdateTask 暂停 chat_label 更新避免锁竞争
     *   - LVGL: lv_anim_*（启动"鱼咬尾"无缝循环滚动动画）
     * @副作用
     *   - 接管 chat_label，长期占用直到下次 AI 对话覆盖
     *   - 改变 label 对齐方式为 TOP_LEFT（动画需要绝对定位）
     * @关键技术 鱼咬尾循环：将文字内容拼接两份[A][A]，滚动 -single_h 后跳回 0，视觉无缝
     */
    void ShowSystemInfo() {
        // 显示详细系统信息到 AI 对话区（启用多行滚动）
        char info[512];
        
        // 内存信息
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        
        // CPU 信息
        rtc_cpu_freq_config_t cpu_freq_conf;
        rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
        uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;
        
        // 电池信息
        int battery_level = 0;
        bool charging = false, discharging = false;
        GetBatteryLevel(battery_level, charging, discharging);
        
        // WiFi 信息
        auto& app = Application::GetInstance();
        const char* wifi_status = "未连接";
        if (app.GetDeviceState() != kDeviceStateStarting && 
            app.GetDeviceState() != kDeviceStateWifiConfiguring) {
            wifi_status = "已连接";
        }
        
        // 运行时间
        uint64_t uptime_sec = esp_timer_get_time() / 1000000;
        uint32_t uptime_hours = uptime_sec / 3600;
        uint32_t uptime_mins = (uptime_sec % 3600) / 60;
        
        // 计算百分比
        int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
        int psram_percent = total_psram > 0 ? 
                           (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
        
        // 详细格式（单份内容，用于鱼咬尾拼接）
        snprintf(info, sizeof(info), 
                 "=== 系统信息 ===\n"
                 "CPU: %luMHz\n"
                 "运行: %luh%lumin\n"
                 "SRAM: \n %dKB/%dKB (%d%%)\n"
                 "PSRAM: \n %dMB/%dMB (%d%%)\n"
                 "电池: %d%% %s\n"
                 "WiFi: %s\n"
                 "==============\n"
                 "\n",  // 分隔符
                 cpu_freq_mhz,
                 uptime_hours, uptime_mins,
                 (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                 (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                 battery_level, charging ? "充电中" : "放电中",
                 wifi_status);
        
        if (display_) {
            lv_obj_t* chat_label = display_->GetChatStatusLabel();
            if (chat_label) {
                // 保存当前 chat_status_label_ 的文字，退出时恢复
                // 避免配网/AI对话等信息在退出系统信息后丢失
                const char* cur = lv_label_get_text(chat_label);
                display_->SaveChatText(cur ? cur : "");
                
                // 暂停 DataUpdateTask 对 UI 的更新（避免锁竞争导致 watchdog 超时）
                display_->SetShowingSystemInfo(true);
                
                {
                    DisplayLockGuard lock(display_);
                    
                    // 先删除旧动画（防止冲突）
                    lv_anim_delete(chat_label, nullptr);
                    
                    // 🐟 鱼咬尾：拼接两份相同内容
                    std::string info_double = std::string(info) + std::string(info);
                    
                    // 🔑 关键修复：切换到 TOP_LEFT 绝对定位
                    // 原因：label 初始化时用的是 LV_ALIGN_LEFT_MID（居中对齐），
                    // LVGL 内部会存储这个对齐方式，布局刷新时会重新计算位置，
                    // 导致动画里 set_y 设的值被覆盖。
                    // 切换到 TOP_LEFT 后，Y=0 就是父容器顶部，动画不会被干扰。
                    const int text_x = 64 + 20;  // emotion_w + 间距，保持文字在分隔线右侧
                    lv_obj_align(chat_label, LV_ALIGN_TOP_LEFT, text_x, 0);
                    
                    lv_label_set_text(chat_label, info_double.c_str());
                    lv_label_set_long_mode(chat_label, LV_LABEL_LONG_WRAP);
                    
                    // 强制计算布局，获取实际高度
                    lv_obj_update_layout(chat_label);
                    int label_h = lv_obj_get_height(chat_label);  // 双份内容的总高度
                    int single_h = label_h / 2;  // 单份内容高度
                    
                    // 🐟 鱼咬尾动画原理：
                    // 内容 = [A][A]（两份完全相同的文字首尾相接）
                    // Y=0 时显示第一个 A 的开头
                    // 向上滚动到 Y=-single_h 时，显示第二个 A 的开头
                    // 因为两个 A 完全一样，动画重复跳回 Y=0 时视觉上无缝衔接！
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, chat_label);
                    lv_anim_set_values(&a, 0, -single_h);
                    lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒，让用户先看到开头
                    lv_anim_set_duration(&a, single_h * 30);  // 速度：每像素 30ms
                    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_set_repeat_delay(&a, 0);  // 无缝重复，不停顿
                    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
                        lv_obj_set_y((lv_obj_t *)obj, v);
                    });
                    lv_anim_start(&a);
                }  // ← DisplayLockGuard 在这里自动释放
            }
        }
        
        ESP_LOGI(TAG, "系统信息: CPU=%luMHz, 运行=%luh%lumin, SRAM=%d%%, PSRAM=%d%%, 电量=%d%%", 
                 cpu_freq_mhz, uptime_hours, uptime_mins, heap_percent, psram_percent, battery_level);
    }

    /**
     * @brief 双击 USER 键时手动刷新所有数据（NTP 时间为主，天气需另由 MCP 触发）
     * @调用者 user_button_.OnDoubleClick 回调（行 117）
     * @回调 SensorManager::syncNtpTime；display_->SetChatMessage（提示用户）
     * @副作用 触发后台 NTP 重新同步；在对话区显示一条提示消息
     */
    void RefreshAllData() {
        ESP_LOGI(TAG, "手动刷新所有数据...");

        // 重新同步 NTP 时间
        SensorManager::getInstance().syncNtpTime();
        
        // 强制刷新屏幕显示
        if (display_) {
            display_->SetChatMessage("system", "正在刷新数据...\n时间已更新");
        }
        
        ESP_LOGI(TAG, "数据刷新完成");
    }

    /**
     * @brief 注册全部 11 个 MCP 工具到 McpServer 单例
     * @调用者 CustomBoard 构造函数（行 738），构造序列倒数第二步
     * @回调 McpServer::AddTool（11 次）。每条工具都包含 name + description（给 AI 看的英文+中文触发短语）+ 参数 schema + lambda 回调
     * @副作用
     *   - 工具被全局注册，AI 通过 MCP 协议（websocket）即可远程调用
     *   - 多个工具持有 [this] 捕获，访问 display_ / EnterWifiConfigMode（继承自 WifiBoard）
     * @工具清单（按出现顺序）
     *   1) self.system.info     - 查询 CPU/内存/电池/WiFi
     *   2) self.weather.update  - AI 回写天气数据到屏幕
     *   3) self.disp.network    - 重新进入配网模式
     *   4) self.disp.switch     - 切换显示页（toggle/music/weather/pomodoro）
         *   5) self.pomodoro.start  - 启动番茄钟
     *   6) self.pomodoro.stop   - 停止番茄钟
     *   7) self.pomodoro.status - 查询番茄钟状态
     *   8) self.pomodoro.pause  - 暂停/恢复番茄钟
     *   9) self.memo.add        - 添加备忘
     *   10) self.memo.list      - 列出所有备忘
     *   11) self.memo.done      - 完成（删除）某条备忘
     *   12) self.memo.clear     - 清空所有备忘
     *   （共 12 个，README 说 11 是旧版数字）
     */
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        /**
         * @工具 [1/12] self.system.info
         * @用途 AI 查询设备运行状态（CPU/内存/电池/WiFi/运行时长），用语音播报
         * @参数 无
         * @返回 中文自然语言字符串（约 200 字），AI 可直接朗读
         * @触发示例 「系统信息」「CPU频率」「内存使用情况」「电量多少」
         * @数据源
         *   - esp_get_free_heap_size / heap_caps_get_total_size：SRAM 与 PSRAM 占用
         *   - rtc_clk_cpu_freq_get_config：CPU 当前频率（MHz）
         *   - GetBatteryLevel：电池百分比 + 充放电状态（来自 BatterygetPercent + GPIO35）
         *   - Application::GetDeviceState：判断 WiFi 是否已连接
         *   - esp_timer_get_time：系统启动后微秒数 → 转小时:分钟
         * @捕获 [this]：用于调用成员函数 GetBatteryLevel
         * @无副作用 只读
         */
        mcp_server.AddTool("self.system.info",
            "Get device system information (CPU, memory, battery, WiFi status).\n"
            "Use when user asks: '系统信息', 'CPU频率', '内存使用情况', '电量多少', 'system status', 'how much RAM'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                // 收集系统信息
                size_t free_heap = esp_get_free_heap_size();
                size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                
                rtc_cpu_freq_config_t cpu_freq_conf;
                rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
                uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;
                
                int battery_level = 0;
                bool charging = false, discharging = false;
                GetBatteryLevel(battery_level, charging, discharging);
                
                auto& app = Application::GetInstance();
                const char* wifi_status = "未连接";
                if (app.GetDeviceState() != kDeviceStateStarting && 
                    app.GetDeviceState() != kDeviceStateWifiConfiguring) {
                    wifi_status = "已连接";
                }
                
                uint64_t uptime_sec = esp_timer_get_time() / 1000000;
                uint32_t uptime_hours = uptime_sec / 3600;
                uint32_t uptime_mins = (uptime_sec % 3600) / 60;
                
                int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
                int psram_percent = total_psram > 0 ? 
                                   (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
                
                // 格式化为自然语言（AI 容易读出来）
                char info[512];
                snprintf(info, sizeof(info),
                         "系统运行正常。CPU频率%luMHz，已运行%lu小时%lu分钟。"
                         "内存方面，SRAM使用了%dKB，占总量%dKB的%d%%；"
                         "PSRAM使用了%dMB，占总量%dMB的%d%%。"
                         "电池电量%d%%，当前%s。WiFi%s。",
                         cpu_freq_mhz, uptime_hours, uptime_mins,
                         (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                         (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                         battery_level, charging ? "正在充电" : "使用电池供电",
                         wifi_status);
                
                ESP_LOGI(TAG, "AI查询系统信息");
                return std::string(info);
            });

        /**
         * @工具 [2/12] self.weather.update
         * @用途 AI 把外部天气 MCP（高德/和风等）查到的天气写入设备屏幕缓存
         * @参数
         *   - city (string)        城市中文名，如「苏州」
         *   - text (string)        天气文本，如「晴」「多云」「小雨」
         *   - temp (string)        温度数字字符串，无单位，如「5」「-2」「26」
         *   - update_time (string) 可选，更新时间显示文本
         * @返回 成功："天气已更新：城市 文本 温度°C"；失败："天气写入失败..."
         * @触发示例 用户「查苏州天气」→ AI 先调外部天气 → 再调此工具回写设备
         * @核心调用 WeatherManager::getInstance().updateFromExternal(city, text, temp, update_time)
         *           内部更新单例缓存 → 触发 LVGL 重绘天气区域（在 data_update_task 下一次 tick 拉取时）
         * @捕获 无（不需要 this，纯静态访问 WeatherManager 单例）
         * @副作用 写 WeatherManager 单例字段 + 屏幕异步刷新
         */
        mcp_server.AddTool("self.weather.update",
            "Write weather data to the device screen cache.\n"
            "Use this after AI gets weather from an external MCP/weather source.\n"
            "Args:\n"
            "  `city`: City name (e.g. '苏州')\n"
            "  `text`: Weather text (e.g. '晴', '多云', '小雨')\n"
            "  `temp`: Temperature string without unit (e.g. '5', '-2', '26')\n"
            "  `update_time`: Optional time text (e.g. '2026-02-11 23:45')",
            PropertyList({
                Property("city", kPropertyTypeString),
                Property("text", kPropertyTypeString),
                Property("temp", kPropertyTypeString),
                Property("update_time", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto city = properties["city"].value<std::string>();
                auto text = properties["text"].value<std::string>();
                auto temp = properties["temp"].value<std::string>();
                auto update_time = properties["update_time"].value<std::string>();

                bool ok = WeatherManager::getInstance().updateFromExternal(city, text, temp, update_time);
                if (!ok) {
                    return std::string("天气写入失败：请检查 city/text/temp 是否为空");
                }

                ESP_LOGI(TAG, "AI写入天气成功: %s %s %s°C", city.c_str(), text.c_str(), temp.c_str());
                return std::string("天气已更新：") + city + " " + text + " " + temp + "°C";
            });
        
        /**
         * @工具 [3/12] self.disp.network
         * @用途 让设备退出当前 WiFi、重新进入配网模式（BluFi/热点二选一）
         * @参数 无
         * @返回 true（恒为真，不阻塞 AI 应答）
         * @触发示例 「重新配网」「换个 WiFi」「连不上网」
         * @核心调用 EnterWifiConfigMode()（继承自 WifiBoard 父类，会重启网络栈）
         * @捕获 [this]：访问继承的 EnterWifiConfigMode 成员
         * @副作用 断开当前 WiFi → 启动 BluFi/AP → 用户须用手机 App 重新输入凭据
         */
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });

        /**
         * @工具 [4/12] self.disp.switch
         * @用途 切换屏幕显示页（天气页 / 音乐页 / 番茄钟页）
         * @参数 mode (string)：toggle | music | weather | pomodoro，默认 "toggle"
         *       toggle = 循环切换三页；其余为指定页
         * @返回
         *   - "已切换到音乐页/天气页/番茄钟页"
         *   - display_ 为空：「显示器未初始化...」
         *   - mode 非法：「参数 mode 无效...」
         * @触发示例 「切到音乐页」「打开天气页」「打开番茄钟页面」
         * @核心调用
         *   - display_->NotifyUserActivity()  退出省电（5 分钟无操作降频刷新）
         *   - display_->CycleDisplayMode / SwitchToXxxPage  实际切页
         * @捕获 [this]：访问 display_ 指针
         * @副作用 修改 LVGL 当前活动屏幕 + 重置省电计时
         */
        mcp_server.AddTool(
            "self.disp.switch",
            "Switch display page between weather, music, and pomodoro.\n"
            "Use when user says: '切到音乐页', '打开天气页', '切换屏幕', '打开番茄钟页面', 'switch screen'.\n"
            "Args:\n"
            "  `mode`: 'toggle' | 'music' | 'weather' | 'pomodoro' (default: 'toggle')",
            PropertyList({
                Property("mode", kPropertyTypeString, std::string("toggle"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!display_) {
                    return std::string("显示器未初始化，暂时无法切换页面");
                }

                auto mode = properties["mode"].value<std::string>();

                // 统一小写判断
                for (auto& ch : mode) {
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = static_cast<char>(ch - 'A' + 'a');
                    }
                }

                display_->NotifyUserActivity();

                if (mode == "toggle") {
                    display_->CycleDisplayMode();
                } else if (mode == "music") {
                    display_->SwitchToMusicPage();
                } else if (mode == "weather") {
                    display_->SwitchToWeatherPage();
                } else if (mode == "pomodoro") {
                    display_->SwitchToPomodoroPage();
                } else {
                    return std::string("参数 mode 无效，请使用 toggle/music/weather/pomodoro");
                }

                if (display_->IsMusicMode()) return std::string("已切换到音乐页");
                if (display_->IsPomodoroMode()) return std::string("已切换到番茄钟页");
                return std::string("已切换到天气页");
            }
        );

        // ===== 番茄钟工具组（5/12 ~ 8/12）=====
        /**
         * @工具 [5/12] self.pomodoro.start
         * @用途 启动番茄钟倒计时（专注+休息循环）
         * @参数
         *   - minutes (int, 1-120)  专注时长分钟，默认从NVS读取（标准番茄钟25分钟）
         * @返回 "番茄钟已启动：专注N分钟 / 休息M分钟" 或 "番茄钟启动失败"
         * @触发示例 「开始番茄钟」「专注25分钟」「倒计时10分钟」
         * @核心调用
         *   - PomodoroManager::start(minutes, false, break_min)  启动 FreeRTOS 倒计时任务
         *   - display_->SwitchToPomodoroPage()        启动后自动切到番茄钟页
         * @捕获 [this]：访问 display_
         * @异常处理 properties[].value<T>() 用 try/catch 包裹，参数缺失时回退NVS默认值
         * @参数夹紧 minutes < 1 → 1，> 120 → 120（防御 AI 误传超大值）
         * @副作用 PomodoroManager 进入 COUNTING 状态 + 屏幕切页
         */
        mcp_server.AddTool("self.pomodoro.start",
            "Start a pomodoro focus timer.\n"
            "Use when user says: '开始番茄钟', '专注25分钟', '倒计时10分钟', 'start pomodoro', '番茄工作法'\n"
            "Args:\n"
            "  `minutes`: Focus duration in minutes (default from device settings, range 1-300)",
            PropertyList({
                Property("minutes", kPropertyTypeInteger, 1, 300)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                Settings pomo_settings("pomodoro", false);
                int minutes = pomo_settings.GetInt("focus", 25);
                int break_min = pomo_settings.GetInt("break", 5);
                
                try { minutes = properties["minutes"].value<int>(); } catch (...) {}

                if (minutes < 1) minutes = 1;
                if (minutes > 300) minutes = 300;

                auto& pomo = PomodoroManager::getInstance();
                bool ok = pomo.start(minutes, false, break_min);
                
                // 自动切换到番茄钟页面
                if (ok && display_) {
                    display_->NotifyUserActivity();
                    display_->SwitchToPomodoroPage();
                }

                if (ok) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), 
                             "番茄钟已启动：专注%d分钟 / 休息%d分钟",
                             minutes, break_min);
                    return std::string(buf);
                }
                return std::string("番茄钟启动失败");
            });

        /**
         * @工具 [6/12] self.pomodoro.stop
         * @用途 停止当前番茄钟（无论 COUNTING / BREAKING / PAUSED）
         * @参数 无
         * @返回 "番茄钟已停止" 或 "番茄钟当前没有在运行"
         * @触发示例 「停止番茄钟」「结束专注」「不专注了」
         * @核心调用
         *   - PomodoroManager::stop()      置位 IDLE，发停止信号给倒计时任务
         *   - display_->SwitchToWeatherPage()  自动切回天气页（默认页）
         * @捕获 [this]：访问 display_
         * @副作用 PomodoroManager → IDLE + 屏幕切回天气
         */
        mcp_server.AddTool("self.pomodoro.stop",
            "Stop the current Pomodoro timer.\n"
            "Use when user says: '停止番茄钟', '结束专注', 'stop pomodoro', '不专注了'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                auto& pomo = PomodoroManager::getInstance();
                if (pomo.getState() == PomodoroManager::IDLE) {
                    return std::string("番茄钟当前没有在运行");
                }
                pomo.stop();
                
                // 切回天气页
                if (display_) {
                    display_->SwitchToWeatherPage();
                }
                return std::string("番茄钟已停止");
            });

        /**
         * @工具 [7/12] self.pomodoro.status
         * @用途 查询番茄钟当前状态（运行/暂停/空闲、剩余时间、总设定）
         * @参数 无
         * @返回 IDLE：「番茄钟当前未运行...」；其他：「番茄钟状态：XX，剩余 MM:SS，共设定 N 分钟」
         * @触发示例 「番茄钟状态」「还剩多少时间」「专注了多久」
         * @核心调用 PomodoroManager 单例的 getState/getStateText/getRemainingTimeStr/getMinutes 只读 getter
         * @捕获 无
         * @无副作用 只读
         */
        mcp_server.AddTool("self.pomodoro.status",
            "Get current Pomodoro timer status.\n"
            "Use when user asks: '番茄钟状态', '还剩多少时间', '专注了多久', 'pomodoro status'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& pomo = PomodoroManager::getInstance();
                auto state = pomo.getState();
                if (state == PomodoroManager::IDLE) {
                    return std::string("番茄钟当前未运行。你可以说「开始番茄钟」来启动。");
                }

                char buf[256];
                snprintf(buf, sizeof(buf),
                         "番茄钟状态：%s，剩余 %s，共设定 %d 分钟",
                         pomo.getStateText().c_str(),
                         pomo.getRemainingTimeStr().c_str(),
                         pomo.getMinutes());
                return std::string(buf);
            });

        /**
         * @工具 [8/12] self.pomodoro.pause
         * @用途 切换番茄钟暂停 / 恢复（开关式）
         * @参数 无
         * @返回 RUNNING→PAUSED：「番茄钟已暂停」；PAUSED→RUNNING：「番茄钟已恢复」；IDLE：「无法暂停」
         * @触发示例 「暂停番茄钟」「继续番茄钟」
         * @核心调用 PomodoroManager::togglePause()  根据当前状态翻转
         * @捕获 无（不需要 display_，不切页）
         * @副作用 倒计时定时器暂停/恢复
         */
        mcp_server.AddTool("self.pomodoro.pause",
            "Pause or resume the current Pomodoro timer.\n"
            "Use when user says: '暂停番茄钟', '继续番茄钟', 'pause pomodoro', 'resume'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& pomo = PomodoroManager::getInstance();
                auto state = pomo.getState();
                if (state == PomodoroManager::IDLE) {
                    return std::string("番茄钟当前未运行，无法暂停");
                }
                pomo.togglePause();
                return pomo.getState() == PomodoroManager::PAUSED 
                    ? std::string("番茄钟已暂停") 
                    : std::string("番茄钟已恢复");
            });

        // ===== 备忘录工具组（9/12 ~ 12/12）=====
        // NVS namespace="memo" 中 key="items" 持久化为 JSON 数组：[{"t":"15:00","c":"开会"}, ...]
        // 上电后 data_update_task 周期扫描，到点的条目触发 PopupMemo 弹窗并自动删除

        /**
         * @工具 [9/12] self.memo.add
         * @用途 添加一条备忘 / 提醒 / 待办，持久化到 NVS，并立即在屏幕右下区显示
         * @参数
         *   - content (string)  备忘内容（建议 ≤8 个中文字以适配小屏幕）
         *   - time (string)     HH:MM 24 小时制，如 "07:30" "15:00"；空串表示无时间标记
         * @返回 成功："已添加备忘: 内容（共 N 条）"；时间格式错："时间格式无效..."；满 10 条："备忘已满..."
         * @触发示例 「提醒我下午 3 点开会」「记住买牛奶」「待办写周报」
         * @重要约束 AI 必须自己把「5 分钟后」「明天」转成 HH:MM 后再调用，本工具只接受严格时间格式
         * @核心流程
         *   1) IsValidMemoTimeLabel 校验时间字符串
         *   2) Settings("memo", false).GetString("items", "[]") 读现有 JSON 数组
         *   3) cJSON_Parse → 检查 ≤10 条 → cJSON_AddItemToArray 追加 {"t":..., "c":...}
         *   4) Settings("memo", true).SetString 写回 NVS（true=可写）
         *   5) display_->RefreshMemoDisplay() 立即刷屏
         * @捕获 [this]：访问 IsValidMemoTimeLabel + display_
         * @副作用 NVS 写入 + LVGL 区域重绘
         */
        mcp_server.AddTool("self.memo.add",
            "Add a memo / reminder / todo item. It will be persistently displayed on the device screen and survives reboot.\n"
            "Use when user says: '提醒我下午3点开会', '记住买牛奶', '明天提醒我...'\n"
            "Args:\n"
            "  `content`: Short memo text (max ~8 Chinese chars for best display on the small screen)\n"
            "  `time`: Time label in strict HH:MM 24-hour format (e.g. '07:30', '15:00'). Empty string if no specific time.\n"
            "  `date`: Date in YYYY-MM-DD format (e.g. '2026-04-28'). Empty string means today.\n"
            "Important:\n"
            "  - You MUST convert relative expressions before calling this tool.\n"
            "  - Time: '5分钟后' -> '21:18', '半小时后' -> '21:43', '晚上8点' -> '20:00'.\n"
            "  - Date: '明天' -> '2026-04-28', '后天' -> '2026-04-29', '下周一' -> actual date.\n"
            "  - Do NOT pass natural language like '5分钟后' or '明天'.",
            PropertyList({
                Property("content", kPropertyTypeString),
                Property("time", kPropertyTypeString, std::string("")),
                Property("date", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto content = properties["content"].value<std::string>();
                auto time_str = properties["time"].value<std::string>();
                auto date_str = properties["date"].value<std::string>();
                if (!IsValidMemoTimeLabel(time_str)) {
                    return std::string("时间格式无效：请使用 HH:MM（24小时制），例如 07:30、15:00；不要传自然语言");
                }
                if (!date_str.empty() && (date_str.length() != 10 || date_str[4] != '-' || date_str[7] != '-')) {
                    return std::string("日期格式无效：请使用 YYYY-MM-DD，例如 2026-04-28");
                }

                // 读取现有列表
                std::string json_str;
                {
                    Settings settings("memo", false);
                    json_str = settings.GetString("items", "[]");
                }

                cJSON *arr = cJSON_Parse(json_str.c_str());
                if (!arr) arr = cJSON_CreateArray();

                // 限制最多 10 条
                if (cJSON_GetArraySize(arr) >= 10) {
                    cJSON_Delete(arr);
                    return std::string("备忘已满（最多10条），请先完成或清除一些");
                }

                // 追加新条目
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "t", time_str.c_str());
                cJSON_AddStringToObject(item, "c", content.c_str());
                cJSON_AddStringToObject(item, "d", date_str.c_str());
                cJSON_AddItemToArray(arr, item);

                // 写回 NVS
                char *new_json = cJSON_PrintUnformatted(arr);
                {
                    Settings settings("memo", true);
                    settings.SetString("items", new_json);
                }
                int count = cJSON_GetArraySize(arr);
                cJSON_free(new_json);
                cJSON_Delete(arr);

                // 刷新屏幕
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "备忘已添加: 内容=%s, 时间=%s",
                         content.c_str(), time_str.c_str());
                return std::string("已添加备忘: ") + content + "（共" + std::to_string(count) + "条）";
            });

        /**
         * @工具 [10/12] self.memo.list
         * @用途 列出 NVS 中所有备忘，带 1-based 序号供后续 self.memo.done 引用
         * @参数 无
         * @返回 "当前备忘列表:\n1. [HH:MM] 内容\n2. ..."；空列表："当前没有备忘"
         * @触发示例 「我有什么待办」「看看备忘」
         * @核心流程 Settings("memo", false).GetString → cJSON_Parse → 遍历 t/c 字段拼字符串
         * @捕获 无（只读 NVS，不需要 this）
         * @无副作用 只读 NVS
         */
        mcp_server.AddTool("self.memo.list",
            "List all memos / reminders / todos on the device.\n"
            "Use when user asks: '我有什么待办', '看看备忘', 'what do I need to do'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                Settings settings("memo", false);
                std::string json_str = settings.GetString("items", "[]");

                cJSON *arr = cJSON_Parse(json_str.c_str());
                if (!arr || cJSON_GetArraySize(arr) == 0) {
                    if (arr) cJSON_Delete(arr);
                    return std::string("当前没有备忘");
                }

                std::string result = "当前备忘列表:\n";
                int count = cJSON_GetArraySize(arr);
                for (int i = 0; i < count; i++) {
                    cJSON *item = cJSON_GetArrayItem(arr, i);
                    cJSON *t = cJSON_GetObjectItem(item, "t");
                    cJSON *c = cJSON_GetObjectItem(item, "c");
                    result += std::to_string(i + 1) + ". ";
                    if (t && strlen(t->valuestring) > 0) {
                        result += "[";
                        result += t->valuestring;
                        result += "] ";
                    }
                    if (c) result += c->valuestring;
                    result += "\n";
                }
                cJSON_Delete(arr);
                return result;
            });

        /**
         * @工具 [11/12] self.memo.done
         * @用途 按序号完成 / 删除某条备忘
         * @参数 index (int, 1-10)  1-based 序号；建议 AI 不确定时先调 self.memo.list 拿序号
         * @返回 成功："已完成: 内容"；序号越界："序号无效，当前共 N 条备忘"
         * @触发示例 「第一条做完了」「删掉买牛奶那条」
         * @核心流程
         *   1) 读 NVS → cJSON_Parse
         *   2) 范围检查 1 ≤ idx ≤ count
         *   3) 取出待删条目的 c 字段（用于回执反馈）
         *   4) cJSON_DeleteItemFromArray(idx-1)
         *   5) 写回 NVS + display_->RefreshMemoDisplay()
         * @捕获 [this]：访问 display_
         * @副作用 NVS 写入 + 屏幕刷新
         */
        mcp_server.AddTool("self.memo.done",
            "Mark a memo as done and remove it from the list.\n"
            "Use when user says: '第一条做完了', '删掉买牛奶那条', '完成了开会'\n"
            "Args:\n"
            "  `index`: 1-based index of the memo to remove. If unsure, call self.memo.list first.",
            PropertyList({
                Property("index", kPropertyTypeInteger, 1, 10)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int idx = properties["index"].value<int>();

                std::string json_str;
                {
                    Settings settings("memo", false);
                    json_str = settings.GetString("items", "[]");
                }

                cJSON *arr = cJSON_Parse(json_str.c_str());
                if (!arr) return std::string("备忘列表为空");

                int count = cJSON_GetArraySize(arr);
                if (idx < 1 || idx > count) {
                    cJSON_Delete(arr);
                    return std::string("序号无效，当前共") + std::to_string(count) + "条备忘";
                }

                // 获取被删除条目的内容用于反馈
                cJSON *removed = cJSON_GetArrayItem(arr, idx - 1);
                cJSON *c = cJSON_GetObjectItem(removed, "c");
                std::string removed_text = (c && c->valuestring) ? c->valuestring : "";

                cJSON_DeleteItemFromArray(arr, idx - 1);

                // 写回 NVS
                char *new_json = cJSON_PrintUnformatted(arr);
                {
                    Settings settings("memo", true);
                    settings.SetString("items", new_json);
                }
                cJSON_free(new_json);
                cJSON_Delete(arr);

                // 刷新屏幕
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "备忘已完成: %s", removed_text.c_str());
                return std::string("已完成: ") + removed_text;
            });

        /**
         * @工具 [12/12] self.memo.clear
         * @用途 清空全部备忘（一次性删除 NVS 中 "items" 键）
         * @参数 无
         * @返回 "所有备忘已清除"（无论原本是否有备忘）
         * @触发示例 「清空备忘」「全部删掉」
         * @核心调用 Settings("memo", true).EraseKey("items")  直接擦除 NVS key
         * @捕获 [this]：访问 display_
         * @副作用 NVS 删 key + 屏幕刷新（备忘区将显示空）
         * @注意 不可逆。AI 应在用户明确说"清空"时才调用，不要因模糊词触发
         */
        mcp_server.AddTool("self.memo.clear",
            "Clear ALL memos / reminders / todos.\n"
            "Use when user says: '清空备忘', '全部删掉', 'clear all memos'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                {
                    Settings settings("memo", true);
                    settings.EraseKey("items");
                }
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "所有备忘已清除");
                return std::string("所有备忘已清除");
            });
    }

    /**
     * @brief 创建 CustomLcdDisplay 对象（RLCD 400×300 单色屏，SPI 接口）并启动后台数据更新任务
     * @调用者 CustomBoard 构造函数（行 739），构造序列最后一步
     * @回调
     *   - new CustomLcdDisplay(...)：构造内部完成 SPI 总线 + 面板 + LVGL 初始化 + UI 控件创建
     *   - display_->StartDataUpdateTask()：启动 FreeRTOS 后台任务（stack=8192 prio=2），周期更新天气/时间/传感器
     * @副作用
     *   - 分配显示对象到堆，赋值给 display_ 成员
     *   - 申请 SPI 总线、LVGL 缓冲区（PSRAM）、后台任务栈
     */
    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH, RLCD_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, spi_config);
        
        // 启动天气站数据更新任务
        display_->StartDataUpdateTask();
    }

    /**
     * @brief 读取电池电压一次（mV，已乘 3 倍分压补偿）
     * @return 电池电压（毫伏）。失败返回 0
     * @调用者 BatterygetPercent（同文件，循环采样 10 次）
     * @回调 ESP-IDF: adc_oneshot_read / adc_cali_raw_to_voltage
     * @副作用
     *   - 首次调用时初始化 ADC1 + 曲线拟合校准（懒加载，使用静态变量）
     *   - 该静态资源在进程生命周期中持有，不释放
     * @硬件 ADC1_CHANNEL_3（GPIO4），衰减 12dB（量程 ~0~3.1V），分压电阻比 1:3
     */
    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0;
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage = raw_voltage * 3;  // 分压电阻比例
            return (uint16_t)voltage;
        }
        return 0;
    }

    /**
     * @brief 计算电池电量百分比（含 10 次平均 + EMA 平滑 + 抛物线映射）
     * @return 0-100 范围内的百分比
     * @调用者 GetBatteryLevel 虚函数（行末覆盖，被 Application 框架定期查询）
     * @回调 BatterygetVoltage（同文件，连续 10 次）
     * @副作用
     *   - 维护静态 EMA 状态变量（首次调用初始化）
     * @算法
     *   1) 10 次连续采样取算术平均，抑制瞬时噪声
     *   2) EMA 滤波：alpha=0.1，约 10 次采样追平真实变化，消除波动
     *   3) 抛物线拟合：percent = (-V² + 9016V - 19,189,000) / 10000，clip 到 [0,100]
     *      （拟合点参考：4200mV→100%, 3700mV→50%, 3300mV→0%）
     */
    uint8_t BatterygetPercent() {
        // 静态变量用于指数移动平均（EMA）滤波，消除 ADC 噪声导致的电量漂移
        static float ema_voltage = 0.0f;    // 平滑后的电压值
        static bool ema_initialized = false;
        const float alpha = 0.1f;           // 平滑系数：越小越平滑（0.1 ≈ 约 10 次采样才能跟上真实变化）

        // 采样 10 次取平均（减少瞬时噪声）
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }
        voltage /= 10;

        // EMA 滤波：new_value = alpha * 当前值 + (1-alpha) * 历史值
        if (!ema_initialized) {
            ema_voltage = (float)voltage;
            ema_initialized = true;
        } else {
            ema_voltage = alpha * (float)voltage + (1.0f - alpha) * ema_voltage;
        }

        int smoothed = (int)(ema_voltage + 0.5f);  // 四舍五入
        // 电压→百分比映射（抛物线拟合）
        int percent = (-1 * smoothed * smoothed + 9016 * smoothed - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        return (uint8_t)percent;
    }

public:
    /**
     * @brief CustomBoard 构造函数：板上所有外设的统一初始化入口
     * @调用者 Board::GetInstance（main.cc 通过 DECLARE_BOARD 宏生成的工厂函数）
     * @回调 顺序调用 6 个 Initialize* 私有方法（顺序敏感，I2C 必须最先）
     * @副作用 完成全部硬件 + UI + MCP 工具初始化
     * @初始化顺序（顺序敏感，不可调换）
     *   1) InitializeI2c       - I2C 总线（后续传感器/Codec 依赖）
     *   2) InitializeSensors   - SHTC3 + PCF85063 + NTP（依赖 I2C）
     *   3) InitializeSdcard    - SD 卡（独立外设）
     *   4) InitializeButtons   - 按键回调（依赖 display_? 不，回调里做 nullptr 检查）
     *   5) InitializeTools     - MCP 工具注册（依赖 display_? 同上 nullptr 检查）
     *   6) InitializeLcdDisplay - LCD + UI + 数据更新任务（最后启动）
     * @注 Audio Codec 在 GetAudioCodec 第一次被调用时才懒加载
     */
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), user_button_(USER_BUTTON_GPIO) {    
        InitializeI2c();
        InitializeSensors();  // 在 I2C 初始化后立即初始化传感器
        InitializeSdcard();   // SD 卡初始化（白噪音播放需要）
        InitializeButtons();     
        InitializeTools();
        InitializeLcdDisplay();
    }

    /**
     * @brief 返回板上音频编解码器对象（懒加载，首次调用时构造 BoxAudioCodec）
     * @return AudioCodec* 单例指针，永不为 NULL
     * @调用者 Application::Start → AudioService::Initialize（main/audio/audio_service.cc）
     * @回调 BoxAudioCodec 构造（封装 ES8311 解码 + ES7210 编码 + I2S MCLK/BCLK/WS/DOUT/DIN + PA 引脚）
     * @副作用 首次调用申请 I2S DMA 缓冲、初始化 ES8311/ES7210 寄存器；后续返回同一指针
     */
    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    /**
     * @brief 返回板上显示对象指针
     * @return Display* 实际为 CustomLcdDisplay*；若 InitializeLcdDisplay 未执行则为 NULL
     * @调用者 Application 框架（StateMachine 用于发送状态/聊天消息到屏幕）
     * @回调 无（直接返回成员）
     * @副作用 无
     */
    virtual Display* GetDisplay() override {
        return display_;
    }

    /**
     * @brief 返回当前电池电量与充放电状态
     * @param level [out] 电量百分比 0-100
     * @param charging [out] 是否充电中（当前实现恒为 false，硬件无充电检测线）
     * @param discharging [out] 是否放电中（恒为 !charging = true）
     * @return true=数据有效（永远返回 true）
     * @调用者 Application 框架定期查询；ShowSystemInfo 也调用
     * @回调 BatterygetPercent
     * @副作用 触发 10 次 ADC 采样
     * @TODO 当前缺少充电状态检测引脚，charging 永远为 false
     */
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();
        return true;
    }
};

/**
 * @brief 注册 CustomBoard 为本固件的板级实现
 *
 * 展开后等价于：
 *   Board* Board::create_board() { static CustomBoard board; return &board; }
 *
 * @调用流程
 *   app_main (main.cc:50)
 *     → Application::GetInstance().Start()
 *       → Board::GetInstance() → create_board() → 首次调用时构造 CustomBoard 单例
 *
 * @如何被发现
 *   - main/CMakeLists.txt 根据 Kconfig 配置 BOARD_TYPE_WAVESHARE_S3_RLCD_4_2 决定编译此 .cc
 *   - main/Kconfig.projbuild 提供 menuconfig 中的板子选项
 *   - main/board.h 声明 DECLARE_BOARD 宏的展开模板
 */
DECLARE_BOARD(CustomBoard);
