// CustomLcdDisplay 核心类
//
// 负责：
// - 构造/析构（初始化 RLCD 驱动 + LVGL + 创建 UI）
// - LVGL flush 回调（RGB565 → 1-bit 转换）
// - AI 消息适配（重写小智的 SetChatMessage / SetEmotion / ClearChatMessages）
// - 备忘录功能（加载/刷新备忘录列表）
// - 基类方法重写（UpdateStatusBar / SetTheme）
//
// 其他功能拆分到独立文件：
//   rlcd_driver.cc        - RLCD 硬件驱动
//   weather_ui.cc          - 天气站 UI 布局
//   music_ui.cc            - 音乐页 UI 布局
//   data_update_task.cc    - 后台数据更新任务

#include <vector>
#include <string>
#include <cstring>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "lvgl_theme.h"

static const char *TAG = "CustomDisplay";

// ===== LVGL flush 回调 =====

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *self = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    RlcdDriver *rlcd = self->rlcd_;
    uint16_t *buffer = (uint16_t *)color_p;
    for(int y = area->y1; y <= area->y2; y++)
    {
        for(int x = area->x1; x <= area->x2; x++) 
        {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            rlcd->RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    rlcd->RLCD_Display();
    lv_disp_flush_ready(disp);
}

// ===== 构造 / 析构 =====

CustomLcdDisplay::CustomLcdDisplay(
    esp_lcd_panel_io_handle_t panel_io,  // LCD 面板 IO 句柄 — 传 NULL（RLCD 自己管 SPI，不用 ESP-IDF 面板框架）
    esp_lcd_panel_handle_t panel,        // LCD 面板句柄 — 传 NULL（同上）
    int width,                           // 屏幕宽度 — 400 像素（RLCD_WIDTH）
    int height,                          // 屏幕高度 — 300 像素（RLCD_HEIGHT）
    int offset_x,                        // 显示区域 X 偏移 — 像素起始偏移，一般 0
    int offset_y,                        // 显示区域 Y 偏移 — 像素起始偏移，一般 0
    bool mirror_x,                       // 水平镜像 — 左右翻转显示
    bool mirror_y,                       // 垂直镜像 — 上下翻转显示
    bool swap_xy,                        // XY 轴交换 — 横屏/竖屏切换（true=旋转90°）
    spi_display_config_t spiconfig,      // SPI 引脚配置 — MOSI/SCK/CS/DC/RST 等 GPIO 编号
    spi_host_device_t spi_host           // SPI 总线 — SPI2_HOST 或 SPI3_HOST
    ) : LcdDisplay(panel_io, panel, width, height)  // 调用父类 LcdDisplay 构造函数，传入面板句柄和尺寸（panel_io/panel 均为 NULL）
{
    // ========== 第 1 步：创建 RLCD 硬件驱动 ==========
    // 在堆上 new 一个 RlcdDriver，内部会：
    //   - 初始化 SPI 总线（40MHz，DMA 自动选择）
    //   - 配置 RST 复位引脚为输出
    //   - 在 PSRAM 上分配 DispBuffer（15KB，1-bit 帧缓冲）
    //   - 在 PSRAM 上分配 PixelIndexLUT / PixelBitLUT（像素坐标 → 字节位置的查找表，加速 SetPixel）
    rlcd_ = new RlcdDriver(spiconfig, width, height, spi_host);

    // ========== 第 2 步：初始化 LVGL 图形库 ==========
    ESP_LOGI(TAG, "初始化 LVGL");

    // 初始化 LVGL 核心（内部数据结构、定时器系统、主题等）
    lv_init();

    // 获取 esp_lvgl_port 的默认配置（栈大小、tick 周期等）
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // LVGL 内部 FreeRTOS 任务的优先级设为 2（低优先级，不抢占音频/网络）
    port_cfg.task_priority = 2;
    // LVGL 内部任务每 50ms 执行一次 lv_timer_handler()
    // 即最快 20FPS 检查脏控件并触发 flush 回调
    port_cfg.timer_period_ms = 50;
    // 启动 LVGL 端口：内部创建一个 FreeRTOS 任务 + 互斥锁
    // 从此 LVGL 的定时任务开始每 50ms 运行
    lvgl_port_init(&port_cfg);

    // 获取 LVGL 互斥锁（0 = 永久等待直到拿到锁）
    // 后续所有 LVGL API 调用必须在锁内进行（LVGL 非线程安全）
    lvgl_port_lock(0);

    // 总像素数 = 400 × 300 = 120,000
    int transfer = width * height;

    // 创建一个 LVGL display 对象，告诉 LVGL 屏幕尺寸
    display_ = lv_display_create(width, height);

    // 注册 flush 回调：LVGL 画完脏区域后调用此函数把像素推给屏幕
    // Lvgl_flush_cb 内部做 RGB565→1-bit 转换 + SPI 整帧发送
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);

    // 把 this（CustomLcdDisplay 实例）绑定到 display 对象上
    // flush 回调里通过 lv_display_get_user_data(disp) 拿回 this，从而访问 rlcd_ 等成员
    lv_display_set_user_data(display_, this);

    // 计算 LVGL 绘图缓冲区大小：120,000 像素 × 2 字节(RGB565) = 240,000 字节 ≈ 234KB
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;

    // 在 PSRAM 上分配绘图缓冲区（234KB 太大不适合放 SRAM）
    // LVGL 渲染引擎在这块缓冲区上画 RGB565 像素，画完后交给 flush_cb
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    // 分配失败直接崩溃（没有绘图缓冲 LVGL 无法工作）
    assert(lvgl_buffer1);

    // 配置 LVGL 使用单缓冲 + 局部渲染模式
    // 参数：buf1=绘图缓冲, buf2=NULL(不用双缓冲), 大小, 渲染模式
    // PARTIAL 模式：LVGL 只渲染脏区域，不是每次都画全屏，省 CPU
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // ========== 第 3 步：初始化 RLCD 屏幕硬件 ==========
    ESP_LOGI(TAG, "初始化 RLCD 屏幕");
    // 发送硬件复位 + 初始化命令序列（VCOM 电压、扫描方向、色深、显示开启等）
    // 完成后屏幕清为全白，准备接收像素数据
    rlcd_->RLCD_Init();

    // 释放 LVGL 互斥锁，允许 LVGL 定时任务开始正常调度
    lvgl_port_unlock();

    // 安全检查：如果 display 创建失败则放弃后续 UI 初始化
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }

    // ========== 第 4 步：创建三个 UI 页面 ==========
    ESP_LOGI(TAG, "创建天气页 + 音乐页 + 番茄钟页 UI");
    // 每个 Setup 函数内部创建一个 lv_obj（screen），在上面摆放控件（标签、图标、卡片等）
    // 三个页面共存于内存，但同一时刻只有一个是可见的
    SetupWeatherUI();    // 天气页：时钟 + 日历 + 天气 + AI 对话 + 备忘录
    SetupMusicUI();      // 音乐页：唱片封面 + 歌曲信息 + 播放状态
    SetupPomodoroUI();   // 番茄钟页：倒计时 + 进度条 + 状态文字
    SetupStockUI();      // 股票页：证券行情表格（5 支股票实时数据）

    // 标记 UI 初始化完成
    // 基类 LcdDisplay 的 SetStatus/SetChatMessage 等函数会检查此标志
    // 为 false 时这些函数会直接 return，避免操作尚未创建的控件导致崩溃
    setup_ui_called_ = true;

    // 根据 current_mode_（默认 Weather）加载对应页面到屏幕
    // 内部调用 lv_screen_load() 切换当前可见页面
    ApplyDisplayMode();

    // ========== 第 5 步：从 NVS 恢复备忘录数据 ==========
    // 读取上次保存的备忘录 JSON，解析后更新天气页右下角的备忘录列表控件
    // 这样重启后备忘录不会丢失
    LoadMemoFromNvs();
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (update_task_handle_) {
        vTaskDelete(update_task_handle_);
    }
    delete rlcd_;
}

// ===== 备忘录功能 =====

void CustomLcdDisplay::LoadMemoFromNvs() {
    // 直接调用 RefreshMemoDisplay 从 NVS 读取并更新 UI
    RefreshMemoDisplay();
}

// 内部版本：不获取锁（调用者必须已持有 DisplayLock）
void CustomLcdDisplay::RefreshMemoDisplayInternal() {
    if (!memo_list_label_) return;

    // 从 NVS 读取 JSON 数组
    Settings settings("memo", false);
    std::string json_str = settings.GetString("items", "");

    if (json_str.empty()) {
        lv_label_set_text(memo_list_label_, "暂无待办");
        return;
    }

    cJSON *arr = cJSON_Parse(json_str.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        lv_label_set_text(memo_list_label_, "暂无待办");
        if (arr) cJSON_Delete(arr);
        return;
    }

    // 格式化每条备忘为一行: "时间 内容"
    // 卡片高度约 90px，16px 字体每行约 18px，最多显示约 5 行
    std::string display_text;
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count && i < 5; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *t = cJSON_GetObjectItem(item, "t");
        cJSON *c = cJSON_GetObjectItem(item, "c");

        if (i > 0) display_text += "\n";

        // 格式：[时间] 内容  或  · 内容（无时间时）
        if (t && cJSON_IsString(t) && strlen(t->valuestring) > 0) {
            display_text += t->valuestring;
            display_text += " ";
        } else {
            display_text += "· ";
        }
        if (c && cJSON_IsString(c)) {
            display_text += c->valuestring;
        }
    }

    // 如果超过 5 条，提示还有更多
    if (count > 5) {
        display_text += "\n...还有" + std::to_string(count - 5) + "条";
    }

    cJSON_Delete(arr);
    lv_label_set_text(memo_list_label_, display_text.c_str());
    ESP_LOGI(TAG, "备忘列表已刷新，共 %d 条", count);
}

// 外部版本：自动获取锁（供 MCP 工具等外部调用）
void CustomLcdDisplay::RefreshMemoDisplay() {
    DisplayLockGuard lock(this);
    RefreshMemoDisplayInternal();
}

// ===== AI 消息适配（重写小智的方法，只更新左下角卡片）=====

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_status_label_ == nullptr && music_chat_status_label_ == nullptr) return;
    if (!content || strlen(content) == 0) return;

    // 停止可能正在运行的滚动动画（系统信息或之前的 AI 滚动）
    lv_anim_delete(chat_status_label_, nullptr);
    
    // 停止系统信息滚动，恢复 DataUpdateTask 更新
    SetShowingSystemInfo(false);
    // 新消息到来，清除保存的旧文字（不需要恢复了）
    saved_chat_text_.clear();
    
    // 设置文本内容
    lv_label_set_text(chat_status_label_, content);
    lv_label_set_long_mode(chat_status_label_, LV_LABEL_LONG_WRAP);
    
    // 先恢复居中对齐（正常模式），计算内容高度
    lv_obj_align(chat_status_label_, LV_ALIGN_LEFT_MID, 64 + 20, 0);
    
    // 检查内容是否超出父容器（chat_inner）的可见高度
    lv_obj_update_layout(chat_status_label_);
    int label_h = lv_obj_get_height(chat_status_label_);
    // 从父容器动态获取高度，不硬编码（父容器是 chat_inner）
    lv_obj_t *parent = lv_obj_get_parent(chat_status_label_);
    int visible_h = parent ? lv_obj_get_content_height(parent) : 108;
    
    if (label_h > visible_h) {
        // 超长内容：切换到 TOP_LEFT 绝对定位后启用滚动
        // （和鱼咬尾同理，LEFT_MID 对齐会干扰动画的 set_y）
        const int text_x = 64 + 20;
        lv_obj_align(chat_status_label_, LV_ALIGN_TOP_LEFT, text_x, 0);
        
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, chat_status_label_);
        lv_anim_set_values(&a, 0, -(label_h - visible_h));  // 从顶部滚到底部
        lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒
        lv_anim_set_duration(&a, (label_h - visible_h) * 50);  // 速度：每像素 50ms
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 2000);  // 滚完后暂停 2 秒再重新开始
        lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
            lv_obj_set_y((lv_obj_t *)obj, v);
        });
        lv_anim_start(&a);
        
        ESP_LOGI("CustomLcdDisplay", "AI 回答过长（%dpx > %dpx），启用慢速滚动", label_h, visible_h);
    }

    // 音乐页同步显示 AI 文案
    if (music_chat_status_label_) {
        lv_label_set_long_mode(music_chat_status_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(music_chat_status_label_, content);
    }
    // 番茄钟页同步显示 AI 文案
    if (pomo_chat_status_label_) {
        lv_label_set_long_mode(pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(pomo_chat_status_label_, content);
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    
    // 1. 更新左侧文字（完整映射小智所有 21 种表情 + 额外状态）
    const char* text = "待命";
    if (strcmp(emotion, "neutral") == 0)         text = "待命";
    else if (strcmp(emotion, "happy") == 0)      text = "开心";
    else if (strcmp(emotion, "laughing") == 0)   text = "大笑";
    else if (strcmp(emotion, "funny") == 0)      text = "搞笑";
    else if (strcmp(emotion, "sad") == 0)        text = "难过";
    else if (strcmp(emotion, "angry") == 0)      text = "生气";
    else if (strcmp(emotion, "crying") == 0)     text = "哭泣";
    else if (strcmp(emotion, "loving") == 0)     text = "喜爱";
    else if (strcmp(emotion, "embarrassed") == 0) text = "害羞";
    else if (strcmp(emotion, "surprised") == 0)  text = "惊讶";
    else if (strcmp(emotion, "shocked") == 0)    text = "震惊";
    else if (strcmp(emotion, "thinking") == 0)   text = "思考";
    else if (strcmp(emotion, "winking") == 0)    text = "眨眼";
    else if (strcmp(emotion, "cool") == 0)       text = "耍酷";
    else if (strcmp(emotion, "relaxed") == 0)    text = "放松";
    else if (strcmp(emotion, "delicious") == 0)  text = "好吃";
    else if (strcmp(emotion, "kissy") == 0)      text = "亲亲";
    else if (strcmp(emotion, "confident") == 0)  text = "自信";
    else if (strcmp(emotion, "sleepy") == 0)     text = "犯困";
    else if (strcmp(emotion, "silly") == 0)      text = "调皮";
    else if (strcmp(emotion, "confused") == 0)   text = "困惑";
    // 额外状态
    else if (strcmp(emotion, "fear") == 0)       text = "害怕";
    else if (strcmp(emotion, "disgusted") == 0)  text = "嫌弃";
    else if (strcmp(emotion, "microchip_ai") == 0) text = "就绪";
    // 未知情绪也显示中文，不显示英文原文
    else                                         text = "待命";
    
    if (emotion_label_) {
        lv_label_set_text(emotion_label_, text);
    }
    if (music_emotion_label_) {
        lv_label_set_text(music_emotion_label_, text);
    }
    if (pomo_emotion_label_) {
        lv_label_set_text(pomo_emotion_label_, text);
    }
    
    // 2. 尝试加载小智自带的 emoji 图片（天气页 + 音乐页 + 番茄钟页同步更新）
    if (current_theme_) {
        auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
        auto image = emoji_collection ? emoji_collection->GetEmojiImage(emotion) : nullptr;
        bool has_image = (image && !image->IsGif());
        
        // 天气页 emoji
        if (emotion_img_) {
            if (has_image) {
                lv_image_set_src(emotion_img_, image->image_dsc());
                lv_obj_remove_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // 音乐页 emoji（同步显示相同的表情图片）
        if (music_emotion_img_) {
            if (has_image) {
                lv_image_set_src(music_emotion_img_, image->image_dsc());
                lv_obj_remove_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // 番茄钟页 emoji
        if (pomo_emotion_img_) {
            if (has_image) {
                lv_image_set_src(pomo_emotion_img_, image->image_dsc());
                lv_obj_remove_flag(pomo_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(pomo_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void CustomLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_status_label_) lv_label_set_text(chat_status_label_, "");
    if (music_chat_status_label_) lv_label_set_text(music_chat_status_label_, "");
    if (pomo_chat_status_label_) lv_label_set_text(pomo_chat_status_label_, "");
    // 表情不清除，保持常驻
}

// ===== 重写状态栏更新（禁用基类的 Font Awesome 文字更新）=====

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    // 不调用基类实现！
    // 基类会尝试用 lv_label_set_text 更新 network_label_ 和 battery_label_，
    // 但那些是隐藏的占位标签。我们自己的图片图标由 DataUpdateTask 管理。
}

// ===== 重写主题切换 =====

void CustomLcdDisplay::SetTheme(Theme* theme) {
    // RLCD 是 1-bit 单色屏，只有黑白两色，不需要主题切换。
    // 基类的 SetTheme 会操作 container_、content_、top_bar_ 等控件，
    // 我们的天气站 UI 没有创建这些，直接跳过避免崩溃。
    
    // 但需要保存 theme 指针，SetEmotion 需要用它来加载 emoji 图片
    current_theme_ = theme;
    ESP_LOGI(TAG, "RLCD 单色屏，跳过主题切换（已保存 theme 指针）");
}

void CustomLcdDisplay::ApplyDisplayMode() {
    // 先隐藏所有页面
    if (weather_page_) lv_obj_add_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
    if (music_page_) lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
    if (pomodoro_page_) lv_obj_add_flag(pomodoro_page_, LV_OBJ_FLAG_HIDDEN);
    if (stock_page_) lv_obj_add_flag(stock_page_, LV_OBJ_FLAG_HIDDEN);

    // 显示当前页面
    switch (display_mode_) {
        case MODE_WEATHER:
            if (weather_page_) lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_MUSIC:
            if (music_page_) lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_POMODORO:
            if (pomodoro_page_) lv_obj_remove_flag(pomodoro_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_STOCK:
            if (stock_page_) lv_obj_remove_flag(stock_page_, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

void CustomLcdDisplay::CycleDisplayMode() {
    DisplayLockGuard lock(this);
    // 三页循环：天气 → 音乐 → 番茄钟 → 天气
    switch (display_mode_) {
        case MODE_WEATHER:  display_mode_ = MODE_MUSIC; break;
        case MODE_MUSIC:    display_mode_ = MODE_POMODORO; break;
        case MODE_POMODORO: display_mode_ = MODE_STOCK; break;
        case MODE_STOCK:    display_mode_ = MODE_WEATHER; break;
    }
    ApplyDisplayMode();
    const char* name = "未知";
    switch (display_mode_) {
        case MODE_WEATHER:  name = "天气页"; break;
        case MODE_MUSIC:    name = "音乐页"; break;
        case MODE_POMODORO: name = "番茄钟"; break;
        case MODE_STOCK:    name = "股票页"; break;
    }
    ESP_LOGI(TAG, "页面切换: %s", name);

    if (display_mode_ == MODE_STOCK && stock_fetch_task_handle_) {
        xTaskNotifyGive(stock_fetch_task_handle_);
    }
}

void CustomLcdDisplay::SetMusicInfo(const char* title, const char* artist) {
    DisplayLockGuard lock(this);
    if (music_title_label_ == nullptr || music_artist_label_ == nullptr) {
        return;
    }
    lv_label_set_text(music_title_label_, (title && strlen(title) > 0) ? title : "未知歌曲");
    lv_label_set_text(music_artist_label_, (artist && strlen(artist) > 0) ? artist : "未知歌手");
}

void CustomLcdDisplay::SetMusicLyric(const char* lyric) {
    DisplayLockGuard lock(this);
    if (music_lyric_label_ == nullptr) {
        return;
    }

    // 歌词格式："上一句\n当前句\n下一句"（由 application.cc 拼接）
    // 如果没有 \n 分隔符，说明是单行文本（如错误提示），直接显示在当前行
    std::string text(lyric ? lyric : "");
    std::string prev_line, curr_line, next_line;

    size_t first_nl = text.find('\n');
    if (first_nl != std::string::npos) {
        prev_line = text.substr(0, first_nl);
        size_t second_nl = text.find('\n', first_nl + 1);
        if (second_nl != std::string::npos) {
            curr_line = text.substr(first_nl + 1, second_nl - first_nl - 1);
            next_line = text.substr(second_nl + 1);
        } else {
            curr_line = text.substr(first_nl + 1);
        }
    } else {
        // 单行文本（错误提示等），只显示在当前行
        curr_line = text;
    }

    // 更新三个 label
    if (music_lyric_prev_label_) {
        lv_label_set_text(music_lyric_prev_label_, prev_line.c_str());
    }
    lv_label_set_text(music_lyric_label_, curr_line.c_str());
    if (music_lyric_next_label_) {
        lv_label_set_text(music_lyric_next_label_, next_line.c_str());
    }
}

void CustomLcdDisplay::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    DisplayLockGuard lock(this);
    if (music_progress_bar_ == nullptr || music_progress_label_ == nullptr) {
        return;
    }

    if (total_ms > 0) {
        // 有总时长（来自歌词）：正常显示进度条和 "当前 / 总时长"
        if (current_ms > total_ms) {
            current_ms = total_ms;
        }
        lv_bar_set_range(music_progress_bar_, 0, static_cast<int32_t>(total_ms));
        lv_bar_set_value(music_progress_bar_, static_cast<int32_t>(current_ms), LV_ANIM_OFF);

        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu / %02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60),
                 static_cast<unsigned long>(total_ms / 60000),
                 static_cast<unsigned long>((total_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    } else {
        // 无总时长（没有歌词）：进度条不动，只显示已播放时间
        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    }
}

void CustomLcdDisplay::SwitchToMusicPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_MUSIC) {
        display_mode_ = MODE_MUSIC;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到音乐页");
    }
}

void CustomLcdDisplay::SwitchToWeatherPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_WEATHER) {
        display_mode_ = MODE_WEATHER;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到天气页");
    }
}

// ===== 番茄钟页面方法 =====

void CustomLcdDisplay::SwitchToPomodoroPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_POMODORO) {
        display_mode_ = MODE_POMODORO;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到番茄钟页");
    }
}

void CustomLcdDisplay::SwitchToStockPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_STOCK) {
        display_mode_ = MODE_STOCK;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到股票页");
        if (stock_fetch_task_handle_) {
            xTaskNotifyGive(stock_fetch_task_handle_);
        }
    }
}

void CustomLcdDisplay::UpdatePomodoroDisplay(const char* state_text, const char* countdown_text,
                                              int progress_permille, const char* info_text) {
    DisplayLockGuard lock(this);
    if (pomo_state_label_ && state_text) {
        lv_label_set_text(pomo_state_label_, state_text);
    }
    if (pomo_countdown_label_ && countdown_text) {
        lv_label_set_text(pomo_countdown_label_, countdown_text);
    }
    if (pomo_progress_bar_) {
        lv_bar_set_value(pomo_progress_bar_, progress_permille, LV_ANIM_OFF);
    }
    if (pomo_info_label_ && info_text) {
        lv_label_set_text(pomo_info_label_, info_text);
    }
}

// ===== 省电模式 =====

void CustomLcdDisplay::NotifyUserActivity() {
    last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (power_saving_) {
        power_saving_ = false;
        ESP_LOGI(TAG, "用户活动检测到，退出省电模式");
    }
}
