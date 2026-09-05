# STM32F407 NFC 考勤系统

基于 **STM32F407VETx + FreeRTOS** 的 NFC 考勤终端，配套 Python 上位机工具。工程覆盖 NFC 发卡/读卡/清卡、OLED 人机界面、W25Q128 配置与考勤记录持久化、Wi-Fi/NTP/天气，以及串口上位机协议。

> 本仓库由实际工程整理而来。已移除本机调试路径、真实网络凭据和个人/学号类展示信息；`app_wifi_config.h` 不纳入版本管理。

## 功能概览

- **NFC 卡管理**：RC522 + MIFARE Classic，支持普通卡、图像卡、管理员卡。
- **发卡数据**：头像、姓名、部门等数据经串口下发并写入卡片，支持 CRC 校验。
- **考勤记录**：W25Q128 保存配置与考勤日志，支持 `ENTRY / EXIT / BOTH` 模式。
- **OLED 界面**：日期、星期、大号时间、设备编号、考勤模式、温度和结果页。
- **联网功能**：ESP-01 通过 USART6 联网，可进行 NTP 校时和天气查询。
- **PC 上位机**：Python/Tkinter 工具支持发卡、读卡、清卡、日志查看和串口调试。
- **FreeRTOS 任务化设计**：GUI、按键、UART、NFC、Wi-Fi 等任务解耦运行。

## 硬件与软件

| 模块 | 方案 |
|---|---|
| MCU | STM32F407VETx |
| RTOS | FreeRTOS / CMSIS-RTOS V2 |
| NFC | RC522 / SPI |
| 显示 | SSD1306 OLED / I2C |
| 外部 Flash | W25Q128 / SPI |
| 温度 | DS18B20 |
| Wi-Fi | ESP-01 / USART6 |
| PC 通信 | USART1 |
| 上位机 | Python + Tkinter + pyserial + Pillow |

## 工程结构

```text
.
├── mcu/
│   ├── App/                 # 配置、考勤日志
│   ├── Bsp/                 # RC522/OLED/W25Q128/ESP01/DS18B20/按键/LED 等
│   ├── Core/                # CubeMX 生成代码与 FreeRTOS 任务
│   ├── Drivers/             # STM32 HAL / CMSIS
│   ├── Middlewares/         # FreeRTOS
│   ├── NFCAttend.ioc
│   ├── Makefile
│   └── startup_stm32f407xx.s
├── pc/
│   ├── issue_card.py
│   ├── issue_card_gui.py
│   └── requirements.txt
├── .gitignore
└── README.md
```

## FreeRTOS 任务

工程中主要任务包括：

- `guiTask`：OLED 页面与状态刷新
- `keyTask`：按键扫描与交互
- `uartTask`：USART1 命令接收与协议处理
- `nfcTask`：RC522 卡片检测、读写和考勤逻辑
- `wifiTask`：ESP-01、NTP、天气

## 串口协议示例

USART1 面向 PC 上位机，工程中实现了如下命令族：

```text
PING
HELP
CONFIG_GET
CONFIG_SET_MODE
ISSUE_BEGIN
ISSUE_IMAGE_BEGIN
ISSUE_IMAGE_DATA
ISSUE_IMAGE_END
ISSUE_COMMIT
ISSUE_CANCEL
CARD_READ
CARD_CLEAR
LOG_LIST
LOG_CLEAR
LIST
```

发卡流程采用分阶段状态机，包含图像数据分块传输和 CRC 校验；读卡会返回卡片元数据和 payload。

## Wi-Fi / NTP / 天气配置

真实配置文件不会提交。首次使用时复制示例配置：

```bash
cd mcu
cp App/app_wifi_config.example.h App/app_wifi_config.h
```

然后填写自己的热点和天气 API Key。

## MCU 编译

需要 ARM GNU Toolchain：

```bash
arm-none-eabi-gcc --version
cd mcu
make clean
make
```

Makefile 目标为 `NFCAttend`，生成文件位于 `mcu/build/`。

## PC 上位机

```bash
cd pc
python -m venv .venv
pip install -r requirements.txt
python issue_card_gui.py
```

主要功能包括串口选择、发卡、读卡、清卡、日志查询和协议调试。
