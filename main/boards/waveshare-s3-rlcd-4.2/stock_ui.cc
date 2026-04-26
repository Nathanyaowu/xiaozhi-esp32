// 股票行情页 UI 布局（400×300 单色 RLCD）
//
// 表格式布局：
// - 顶部状态栏（时钟 + 温湿度 + WiFi + 电池）
// - 标题 "证券行情"
// - 表头行（名称 | 现价 | 涨跌幅 | 最高/最低）
// - 5 行股票数据
// - 底部更新时间

#include "custom_lcd_display.h"
#include <esp_log.h>

LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);

static const char *TAG = "StockUI";

void CustomLcdDisplay::SetupStockUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    stock_page_ = lv_obj_create(root);
    lv_obj_set_size(stock_page_, 400, 300);
    lv_obj_set_pos(stock_page_, 0, 0);
    lv_obj_set_style_bg_opa(stock_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stock_page_, 0, 0);
    lv_obj_set_style_pad_all(stock_page_, 0, 0);
    lv_obj_set_style_radius(stock_page_, 0, 0);
    lv_obj_remove_flag(stock_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stock_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *screen = stock_page_;
    const lv_font_t *font_small  = &alibaba_puhui_16;
    const lv_font_t *font_title  = &alibaba_puhui_24;
    const lv_font_t *font_data   = &font_puhui_16_4;
    const lv_font_t *font_tiny   = &font_puhui_14_1;

    // ===== 状态栏（右上角白底胶囊，与天气页一致）=====
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 5, 0);

    stock_wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(stock_wifi_icon_img_, &ui_img_wifi_off);

    stock_battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(stock_battery_icon_img_, &ui_img_battery_full);

    stock_battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(stock_battery_pct_label_, font_small, 0);
    lv_obj_set_style_text_color(stock_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(stock_battery_pct_label_, "---%");

    // 左上角温湿度
    stock_sensor_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(stock_sensor_label_, font_small, 0);
    lv_obj_set_style_text_color(stock_sensor_label_, lv_color_white(), 0);
    lv_obj_align(stock_sensor_label_, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_label_set_text(stock_sensor_label_, "--.-°C  --.-%");

    // ===== 标题 "证券行情" =====
    const int title_y = 38;
    lv_obj_t *title_label = lv_label_create(screen);
    lv_obj_set_style_text_font(title_label, font_title, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, title_y);
    lv_label_set_text(title_label, "证券行情");

    // 时钟（标题右侧）
    stock_time_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(stock_time_label_, font_small, 0);
    lv_obj_set_style_text_color(stock_time_label_, lv_color_white(), 0);
    lv_obj_align(stock_time_label_, LV_ALIGN_TOP_RIGHT, -130, title_y + 4);
    lv_label_set_text(stock_time_label_, "00:00");

    // ===== 表头分隔线 =====
    const int header_y = 66;
    lv_obj_t *header_line = lv_obj_create(screen);
    lv_obj_set_size(header_line, 380, 1);
    lv_obj_set_pos(header_line, 10, header_y);
    lv_obj_set_style_bg_color(header_line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(header_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header_line, 0, 0);
    lv_obj_set_style_radius(header_line, 0, 0);
    lv_obj_remove_flag(header_line, LV_OBJ_FLAG_SCROLLABLE);

    // 表头标签
    const int header_label_y = header_y + 4;
    const int col_name_x = 10;
    const int col_price_x = 110;
    const int col_change_x = 220;
    const int col_range_x = 310;

    auto make_header = [&](int x, const char* text) {
        lv_obj_t *lbl = lv_label_create(screen);
        lv_obj_set_style_text_font(lbl, font_tiny, 0);
        lv_obj_set_style_text_color(lbl, lv_color_make(0x80, 0x80, 0x80), 0);
        lv_obj_set_pos(lbl, x, header_label_y);
        lv_label_set_text(lbl, text);
    };
    make_header(col_name_x, "名称");
    make_header(col_price_x, "现价");
    make_header(col_change_x, "涨跌幅");
    make_header(col_range_x, "高/低");

    // 表头下分隔线
    lv_obj_t *header_line2 = lv_obj_create(screen);
    lv_obj_set_size(header_line2, 380, 1);
    lv_obj_set_pos(header_line2, 10, header_label_y + 18);
    lv_obj_set_style_bg_color(header_line2, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(header_line2, LV_OPA_60, 0);
    lv_obj_set_style_border_width(header_line2, 0, 0);
    lv_obj_set_style_radius(header_line2, 0, 0);
    lv_obj_remove_flag(header_line2, LV_OBJ_FLAG_SCROLLABLE);

    // ===== 5 行股票数据 =====
    const int row_start_y = header_label_y + 22;
    const int row_height = 38;

    for (int i = 0; i < MAX_STOCKS; i++) {
        int y = row_start_y + i * row_height;

        // 名称列
        stock_name_labels_[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(stock_name_labels_[i], font_data, 0);
        lv_obj_set_style_text_color(stock_name_labels_[i], lv_color_white(), 0);
        lv_obj_set_pos(stock_name_labels_[i], col_name_x, y);
        lv_label_set_text(stock_name_labels_[i], kDefaultStocks[i].name);

        // 现价列
        stock_price_labels_[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(stock_price_labels_[i], font_data, 0);
        lv_obj_set_style_text_color(stock_price_labels_[i], lv_color_white(), 0);
        lv_obj_set_pos(stock_price_labels_[i], col_price_x, y);
        lv_label_set_text(stock_price_labels_[i], "---");

        // 涨跌幅列
        stock_change_labels_[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(stock_change_labels_[i], font_data, 0);
        lv_obj_set_style_text_color(stock_change_labels_[i], lv_color_white(), 0);
        lv_obj_set_pos(stock_change_labels_[i], col_change_x, y);
        lv_label_set_text(stock_change_labels_[i], "---");

        // 最高/最低列
        stock_range_labels_[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(stock_range_labels_[i], font_tiny, 0);
        lv_obj_set_style_text_color(stock_range_labels_[i], lv_color_make(0xC0, 0xC0, 0xC0), 0);
        lv_obj_set_pos(stock_range_labels_[i], col_range_x, y + 2);
        lv_label_set_text(stock_range_labels_[i], "---");

        // 行间分隔线（最后一行不加）
        if (i < MAX_STOCKS - 1) {
            lv_obj_t *row_sep = lv_obj_create(screen);
            lv_obj_set_size(row_sep, 380, 1);
            lv_obj_set_pos(row_sep, 10, y + row_height - 4);
            lv_obj_set_style_bg_color(row_sep, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(row_sep, LV_OPA_30, 0);
            lv_obj_set_style_border_width(row_sep, 0, 0);
            lv_obj_set_style_radius(row_sep, 0, 0);
            lv_obj_remove_flag(row_sep, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    // ===== 底部更新时间 =====
    stock_update_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(stock_update_label_, font_tiny, 0);
    lv_obj_set_style_text_color(stock_update_label_, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(stock_update_label_, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_label_set_text(stock_update_label_, "等待数据...");

    ESP_LOGI(TAG, "股票行情页 UI 创建完成");
}

void CustomLcdDisplay::UpdateStockDisplay(const StockData* data, int count) {
    DisplayLockGuard lock(this);

    for (int i = 0; i < count && i < MAX_STOCKS; i++) {
        if (!data[i].valid) continue;

        // 现价
        char price_buf[16];
        if (data[i].current_price >= 100.0f) {
            snprintf(price_buf, sizeof(price_buf), "%.1f", data[i].current_price);
        } else {
            snprintf(price_buf, sizeof(price_buf), "%.2f", data[i].current_price);
        }
        if (stock_price_labels_[i]) {
            lv_label_set_text(stock_price_labels_[i], price_buf);
        }

        // 涨跌幅（带方向符号）
        char change_buf[16];
        const char* arrow = data[i].change_pct >= 0 ? "+" : "";
        snprintf(change_buf, sizeof(change_buf), "%s%.2f%%", arrow, data[i].change_pct);
        if (stock_change_labels_[i]) {
            lv_label_set_text(stock_change_labels_[i], change_buf);
        }

        // 最高/最低
        char range_buf[32];
        if (data[i].high >= 100.0f) {
            snprintf(range_buf, sizeof(range_buf), "%.1f/%.1f", data[i].high, data[i].low);
        } else {
            snprintf(range_buf, sizeof(range_buf), "%.2f/%.2f", data[i].high, data[i].low);
        }
        if (stock_range_labels_[i]) {
            lv_label_set_text(stock_range_labels_[i], range_buf);
        }
    }

    // 更新底部时间
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "更新: %02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    if (stock_update_label_) {
        lv_label_set_text(stock_update_label_, time_buf);
    }
}
