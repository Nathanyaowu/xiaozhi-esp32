#ifndef __STOCK_DATA_H__
#define __STOCK_DATA_H__

#include <cstdint>

#define MAX_STOCKS 5

// 股票市场类型（决定 API 请求前缀和解析方式）
enum StockMarket {
    MARKET_SH = 0,  // 上证 A 股（sh）
    MARKET_SZ = 1,  // 深证 A 股（sz）
    MARKET_HK = 2,  // 港股（hk）
    MARKET_US = 3,  // 美股（gb_）
};

struct StockConfig {
    const char* code;       // API 请求代码，如 "sh600519"
    const char* name;       // 显示名称（UTF-8 硬编码，避免 GBK 解码）
    StockMarket market;
};

struct StockData {
    float current_price;
    float yesterday_close;
    float change_pct;       // 涨跌幅 %
    float high;
    float low;
    bool valid;
};

// 默认股票列表
extern const StockConfig kDefaultStocks[MAX_STOCKS];

// 从新浪财经 API 批量获取股票数据
// 返回成功获取的股票数量
int FetchStockData(const StockConfig* configs, StockData* results, int count);

#endif
