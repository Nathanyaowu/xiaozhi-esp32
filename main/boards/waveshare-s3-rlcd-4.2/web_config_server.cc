// Web 配置服务器实现
//
// 提供 REST API + 嵌入式 HTML 配置页面
// 当前支持：股票自选列表的增删改查
// 后续可扩展：显示设置、备忘录等

#include "web_config_server.h"

#include <cstring>
#include <cstdio>
#include <esp_log.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "settings.h"
#include "stock_data.h"

static const char *TAG = "WebConfig";

// 静态成员初始化
httpd_handle_t WebConfigServer::server_ = nullptr;
bool WebConfigServer::started_ = false;

// POST body 最大长度（防止 DoS）
#define MAX_POST_BODY_LEN 1024

// ============================================================
// 嵌入式 HTML 配置页面
// ============================================================
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>设备配置</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#f5f5f5;padding:16px;max-width:600px;margin:0 auto}
h2{margin-bottom:12px;color:#333}
.card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,.1)}
table{width:100%;border-collapse:collapse;font-size:14px}
th,td{padding:8px;text-align:left;border-bottom:1px solid #eee}
th{color:#666;font-weight:500}
.del-btn{color:#e74c3c;cursor:pointer;border:none;background:none;font-size:16px;padding:4px 8px}
.del-btn:hover{background:#fde8e8;border-radius:4px}
.add-form{display:flex;flex-wrap:wrap;gap:8px;align-items:flex-end}
.add-form label{display:flex;flex-direction:column;font-size:12px;color:#666}
.add-form input,.add-form select{margin-top:4px;padding:6px 8px;border:1px solid #ddd;border-radius:4px;font-size:14px}
.add-form input{width:120px}
.btn{padding:8px 16px;border:none;border-radius:4px;cursor:pointer;font-size:14px}
.btn-add{background:#3498db;color:#fff}
.btn-add:hover{background:#2980b9}
.msg{padding:8px 12px;border-radius:4px;margin-bottom:12px;display:none;font-size:14px}
.msg.ok{display:block;background:#d4edda;color:#155724}
.msg.err{display:block;background:#f8d7da;color:#721c24}
.empty{color:#999;text-align:center;padding:20px}
</style>
</head>
<body>
<h2>📈 股票自选配置</h2>
<div id="msg" class="msg"></div>
<div class="card">
<table>
<thead><tr><th>代码</th><th>名称</th><th>市场</th><th></th></tr></thead>
<tbody id="list"></tbody>
</table>
<div id="empty" class="empty" style="display:none">暂无自选股票</div>
</div>
<div class="card">
<h3 style="margin-bottom:8px;font-size:14px;color:#666">添加股票</h3>
<div class="add-form">
<label>代码<input id="code" placeholder="如 600519"></label>
<label>名称<input id="name" placeholder="如 贵州茅台"></label>
<label>市场
<select id="market">
<option value="0">A股(沪)</option>
<option value="1">A股(深)</option>
<option value="2">港股</option>
<option value="3">美股</option>
</select>
</label>
<button class="btn btn-add" onclick="addStock()">添加</button>
</div>
<div style="margin-top:10px;font-size:12px;color:#888;line-height:1.8">
<b>代码格式说明：</b><br>
A股(沪)：主板600/601/603、科创板688、ETF 51x/58x（输入6位数字）<br>
A股(深)：主板000/001、中小板002、创业板300/301、ETF 159xxx<br>
港股：5位数字，补零（如腾讯填 00700）<br>
美股：英文代号，小写（如苹果填 aapl）
</div>
</div>
<div class="card">
<h3 style="margin-bottom:8px;font-size:14px;color:#666">刷新设置</h3>
<p style="font-size:12px;color:#888;margin-bottom:8px">股票页按此间隔刷新(5~300秒)，非股票页固定60秒刷新一次</p>
<div class="add-form">
<label>刷新间隔<input id="interval" type="number" min="5" max="300" value="30" style="width:60px">秒</label>
</div>
<button class="btn btn-add" style="margin-top:10px" onclick="saveInterval()">保存</button>
</div>
<script>
let stocks=[];
const MKT_PREFIX=['sh','sz','hk','gb_'];
const MKT_NAME=['A股(沪)','A股(深)','港股','美股'];

function showMsg(text,ok){
  const m=document.getElementById('msg');
  m.textContent=text;
  m.className='msg '+(ok?'ok':'err');
  setTimeout(()=>{m.className='msg'},3000);
}

function render(){
  const tb=document.getElementById('list');
  const emp=document.getElementById('empty');
  if(stocks.length===0){tb.innerHTML='';emp.style.display='block';return;}
  emp.style.display='none';
  tb.innerHTML=stocks.map((s,i)=>
    `<tr><td>${s.code}</td><td>${s.name}</td><td>${MKT_NAME[s.market]||'?'}</td><td><button class="del-btn" onclick="del(${i})">✕</button></td></tr>`
  ).join('');
}

function del(i){
  stocks.splice(i,1);
  save();
}

function addStock(){
  if(stocks.length>=5){showMsg('最多5支股票',false);return;}
  const code=document.getElementById('code').value.trim();
  const name=document.getElementById('name').value.trim();
  const market=parseInt(document.getElementById('market').value);
  if(!code){showMsg('请输入股票代码',false);return;}
  if(!name){showMsg('请输入股票名称',false);return;}
  if(name.length>32){showMsg('名称过长(最多32字符)',false);return;}
  // 校验代码格式：纯数字(A股/港股)或字母(美股)
  const codeRegex=(market<=2)?/^\d{4,6}$/:/^[a-zA-Z]{1,5}$/;
  if(!codeRegex.test(code)){showMsg('代码格式错误',false);return;}
  const fullCode=MKT_PREFIX[market]+code;
  // 去重
  if(stocks.some(s=>s.code===fullCode)){showMsg('已存在该股票',false);return;}
  stocks.push({code:fullCode,name:name,market:market});
  document.getElementById('code').value='';
  document.getElementById('name').value='';
  save();
}

function save(){
  fetch('/api/stock',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(stocks)})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'保存失败')});return r.json()})
  .then(()=>{showMsg('保存成功',true);render()})
  .catch(e=>showMsg(e.message,false));
}

function saveInterval(){
  const v=parseInt(document.getElementById('interval').value);
  if(isNaN(v)||v<5||v>300){showMsg('间隔范围 5~300 秒',false);return;}
  fetch('/api/stock/interval',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({interval:v})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'保存失败')});return r.json()})
  .then(()=>showMsg('刷新间隔已保存',true))
  .catch(e=>showMsg(e.message,false));
}

fetch('/api/stock').then(r=>r.json()).then(d=>{stocks=d;render()}).catch(()=>render());
fetch('/api/stock/interval').then(r=>r.json()).then(d=>{document.getElementById('interval').value=d.interval||30}).catch(()=>{});
</script>
</body>
</html>
)rawliteral";

// ============================================================
// 股票代码格式校验
// 合法格式: sh+6位数字, sz+6位数字, hk+5位数字, gb_+1~5字母
// ============================================================
static bool ValidateStockCode(const char *code) {
    if (!code) return false;
    size_t len = strlen(code);

    if (strncmp(code, "sh", 2) == 0) {
        if (len != 8) return false;
        for (int i = 2; i < 8; i++) {
            if (code[i] < '0' || code[i] > '9') return false;
        }
        return true;
    }
    if (strncmp(code, "sz", 2) == 0) {
        if (len != 8) return false;
        for (int i = 2; i < 8; i++) {
            if (code[i] < '0' || code[i] > '9') return false;
        }
        return true;
    }
    if (strncmp(code, "hk", 2) == 0) {
        if (len < 6 || len > 7) return false;  // hk + 4~5位数字
        for (size_t i = 2; i < len; i++) {
            if (code[i] < '0' || code[i] > '9') return false;
        }
        return true;
    }
    if (strncmp(code, "gb_", 3) == 0) {
        size_t sym_len = len - 3;
        if (sym_len < 1 || sym_len > 5) return false;
        for (size_t i = 3; i < len; i++) {
            char c = code[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
        }
        return true;
    }
    return false;
}

// 校验 market 值是否与 code 前缀匹配
static bool ValidateMarketMatch(const char *code, int market) {
    if (market == 0) return strncmp(code, "sh", 2) == 0;
    if (market == 1) return strncmp(code, "sz", 2) == 0;
    if (market == 2) return strncmp(code, "hk", 2) == 0;
    if (market == 3) return strncmp(code, "gb_", 3) == 0;
    return false;
}

// ============================================================
// URI Handlers
// ============================================================

esp_err_t WebConfigServer::HandleGetIndex(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebConfigServer::HandleGetStockConfig(httpd_req_t *req) {
    Settings settings("stock", false);
    std::string json = settings.GetString("list", "");

    httpd_resp_set_type(req, "application/json");
    if (json.empty()) {
        // 返回硬编码默认值
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < MAX_STOCKS; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "code", kDefaultStocks[i].code);
            cJSON_AddStringToObject(item, "name", kDefaultStocks[i].name);
            cJSON_AddNumberToObject(item, "market", (int)kDefaultStocks[i].market);
            cJSON_AddItemToArray(arr, item);
        }
        char *out = cJSON_PrintUnformatted(arr);
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        cJSON_free(out);
        cJSON_Delete(arr);
    } else {
        httpd_resp_send(req, json.c_str(), json.length());
    }
    return ESP_OK;
}

esp_err_t WebConfigServer::HandlePostStockConfig(httpd_req_t *req) {
    // 检查 body 长度
    if (req->content_len > MAX_POST_BODY_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"body too large\"}");
        return ESP_OK;
    }

    // 读取 body
    char buf[MAX_POST_BODY_LEN + 1];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    // 解析 JSON
    cJSON *arr = cJSON_Parse(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid JSON format\"}");
        return ESP_OK;
    }

    int count = cJSON_GetArraySize(arr);
    if (count > MAX_STOCKS) {
        cJSON_Delete(arr);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char err[64];
        snprintf(err, sizeof(err), "{\"error\":\"stock count max %d\"}", MAX_STOCKS);
        httpd_resp_sendstr(req, err);
        return ESP_OK;
    }

    // 逐项校验
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *code = cJSON_GetObjectItem(item, "code");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *market = cJSON_GetObjectItem(item, "market");

        if (!cJSON_IsString(code) || !cJSON_IsString(name) || !cJSON_IsNumber(market)) {
            cJSON_Delete(arr);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            char err[80];
            snprintf(err, sizeof(err), "{\"error\":\"item %d: missing or invalid fields\"}", i);
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }

        int m = market->valueint;
        if (m < 0 || m > 3) {
            cJSON_Delete(arr);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            char err[80];
            snprintf(err, sizeof(err), "{\"error\":\"item %d: market must be 0-3\"}", i);
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }

        if (!ValidateStockCode(code->valuestring)) {
            cJSON_Delete(arr);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            char err[80];
            snprintf(err, sizeof(err), "{\"error\":\"item %d: invalid stock code format\"}", i);
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }

        if (!ValidateMarketMatch(code->valuestring, m)) {
            cJSON_Delete(arr);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            char err[80];
            snprintf(err, sizeof(err), "{\"error\":\"item %d: code prefix doesn't match market\"}", i);
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }

        size_t name_len = strlen(name->valuestring);
        if (name_len == 0 || name_len > 32) {
            cJSON_Delete(arr);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            char err[80];
            snprintf(err, sizeof(err), "{\"error\":\"item %d: name length must be 1-32\"}", i);
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }
    }

    // 校验通过，存入 NVS
    char *json_out = cJSON_PrintUnformatted(arr);
    {
        Settings settings("stock", true);
        settings.SetString("list", json_out);
    }
    ESP_LOGI(TAG, "股票配置已更新: %s", json_out);
    cJSON_free(json_out);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // 唤醒 StockFetchTask 立即拉取新配置
    extern TaskHandle_t g_stock_fetch_task_handle;
    if (g_stock_fetch_task_handle) {
        xTaskNotifyGive(g_stock_fetch_task_handle);
    }

    return ESP_OK;
}

// ============================================================
// 刷新间隔 GET/POST
// ============================================================

static esp_err_t HandleGetInterval(httpd_req_t *req) {
    Settings settings("stock", false);
    int interval = settings.GetInt("interval", 30);
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"interval\":%d}", interval);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandlePostInterval(httpd_req_t *req) {
    char buf[64];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *val = cJSON_GetObjectItem(root, "interval");
    if (!cJSON_IsNumber(val) || val->valueint < 5 || val->valueint > 300) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"interval must be 5~300\"}");
        return ESP_OK;
    }

    int interval = val->valueint;
    cJSON_Delete(root);

    {
        Settings settings("stock", true);
        settings.SetInt("interval", interval);
    }
    ESP_LOGI(TAG, "刷新间隔已更新: %d 秒", interval);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    extern TaskHandle_t g_stock_fetch_task_handle;
    if (g_stock_fetch_task_handle) {
        xTaskNotifyGive(g_stock_fetch_task_handle);
    }

    return ESP_OK;
}

// ============================================================
// Server 启动/停止
// ============================================================

void WebConfigServer::Start() {
    if (started_) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败: %s", esp_err_to_name(err));
        return;
    }

    // 注册路由
    httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleGetIndex,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_get_stock = {
        .uri = "/api/stock",
        .method = HTTP_GET,
        .handler = HandleGetStockConfig,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_stock = {
        .uri = "/api/stock",
        .method = HTTP_POST,
        .handler = HandlePostStockConfig,
        .user_ctx = nullptr
    };

    httpd_register_uri_handler(server_, &uri_index);
    httpd_register_uri_handler(server_, &uri_get_stock);
    httpd_register_uri_handler(server_, &uri_post_stock);

    httpd_uri_t uri_get_interval = {
        .uri = "/api/stock/interval",
        .method = HTTP_GET,
        .handler = HandleGetInterval,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_interval = {
        .uri = "/api/stock/interval",
        .method = HTTP_POST,
        .handler = HandlePostInterval,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &uri_get_interval);
    httpd_register_uri_handler(server_, &uri_post_interval);

    started_ = true;
    ESP_LOGI(TAG, "Web 配置服务器已启动 (端口 80)");
}

void WebConfigServer::Stop() {
    if (!started_ || !server_) return;
    httpd_stop(server_);
    server_ = nullptr;
    started_ = false;
    ESP_LOGI(TAG, "Web 配置服务器已停止");
}

bool WebConfigServer::IsRunning() {
    return started_;
}
