// Web 配置服务器
//
// 基于 esp_http_server 的轻量 HTTP 服务，WiFi 连接后常驻运行。
// 提供设备配置的 Web 管理界面（股票自选、后续可扩展其他配置）。
// esp_http_server 内部自带 task，无需额外创建 FreeRTOS 任务。

#ifndef __WEB_CONFIG_SERVER_H__
#define __WEB_CONFIG_SERVER_H__

#include <esp_http_server.h>

class WebConfigServer {
public:
    // 启动 HTTP 服务器（端口 80）
    // 调用一次即可，重复调用安全（内部有 started 标志）
    static void Start();

    // 停止 HTTP 服务器（一般不需要调用）
    static void Stop();

    // 是否已启动
    static bool IsRunning();

private:
    static httpd_handle_t server_;
    static bool started_;

    // URI handlers
    static esp_err_t HandleGetIndex(httpd_req_t *req);
    static esp_err_t HandleGetStockConfig(httpd_req_t *req);
    static esp_err_t HandlePostStockConfig(httpd_req_t *req);
};

#endif
