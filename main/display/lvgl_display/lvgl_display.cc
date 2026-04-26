/**
 * @file lvgl_display.cc
 * @brief LvglDisplay 基类实现 — 状态栏更新、通知弹窗、电源管理、屏幕截图
 *
 * 本文件实现 LvglDisplay 的通用逻辑，子类（CustomLcdDisplay / LcdDisplay 等）
 * 继承后只需实现 Lock/Unlock 和具体 UI 布局，状态栏相关功能由此处统一处理。
 */
#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>       // FontAwesome 图标常量（电池、WiFi、静音等）

#include "lvgl_display.h"
#include "board.h"              // Board::GetInstance() 获取硬件抽象
#include "application.h"        // Application::GetInstance() 获取设备状态
#include "audio_codec.h"        // AudioCodec::output_volume() 判断静音
#include "settings.h"           // NVS 键值对读写
#include "assets/lang_config.h" // 多语言字符串（低电量提示音等）
#include "jpg/image_to_jpeg.h"  // RGB565 → JPEG 转换工具

#define TAG "Display"

/**
 * @brief 构造函数 — 创建通知定时器 + 电源管理锁
 *
 * 通知定时器：ShowNotification 显示通知后启动，到期后自动隐藏通知、恢复状态标签
 * 电源管理锁：持有期间 APB 总线保持最高频率，确保 SPI 刷屏不被 CPU 降频拖慢
 */
LvglDisplay::LvglDisplay() {
    // ===== 创建通知自动消失定时器 =====
    // 当 ShowNotification 被调用时，启动此定时器
    // 到期后回调函数会：隐藏通知标签 → 恢复显示状态标签
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            // 定时器回调：通知显示时间到，切回状态标签
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);  // 操作 LVGL 控件前必须加锁
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);   // 隐藏通知
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);      // 恢复状态
        },
        .arg = this,                            // 传入 this，回调里转回 LvglDisplay*
        .dispatch_method = ESP_TIMER_TASK,       // 在 esp_timer 任务上下文中执行回调
        .name = "notification_timer",            // 调试用名称
        .skip_unhandled_events = false,          // 不跳过未处理的事件
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // ===== 创建电源管理锁 =====
    // ESP_PM_APB_FREQ_MAX：持有此锁期间，APB 总线时钟保持 80MHz 不降频
    // 用途：UpdateStatusBar 期间读 ADC、刷 SPI 需要稳定的时钟频率
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        // 未开启 CONFIG_PM_ENABLE 时电源管理不可用，跳过（不影响功能）
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

/**
 * @brief 析构函数 — 清理所有资源
 *
 * 依次销毁：定时器 → LVGL 控件 → 电源锁
 * 注意：LVGL 控件删除顺序不重要，lv_obj_del 会自动处理子对象
 */
LvglDisplay::~LvglDisplay() {
    // 停止并删除通知定时器
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    // 逐个删除 LVGL 控件（NULL 检查防止未初始化的情况）
    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    // 释放电源管理锁
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

/**
 * @brief 设置状态栏文字
 * @param status 要显示的字符串（如"聆听中..."、"14:30"、"连接中..."）
 *
 * 行为：
 *   1. 更新 status_label_ 的文字
 *   2. 显示 status_label_，隐藏 notification_label_（两者互斥显示）
 *   3. 记录时间戳，供 UpdateStatusBar 判断"空闲超过 10 秒则显示时钟"
 *
 * @调用者 Application 状态变化时 / UpdateStatusBar 空闲时自动显示时钟
 */
void LvglDisplay::SetStatus(const char* status) {
    // UI 未初始化时调用会丢失消息，打印警告方便调试
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);  // RAII 锁：构造时 Lock()，析构时 Unlock()
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but label not created)", status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);                       // 更新文字
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);          // 显示状态标签
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);       // 隐藏通知标签

    // 记录更新时间，UpdateStatusBar 用这个判断是否需要自动切回时钟显示
    last_status_update_time_ = std::chrono::system_clock::now();
}

/**
 * @brief ShowNotification 的 std::string 重载，转发到 const char* 版本
 */
void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

/**
 * @brief 显示一条临时通知，duration_ms 毫秒后自动消失
 * @param notification 通知文字
 * @param duration_ms 显示时长（毫秒），默认 3000
 *
 * 行为：
 *   1. 显示 notification_label_，隐藏 status_label_（两者互斥）
 *   2. 启动单次定时器，到期后回调自动隐藏通知、恢复状态
 *   3. 如果之前有通知定时器在跑，先停掉再重新启动（覆盖旧通知）
 *
 * @调用者 Application::Alert() 配网提示、OTA 升级提示等
 */
void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!", notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was called but label not created)", notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);            // 设置通知文字
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);    // 显示通知
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);             // 隐藏状态

    // 重启定时器：先停（如果之前还在跑），再启动单次定时
    // duration_ms * 1000 是因为 esp_timer 的单位是微秒（us）
    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

/**
 * @brief 更新状态栏所有图标（静音、时钟、电池、网络）
 * @param update_all true=强制刷新所有图标，false=仅在状态变化时更新
 *
 * 此函数由外部定期调用（如 DataUpdateTask 每秒一次），内部按需更新：
 *   1. 静音图标：音量变为 0 时显示，恢复时隐藏
 *   2. 时钟显示：设备空闲 且 10 秒内没有其他 SetStatus 调用 → 自动显示 HH:MM
 *   3. 电池图标：根据电量百分比切换 5 档图标 + 充电图标 + 低电量弹窗
 *   4. 网络图标：每 10 秒更新一次（避免频繁读取 4G 模块的 UART）
 */
void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // ===== 1. 静音图标更新 =====
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;  // UI 未初始化，直接返回
        }

        // 音量变为 0 → 显示静音图标；音量恢复 → 清空图标
        // 用 muted_ 标志位避免每次都重设（只在状态变化时操作 LVGL）
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);  // 🔇
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");  // 清空
        }
    }

    // ===== 2. 空闲时自动显示时钟 =====
    // 条件：设备处于 Idle 状态 且 距离上次 SetStatus 超过 10 秒
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            // 格式化当前时间为 "HH:MM"
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            // 检查系统时间是否已设置（年份 >= 2025 说明 NTP 已同步）
            if (tm->tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm);
                SetStatus(time_str);  // 显示时钟
            } else {
                ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm->tm_year);
            }
        }
    }

    // ===== 获取电源锁，防止后续 ADC 读取和 SPI 操作被 CPU 降频影响 =====
    esp_pm_lock_acquire(pm_lock_);

    // ===== 3. 电池图标更新 =====
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            // 充电中：显示带闪电的电池图标
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            // 未充电：根据电量百分比选择 5 档电池图标
            // 0-19% → 空, 20-39% → 1/4, 40-59% → 1/2, 60-79% → 3/4, 80-100% → 满
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY,
                FONT_AWESOME_BATTERY_QUARTER,
                FONT_AWESOME_BATTERY_HALF,
                FONT_AWESOME_BATTERY_THREE_QUARTERS,
                FONT_AWESOME_BATTERY_FULL,
                FONT_AWESOME_BATTERY_FULL,
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        // 指针比较：icon 是编译期常量字符串，地址不同说明图标变了才更新
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        // 低电量弹窗：电量 <20% 且正在放电时显示，否则隐藏
        if (low_battery_popup_ != nullptr) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                // 弹窗当前隐藏 → 显示，并播放低电量提示音
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                }
            } else {
                // 电量恢复或正在充电 → 隐藏弹窗
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // ===== 4. 网络图标更新（每 10 秒一次）=====
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // 固件升级期间不读取网络状态，避免占用 4G 模块的 UART 资源
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        // 只在允许的状态下更新网络图标
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            // 和电池图标一样，指针比较判断是否变化
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    // 释放电源锁，允许 CPU 进入低功耗模式
    esp_pm_lock_release(pm_lock_);
}

/**
 * @brief 设置摄像头预览图片 — 基类空实现
 *
 * 有摄像头的板子（如 atoms3r-cam）会重写此方法，在屏幕上显示预览画面。
 * 无摄像头的板子（如 RLCD 4.2）不需要此功能，保持空实现。
 */
void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

/**
 * @brief 进入/退出省电模式
 * @param on true=进入省电，false=退出省电
 *
 * 省电时：清空对话内容，表情设为 sleepy（😴）
 * 唤醒时：清空对话内容，表情恢复 neutral（😐）
 *
 * @调用者 Application 在无操作超时后调用
 */
void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");   // 清空 AI 对话区
        SetEmotion("sleepy");           // 😴 表情
    } else {
        SetChatMessage("system", "");   // 清空
        SetEmotion("neutral");          // 😐 表情
    }
}

/**
 * @brief 截取当前屏幕内容并转为 JPEG
 * @param jpeg_data [out] 输出的 JPEG 二进制数据
 * @param quality JPEG 压缩质量（1-100）
 * @return true=成功，false=失败
 *
 * 流程：
 *   1. 调用 LVGL snapshot API 获取当前屏幕的 RGB565 像素数据
 *   2. 字节序交换（LVGL 是大端序，JPEG 编码器需要小端序）
 *   3. 使用回调式 JPEG 编码器逐块输出，避免预分配大内存
 *   4. 释放 snapshot 缓冲区
 *
 * @前提 需要在 menuconfig 中开启 CONFIG_LV_USE_SNAPSHOT
 * @调用者 摄像头相关功能 / 远程屏幕共享
 */
bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    // 截取当前活动屏幕的所有像素，格式为 RGB565
    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // LVGL 内部使用大端序 RGB565，JPEG 编码器期望小端序
    // 逐像素交换高低字节：0xRRGG → 0xGGRR
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);  // GCC 内建函数，编译为单条 REV16 指令
    }

    // 清空输出缓冲
    jpeg_data.clear();

    // 使用回调式 JPEG 编码器：编码器每产出一块数据就调用 lambda 追加到 jpeg_data
    // 优势：不需要预先知道 JPEG 大小，不用一次性分配大内存
    bool ret = image_to_jpeg_cb(
        (uint8_t*)draw_buffer->data,       // 输入像素数据
        draw_buffer->data_size,            // 数据大小（字节）
        draw_buffer->header.w,             // 图片宽度
        draw_buffer->header.h,             // 图片高度
        V4L2_PIX_FMT_RGB565,              // 像素格式
        quality,                           // JPEG 质量
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
            // 回调：每产出一块 JPEG 数据就追加到 output 字符串
            std::string* output = static_cast<std::string*>(arg);
            if (data && len > 0) {
                output->append(static_cast<const char*>(data), len);
            }
            return len;
        },
        &jpeg_data                         // 回调参数：输出字符串指针
    );
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    // 释放 snapshot 分配的绘图缓冲区
    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    // 未开启 snapshot 功能，直接返回失败
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
