# Waveshare ESP32-S3-RLCD-4.2 编译指南

在新电脑上编译和烧录本项目的完整步骤。

## 前置条件

- **ESP-IDF v5.5.x**（推荐 5.5.1 或 5.5.2）
- **Python 3.8+**
- **Git**

ESP-IDF 安装参考：https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/

## 编译步骤

### 1. 克隆仓库并切换分支

```bash
git clone https://github.com/Nathanyaowu/xiaozhi-esp32.git
cd xiaozhi-esp32
git checkout feat/rlcd-enhancements
```

> ⚠️ **必须切到 `feat/rlcd-enhancements` 分支**，`main` 分支不包含本开发板的定制功能。

### 2. 创建密钥配置文件

```bash
cd main/boards/waveshare-s3-rlcd-4.2
cp secret_config.h.example secret_config.h
```

编辑 `secret_config.h`，填入你的真实配置：

| 配置项 | 说明 | 获取方式 |
|---|---|---|
| `WEATHER_API_KEY` | 和风天气 API Key | https://dev.qweather.com/ 注册 |
| `WEATHER_API_HOST` | 和风天气 API Host | 控制台中查看（格式：`xxx.re.qweatherapi.com`） |
| `TIMEZONE_STRING` | 时区 | 中国用 `"CST-8"`，其他参考文件内注释 |
| `NTP_SERVER` | NTP 服务器 | 国内推荐 `"ntp.aliyun.com"` |
| `STOCK_API_BASE_URL` | 股票行情 API 地址 | 默认新浪财经，一般无需修改 |
| `STOCK_API_REFERER` | 股票 API Referer | 配合 BASE_URL 使用，一般无需修改 |

> ⚠️ `secret_config.h` 已被 `.gitignore` 忽略，不会被提交到仓库。**不创建此文件编译会报错。**

### 3. 加载 ESP-IDF 环境

```bash
# 回到项目根目录
cd /path/to/xiaozhi-esp32

# 加载 ESP-IDF（路径根据你的安装位置调整）
source ~/esp/esp-idf/export.sh
```

### 4. 设置目标芯片并选择板子

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在 menuconfig 中：
```
Xiaozhi Assistant → Board Type → Waveshare ESP32-S3-RLCD-4.2
```

保存退出（`S` 保存 → `Q` 退出）。

### 5. 编译

```bash
idf.py build
```

首次编译约需 5-10 分钟（取决于机器性能），后续增量编译很快。

### 6. 烧录

```bash
# 烧录并打开串口监控
idf.py flash monitor

# 仅烧录（不监控）
idf.py flash

# 仅监控串口
idf.py monitor
```

退出串口监控：`Ctrl + ]`

## 常见问题

### menuconfig 里找不到板子型号

确认你在 `feat/rlcd-enhancements` 分支上：

```bash
git branch --show-current
# 应输出：feat/rlcd-enhancements
```

### 编译报 `secret_config.h: No such file or directory`

没有创建密钥配置文件，参考上面第 2 步。

### `idf.py: command not found`

ESP-IDF 环境未加载，执行 `source ~/esp/esp-idf/export.sh`（路径按你的实际安装位置）。

### 修改了 `sdkconfig.defaults.esp32s3` 后编译不生效

需要清除 build 目录重新生成：

```bash
rm -rf build
idf.py set-target esp32s3
idf.py menuconfig  # 重新选择板子
idf.py build
```

> ⚠️ 不要用 `idf.py fullclean`，会删除 `managed_components` 目录导致问题。

### 烧录后 WiFi 配网

首次烧录后设备会进入配网模式，使用 EspBlufi App 或热点配网连接 WiFi。详见 [README.md](README.md)。
