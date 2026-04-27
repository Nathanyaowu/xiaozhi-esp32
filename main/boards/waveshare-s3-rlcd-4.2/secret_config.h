#pragma once

// ============================================================
// 敏感配置模板 - 请复制此文件为 secret_config.h 并填入真实值
// cp secret_config.h.example secret_config.h
//
// 注意：secret_config.h 已被 .gitignore 忽略，不会提交到仓库
// ============================================================

// 和风天气 API 配置
// 申请地址：https://dev.qweather.com/
#define WEATHER_API_KEY     "your_qweather_api_key_here"
#define WEATHER_API_HOST    "your_host.re.qweatherapi.com"

// 时区配置
// 格式参考：https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
// 常见值：
//   中国 (UTC+8):  "CST-8"
//   日本 (UTC+9):  "JST-9"
//   美西 (UTC-8):  "PST8PDT"
//   美东 (UTC-5):  "EST5EDT"
#define TIMEZONE_STRING     "CST-8"

// NTP 时间同步服务器
// 国内推荐：ntp.aliyun.com / ntp.tencent.com
// 海外推荐：pool.ntp.org / time.google.com
#define NTP_SERVER          "ntp.aliyun.com"

// 新浪财经股票行情 API
// STOCK_API_BASE_URL: 行情接口地址，代码会在末尾拼接股票代码发起请求
//   例如最终请求: http://hq.sinajs.cn/list=sh600519,sz000001
// STOCK_API_REFERER: HTTP 请求头中的 Referer 字段，新浪接口有防盗链校验，
//   不带此头或值不匹配会返回空数据
// 如需更换数据源（如腾讯财经），两个值需同步修改
#define STOCK_API_BASE_URL  "http://hq.sinajs.cn/list="
#define STOCK_API_REFERER   "http://finance.sina.com.cn/"
