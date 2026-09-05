# STM32F407 NFC 考勤系统

基于 **STM32F407VETx + FreeRTOS** 的 NFC 考勤终端，并配套 Python/Tkinter 上位机。项目覆盖 RC522 卡片管理、OLED 人机界面、W25Q128 配置与考勤记录持久化、ESP-01 联网/NTP/天气，以及自定义串口协议和 PC 端发卡工具。

> 本仓库按求职作品集方式整理：保留本人项目的核心业务代码、板级驱动、CubeMX 配置和上位机源码；不重复上传 STM32 HAL、CMSIS、FreeRTOS 等官方 SDK 源码及 Debug/编译缓存。真实 Wi-Fi、天气 API 等配置不进入版本管理。

## 主要功能

- **NFC 卡管理**：RC522 + MIFARE Classic，支持发卡、读卡、清卡及不同卡类型处理。
- **图文卡数据**：头像 48×64 1bit、姓名/部门 80×16 1bit，经串口分块传输并写入卡片，使用 CRC-16/XMODEM 校验。
- **考勤记录**：W25Q128 保存设备配置与考勤日志，支持 `ENTRY / EXIT / BOTH` 模式。
- **FreeRTOS 多任务**：GUI、NFC、按键、UART、Wi-Fi 等任务解耦运行。
- **OLED 界面**：显示时间、日期、星期、设备编号、考勤模式、温度及刷卡结果。
- **联网功能**：ESP-01 通过 USART6 联网，支持 NTP 校时与天气信息获取。
- **PC 上位机**：Python/Tkinter + pyserial + Pillow，支持发卡、读卡、清卡、预览和串口交互。

## 技术栈

| 模块 | 方案 |
|---|---|
| MCU | STM32F407VETx |
| RTOS | FreeRTOS / CMSIS-RTOS V2 |
| NFC | RC522 / SPI |
| 显示 | SSD1306 OLED / I2C |
| 外部 Flash | W25Q128 / SPI |
| 温度 | DS18B20 |
| Wi-Fi | ESP-01 / USART6 |
| PC 通信 | USART1 115200 8N1 |
| 上位机 | Python / Tkinter / pyserial / Pillow |

## 软件结构

```text
.
├── mcu/
│   ├── App/                     # 设备配置、考勤日志等业务模块
│   │   ├── app_config.c/.h
│   │   ├── attendance_log.c/.h
│   │   └── app_wifi_config.example.h
│   ├── Bsp/                     # 项目使用的板级驱动
│   │   ├── ESP01/
│   │   ├── NFC/
│   │   ├── OLED/
│   │   ├── RTC/
│   │   ├── UartDrv/
│   │   ├── w25qxx/
│   │   ├── ds18b20/
│   │   ├── Key/
│   │   ├── LED/
│   │   └── delay/
│   ├── Core/
│   │   ├── Inc/
│   │   └── Src/
│   │       ├── main.c
│   │       └── freertos.c       # 主要任务与业务流程
│   ├── NFCAttend.ioc            # CubeMX 配置
│   ├── Makefile
│   ├── STM32F407XX_FLASH.ld
│   └── startup_stm32f407xx.s
├── pc/
│   ├── issue_card.py             # CLI 上位机
│   ├── issue_card_gui.py         # GUI 上位机
│   └── requirements.txt
├── .gitignore
└── README.md
```

## FreeRTOS 任务设计

`freertos.c` 中包含项目的主要业务逻辑，核心任务包括：

- `guiTask`：OLED 页面与状态刷新。
- `nfcTask`：RC522 卡片检测、读写与考勤流程。
- `keyTask`：按键扫描、消抖和交互。
- `uartTask`：USART1 命令接收与上位机协议处理。
- `wifiTask`：ESP-01 联网、NTP 校时和天气更新。

## 串口协议

上位机通过 USART1 与 MCU 通信，主要命令包括：

```text
PING / HELP
ISSUE_BEGIN / ISSUE_CANCEL
IMAGE_BEGIN / IMAGE_DATA / IMAGE_END / COMMIT
CARD_READ / CARD_CLEAR
CONFIG_GET / CONFIG_SET_MODE
```

图像与文本数据按固定块分片发送，MCU 完成 CRC 校验后再提交写卡；读卡流程返回 UID、ID、卡类型、数据长度、CRC 与 payload。

## Wi-Fi / NTP / 天气配置

仓库只保留示例文件：

```text
mcu/App/app_wifi_config.example.h
```

实际使用时复制为：

```bash
cp mcu/App/app_wifi_config.example.h mcu/App/app_wifi_config.h
```

再填写自己的 Wi-Fi 和天气 API 配置。`app_wifi_config.h` 已加入 `.gitignore`，避免将真实凭据提交到仓库。

## MCU 工程说明

本仓库是作品集核心源码版，因此未重复收录 STM32CubeF4 HAL、CMSIS 和 FreeRTOS 官方源码。`NFCAttend.ioc`、启动文件、链接脚本以及项目自有源码均保留，可用于查看外设配置、任务划分和业务实现。

## PC 上位机

安装依赖：

```bash
cd pc
python -m venv .venv
pip install -r requirements.txt
```

启动 GUI：

```bash
python issue_card_gui.py
```

也可以使用 `issue_card.py` 通过命令行完成数据生成、预览和串口发送。

## 仓库整理说明

- 未提交 `Debug/`、`build/`、目标文件、链接输出和 IDE 缓存。
- 未提交真实 `app_wifi_config.h`。
- 保留项目核心业务代码和自编写/修改的板级驱动，方便招聘方直接查看关键实现。
