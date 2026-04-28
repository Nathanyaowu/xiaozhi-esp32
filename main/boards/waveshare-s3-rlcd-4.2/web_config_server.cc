// Web 配置服务器实现
//
// 提供 REST API + 嵌入式 HTML 配置页面
// 当前支持：股票自选列表的增删改查
// 后续可扩展：显示设置、备忘录等

#include "web_config_server.h"

#include <cstring>
#include <cstdio>
#include <atomic>
#include <esp_log.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_client.h>

#include "settings.h"
#include "stock_data.h"
#include "custom_lcd_display.h"
#include "board.h"
#include "audio_codec.h"
#include "application.h"
#include "managers/weather_manager.h"

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
<h2>⚙️ 系统设置</h2>
<div id="msg-sys" class="msg"></div>
<div class="card">
<h3 style="margin-bottom:8px;font-size:14px;color:#666">音量控制</h3>
<div class="add-form" style="align-items:center">
<input id="volume" type="range" min="0" max="100" value="50" style="width:200px" oninput="document.getElementById('vol-val').textContent=this.value+'%'">
<span id="vol-val" style="font-size:14px;min-width:40px">50%</span>
<button class="btn btn-add" onclick="saveVolume()">设置</button>
</div>
<p style="font-size:12px;color:#888;margin-top:8px">快捷档位：0% / 34% / 67% / 100%（与BOOT长按一致）</p>
<h3 style="margin:16px 0 8px;font-size:14px;color:#666">天气城市</h3>
<div class="add-form" style="align-items:center">
<select id="city" style="width:160px;padding:4px">
<option value="">自动(默认北京)</option>
<option value="beijing">北京</option>
<option value="shanghai">上海</option>
<option value="guangzhou">广州</option>
<option value="shenzhen">深圳</option>
<option value="chengdu">成都</option>
<option value="hangzhou">杭州</option>
<option value="wuhan">武汉</option>
<option value="xian">西安</option>
<option value="nanjing">南京</option>
<option value="chongqing">重庆</option>
<option value="tianjin">天津</option>
<option value="suzhou">苏州</option>
<option value="zhengzhou">郑州</option>
<option value="changsha">长沙</option>
<option value="dongguan">东莞</option>
<option value="foshan">佛山</option>
<option value="kunming">昆明</option>
<option value="hefei">合肥</option>
<option value="jinan">济南</option>
<option value="fuzhou">福州</option>
<option value="dalian">大连</option>
<option value="xiamen">厦门</option>
<option value="taiyuan">太原</option>
<option value="shenyang">沈阳</option>
<option value="nanning">南宁</option>
<option value="guiyang">贵阳</option>
<option value="shijiazhuang">石家庄</option>
<option value="harbin">哈尔滨</option>
<option value="changchun">长春</option>
<option value="lhasa">拉萨</option>
<option value="urumqi">乌鲁木齐</option>
<option value="hohhot">呼和浩特</option>
<option value="haikou">海口</option>
<option value="lanzhou">兰州</option>
<option value="yinchuan">银川</option>
<option value="xining">西宁</option>
<option value="hongkong">香港</option>
<option value="macau">澳门</option>
</select>
<button class="btn btn-add" onclick="saveCity()">保存</button>
</div>
<p style="font-size:12px;color:#888;margin-top:8px">保存后将立即同步天气数据</p>
<h3 style="margin:16px 0 8px;font-size:14px;color:#666">番茄钟</h3>
<div class="add-form" style="align-items:center;flex-wrap:wrap;gap:8px">
<label style="font-size:13px">专注 <input id="pomo-focus" type="number" min="1" max="300" value="25" style="width:50px;padding:4px"> 分钟</label>
<label style="font-size:13px">休息 <input id="pomo-break" type="number" min="1" max="300" value="5" style="width:50px;padding:4px"> 分钟</label>
<button class="btn btn-add" onclick="savePomodoro()">保存</button>
</div>
<p style="font-size:12px;color:#888;margin-top:8px">长按USER键启动番茄钟（番茄钟页面时）</p>
</div>
<h2 style="margin-top:20px">🎵 音乐播放</h2>
<div id="msg-music" class="msg"></div>
<div class="card">
<h3 style="margin-bottom:8px;font-size:14px;color:#666">搜索音乐</h3>
<div class="add-form" style="align-items:center">
<input id="music-kw" placeholder="歌名/歌手" style="width:160px;padding:6px 8px;border:1px solid #ddd;border-radius:4px;font-size:14px">
<button class="btn btn-add" onclick="searchMusic()">搜索</button>
</div>
<div id="music-results" style="margin-top:12px"></div>
<p style="font-size:12px;color:#888;margin-top:8px">搜索后点击歌曲即可播放；URL失效可重新搜索</p>
</div>
<h2 style="margin-top:20px">📝 备忘录</h2>
<div id="msg-memo" class="msg"></div>
<div class="card">
<table>
<thead><tr><th>日期</th><th>时间</th><th>内容</th><th></th></tr></thead>
<tbody id="memolist"></tbody>
</table>
<div id="memoempty" class="empty" style="display:none">暂无备忘</div>
</div>
<div class="card">
<h3 style="margin-bottom:8px;font-size:14px;color:#666">添加备忘</h3>
<div class="add-form">
<label>日期<input id="memodate" type="date" style="width:140px"></label>
<label>时间<input id="memotime" placeholder="如 15:00 (可选)" style="width:120px"></label>
<label>内容<input id="memocontent" placeholder="备忘内容" style="width:160px"></label>
<button class="btn btn-add" onclick="addMemo()">添加</button>
</div>
<p style="font-size:12px;color:#888;margin-top:8px">日期不填=当天触发；时间不填=仅显示不提醒</p>
</div>
<h2 style="margin-top:20px">📈 股票自选配置</h2>
<div id="msg-stock" class="msg"></div>
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

function showMsg(text,ok,target){
  const m=document.getElementById(target||'msg-stock');
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
  const codeRegex=(market<=2)?/^\d{4,6}$/:/^[a-zA-Z][a-zA-Z0-9_.]{0,9}$/;
  if(!codeRegex.test(code)){showMsg('代码格式错误',false);return;}
  const fullCode=MKT_PREFIX[market]+code.toLowerCase();
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
  if(isNaN(v)||v<5||v>300){showMsg('间隔范围 5~300 秒',false,'msg-stock');return;}
  fetch('/api/stock/interval',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({interval:v})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'保存失败')});return r.json()})
  .then(()=>showMsg('刷新间隔已保存',true,'msg-stock'))
  .catch(e=>showMsg(e.message,false,'msg-stock'));
}

fetch('/api/stock').then(r=>r.json()).then(d=>{stocks=d;render()}).catch(()=>render());
fetch('/api/stock/interval').then(r=>r.json()).then(d=>{document.getElementById('interval').value=d.interval||30}).catch(()=>{});

let memos=[];
function renderMemo(){
  const tb=document.getElementById('memolist');
  const emp=document.getElementById('memoempty');
  if(memos.length===0){tb.innerHTML='';emp.style.display='block';return;}
  emp.style.display='none';
  tb.innerHTML=memos.map((m,i)=>
    `<tr><td>${m.d||'今天'}</td><td>${m.t||'-'}</td><td>${m.c}</td><td><button class="del-btn" onclick="delMemo(${i})">✕</button></td></tr>`
  ).join('');
}
function delMemo(i){
  memos.splice(i,1);
  saveMemo();
}
function addMemo(){
  if(memos.length>=10){showMsg('最多10条备忘',false,'msg-memo');return;}
  const d=document.getElementById('memodate').value;
  const t=document.getElementById('memotime').value.trim();
  const c=document.getElementById('memocontent').value.trim();
  if(!c){showMsg('请输入备忘内容',false,'msg-memo');return;}
  if(c.length>48){showMsg('内容过长(最多48字符)',false,'msg-memo');return;}
  if(t&&!/^\d{2}:\d{2}$/.test(t)){showMsg('时间格式应为 HH:MM',false,'msg-memo');return;}
  if(t){const[h,m]=[parseInt(t),parseInt(t.slice(3))];if(h>23||m>59){showMsg('时间无效(00:00~23:59)',false,'msg-memo');return;}}
  memos.push({t:t,c:c,d:d});
  document.getElementById('memodate').value='';
  document.getElementById('memotime').value='';
  document.getElementById('memocontent').value='';
  saveMemo();
}
function saveMemo(){
  fetch('/api/memo',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(memos)})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'保存失败')});return r.json()})
  .then(()=>{showMsg('备忘已保存',true,'msg-memo');renderMemo()})
  .catch(e=>showMsg(e.message,false,'msg-memo'));
}
fetch('/api/memo').then(r=>r.json()).then(d=>{memos=d;renderMemo()}).catch(()=>renderMemo());

function saveVolume(){
  const v=parseInt(document.getElementById('volume').value);
  fetch('/api/volume',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({volume:v})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'设置失败')});return r.json()})
  .then(()=>showMsg('音量已设置为 '+v+'%',true,'msg-sys'))
  .catch(e=>showMsg(e.message,false,'msg-sys'));
}
fetch('/api/volume').then(r=>r.json()).then(d=>{
  const v=d.volume||50;
  document.getElementById('volume').value=v;
  document.getElementById('vol-val').textContent=v+'%';
}).catch(()=>{});

function saveCity(){
  const c=document.getElementById('city').value;
  fetch('/api/weather/city',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({city:c})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'保存失败')});return r.json()})
  .then(()=>showMsg('天气城市已保存，正在同步...',true,'msg-sys'))
  .catch(e=>showMsg(e.message,false,'msg-sys'));
}
fetch('/api/weather/city').then(r=>r.json()).then(d=>{
  if(d.city)document.getElementById('city').value=d.city;
}).catch(()=>{});

function savePomodoro(){
  const f=parseInt(document.getElementById('pomo-focus').value)||25;
  const b=parseInt(document.getElementById('pomo-break').value)||5;
  fetch('/api/pomodoro',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({focus:f,break_min:b})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'保存失败')});return r.json()})
  .then(()=>showMsg('番茄钟设置已保存',true,'msg-sys'))
  .catch(e=>showMsg(e.message,false,'msg-sys'));
}
fetch('/api/pomodoro').then(r=>r.json()).then(d=>{
  document.getElementById('pomo-focus').value=d.focus||25;
  document.getElementById('pomo-break').value=d.break_min||5;
}).catch(()=>{});

function searchMusic(){
  const kw=document.getElementById('music-kw').value.trim();
  if(!kw){showMsg('请输入关键词',false,'msg-music');return;}
  const rd=document.getElementById('music-results');
  rd.innerHTML='<p style="color:#888;font-size:13px">搜索中...</p>';
  fetch('/api/music/search',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({keyword:kw})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'搜索失败')});return r.json()})
  .then(d=>{
    if(!d.list||d.list.length===0){rd.innerHTML='<p style="color:#888;font-size:13px">无结果</p>';return;}
    rd.innerHTML='<table style="width:100%;font-size:13px"><thead><tr><th>歌名</th><th>歌手</th><th></th></tr></thead><tbody>'+
      d.list.map((s,i)=>`<tr><td>${s.name}</td><td>${s.artist}</td><td><button class="btn btn-add" style="padding:4px 10px;font-size:12px" onclick="playMusic(${s.rid},'${s.name.replace(/'/g,"\\'")}','${s.artist.replace(/'/g,"\\'")}')">播放</button></td></tr>`).join('')+
      '</tbody></table>';
  })
  .catch(e=>{rd.innerHTML='';showMsg(e.message,false,'msg-music')});
}

function playMusic(rid,name,artist){
  showMsg('正在获取播放链接...',true,'msg-music');
  fetch('/api/music/play',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({rid:rid,name:name,artist:artist})})
  .then(r=>{if(!r.ok)return r.json().then(j=>{throw new Error(j.error||'播放失败')});return r.json()})
  .then(()=>showMsg('开始播放: '+name,true,'msg-music'))
  .catch(e=>showMsg(e.message,false,'msg-music'));
}
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
        if (sym_len < 1 || sym_len > 10) return false;
        for (size_t i = 3; i < len; i++) {
            char c = code[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
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
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
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
// 备忘录 GET/POST
// ============================================================

static esp_err_t HandleGetMemo(httpd_req_t *req) {
    Settings settings("memo", false);
    std::string json = settings.GetString("items", "[]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

static esp_err_t HandlePostMemo(httpd_req_t *req) {
    char buf[MAX_POST_BODY_LEN];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"expected JSON array\"}");
        return ESP_OK;
    }

    int count = cJSON_GetArraySize(arr);
    if (count > 10) {
        cJSON_Delete(arr);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"max 10 memos\"}");
        return ESP_OK;
    }

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *t = cJSON_GetObjectItem(item, "t");
        cJSON *c = cJSON_GetObjectItem(item, "c");

        if (!cJSON_IsString(c) || strlen(c->valuestring) == 0 || strlen(c->valuestring) > 48) {
            cJSON_Delete(arr);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            char err[80];
            snprintf(err, sizeof(err), "{\"error\":\"item %d: content invalid\"}", i);
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }

        if (t && cJSON_IsString(t) && strlen(t->valuestring) > 0) {
            const char *tv = t->valuestring;
            if (strlen(tv) != 5 || tv[2] != ':' ||
                !isdigit(tv[0]) || !isdigit(tv[1]) || !isdigit(tv[3]) || !isdigit(tv[4])) {
                cJSON_Delete(arr);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                char err[80];
                snprintf(err, sizeof(err), "{\"error\":\"item %d: time format HH:MM\"}", i);
                httpd_resp_sendstr(req, err);
                return ESP_OK;
            }
        }

        cJSON *d = cJSON_GetObjectItem(item, "d");
        if (d && cJSON_IsString(d) && strlen(d->valuestring) > 0) {
            const char *dv = d->valuestring;
            if (strlen(dv) != 10 || dv[4] != '-' || dv[7] != '-') {
                cJSON_Delete(arr);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                char err[80];
                snprintf(err, sizeof(err), "{\"error\":\"item %d: date format YYYY-MM-DD\"}", i);
                httpd_resp_sendstr(req, err);
                return ESP_OK;
            }
        }
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        Settings settings("memo", true);
        settings.SetString("items", json_str);
        ESP_LOGI(TAG, "备忘录已更新: %s", json_str);
        free(json_str);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    extern CustomLcdDisplay* g_display_instance;
    if (g_display_instance) {
        g_display_instance->RefreshMemoDisplay();
    }

    return ESP_OK;
}

// ============================================================
// 音量 GET/POST
// ============================================================

static esp_err_t HandleGetVolume(httpd_req_t *req) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    int vol = codec ? codec->output_volume() : 50;
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"volume\":%d}", vol);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandlePostVolume(httpd_req_t *req) {
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

    cJSON *val = cJSON_GetObjectItem(root, "volume");
    if (!cJSON_IsNumber(val) || val->valueint < 0 || val->valueint > 100) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"volume must be 0~100\"}");
        return ESP_OK;
    }

    int volume = val->valueint;
    cJSON_Delete(root);

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->SetOutputVolume(volume);
        ESP_LOGI(TAG, "Web 设置音量: %d%%", volume);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================
// 天气城市 GET/POST
// ============================================================

struct CityInfo {
    const char* key;
    const char* name;
    double lat;
    double lon;
};

static const CityInfo CITY_TABLE[] = {
    {"beijing",      "北京",     39.90, 116.41},
    {"shanghai",     "上海",     31.23, 121.47},
    {"guangzhou",    "广州",     23.13, 113.26},
    {"shenzhen",     "深圳",     22.54, 114.06},
    {"chengdu",      "成都",     30.57, 104.07},
    {"hangzhou",     "杭州",     30.27, 120.15},
    {"wuhan",        "武汉",     30.58, 114.30},
    {"xian",         "西安",     34.26, 108.94},
    {"nanjing",      "南京",     32.06, 118.80},
    {"chongqing",    "重庆",     29.56, 106.55},
    {"tianjin",      "天津",     39.13, 117.20},
    {"suzhou",       "苏州",     31.30, 120.62},
    {"zhengzhou",    "郑州",     34.75, 113.65},
    {"changsha",     "长沙",     28.23, 112.94},
    {"dongguan",     "东莞",     23.04, 113.75},
    {"foshan",       "佛山",     23.02, 113.12},
    {"kunming",      "昆明",     25.04, 102.71},
    {"hefei",        "合肥",     31.82, 117.23},
    {"jinan",        "济南",     36.65, 116.99},
    {"fuzhou",       "福州",     26.07, 119.31},
    {"dalian",       "大连",     38.91, 121.60},
    {"xiamen",       "厦门",     24.48, 118.09},
    {"taiyuan",      "太原",     37.87, 112.55},
    {"shenyang",     "沈阳",     41.80, 123.43},
    {"nanning",      "南宁",     22.82, 108.37},
    {"guiyang",      "贵阳",     26.65, 106.63},
    {"shijiazhuang", "石家庄",   38.04, 114.51},
    {"harbin",       "哈尔滨",   45.75, 126.65},
    {"changchun",    "长春",     43.88, 125.32},
    {"lhasa",        "拉萨",     29.65, 91.13},
    {"urumqi",       "乌鲁木齐", 43.83, 87.62},
    {"hohhot",       "呼和浩特", 40.84, 111.75},
    {"haikou",       "海口",     20.03, 110.35},
    {"lanzhou",      "兰州",     36.06, 103.83},
    {"yinchuan",     "银川",     38.49, 106.23},
    {"xining",       "西宁",     36.62, 101.78},
    {"hongkong",     "香港",     22.28, 114.15},
    {"macau",        "澳门",    22.20, 113.55},
};
static const int CITY_COUNT = sizeof(CITY_TABLE) / sizeof(CITY_TABLE[0]);

static const CityInfo* FindCity(const char* key) {
    for (int i = 0; i < CITY_COUNT; i++) {
        if (strcmp(CITY_TABLE[i].key, key) == 0) return &CITY_TABLE[i];
    }
    return nullptr;
}

static esp_err_t HandleGetWeatherCity(httpd_req_t *req) {
    Settings settings("weather", false);
    std::string city = settings.GetString("city");
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"city\":\"%s\"}", city.c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandlePostWeatherCity(httpd_req_t *req) {
    char buf[128];
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

    cJSON *val = cJSON_GetObjectItem(root, "city");
    if (!val || !cJSON_IsString(val)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"missing city field\"}");
        return ESP_OK;
    }

    const char* city_key = val->valuestring;
    if (strlen(city_key) > 0 && !FindCity(city_key)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"unknown city\"}");
        return ESP_OK;
    }

    {
        Settings settings("weather", true);
        settings.SetString("city", city_key);
    }

    const CityInfo* info = FindCity(city_key);
    if (info) {
        WeatherManager::getInstance().setCityConfig(info->lat, info->lon, info->name);
        ESP_LOGI(TAG, "Web 设置天气城市: %s (%.2f, %.2f)", info->name, info->lat, info->lon);
    } else {
        WeatherManager::getInstance().clearCityConfig();
        ESP_LOGI(TAG, "Web 清除天气城市配置，使用默认");
    }

    // 触发立即拉取天气
    extern std::atomic<bool> g_force_weather_update;
    g_force_weather_update.store(true);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================
// 番茄钟设置 GET/POST
// ============================================================

static esp_err_t HandleGetPomodoro(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    Settings settings("pomodoro", false);
    int focus = settings.GetInt("focus", 25);
    int break_min = settings.GetInt("break", 5);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"focus\":%d,\"break_min\":%d}", focus, break_min);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandlePostPomodoro(httpd_req_t *req) {
    char buf[256];
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

    cJSON *focus_val = cJSON_GetObjectItem(root, "focus");
    cJSON *break_val = cJSON_GetObjectItem(root, "break_min");
    if (!focus_val || !cJSON_IsNumber(focus_val) || !break_val || !cJSON_IsNumber(break_val)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"missing focus/break_min\"}");
        return ESP_OK;
    }

    int focus = focus_val->valueint;
    int break_min = break_val->valueint;
    if (focus < 1 || focus > 300 || break_min < 1 || break_min > 300) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"focus: 1-300, break: 1-300\"}");
        return ESP_OK;
    }

    {
        Settings settings("pomodoro", true);
        settings.SetInt("focus", focus);
        settings.SetInt("break", break_min);
    }
    ESP_LOGI(TAG, "Web 设置番茄钟: 专注%d分钟, 休息%d分钟", focus, break_min);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================
// 音乐搜索与播放（酷我音乐 API）
// ============================================================

// 酷我 API 响应缓冲区（每条搜索结果约 2.2KB，5条约 11KB）
#define KUWO_RESP_BUF_SIZE 24576

// esp_http_client 事件回调：将响应体追加到用户缓冲区
static esp_err_t kuwo_http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        // user_data 指向 { char* buf, size_t buf_size, size_t cur_len }
        // 为简化，这里用与 stock_data.cc 类似的模式
        char *buf = (char *)evt->user_data;
        size_t cur_len = strlen(buf);
        size_t remaining = KUWO_RESP_BUF_SIZE - cur_len - 1;
        if ((size_t)evt->data_len <= remaining) {
            memcpy(buf + cur_len, evt->data, evt->data_len);
            buf[cur_len + evt->data_len] = '\0';
        }
    }
    return ESP_OK;
}

// 调用酷我搜索 API（旧版 search.kuwo.cn 端点，无需 token 认证）
// 返回 cJSON 数组（调用者负责 cJSON_Delete），nullptr 表示失败
static cJSON* KuwoSearchMusic(const char* keyword, int limit) {
    // URL 编码关键词（简易：中文 UTF-8 百分号编码）
    char encoded_kw[256] = {0};
    const unsigned char *p = (const unsigned char*)keyword;
    char *out = encoded_kw;
    while (*p && (out - encoded_kw) < 240) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *out++ = *p++;
        } else {
            snprintf(out, 4, "%%%02X", *p);
            out += 3;
            p++;
        }
    }
    *out = '\0';

    char url[512];
    snprintf(url, sizeof(url),
        "http://search.kuwo.cn/r.s?all=%s&ft=music&rn=%d&pn=0&encoding=utf8&rformat=json&mobi=1",
        encoded_kw, limit);

    char *resp_buf = (char *)malloc(KUWO_RESP_BUF_SIZE);
    if (!resp_buf) return nullptr;
    resp_buf[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = kuwo_http_event_handler;
    config.user_data = resp_buf;
    config.timeout_ms = 8000;

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    cJSON *result = nullptr;

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200 && strlen(resp_buf) > 10) {
        size_t resp_len = strlen(resp_buf);
        if (resp_len >= KUWO_RESP_BUF_SIZE - 2) {
            ESP_LOGW(TAG, "酷我搜索响应可能被截断: %zu bytes (buf=%d)", resp_len, KUWO_RESP_BUF_SIZE);
        }
        cJSON *root = cJSON_Parse(resp_buf);
        if (root) {
            cJSON *abslist = cJSON_GetObjectItem(root, "abslist");
            if (abslist && cJSON_IsArray(abslist)) {
                // 提取需要的字段，构建精简数组（保持前端 rid/name/artist 接口不变）
                result = cJSON_CreateArray();
                int count = cJSON_GetArraySize(abslist);
                for (int i = 0; i < count && i < limit; i++) {
                    cJSON *item = cJSON_GetArrayItem(abslist, i);
                    cJSON *musicrid = cJSON_GetObjectItem(item, "MUSICRID");
                    cJSON *name = cJSON_GetObjectItem(item, "SONGNAME");
                    cJSON *artist = cJSON_GetObjectItem(item, "ARTIST");
                    if (musicrid && cJSON_IsString(musicrid) && name && artist) {
                        // MUSICRID 格式为 "MUSIC_123456"，提取数字部分
                        const char *rid_str = musicrid->valuestring;
                        if (strncmp(rid_str, "MUSIC_", 6) == 0) {
                            rid_str += 6;
                        }
                        int rid = atoi(rid_str);
                        if (rid > 0) {
                            cJSON *entry = cJSON_CreateObject();
                            cJSON_AddNumberToObject(entry, "rid", rid);
                            cJSON_AddStringToObject(entry, "name", name->valuestring);
                            cJSON_AddStringToObject(entry, "artist", artist->valuestring);
                            cJSON_AddItemToArray(result, entry);
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGW(TAG, "酷我搜索请求失败: err=%d, status=%d", err,
                 err == ESP_OK ? esp_http_client_get_status_code(client) : -1);
    }

    esp_http_client_cleanup(client);
    free(resp_buf);
    return result;
}

// 获取酷我音乐播放 URL（通过 rid，使用 antiserver 端点，无需认证）
// convert_url 返回 HTTP 链接（避免 ESP32 TLS 握手失败），响应为纯文本 URL
static std::string KuwoGetPlayUrl(int rid) {
    char url[256];
    snprintf(url, sizeof(url),
        "http://antiserver.kuwo.cn/anti.s?type=convert_url&rid=%d&format=mp3", rid);

    char *resp_buf = (char *)malloc(4096);
    if (!resp_buf) return "";
    resp_buf[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = kuwo_http_event_handler;
    config.user_data = resp_buf;
    config.timeout_ms = 8000;

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    std::string play_url;

    if (err == ESP_OK && esp_http_client_get_status_code(client) == 200 && strlen(resp_buf) > 10) {
        // convert_url 端点直接返回纯文本 URL（非 JSON）
        // 去除首尾空白
        char *start = resp_buf;
        while (*start == ' ' || *start == '\n' || *start == '\r') start++;
        char *end = start + strlen(start) - 1;
        while (end > start && (*end == ' ' || *end == '\n' || *end == '\r')) *end-- = '\0';
        if (strncmp(start, "http", 4) == 0) {
            play_url = start;
        }
    } else {
        ESP_LOGW(TAG, "酷我播放URL请求失败: err=%d, rid=%d", err, rid);
    }

    esp_http_client_cleanup(client);
    free(resp_buf);
    return play_url;
}

// POST /api/music/search — 搜索音乐
static esp_err_t HandlePostMusicSearch(httpd_req_t *req) {
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *kw = cJSON_GetObjectItem(root, "keyword");
    if (!kw || !cJSON_IsString(kw) || strlen(kw->valuestring) == 0 || strlen(kw->valuestring) > 64) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"keyword required (max 64 chars)\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "音乐搜索: %s", kw->valuestring);
    cJSON *results = KuwoSearchMusic(kw->valuestring, 5);
    cJSON_Delete(root);

    if (!results) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"search failed\"}");
        return ESP_OK;
    }

    // 构建响应 {"list": [...]}
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "list", results);
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    if (json_str) {
        httpd_resp_sendstr(req, json_str);
        cJSON_free(json_str);
    } else {
        httpd_resp_sendstr(req, "{\"list\":[]}");
    }
    return ESP_OK;
}

// POST /api/music/play — 获取播放 URL 并播放
static esp_err_t HandlePostMusicPlay(httpd_req_t *req) {
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *rid_val = cJSON_GetObjectItem(root, "rid");
    cJSON *name_val = cJSON_GetObjectItem(root, "name");
    cJSON *artist_val = cJSON_GetObjectItem(root, "artist");

    if (!rid_val || !cJSON_IsNumber(rid_val)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"rid required\"}");
        return ESP_OK;
    }

    int rid = (int)rid_val->valuedouble;
    std::string name = (name_val && cJSON_IsString(name_val)) ? name_val->valuestring : "";
    std::string artist = (artist_val && cJSON_IsString(artist_val)) ? artist_val->valuestring : "";
    cJSON_Delete(root);

    ESP_LOGI(TAG, "获取播放URL: rid=%d, name=%s, artist=%s", rid, name.c_str(), artist.c_str());

    std::string play_url = KuwoGetPlayUrl(rid);
    if (play_url.empty()) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"failed to get play URL\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "开始播放: %s - %s, URL: %s", name.c_str(), artist.c_str(), play_url.c_str());

    // 调用 Application 播放音乐
    bool ok = Application::GetInstance().PlayMusicFromUrl(play_url, name, artist, "", "");

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"playback failed\"}");
    }
    return ESP_OK;
}

// ============================================================
// Server 启动/停止
// ============================================================

void WebConfigServer::Start() {
    if (started_) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
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

    httpd_uri_t uri_get_memo = {
        .uri = "/api/memo",
        .method = HTTP_GET,
        .handler = HandleGetMemo,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_memo = {
        .uri = "/api/memo",
        .method = HTTP_POST,
        .handler = HandlePostMemo,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &uri_get_memo);
    httpd_register_uri_handler(server_, &uri_post_memo);

    httpd_uri_t uri_get_volume = {
        .uri = "/api/volume",
        .method = HTTP_GET,
        .handler = HandleGetVolume,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_volume = {
        .uri = "/api/volume",
        .method = HTTP_POST,
        .handler = HandlePostVolume,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &uri_get_volume);
    httpd_register_uri_handler(server_, &uri_post_volume);

    httpd_uri_t uri_get_city = {
        .uri = "/api/weather/city",
        .method = HTTP_GET,
        .handler = HandleGetWeatherCity,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_city = {
        .uri = "/api/weather/city",
        .method = HTTP_POST,
        .handler = HandlePostWeatherCity,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &uri_get_city);
    httpd_register_uri_handler(server_, &uri_post_city);

    httpd_uri_t uri_get_pomodoro = {
        .uri = "/api/pomodoro",
        .method = HTTP_GET,
        .handler = HandleGetPomodoro,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_pomodoro = {
        .uri = "/api/pomodoro",
        .method = HTTP_POST,
        .handler = HandlePostPomodoro,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &uri_get_pomodoro);
    httpd_register_uri_handler(server_, &uri_post_pomodoro);

    httpd_uri_t uri_post_music_search = {
        .uri = "/api/music/search",
        .method = HTTP_POST,
        .handler = HandlePostMusicSearch,
        .user_ctx = nullptr
    };
    httpd_uri_t uri_post_music_play = {
        .uri = "/api/music/play",
        .method = HTTP_POST,
        .handler = HandlePostMusicPlay,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server_, &uri_post_music_search);
    httpd_register_uri_handler(server_, &uri_post_music_play);

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
