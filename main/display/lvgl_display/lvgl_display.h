/**
 * @file lvgl_display.h
 * @brief LVGL 显示基类 — 所有使用 LVGL 的屏幕显示类的公共父类
 *
 * 继承关系：Display（纯接口）← LvglDisplay（本类，LVGL 通用逻辑）← LcdDisplay / OledDisplay / CustomLcdDisplay（具体屏幕）
 *
 * 本类职责：
 *   1. 管理状态栏通用控件（网络图标、电池图标、静音图标、状态文字、通知文字）
 *   2. 提供 SetStatus / ShowNotification / UpdateStatusBar 等通用 UI 操作
 *   3. 提供电源管理锁（防止 CPU 降频导致 SPI 刷屏变慢）
 *   4. 提供屏幕截图转 JPEG 功能（用于摄像头预览等场景）
 *   5. 定义 Lock/Unlock 纯虚函数，由子类实现 LVGL 线程安全锁
 *
 * @调用者 Application（通过 Board::GetDisplay() 获取实例后调用）
 * @子类实现 CustomLcdDisplay（RLCD 4.2 板子）、LcdDisplay、OledDisplay 等
 */
#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"       // 父类 Display 纯接口定义
#include "lvgl_image.h"    // LVGL 图片封装类

#include <lvgl.h>          // LVGL 核心 API
#include <esp_timer.h>     // ESP-IDF 高精度定时器（用于通知自动消失）
#include <esp_log.h>       // ESP-IDF 日志宏
#include <esp_pm.h>        // ESP-IDF 电源管理锁 API

#include <string>
#include <chrono>          // C++ 时间库（记录上次状态更新时间）

/**
 * @class LvglDisplay
 * @brief 基于 LVGL 的显示抽象层，提供状态栏、通知、电源管理等通用功能
 *
 * 子类只需实现 Lock/Unlock（LVGL 线程锁）和具体 UI 布局，
 * 状态栏更新、通知弹窗等逻辑由本类统一处理。
 */
class LvglDisplay : public Display {
public:
    LvglDisplay();           // 构造：创建通知定时器 + 电源管理锁
    virtual ~LvglDisplay();  // 析构：销毁定时器 + LVGL 控件 + 电源锁

    /**
     * @brief 设置状态栏文字（如"聆听中..."、"14:30"）
     * @param status 要显示的文字
     * @副作用 隐藏通知标签，显示状态标签；记录更新时间戳
     */
    virtual void SetStatus(const char* status);

    /**
     * @brief 显示一条临时通知（自动在 duration_ms 后消失）
     * @param notification 通知文字
     * @param duration_ms 显示时长（毫秒），默认 3000ms
     * @副作用 隐藏状态标签，显示通知标签；启动单次定时器到期后恢复
     */
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);

    /**
     * @brief 设置摄像头预览图片（本类为空实现，有摄像头的子类重写）
     */
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);

    /**
     * @brief 更新状态栏图标（电池、网络、静音）
     * @param update_all true=强制全部更新，false=仅在变化时更新
     * @调用者 DataUpdateTask 每秒调用 / Application 状态变化时调用
     */
    virtual void UpdateStatusBar(bool update_all = false);

    /**
     * @brief 进入/退出省电模式
     * @param on true=省电（表情设为 sleepy），false=唤醒（表情设为 neutral）
     */
    virtual void SetPowerSaveMode(bool on);

    /**
     * @brief 截取当前屏幕内容并转为 JPEG 格式
     * @param jpeg_data [out] JPEG 二进制数据
     * @param quality JPEG 质量（1-100），默认 80
     * @return true=成功，false=失败（需开启 CONFIG_LV_USE_SNAPSHOT）
     */
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);

protected:
    // ===== 电源管理 =====
    esp_pm_lock_handle_t pm_lock_ = nullptr;   // 电源锁：持有期间 CPU 保持最高频率，防止 SPI 刷屏被降频拖慢

    // ===== LVGL 核心对象 =====
    lv_display_t *display_ = nullptr;          // LVGL display 对象（由子类创建，绑定 flush 回调）

    // ===== 状态栏 LVGL 控件指针（由子类的 SetupUI 创建并赋值）=====
    lv_obj_t *network_label_ = nullptr;        // 网络状态图标（WiFi 满格/弱信号/断开）
    lv_obj_t *status_label_ = nullptr;         // 状态文字标签（"聆听中..."、"14:30" 等）
    lv_obj_t *notification_label_ = nullptr;   // 通知文字标签（临时显示，定时器到期后隐藏）
    lv_obj_t *mute_label_ = nullptr;           // 静音图标（音量为 0 时显示 🔇）
    lv_obj_t *battery_label_ = nullptr;        // 电池图标（满/中/低/充电 四种状态）
    lv_obj_t* low_battery_popup_ = nullptr;    // 低电量弹窗容器（电量 <20% 且放电时显示）
    lv_obj_t* low_battery_label_ = nullptr;    // 低电量弹窗内的文字标签
    
    // ===== 状态缓存（避免重复刷新未变化的图标）=====
    const char* battery_icon_ = nullptr;       // 上一次设置的电池图标指针（指针比较即可判断是否变化）
    const char* network_icon_ = nullptr;       // 上一次设置的网络图标指针
    bool muted_ = false;                       // 当前是否处于静音状态

    // ===== 定时器 =====
    std::chrono::system_clock::time_point last_status_update_time_;  // 上次 SetStatus 的时间戳（用于空闲 10 秒后自动显示时钟）
    esp_timer_handle_t notification_timer_ = nullptr;                // 通知自动消失定时器（单次触发，到期后隐藏通知恢复状态）

    // ===== LVGL 线程安全锁（纯虚函数，子类必须实现）=====
    friend class DisplayLockGuard;             // 友元类：RAII 风格锁守卫，构造时 Lock，析构时 Unlock
    virtual bool Lock(int timeout_ms = 0) = 0; // 获取 LVGL 互斥锁（0=永久等待）
    virtual void Unlock() = 0;                  // 释放 LVGL 互斥锁
};


#endif
