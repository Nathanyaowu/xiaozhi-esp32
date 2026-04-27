// 股票数据获取模块
//
// 从新浪财经 hq.sinajs.cn 批量拉取股票行情
// 支持 A 股（sh/sz）、港股（hk）、美股（gb_）
// 解析各市场不同的字段格式，统一输出 StockData 结构

#include "stock_data.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <esp_log.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include "settings.h"
#include "secret_config.h"

static const char *TAG = "StockData";

// 默认跟踪的 5 支股票（硬编码，后续可改为从 SD 卡/Web 配置读取）
const StockConfig kDefaultStocks[MAX_STOCKS] = {
    {"sh600519", "贵州茅台", MARKET_SH},
    {"sz000001", "平安银行", MARKET_SZ},
    {"sh601318", "中国平安", MARKET_SH},
    {"sz000858", "五粮液",   MARKET_SZ},
    {"sz300750", "宁德时代", MARKET_SZ},
};

// HTTP 响应缓冲区（多支股票一次请求，每支约 300 字节）
#define HTTP_RESP_BUF_SIZE 4096

// esp_http_client 事件回调：将响应体追加到用户缓冲区
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        char *buf = (char *)evt->user_data;
        size_t cur_len = strlen(buf);
        size_t remaining = HTTP_RESP_BUF_SIZE - cur_len - 1;
        if ((size_t)evt->data_len <= remaining) {
            memcpy(buf + cur_len, evt->data, evt->data_len);
            buf[cur_len + evt->data_len] = '\0';
        }
    }
    return ESP_OK;
}

// 在 CSV 字段中按逗号分割，返回第 n 个字段的起始指针
// out_len: 输出该字段的长度
static const char* get_field(const char* csv, int field_idx, int* out_len) {
    const char* p = csv;
    int idx = 0;
    while (idx < field_idx && *p) {
        if (*p == ',') idx++;
        p++;
    }
    if (idx != field_idx) return nullptr;

    const char* end = strchr(p, ',');
    if (!end) end = strchr(p, '"');
    if (!end) end = p + strlen(p);
    *out_len = (int)(end - p);
    return p;
}

static float parse_field_float(const char* csv, int field_idx) {
    int len = 0;
    const char* f = get_field(csv, field_idx, &len);
    if (!f || len == 0) return 0.0f;
    char tmp[32];
    if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, f, len);
    tmp[len] = '\0';
    return strtof(tmp, nullptr);
}

// 解析单支 A 股数据（sh/sz 前缀）
// 字段: 0=名称, 1=今开, 2=昨收, 3=现价, 4=最高, 5=最低
static void parse_a_share(const char* csv_data, StockData* out) {
    out->yesterday_close = parse_field_float(csv_data, 2);
    out->current_price   = parse_field_float(csv_data, 3);
    out->high            = parse_field_float(csv_data, 4);
    out->low             = parse_field_float(csv_data, 5);

    if (out->yesterday_close > 0.001f) {
        out->change_pct = (out->current_price - out->yesterday_close) / out->yesterday_close * 100.0f;
    }
    out->valid = (out->current_price > 0.001f);
}

// 解析港股数据（hk 前缀）
// 字段: 0=英文名, 1=中文名, 2=今开, 3=昨收, 4=最高, 5=最低, 6=现价, 8=涨跌幅%
static void parse_hk_stock(const char* csv_data, StockData* out) {
    out->yesterday_close = parse_field_float(csv_data, 3);
    out->high            = parse_field_float(csv_data, 4);
    out->low             = parse_field_float(csv_data, 5);
    out->current_price   = parse_field_float(csv_data, 6);
    out->change_pct      = parse_field_float(csv_data, 8);
    out->valid = (out->current_price > 0.001f);
}

// 解析美股数据（gb_ 前缀）
// 字段: 0=名称, 1=现价, 2=涨跌幅%, 4=涨跌额, 5=今开, 6=最高, 7=最低
static void parse_us_stock(const char* csv_data, StockData* out) {
    out->current_price = parse_field_float(csv_data, 1);
    out->change_pct    = parse_field_float(csv_data, 2);
    out->high          = parse_field_float(csv_data, 6);
    out->low           = parse_field_float(csv_data, 7);
    float change_amt   = parse_field_float(csv_data, 4);
    if (fabsf(out->change_pct) > 0.001f && fabsf(change_amt) > 0.001f) {
        out->yesterday_close = out->current_price - change_amt;
    }
    out->valid = (out->current_price > 0.001f);
}

int FetchStockData(const StockConfig* configs, StockData* results, int count) {
    if (count <= 0 || count > MAX_STOCKS) return 0;

    // 清空结果
    for (int i = 0; i < count; i++) {
        memset(&results[i], 0, sizeof(StockData));
    }

    // 拼接请求 URL: http://hq.sinajs.cn/list=sh600519,sz000001,...
    char url[256] = STOCK_API_BASE_URL;
    for (int i = 0; i < count; i++) {
        if (i > 0) strcat(url, ",");
        strcat(url, configs[i].code);
    }

    // 分配 HTTP 响应缓冲区
    char *resp_buf = (char *)malloc(HTTP_RESP_BUF_SIZE);
    if (!resp_buf) {
        ESP_LOGE(TAG, "内存分配失败");
        return 0;
    }
    resp_buf[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = resp_buf;
    config.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Sina API 需要 Referer 头，否则可能返回空数据
    esp_http_client_set_header(client, "Referer", STOCK_API_REFERER);

    esp_err_t err = esp_http_client_perform(client);
    int success_count = 0;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && strlen(resp_buf) > 10) {
            ESP_LOGI(TAG, "股票数据获取成功 (%d bytes)", (int)strlen(resp_buf));

            // 逐行解析：每行格式 var hq_str_XXXX="data1,data2,...";
            char *line = resp_buf;
            int stock_idx = 0;
            while (line && *line && stock_idx < count) {
                // 找到引号内的数据部分
                char *quote_start = strchr(line, '"');
                if (!quote_start) break;
                quote_start++; // 跳过开引号

                char *quote_end = strchr(quote_start, '"');
                if (!quote_end) break;
                *quote_end = '\0'; // 截断闭引号

                // 空数据跳过（停牌或无效代码）
                if (strlen(quote_start) < 5) {
                    ESP_LOGW(TAG, "股票 %s 数据为空（可能停牌）", configs[stock_idx].code);
                    stock_idx++;
                    line = quote_end + 1;
                    // 跳到下一行
                    char *next_line = strchr(line, '\n');
                    line = next_line ? next_line + 1 : nullptr;
                    continue;
                }

                // 根据市场类型选择解析器
                switch (configs[stock_idx].market) {
                    case MARKET_SH:
                    case MARKET_SZ:
                        parse_a_share(quote_start, &results[stock_idx]);
                        break;
                    case MARKET_HK:
                        parse_hk_stock(quote_start, &results[stock_idx]);
                        break;
                    case MARKET_US:
                        parse_us_stock(quote_start, &results[stock_idx]);
                        break;
                }

                if (results[stock_idx].valid) {
                    success_count++;
                    ESP_LOGI(TAG, "%s: %.2f (%.2f%%)",
                             configs[stock_idx].name,
                             results[stock_idx].current_price,
                             results[stock_idx].change_pct);
                }

                stock_idx++;
                line = quote_end + 1;
                char *next_line = strchr(line, '\n');
                line = next_line ? next_line + 1 : nullptr;
            }
        } else {
            ESP_LOGW(TAG, "HTTP %d，响应长度 %d", status, (int)strlen(resp_buf));
        }
    } else {
        ESP_LOGE(TAG, "HTTP 请求失败: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(resp_buf);
    return success_count;
}

// NVS 中缓存的动态配置（静态存储，避免每次 malloc）
static StockConfig s_dynamic_configs[MAX_STOCKS];
static char s_code_bufs[MAX_STOCKS][16];
static char s_name_bufs[MAX_STOCKS][48];

int GetStockConfigs(StockConfig* out_configs) {
    Settings settings("stock", false);
    std::string json = settings.GetString("list", "");

    if (json.empty()) {
        memcpy(out_configs, kDefaultStocks, sizeof(StockConfig) * MAX_STOCKS);
        return MAX_STOCKS;
    }

    cJSON *arr = cJSON_Parse(json.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        memcpy(out_configs, kDefaultStocks, sizeof(StockConfig) * MAX_STOCKS);
        return MAX_STOCKS;
    }

    int count = cJSON_GetArraySize(arr);
    if (count <= 0) {
        cJSON_Delete(arr);
        return 0;
    }
    if (count > MAX_STOCKS) count = MAX_STOCKS;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *code = cJSON_GetObjectItem(item, "code");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *market = cJSON_GetObjectItem(item, "market");

        if (!cJSON_IsString(code) || !cJSON_IsString(name) || !cJSON_IsNumber(market)) {
            cJSON_Delete(arr);
            memcpy(out_configs, kDefaultStocks, sizeof(StockConfig) * MAX_STOCKS);
            return MAX_STOCKS;
        }

        strncpy(s_code_bufs[i], code->valuestring, sizeof(s_code_bufs[i]) - 1);
        s_code_bufs[i][sizeof(s_code_bufs[i]) - 1] = '\0';
        strncpy(s_name_bufs[i], name->valuestring, sizeof(s_name_bufs[i]) - 1);
        s_name_bufs[i][sizeof(s_name_bufs[i]) - 1] = '\0';

        s_dynamic_configs[i].code = s_code_bufs[i];
        s_dynamic_configs[i].name = s_name_bufs[i];
        s_dynamic_configs[i].market = (StockMarket)market->valueint;
    }

    cJSON_Delete(arr);
    memcpy(out_configs, s_dynamic_configs, sizeof(StockConfig) * count);
    return count;
}
