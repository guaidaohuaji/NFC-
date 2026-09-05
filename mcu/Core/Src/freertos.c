/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "delay_us.h"
#include "led.h"
#include "key.h"
#include "rc522.h"
#include "ssd1306.h"
#include "GUI.h"
#include "splash_bitmap.h"
#include "bsp_rtc.h"
#include "uart_drv.h"
#include "usart.h"
#include "tim.h"
#include <stdio.h>
#include <string.h>
#include "w25qxx.h"
#include "app_config.h"
#include "attendance_log.h"
#include "ds18B20.h"
#include "big_digits.h"
#include "app_wifi_config.h"
#include "esp01s.h"
#include "rtc.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* ====== 应用消息类型 ====== */
typedef enum {
    MSG_INIT_DONE        = 0,  /**< 系统初始化完成 */
    MSG_NFC_CARD         = 1,  /**< NFC 卡片检测 */
    MSG_KEY_SHORT        = 2,  /**< 按键短按事件 */
    MSG_KEY_LONG         = 3,  /**< 按键长按事件 */
    MSG_DISPLAY_CLOCK    = 4,  /**< 切换到时钟界面 */
    MSG_FEEDBACK_VALID   = 5,  /**< 有效卡反馈：L1 亮 150ms + 短鸣 */
    MSG_FEEDBACK_INVALID = 6,  /**< 无效卡反馈：L7 闪 2 次 + 短鸣 2 次 */
    MSG_FEEDBACK_DUP     = 7,  /**< 重复刷卡反馈：L4 亮 50ms + 短鸣 */
    MSG_FEEDBACK_ADMIN   = 8,  /**< 管理员卡反馈：L1~L7 全亮 300ms + 长鸣 */
} AppMsgType_t;

/** 全局应用消息体 */
typedef struct {
    AppMsgType_t type;
    union {
        uint8_t uid[8];      /**< NFC 卡片 UID，最多 8 字节 */
        uint8_t keyIndex;    /**< 按键索引 0~5 */
        uint8_t cardType;    /**< 卡类型: 0=普通卡, 1=图像卡, 2=管理员卡 */
    } param;
} AppMessage_t;

/** GUI 状态机 */
typedef enum {
    GUI_SHOW_CLOCK          = 0,  /**< 时钟主界面 */
    GUI_SHOW_RESULT         = 1,  /**< 卡片识别结果界面 */
    GUI_STATE_TIME_SETTING  = 2,  /**< 时间设置界面 */
    GUI_SHOW_ADMIN_CONFIG   = 3,  /**< 管理员配置界面 */
} GuiState_t;

/** 时间设置字段枚举 */
typedef enum {
    TIME_FIELD_YEAR = 0,
    TIME_FIELD_MONTH,
    TIME_FIELD_DAY,
    TIME_FIELD_HOUR,
    TIME_FIELD_MINUTE,
    TIME_FIELD_COUNT
} TimeField_t;

/** 时间设置数据 */
typedef struct {
    uint16_t year;          /**< 年 2024-2099 */
    uint8_t month;          /**< 月 1-12 */
    uint8_t day;            /**< 日 自动限制 */
    uint8_t hour;           /**< 时 0-23 */
    uint8_t minute;         /**< 分 0-59 */
    TimeField_t selected;   /**< 当前选中字段 */
} TimeSetting_t;

/* ---- 前向声明 ---- */
static uint8_t TimeSetting_HandleKey(uint8_t ki, TimeSetting_t *ts);

/* ====== 发卡协议状态机 ====== */
typedef enum {
    ISSUE_IDLE = 0,           /**< 空闲，等待 ISSUE_BEGIN */
    ISSUE_WAIT_IMAGE,         /**< 已收到 ISSUE_BEGIN，等待图像数据 */
    ISSUE_IMAGE_READY,        /**< 图像接收完成且 CRC 校验通过 */
    ISSUE_WAIT_CARD,          /**< 等待放卡（ISSUE_COMMIT 后） */
    ISSUE_WRITING,            /**< 正在写卡 */
    ISSUE_DONE,               /**< 写卡成功 */
    ISSUE_ERROR               /**< 写卡失败 */
} IssueState_t;

/* ====== 读卡/清卡独立状态机 ====== */
typedef enum {
    CARD_OP_IDLE = 0,
    CARD_OP_READ_WAIT_CARD,
    CARD_OP_READ_DONE,
    CARD_OP_CLEAR_WAIT_CARD,
    CARD_OP_CLEAR_DONE,
    CARD_OP_ERROR
} CardOpState_t;

/* ---- 发卡写卡错误码 ---- */
#define ISSUE_ERR_NONE      0  /**< 无错误 */
#define ISSUE_ERR_NO_IMAGE  1  /**< 无图像 payload */
#define ISSUE_ERR_NO_CARD   2  /**< 10 秒内未检测到卡片 */
#define ISSUE_ERR_AUTH      3  /**< 认证失败 */
#define ISSUE_ERR_WRITE     4  /**< 写块失败 */
#define ISSUE_ERR_VERIFY    5  /**< 读回校验失败 */
#define ISSUE_ERR_FAIL      6  /**< 其他未分类错误 */

/* ---- 读卡/清卡操作错误码 (独立于 ISSUE) ---- */
#define CARD_OP_ERR_NONE     0  /**< 无错误 */
#define CARD_OP_ERR_NO_CARD  1  /**< 10 秒内未检测到卡片 */
#define CARD_OP_ERR_AUTH     2  /**< 认证失败 */
#define CARD_OP_ERR_READ     3  /**< 读块失败 */
#define CARD_OP_ERR_WRITE    4  /**< 写块失败 */
#define CARD_OP_ERR_VERIFY   5  /**< 读回校验失败 */
#define CARD_OP_ERR_INVALID  6  /**< 卡片数据无效 (magic/checksum/type) */
#define CARD_OP_ERR_FAIL     7  /**< 其他未分类错误 */

/* ---- 发卡协议辅助函数前向声明 ---- */
static uint16_t Issue_Crc16Xmodem(const uint8_t *data, uint16_t len);
static void     Issue_ResetContext(void);
static void     Issue_SetBlockReceived(uint8_t block);
static uint8_t  Issue_IsBlockReceived(uint8_t block);
static uint8_t  Issue_ParseHex16(const char *hex, uint8_t out[16]);
static uint8_t  Issue_WaitAndWriteCard(void);
static uint8_t  Issue_GetPayloadTarget(uint8_t payloadBlock, uint8_t *sector, uint8_t *block);

/* ---- 读卡/清卡操作辅助函数前向声明 ---- */
static void     CardOp_Reset(void);
static void     CardOp_ReadCard(void);
static void     CardOp_ClearCard(void);

/* ---- 账户头校验（在读卡/清卡流程中复用） ---- */
static uint8_t  CardOp_ParseAccountHeader(const uint8_t buf[16], const uint8_t uid[4], uint32_t *workerId, uint8_t *cardType, uint8_t *status);

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#define CARD_OP_DEBUG 0

#if CARD_OP_DEBUG
#define CARD_DBG(s) UartDrv_SendStr(&s_uart1Drv, s)
#else
#define CARD_DBG(s) do {} while (0)
#endif

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/** 系统初始化完成标志，其他任务等待此标志 */
static volatile uint8_t g_systemReady = 0;

/** NFC 卡片数据缓冲区 */
static uint8_t g_cardAvatar[384];
static uint8_t g_cardNameImg[160];
static uint8_t g_cardStuIDImg[160];
static volatile uint8_t g_cardDataReady = 0;

/** 卡片校验状态：0=无效/未校验, 1=校验通过 */
static volatile uint8_t g_cardValidated = 0;

/** 当前刷卡卡类型：0=普通卡, 1=图像卡, 2=管理员卡 */
static volatile uint8_t g_cardType = 0;

/** 最近一次刷卡时间 */
static BSP_RTC_DateTime_t g_lastSwipeTime;

/** 普通卡考勤结果状态（供 guiTask 显示 IN/OUT/DUR 和控制 L4~L7） */
static volatile uint8_t  g_lastAttendValid       = 0;  /**< 1=有效考勤记录已写入 */
static volatile uint8_t  g_lastAttendRejected    = 0;  /**< 1=重复签到/无效离开被拒绝 */
static volatile uint8_t  g_lastAttendEvent       = 0;  /**< ATT_LOG_EVENT_IN(1) / ATT_LOG_EVENT_OUT(2) */
static volatile uint8_t  g_lastAttendMode        = 0;  /**< ATT_MODE_ENTRY/EXIT/BOTH */
static volatile uint32_t g_lastAttendDurationSec = 0;  /**< OUT 时的到场时长（秒） */

/** USART1 驱动实例 — UartDrv 库管理的串口驱动 */
static UartDrv_t s_uart1Drv;
static UartDrv_t s_uart6Drv;

/** USART1 接收消息队列 — ISR 向此队列发送数据, uartTask 从此队列读取 */
static osMessageQueueId_t s_uart1RxQueue;

/* ====== 发卡协议静态缓冲区 ====== */
static uint8_t  s_issuePayload[704];       /**< 704 字节图像 payload 缓冲区 */
static uint8_t  s_issueBlockMask[6];       /**< 44 bit block 接收位掩码 (6×8=48 bit) */
static uint8_t  s_issueBlockCount;         /**< 已接收 block 计数 */
static uint16_t s_issueExpectedCrc;        /**< ISSUE_IMAGE_BEGIN 声明的期望 CRC */
static uint8_t  s_issueCardType;           /**< 卡类型: 0=normal, 1=image, 2=admin */
static uint32_t s_issueWorkerId;          /**< 工号 */
static IssueState_t s_issueState;         /**< 发卡协议当前状态 */

/* ====== 读卡/清卡操作静态变量 ====== */
static volatile CardOpState_t s_cardOpState = CARD_OP_IDLE;   /**< 读卡/清卡当前状态 */
static volatile uint8_t s_cardOpErr = CARD_OP_ERR_NONE; /**< 读卡/清卡错误码 */
static uint8_t  s_cardReadPayload[704];               /**< 读卡 payload 缓冲区 */
static uint8_t  s_cardReadUid[4];                     /**< 读卡获取的 UID */
static uint32_t s_cardReadWorkerId;                   /**< 读卡获取的工号 */
static uint8_t  s_cardReadCardType;                   /**< 读卡获取的卡类型 */
static uint8_t  s_cardReadStatus;                     /**< 读卡获取的状态 */
static uint16_t s_cardReadPayloadCrc;                 /**< 读卡 payload CRC-16 */

/** DS18B20 温度缓存 — otherTask 写入, guiTask 只读 */
static float   g_temperature = 0.0f;
static uint8_t g_tempValid   = 0;
static uint8_t g_ds18b20InitOk = 0;

/* ====== WiFi 状态 — wifiTask 写入, guiTask 只读 ====== */
typedef enum {
    WIFI_STATE_DISABLED = 0,
    WIFI_STATE_INIT,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED
} AppWifiState_t;

static volatile AppWifiState_t g_wifiState = WIFI_STATE_DISABLED;
static volatile uint8_t       g_wifiConnected = 0;
static volatile uint8_t       g_uart6DrvReady = 0;

/* ====== NTP 校时状态 — wifiTask 写入, guiTask 可读 ====== */
typedef enum {
    NTP_STATE_DISABLED = 0,
    NTP_STATE_WAIT_WIFI,
    NTP_STATE_SYNCING,
    NTP_STATE_SYNCED,
    NTP_STATE_FAILED
} AppNtpState_t;

static volatile AppNtpState_t g_ntpState = NTP_STATE_DISABLED;
static volatile uint8_t       g_ntpSynced = 0;

/* ====== 天气查询状态 — wifiTask 写入, guiTask 只读 ====== */
typedef enum {
    WEATHER_STATE_DISABLED = 0,
    WEATHER_STATE_WAIT_WIFI,
    WEATHER_STATE_QUERYING,
    WEATHER_STATE_VALID,
    WEATHER_STATE_FAILED
} AppWeatherState_t;

static volatile AppWeatherState_t g_weatherState = WEATHER_STATE_DISABLED;
static volatile uint8_t           g_weatherValid = 0;
static char                       g_weatherText[96];

extern RTC_HandleTypeDef hrtc;

/** DS18B20 诊断统计（供 TEMP_GET 使用，不主动输出） */
static uint8_t  g_tempLastRet      = 0xFF;
static float    g_tempLastRaw      = 0.0f;
static uint32_t g_tempReadCount    = 0;
static uint32_t g_tempOkCount      = 0;
static uint32_t g_tempInitRetryCount = 0;

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for guiTask */
osThreadId_t guiTaskHandle;
const osThreadAttr_t guiTask_attributes = {
  .name = "guiTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for keyTask */
osThreadId_t keyTaskHandle;
const osThreadAttr_t keyTask_attributes = {
  .name = "keyTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for uartTask */
osThreadId_t uartTaskHandle;
const osThreadAttr_t uartTask_attributes = {
  .name = "uartTask",
  .stack_size = 1536 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for otherTask */
osThreadId_t otherTaskHandle;
const osThreadAttr_t otherTask_attributes = {
  .name = "otherTask",
  .stack_size = 768 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for nfcTask */
osThreadId_t nfcTaskHandle;
const osThreadAttr_t nfcTask_attributes = {
  .name = "nfcTask",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for wifiTask */
osThreadId_t wifiTaskHandle;
const osThreadAttr_t wifiTask_attributes = {
  .name = "wifiTask",
  .stack_size = 1536 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for myQueue01 */
osMessageQueueId_t myQueue01Handle;
const osMessageQueueAttr_t myQueue01_attributes = {
  .name = "myQueue01"
};
/* Definitions for myQueue04 */
osMessageQueueId_t myQueue04Handle;
const osMessageQueueAttr_t myQueue04_attributes = {
  .name = "myQueue04"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartTaskGui(void *argument);
void StartTaskKey(void *argument);
void StartTaskUart(void *argument);
void StartTaskOther(void *argument);
void StartTaskNFC(void *argument);
void StartWifiTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of myQueue01 */
  myQueue01Handle = osMessageQueueNew (8, sizeof(AppMessage_t), &myQueue01_attributes);

  /* creation of myQueue04 */
  myQueue04Handle = osMessageQueueNew (4, sizeof(AppMessage_t), &myQueue04_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of guiTask */
  guiTaskHandle = osThreadNew(StartTaskGui, NULL, &guiTask_attributes);

  /* creation of keyTask */
  keyTaskHandle = osThreadNew(StartTaskKey, NULL, &keyTask_attributes);

  /* creation of uartTask */
  uartTaskHandle = osThreadNew(StartTaskUart, NULL, &uartTask_attributes);

  /* creation of otherTask */
  otherTaskHandle = osThreadNew(StartTaskOther, NULL, &otherTask_attributes);

  /* creation of nfcTask */
  nfcTaskHandle = osThreadNew(StartTaskNFC, NULL, &nfcTask_attributes);

  /* creation of wifiTask */
  wifiTaskHandle = osThreadNew(StartWifiTask, NULL, &wifiTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* ================================================================
 *  阶段一·第二步：初始化任务 (defaultTask)
 *  初始化所有 BSP 驱动：延时 -> LED -> 按键 -> RTC -> OLED -> NFC
 * ================================================================ */
/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */

  /* ---- 1. 初始化微秒延时 (DWT) ---- */
  delay_us_init();

  /* ---- 2. 初始化 LED (L1~L7, 低电平点亮) ---- */
  {
    static const Led_Config_t ledCfgs[] = {
      {L1_GPIO_Port, L1_Pin, LED_ON_LOW},
      {L2_GPIO_Port, L2_Pin, LED_ON_LOW},
      {L3_GPIO_Port, L3_Pin, LED_ON_LOW},
      {L4_GPIO_Port, L4_Pin, LED_ON_LOW},
      {L5_GPIO_Port, L5_Pin, LED_ON_LOW},
      {L6_GPIO_Port, L6_Pin, LED_ON_LOW},
      {L7_GPIO_Port, L7_Pin, LED_ON_LOW},
    };
    LED_Init(ledCfgs, 7);
  }

  /* 上电自检：点亮所有 LED 500ms 后熄灭 */
  LED_SetLeds(0x7F);
  osDelay(500);
  LED_SetLeds(0x00);

  /* ---- 3. 初始化按键 (K1~K6) ---- */
  {
    static const Key_Config_t keyCfgs[] = {
      {K1_GPIO_Port, K1_Pin, KEY_ACTIVE_LOW},   /* K1: PE1, 上拉输入 */
      {K2_GPIO_Port, K2_Pin, KEY_ACTIVE_LOW},   /* K2: PE2, 上拉输入 */
      {K3_GPIO_Port, K3_Pin, KEY_ACTIVE_LOW},   /* K3: PE3, 上拉输入 */
      {K4_GPIO_Port, K4_Pin, KEY_ACTIVE_LOW},   /* K4: PE4, 上拉输入 */
      {K5_GPIO_Port, K5_Pin, KEY_ACTIVE_HIGH},  /* K5: PE5, 下拉输入 */
      {K6_GPIO_Port, K6_Pin, KEY_ACTIVE_HIGH},  /* K6: PE6, 下拉输入 */
    };
    Key_Init(keyCfgs, 6);
  }

  /* ---- 4. 初始化 RTC ---- */
  /* 先读一次 RTC 时间以初始化全局变量 g_rtc_datetime */
  BSP_RTC_GetDateTime(NULL);

  if (BSP_RTC_IsFirstPowerOn() || g_rtc_datetime.year < 2025)
  {
    /* 首次上电 或 RTC 时间明显无效 → 设置默认日期时间 */
    BSP_RTC_SetDate(2026, 6, 29);
    BSP_RTC_SetTime(12, 0, 0);
    BSP_RTC_MarkInitialized();
  }

  /* ---- 5. 初始化 OLED (SSD1306 + GUI) ---- */
  GUI_Init();
  GUI_SetColor(GUI_COLOR_WHITE);
  GUI_Clear();
  {
    GUI_BITMAP splashBM = {128, 64, 16, 1, splash_bitmap_data, NULL};
    GUI_DrawBitmap(&splashBM, 0, 0);
  }
  GUI_Update();
  osDelay(2500);
  GUI_Clear();
  GUI_Update();

  /* ---- 6. 初始化 NFC (RC522) ---- */
  RC522_Platform_Init();
  RC522_ConfigISOType('A');   /* 配置 ISO14443A 协议并开启天线 */

  /* ---- 7. 初始化 UART 驱动 ---- */
  {
    UartDrv_Init(&s_uart1Drv, &huart1);
    UartDrv_Init(&s_uart6Drv, &huart6);

    /* 为 USART1 创建接收消息队列并绑定到 UartDrv 实例
     * ISR 收到数据后通过此队列转发给 uartTask，实现零阻塞接收 */
    s_uart1RxQueue = osMessageQueueNew(8, sizeof(UartDrv_QueueEvent_t), NULL);
    UartDrv_RegisterRxQueue(&s_uart1Drv, s_uart1RxQueue);

    /* 启动 USART1 空闲中断接收（ReceiveToIdle_IT 自动循环） */
    UartDrv_StartRecv(&s_uart1Drv);
  }

  /* ---- 8. 初始化 W25Q128 和设备配置区 ---- */
  W25QXX_Init();
  if (AppConfig_Init() != 0)
  {
    /* 初始化失败时不卡死系统，通过 USART1 和 OLED 提示 */
    UartDrv_SendStr(&s_uart1Drv, "[WARN] AppConfig_Init failed\r\n");
  }

  /* ---- 9. 初始化考勤日志区 ---- */
  if (AttendanceLog_Init() != 0)
  {
    UartDrv_SendStr(&s_uart1Drv, "[WARN] AttendanceLog_Init failed\r\n");
  }

  /* ---- 9.5. 初始化 DS18B20 温度传感器 ---- */
  g_ds18b20InitOk = (ds18b20_init() == 0) ? 1 : 0;

  /* ---- 9.6. 标记 USART6 驱动就绪 (wifiTask 等待此标志) ---- */
  g_uart6DrvReady = 1;

  /* ---- 10. 通知所有任务：系统已就绪 ---- */
  {
    AppMessage_t msg;
    msg.type = MSG_INIT_DONE;
    osMessageQueuePut(myQueue01Handle, &msg, 0, 0);
    osMessageQueuePut(myQueue04Handle, &msg, 0, 0);
  }
  g_systemReady = 1;

  /* ---- 9. 初始化完成，进入空闲循环 ---- */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1000);
  }
  /* USER CODE END StartDefaultTask */
}

/* ================================================================
 *  阶段一·第四步：OLED 显示任务 (guiTask)
 *  时钟界面 ↔ 结果界面 状态机
 * ================================================================ */
/* USER CODE BEGIN Header_StartTaskGui */
/**
* @brief Function implementing the guiTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskGui */
void StartTaskGui(void *argument)
{
  /* USER CODE BEGIN StartTaskGui */

  /* 等待系统初始化完成 */
  {
    AppMessage_t msg;
    osMessageQueueGet(myQueue01Handle, &msg, NULL, osWaitForever);
  }

  GuiState_t state = GUI_SHOW_CLOCK;
  uint8_t uidBuf[8] = {0};
  char dispBuf[32];

  /* 每周英文简称 */
  static const char *weekCN[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

  /* 时钟刷新节流：秒变化时才重绘，减少 I2C 压力 */
  static uint8_t lastSecond = 0xFF;

  /* 时间设置数据 */
  static TimeSetting_t ts;

  /* 结果界面自动返回计时（非阻塞 tick 判断） */
  uint32_t resultEnterTick = 0;
  #define RESULT_TIMEOUT_MS  3000U   /* 结果界面 3 秒后自动返回时钟 */

  /* ---- 管理员配置临时变量 ---- */
  static uint32_t adminTempDevId = 1;
  static uint8_t  adminTempMode = 2;         /* 0=ENTRY, 1=EXIT, 2=BOTH */
  static uint8_t  adminSelectedField = 0;    /* 0=Dev, 1=Mode */
  static uint32_t adminLastKeyTick = 0;
  #define ADMIN_TIMEOUT_MS   30000U          /* 30 秒无操作自动退出 */
  #define ADMIN_DEV_ID_MIN   1U
  #define ADMIN_DEV_ID_MAX   9999U

  /* 管理员模式字符串 */
  static const char *adminModeStr[] = {"ENTRY", "EXIT", "BOTH"};

  /* Infinite loop */
  for(;;)
  {
    /* ---- 非阻塞接收消息（使用有限 timeout 20ms 周期性处理） ---- */
    AppMessage_t msg;
    osStatus_t qstat = osMessageQueueGet(myQueue01Handle, &msg, NULL, 20U);

    if (qstat == osOK)
    {
      switch (msg.type)
      {
      case MSG_NFC_CARD:
        /* 时间设置界面和管理员配置界面忽略 NFC 消息 */
        if (state == GUI_STATE_TIME_SETTING || state == GUI_SHOW_ADMIN_CONFIG)
        {
          break;
        }

        /* 管理员卡 (cardType == 2)：进入管理员配置界面 */
        if (g_cardType == 2)
        {
          /* 读取当前配置到临时变量 */
          {
            const DeviceConfig_t *cfg = AppConfig_Get();
            if (cfg != NULL)
            {
              adminTempDevId = cfg->deviceId;
              adminTempMode  = cfg->attendanceMode;
            }
          }
          adminSelectedField = 0;
          adminLastKeyTick   = osKernelGetTickCount();
          state = GUI_SHOW_ADMIN_CONFIG;
          break;
        }

        /* 普通卡/图像卡：进入结果界面 */
        memcpy(uidBuf, msg.param.uid, 4);
        state = GUI_SHOW_RESULT;
        resultEnterTick = osKernelGetTickCount();
        break;

      case MSG_DISPLAY_CLOCK:
        /* 管理员配置界面和时间设置界面忽略卡离开触发的返回时钟消息 */
        if (state == GUI_SHOW_ADMIN_CONFIG || state == GUI_STATE_TIME_SETTING)
        {
          break;
        }
        state = GUI_SHOW_CLOCK;
        lastSecond = 0xFF;  /* 强制刷新时钟 */
        break;

      case MSG_KEY_SHORT:
      {
        uint8_t ki = msg.param.keyIndex;

        /* 管理员配置界面按键处理 */
        if (state == GUI_SHOW_ADMIN_CONFIG)
        {
          adminLastKeyTick = osKernelGetTickCount();

          switch (ki)
          {
          case 0: /* K1: 当前字段减 */
            if (adminSelectedField == 0)
            {
              /* deviceId--, clamp 1~9999 */
              if (adminTempDevId > ADMIN_DEV_ID_MIN)
                adminTempDevId--;
            }
            else
            {
              /* mode 向前循环: BOTH(2) -> EXIT(1) -> ENTRY(0) -> BOTH(2) */
              if (adminTempMode == 0)
                adminTempMode = 2;
              else
                adminTempMode--;
            }
            break;

          case 1: /* K2: 选择上一个字段 */
            adminSelectedField = (uint8_t)((adminSelectedField == 0) ? 1 : 0);
            break;

          case 2: /* K3: 选择下一个字段 */
            adminSelectedField = (uint8_t)((adminSelectedField == 0) ? 1 : 0);
            break;

          case 3: /* K4: 当前字段加 */
            if (adminSelectedField == 0)
            {
              /* deviceId++, clamp 1~9999 */
              if (adminTempDevId < ADMIN_DEV_ID_MAX)
                adminTempDevId++;
            }
            else
            {
              /* mode 向后循环: ENTRY(0) -> EXIT(1) -> BOTH(2) -> ENTRY(0) */
              if (adminTempMode >= 2)
                adminTempMode = 0;
              else
                adminTempMode++;
            }
            break;

          case 4: /* K5: 保存并退出 */
          {
            uint8_t ret = AppConfig_Update(adminTempDevId, adminTempMode);
            if (ret == 0)
            {
              /* 保存成功：有效卡声光反馈 */
              {
                AppMessage_t fb;
                fb.type = MSG_FEEDBACK_VALID;
                osMessageQueuePut(myQueue04Handle, &fb, 0, 50U);
              }
            }
            else
            {
              /* 保存失败：无效卡声光反馈 */
              {
                AppMessage_t fb;
                fb.type = MSG_FEEDBACK_INVALID;
                osMessageQueuePut(myQueue04Handle, &fb, 0, 50U);
              }
            }
            state = GUI_SHOW_CLOCK;
            lastSecond = 0xFF;
            break;
          }

          case 5: /* K6: 取消并退出（不保存） */
            state = GUI_SHOW_CLOCK;
            lastSecond = 0xFF;
            break;

          default:
            break;
          }
        }
        /* 时间设置界面：处理所有按键 */
        else if (state == GUI_STATE_TIME_SETTING)
        {
          if (TimeSetting_HandleKey(ki, &ts))
          {
            /* K5: 保存时间 */
            BSP_RTC_DateTime_t sdt;
            sdt.year   = ts.year;
            sdt.month  = ts.month;
            sdt.day    = ts.day;
            sdt.weekday = BSP_RTC_CalcWeekday(ts.year, ts.month, ts.day);
            sdt.hour   = ts.hour;
            sdt.minute = ts.minute;
            sdt.second = 0;  /* 秒数清零 */
            BSP_RTC_SetDateTime(&sdt);
            /* 刷新全局变量 */
            BSP_RTC_GetDateTime(NULL);
            state = GUI_SHOW_CLOCK;
            lastSecond = 0xFF;  /* 强制刷新时钟 */
          }
        }
        /* 时钟界面：K5 进入时间设置 */
        else if (state == GUI_SHOW_CLOCK && ki == 4)
        {
          BSP_RTC_DateTime_t rdt;
          if (BSP_RTC_GetDateTime(&rdt) == HAL_OK)
          {
            ts.year   = rdt.year;
            ts.month  = rdt.month;
            ts.day    = rdt.day;
            ts.hour   = rdt.hour;
            ts.minute = rdt.minute;
          }
          else
          {
            /* RTC 故障时使用默认值 */
            ts.year   = 2026;
            ts.month  = 1;
            ts.day    = 1;
            ts.hour   = 12;
            ts.minute = 0;
          }
          ts.selected = TIME_FIELD_YEAR;
          state = GUI_STATE_TIME_SETTING;
        }
        break;
      }

      default:
        break;
      }
    }

    /* ---- 状态机绘制 ---- */
    switch (state)
    {
    case GUI_SHOW_CLOCK:
    {
      BSP_RTC_DateTime_t dt;
      uint8_t needRedraw = 0;

      if (BSP_RTC_GetDateTime(&dt) == HAL_OK)
      {
        dt.weekday = BSP_RTC_CalcWeekday(dt.year, dt.month, dt.day);
        if (dt.second != lastSecond)
        {
          lastSecond = dt.second;
          needRedraw = 1;
        }
      }
      else
      {
        /* RTC 读取失败也重绘，确保界面不冻结 */
        needRedraw = 1;
      }

      /* 仅秒数变化或首次进入时才重绘，大幅减少 I2C 操作 */
      if (needRedraw)
      {
        GUI_Clear();
        GUI_SetColor(GUI_COLOR_WHITE);

        /* ---- 第一行: 年月日 + 英文星期 (Y=0) ---- */
        sprintf(dispBuf, "%04d-%02d-%02d %s",
                dt.year, dt.month, dt.day, weekCN[dt.weekday]);
        GUI_DispStringAt(dispBuf, 0, 0);

        /* ---- 第二行: 大号 HH:MM:SS (Y=18, 16x24 字体) ---- */
        {
          char timeBuf[9];
          sprintf(timeBuf, "%02d:%02d:%02d",
                  dt.hour, dt.minute, dt.second);
          /* 8 字符 * 16 = 128 像素，完美填充 OLED 宽度 */
          GUI_DispBigNumber(0, 18, timeBuf);
        }

        /* ---- 第三行: 底部交替显示 — 状态行 5s / 天气行 8s (Y=48) ---- */
        {
          static TickType_t s_bottomSwitchTick = 0;
          static uint8_t    s_bottomShowWeather = 0;
          static uint8_t    s_weatherScrollOffset = 0;
          static TickType_t s_weatherScrollTick = 0;

          const DeviceConfig_t *cfg = AppConfig_Get();
          char modeAbbr[4] = "BO";
          char tempStr[8];
          const char *wifiStr = "WOFF";
          char bottomLine[17];

          /* WiFi 状态缩写 */
          {
            AppWifiState_t ws = g_wifiState;
            switch (ws)
            {
              case WIFI_STATE_DISABLED:   wifiStr = "WOFF";  break;
              case WIFI_STATE_INIT:       wifiStr = "WINI";  break;
              case WIFI_STATE_CONNECTING: wifiStr = "W...";  break;
              case WIFI_STATE_CONNECTED:  wifiStr = "WOK";   break;
              case WIFI_STATE_FAILED:     wifiStr = "WFAIL"; break;
              default:                    wifiStr = "WOFF";  break;
            }
          }

          if (cfg != NULL)
          {
            switch (cfg->attendanceMode)
            {
              case 0:  /* ENTRY */ modeAbbr[0]='E'; modeAbbr[1]='N'; break;
              case 1:  /* EXIT  */ modeAbbr[0]='E'; modeAbbr[1]='X'; break;
              default: /* BOTH  */ modeAbbr[0]='B'; modeAbbr[1]='O'; break;
            }
            modeAbbr[2] = '\0';
          }

          if (g_tempValid)
          {
            snprintf(tempStr, sizeof(tempStr), "%dC", (int)g_temperature);
          }
          else
          {
            snprintf(tempStr, sizeof(tempStr), "--C");
          }

          /* 判断是否有天气 */
          uint8_t hasWeather = 0;
#if WEATHER_ENABLED
          if (g_weatherValid && g_weatherText[0] != '\0')
          {
            hasWeather = 1;
          }
#endif

          TickType_t now = xTaskGetTickCount();

          /* 天气有效时交替切换 */
          if (hasWeather)
          {
            /* 首次进入或状态未初始化 */
            if (s_bottomSwitchTick == 0)
            {
              s_bottomSwitchTick = now;
              s_bottomShowWeather = 0;
              s_weatherScrollOffset = 0;
              s_weatherScrollTick = 0;
            }

            uint32_t phaseMs = (now - s_bottomSwitchTick) * portTICK_PERIOD_MS;

            if (s_bottomShowWeather)
            {
              /* 天气行显示 12 秒后切回状态行 */
              if (phaseMs >= 12000U)
              {
                s_bottomShowWeather = 0;
                s_bottomSwitchTick = now;
                s_weatherScrollOffset = 0;
                s_weatherScrollTick = 0;
              }
            }
            else
            {
              /* 状态行显示 5 秒后切到天气行 */
              if (phaseMs >= 5000U)
              {
                s_bottomShowWeather = 1;
                s_bottomSwitchTick = now;
                s_weatherScrollOffset = 0;
                s_weatherScrollTick = 0;
              }
            }

            if (s_bottomShowWeather)
            {
              /* 天气行 — 临界区安全读取 g_weatherText */
              char localWeather[96];
              taskENTER_CRITICAL();
              memcpy(localWeather, g_weatherText, sizeof(localWeather));
              localWeather[sizeof(localWeather) - 1] = '\0';
              taskEXIT_CRITICAL();

              uint8_t wlen = (uint8_t)strlen(localWeather);

              if (wlen <= 16)
              {
                /* 不滚动，直接显示 */
                snprintf(bottomLine, sizeof(bottomLine), "%s", localWeather);
              }
              else
              {
                /* 逐字符滚动 */
                uint32_t scrollMs = (now - s_weatherScrollTick) * portTICK_PERIOD_MS;
                if (scrollMs >= 1000U)
                {
                  s_weatherScrollTick = now;
                  s_weatherScrollOffset++;
                  if (s_weatherScrollOffset >= wlen + 16)
                  {
                    s_weatherScrollOffset = 0;
                  }
                }

                uint8_t i;
                for (i = 0; i < 16; i++)
                {
                  int16_t srcIdx = (int16_t)s_weatherScrollOffset + i - 16;
                  if (srcIdx < 0)
                  {
                    bottomLine[i] = ' ';
                  }
                  else if ((uint16_t)srcIdx >= wlen)
                  {
                    bottomLine[i] = ' ';
                  }
                  else
                  {
                    bottomLine[i] = localWeather[(uint16_t)srcIdx];
                  }
                }
                bottomLine[16] = '\0';
              }
            }
          }

          /* 显示状态行 */
          if (!hasWeather || !s_bottomShowWeather)
          {
            if (cfg != NULL)
            {
              snprintf(bottomLine, sizeof(bottomLine), "I%03lu %s %s %s",
                      (unsigned long)cfg->deviceId, modeAbbr, tempStr, wifiStr);
            }
            else
            {
              snprintf(bottomLine, sizeof(bottomLine), "I--- -- %s %s", tempStr, wifiStr);
            }
          }

          GUI_DispStringAt(bottomLine, 0, 48);
        }

        GUI_Update();
      }
      break;
    }

    case GUI_SHOW_RESULT:
    {
      /* 3 秒自动返回时钟（非阻塞 tick 判断，不阻塞 guiTask） */
      if ((osKernelGetTickCount() - resultEnterTick) >= pdMS_TO_TICKS(RESULT_TIMEOUT_MS))
      {
        /* 返回待机前关闭考勤状态灯 L4~L7 */
        LED_Off(3);
        LED_Off(4);
        LED_Off(5);
        LED_Off(6);
        state = GUI_SHOW_CLOCK;
        lastSecond = 0xFF;  /* 强制刷新时钟 */
        break;
      }

      GUI_Clear();
      GUI_SetColor(GUI_COLOR_WHITE);

      /* --- 绘制头像位图 (48×64, 1bpp) 位置 (0, 0) ---- */
      /* 仅图像卡 (cardType == 1) 显示头像，普通卡和管理员卡不显示 */
      if (g_cardType == 1)
      {
        GUI_BITMAP bm = {48, 64, 6, 1, g_cardAvatar, NULL};
        GUI_DrawBitmap(&bm, 0, 0);
      }

      /* --- 绘制姓名位图 (80×16, 1bpp) 位置 (48, 0) --- */
      {
        GUI_BITMAP bm = {80, 16, 10, 1, g_cardNameImg, NULL};
        GUI_DrawBitmap(&bm, 48, 0);
      }

      /* --- 绘制学号位图 (80×16, 1bpp) 位置 (48, 16) --- */
      {
        GUI_BITMAP bm = {80, 16, 10, 1, g_cardStuIDImg, NULL};
        GUI_DrawBitmap(&bm, 48, 16);
      }

      /* --- 底部考勤结果显示 --- */
      GUI_GotoXY(0, 50);

      if (g_cardType == 0)
      {
        /* 普通卡：根据考勤状态显示 IN/OUT/DUR/DUP IN/INV OUT */
        if (g_lastAttendRejected)
        {
          if (g_lastAttendEvent == 1) /* ATT_LOG_EVENT_IN */
          {
            GUI_DispString("DUP IN");
          }
          else
          {
            GUI_DispString("INV OUT");
          }
        }
        else if (g_lastAttendValid)
        {
          if (g_lastAttendEvent == 1) /* ATT_LOG_EVENT_IN */
          {
            sprintf(dispBuf, "IN %02d:%02d:%02d",
                    g_lastSwipeTime.hour,
                    g_lastSwipeTime.minute,
                    g_lastSwipeTime.second);
            GUI_DispString(dispBuf);
          }
          else if (g_lastAttendEvent == 2) /* ATT_LOG_EVENT_OUT */
          {
            if (g_lastAttendMode == 2 && g_lastAttendDurationSec > 0)
            {
              /* BOTH 模式 OUT：显示到场时长 */
              uint32_t h = g_lastAttendDurationSec / 3600;
              uint32_t m = (g_lastAttendDurationSec % 3600) / 60;
              sprintf(dispBuf, "DUR %02luh%02lum",
                      (unsigned long)h, (unsigned long)m);
              GUI_DispString(dispBuf);
            }
            else
            {
              sprintf(dispBuf, "OUT %02d:%02d:%02d",
                      g_lastSwipeTime.hour,
                      g_lastSwipeTime.minute,
                      g_lastSwipeTime.second);
              GUI_DispString(dispBuf);
            }
          }
          else
          {
            /* fallback: 显示刷卡时间 */
            sprintf(dispBuf, "Time %02d:%02d:%02d",
                    g_lastSwipeTime.hour,
                    g_lastSwipeTime.minute,
                    g_lastSwipeTime.second);
            GUI_DispString(dispBuf);
          }
        }
        else
        {
          /* 没有考勤状态：显示刷卡时间 */
          sprintf(dispBuf, "Time %02d:%02d:%02d",
                  g_lastSwipeTime.hour,
                  g_lastSwipeTime.minute,
                  g_lastSwipeTime.second);
          GUI_DispString(dispBuf);
        }
      }
      else
      {
        /* 非普通卡：显示刷卡时间 */
        sprintf(dispBuf, "Time %02d:%02d:%02d",
                g_lastSwipeTime.hour,
                g_lastSwipeTime.minute,
                g_lastSwipeTime.second);
        GUI_DispString(dispBuf);
      }

      GUI_Update();
      break;
    }

    case GUI_SHOW_ADMIN_CONFIG:
    {
      /* 30 秒无操作自动退出 */
      if ((osKernelGetTickCount() - adminLastKeyTick) >= pdMS_TO_TICKS(ADMIN_TIMEOUT_MS))
      {
        state = GUI_SHOW_CLOCK;
        lastSecond = 0xFF;
        break;
      }

      GUI_Clear();
      GUI_SetColor(GUI_COLOR_WHITE);

      /* 标题 */
      GUI_GotoXY(0, 0);
      GUI_DispString("ADMIN SET");

      /* Dev 行 */
      GUI_GotoXY(0, 16);
      {
        uint8_t isDev = (adminSelectedField == 0);
        char lineBuf[16];
        sprintf(lineBuf, "%cDev: %04lu",
                isDev ? '>' : ' ',
                (unsigned long)adminTempDevId);
        GUI_DispString(lineBuf);
      }

      /* Mode 行 */
      GUI_GotoXY(0, 32);
      {
        uint8_t isMode = (adminSelectedField == 1);
        const char *ms = "BOTH";
        if (adminTempMode < 3)
          ms = adminModeStr[adminTempMode];
        char lineBuf[16];
        sprintf(lineBuf, "%cMode: %s", isMode ? '>' : ' ', ms);
        GUI_DispString(lineBuf);
      }

      /* 操作提示 */
      GUI_GotoXY(0, 48);
      GUI_DispString("K5 Save K6 Exit");

      GUI_Update();
      break;
    }

    case GUI_STATE_TIME_SETTING:
    {
      GUI_Clear();
      GUI_SetColor(GUI_COLOR_WHITE);

      /* 标题 */
      GUI_GotoXY(0, 0);
      GUI_DispString("TIME SET");

      /* 年-月-日 */
      GUI_GotoXY(0, 14);
      sprintf(dispBuf, "%04d-%02d-%02d",
              ts.year, ts.month, ts.day);
      GUI_DispString(dispBuf);

      /* 时:分 */
      GUI_GotoXY(0, 28);
      sprintf(dispBuf, "%02d:%02d",
              ts.hour, ts.minute);
      GUI_DispString(dispBuf);

      /* 选中字段标识：在对应字符位置下方画下划线
       * 8x8 ASCII 字体，每个字符宽度 8px。
       * 年: 列 (0~3), 月: 列 (5~6), 日: 列 (8~9)
       * 时: 列 (0~1), 分: 列 (3~4)
       * 下划线 Y = 行Y + 8, 宽度 = 字符数*8
       */
      {
        const uint8_t fieldStartX[5] = {0, 40, 64, 0, 24};   /* 各行首字符 X */
        const uint8_t fieldLen[5]    = {4, 2, 2, 2, 2};       /* 字符宽度个数 */
        const uint8_t fieldLineY[5]  = {14, 14, 14, 28, 28}; /* 行 Y 坐标 */
        uint8_t idx = ts.selected;
        uint8_t sx = fieldStartX[idx];
        uint8_t ly = fieldLineY[idx] + 8;  /* 字符底部 + 1px 间距 */
        /* 反色矩形表示选中字段 */
        GUI_SetColor(GUI_COLOR_WHITE);
        GUI_FillRect(sx, ly, sx + fieldLen[idx] * 8, ly + 1);
      }

      /* 提示 */
      GUI_GotoXY(0, 44);
      GUI_DispString("K2/K3:Field K1/K4:+/-");
      GUI_GotoXY(0, 52);
      GUI_DispString("K5:Save");

      GUI_Update();
      break;
    }
    }
  }
  /* USER CODE END StartTaskGui */
}

/* ================================================================
 *  时间设置按键处理（由 guiTask 调用）
 *  @param ki  按键索引: 0=K1, 1=K2, 2=K3, 3=K4, 4=K5
 *  @param ts  时间设置数据指针
 *  @retval 1  退出设置界面 (K5 保存)
 *  @retval 0  继续停留在设置界面
 * ================================================================ */
static uint8_t TimeSetting_HandleKey(uint8_t ki, TimeSetting_t *ts)
{
  if (ki == 4) /* K5: 保存并退出 */
    return 1;

  if (ki == 1) /* K2: 选择上一个字段 */
  {
    ts->selected = (TimeField_t)((ts->selected - 1 + TIME_FIELD_COUNT) % TIME_FIELD_COUNT);
    return 0;
  }

  if (ki == 2) /* K3: 选择下一个字段 */
  {
    ts->selected = (TimeField_t)((ts->selected + 1) % TIME_FIELD_COUNT);
    return 0;
  }

  int8_t delta = 0;
  if (ki == 0)      delta = -1; /* K1: 当前字段减 1 */
  else if (ki == 3) delta = +1; /* K4: 当前字段加 1 */

  if (delta != 0)
  {
    switch (ts->selected)
    {
    case TIME_FIELD_YEAR:
    {
      int16_t y = (int16_t)ts->year + delta;
      if (y < 2024)      y = 2024;
      else if (y > 2099) y = 2099;
      ts->year = (uint16_t)y;
      /* 修正：2月天数可能变化 */
      {
        uint8_t maxDay = BSP_RTC_DaysInMonth(ts->year, ts->month);
        if (ts->day > maxDay) ts->day = maxDay;
      }
      break;
    }
    case TIME_FIELD_MONTH:
    {
      int8_t m = (int8_t)ts->month + delta;
      if (m < 1)      m = 12;
      else if (m > 12) m = 1;
      ts->month = (uint8_t)m;
      /* 修正日期 */
      {
        uint8_t maxDay = BSP_RTC_DaysInMonth(ts->year, ts->month);
        if (ts->day > maxDay) ts->day = maxDay;
      }
      break;
    }
    case TIME_FIELD_DAY:
    {
      uint8_t maxDay = BSP_RTC_DaysInMonth(ts->year, ts->month);
      int8_t d = (int8_t)ts->day + delta;
      if (d < 1)       d = maxDay;
      else if (d > maxDay) d = 1;
      ts->day = (uint8_t)d;
      break;
    }
    case TIME_FIELD_HOUR:
    {
      int8_t h = (int8_t)ts->hour + delta;
      if (h < 0)      h = 23;
      else if (h > 23) h = 0;
      ts->hour = (uint8_t)h;
      break;
    }
    case TIME_FIELD_MINUTE:
    {
      int8_t mi = (int8_t)ts->minute + delta;
      if (mi < 0)       mi = 59;
      else if (mi > 59) mi = 0;
      ts->minute = (uint8_t)mi;
      break;
    }
    default:
      break;
    }
  }

  return 0;
}

/* ================================================================
 *  阶段一·第六步：按键任务 (keyTask)
 *  每 10ms 扫描 6 键，检测短按/长按并分发事件
 * ================================================================ */
/* USER CODE BEGIN Header_StartTaskKey */
/**
* @brief Function implementing the keyTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskKey */
void StartTaskKey(void *argument)
{
  /* USER CODE BEGIN StartTaskKey */

  /* 等待系统初始化完成 */
  while (!g_systemReady)
  {
    osDelay(50);
  }

  TickType_t lastWakeTime = xTaskGetTickCount();

  /* Infinite loop */
  for(;;)
  {
    /* 严格 10ms 周期 */
    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(KEY_SCAN_INTERVAL_MS));

    /* 执行按键扫描 */
    Key_Scan();

    /* 遍历所有按键，分发事件
     * 限流：每个扫描周期最多向 GUI 队列发送 2 条消息，防止按键抖动填满队列 */
    uint8_t sentCount = 0;
    for (uint8_t i = 0; i < 6 && sentCount < 2; i++)
    {
      if (Key_IsShortPressed(i))
      {
        AppMessage_t msg;
        msg.type = MSG_KEY_SHORT;
        msg.param.keyIndex = i;
        /* 使用 10ms timeout，队列满则丢弃（避免阻塞 10ms 扫描周期） */
        if (osMessageQueuePut(myQueue01Handle, &msg, 0, 10U) == osOK)
        {
          sentCount++;
        }
      }

      if (Key_IsLongPressed(i) && sentCount < 2)
      {
        AppMessage_t msg;
        msg.type = MSG_KEY_LONG;
        msg.param.keyIndex = i;
        if (osMessageQueuePut(myQueue01Handle, &msg, 0, 10U) == osOK)
        {
          sentCount++;
        }
      }

      /* 长按连发可用，阶段一暂不处理 */
      Key_IsRepeat(i);
    }

  }
  /* USER CODE END StartTaskKey */
}

/* ================================================================
 *  发卡协议辅助函数
 * ================================================================ */

/**
  * @brief  CRC-16/XMODEM 计算
  * @note   poly=0x1021, init=0x0000, refin=false, refout=false, xorout=0x0000
  * @param  data: 输入数据指针
  * @param  len:  数据字节数
  * @retval CRC-16 值（大端）
  */
static uint16_t Issue_Crc16Xmodem(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0x0000;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= ((uint16_t)data[i] << 8);
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/**
  * @brief  重置发卡上下文到初始状态
  */
static void Issue_ResetContext(void)
{
    s_issueState = ISSUE_IDLE;
    s_issueCardType = 0;
    s_issueWorkerId = 0;
    s_issueExpectedCrc = 0;
    s_issueBlockCount = 0;
    memset(s_issuePayload, 0, sizeof(s_issuePayload));
    memset(s_issueBlockMask, 0, sizeof(s_issueBlockMask));
}

/**
  * @brief  标记 block 已接收
  * @param  block: block 索引 0~43
  */
static void Issue_SetBlockReceived(uint8_t block)
{
    if (block < 44)
    {
        s_issueBlockMask[block / 8] |= (1 << (block % 8));
    }
}

/**
  * @brief  检查 block 是否已接收
  * @param  block: block 索引 0~43
  * @retval 1 已接收, 0 未接收
  */
static uint8_t Issue_IsBlockReceived(uint8_t block)
{
    if (block < 44)
    {
        return (s_issueBlockMask[block / 8] & (1 << (block % 8))) ? 1 : 0;
    }
    return 0;
}

/**
  * @brief  将 32 字符十六进制字符串转换为 16 字节
  * @param  hex: 输入字符串 (必须正好 32 个有效 hex 字符)
  * @param  out: 输出 16 字节
  * @retval 1 成功, 0 格式错误
  */
static uint8_t Issue_ParseHex16(const char *hex, uint8_t out[16])
{
    for (uint8_t i = 0; i < 16; i++)
    {
        uint8_t hi = 0, lo = 0;
        char c = hex[i * 2];
        if (c >= '0' && c <= '9')       hi = (uint8_t)(c - '0');
        else if (c >= 'A' && c <= 'F')  hi = (uint8_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f')  hi = (uint8_t)(c - 'a' + 10);
        else return 0;

        c = hex[i * 2 + 1];
        if (c >= '0' && c <= '9')       lo = (uint8_t)(c - '0');
        else if (c >= 'A' && c <= 'F')  lo = (uint8_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f')  lo = (uint8_t)(c - 'a' + 10);
        else return 0;

        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/**
  * @brief  解析十六进制子串（4字符 -> uint16_t）
  * @note   用于解析 crc=XXXX 中的 4 字符 hex
  * @param  hex: 4 字符 hex 字符串
  * @param  val: 输出值
  * @retval 1 成功, 0 格式错误
  */
/**
  * @brief  将单个 hex 字符转换为 4-bit 值
  * @param  c: hex 字符 '0'~'9', 'A'~'F', 'a'~'f'
  * @retval 0~15 有效, -1 格式错误
  */
static int Issue_HexNibble(char c)
{
    if (c >= '0' && c <= '9')       return (int)(c - '0');
    else if (c >= 'A' && c <= 'F')  return (int)(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f')  return (int)(c - 'a' + 10);
    return -1;
}

/**
  * @brief  解析 4 字符十六进制 CRC (大小写不敏感)
  * @note   s 不能为 NULL, s[0..3] 必须都是合法 hex 字符
  * @param  s: 4 字符 hex 字符串
  * @param  outCrc: 输出 CRC-16 值
  * @retval 1 成功 (crc=0x0000 也返回 1), 0 格式错误
  */
static uint8_t Issue_ParseCrc16Hex4(const char *s, uint16_t *outCrc)
{
    if (s == NULL) return 0;
    uint16_t v = 0;
    for (uint8_t i = 0; i < 4; i++)
    {
        int nib = Issue_HexNibble(s[i]);
        if (nib < 0) return 0;
        v = (uint16_t)((v << 4) | (uint16_t)nib);
    }
    *outCrc = v;
    return 1;
}

/**
  * @brief  解析十进制无符号整数（最多 8 位工号）
  * @param  str: 数字字符串
  * @param  val: 输出值
  * @retval 1 成功, 0 格式错误
  */
static uint8_t Issue_ParseDec(const char *str, uint32_t *val)
{
    uint32_t v = 0;
    uint8_t digits = 0;
    while (*str >= '0' && *str <= '9')
    {
        v = v * 10 + (uint32_t)(*str - '0');
        str++;
        digits++;
        if (digits > 10) return 0;  /* 防止溢出 */
    }
    if (digits == 0) return 0;
    *val = v;
    return 1;
}

/* ====== 发卡写卡辅助函数 ====== */

/** 写卡错误码存储（供 uartTask 读取） */
static volatile uint8_t s_issueWriteErr = ISSUE_ERR_NONE;

/**
  * @brief  计算账户头校验和
  * @note   与 NFC_ValidateCard 中校验算法一致：前 14 字节逐字节累加，取低 16 位
  * @param  header: 16 字节账户头（前 14 字节已填充）
  * @retval 16 位校验和（小端存储到 header[14..15]）
  */
static uint16_t Issue_CalcHeaderChecksum(const uint8_t header[16])
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 14; i++)
    {
        sum += header[i];
    }
    return sum;
}

/**
  * @brief  构造卡片账户头（扇区 0 块 1）
  * @note   与 NFC_ValidateCard 验证逻辑完全一致：
  *         bytes  0~3: cardId (UID)
  *         bytes  4~7: magic (0xA5A5A5A5)
  *         bytes 8~11: workerId (大端，uint32_t)
  *         byte  12:   cardType
  *         byte  13:   status (0)
  *         bytes 14~15: checksum (小端，前 14 字节累加和低 16 位)
  * @param  header: 输出 16 字节
  * @param  uid: 4 字节卡片物理 UID
  */
static void Issue_BuildCardHeader(uint8_t header[16], const uint8_t uid[4])
{
    memset(header, 0, 16);

    /* bytes 0~3: cardId = 物理 UID（与 NFC_ValidateCard 中 memcmp(buf, uid, 4) 一致） */
    memcpy(&header[0], uid, 4);

    /* bytes 4~7: magic = 0xA5A5A5A5（大端写入，与 NFC_ValidateCard 兼容） */
    header[4] = 0xA5;
    header[5] = 0xA5;
    header[6] = 0xA5;
    header[7] = 0xA5;

    /* bytes 8~11: workerId（大端，uint32_t） */
    header[8]  = (uint8_t)((s_issueWorkerId >> 24) & 0xFF);
    header[9]  = (uint8_t)((s_issueWorkerId >> 16) & 0xFF);
    header[10] = (uint8_t)((s_issueWorkerId >> 8) & 0xFF);
    header[11] = (uint8_t)(s_issueWorkerId & 0xFF);

    /* byte 12: cardType */
    header[12] = s_issueCardType;

    /* byte 13: status = 0 */
    header[13] = 0;

    /* bytes 14~15: checksum（小端，与 NFC_ValidateCard 一致） */
    uint16_t cs = Issue_CalcHeaderChecksum(header);
    header[14] = (uint8_t)(cs & 0xFF);
    header[15] = (uint8_t)((cs >> 8) & 0xFF);
}

/**
  * @brief  将 payload block 编号映射到 M1 卡扇区/块
  * @note   44 个 block (0~43) → 16 字节每块
  *         映射规则：
  *         扇区 0 块 0: 不写（厂商块）
  *         扇区 0 块 1: 账户头（独立写入，不经过此函数）
  *         扇区 0 块 2: 不使用
  *         所有扇区块 3: 不写（密钥/控制块）
  *         block  0~23 → 扇区 1~8  块 0,1,2（头像 384B）
  *         block 24~33 → 扇区 9~12 块 0,1,2/s12b0（姓名 160B）
  *         block 34~43 → 扇区 12~15 块 1,2/0,1,2/0,1（部门 160B）
  * @param  payloadBlock: 0~43
  * @param  sector: 输出扇区号 1~15
  * @param  block:  输出块号 0~2
  * @retval 1 有效映射, 0 无效（block 超出范围或映射到非法位置）
  */
static uint8_t Issue_GetPayloadTarget(uint8_t payloadBlock, uint8_t *sector, uint8_t *block)
{
    if (payloadBlock > 43) return 0;

    /* 映射表：每个 payloadBlock → {sector, block}
     * sector 0: block 0=厂商, block 1=账户头, block 2=保留, block 3=密钥
     * sector 1~8: block 0~2 各 3 块 = 24 块 (payload 0~23)
     * sector 9~11: block 0~2 各 3 块 = 9 块 (payload 24~32)
     * sector 12: block 0=payload 33, block 1=payload 34, block 2=payload 35
     * sector 13~14: block 0~2 各 3 块 = 6 块 (payload 36~41)
     * sector 15: block 0=payload 42, block 1=payload 43, block 2=保留
     */

    /* 所有扇区的块 3 均保留为密钥块, 不应映射 */
    static const struct { uint8_t sector; uint8_t block; } map[44] = {
        /* 头像 — block 0~23 → sector 1~8 */
        {1, 0}, {1, 1}, {1, 2},
        {2, 0}, {2, 1}, {2, 2},
        {3, 0}, {3, 1}, {3, 2},
        {4, 0}, {4, 1}, {4, 2},
        {5, 0}, {5, 1}, {5, 2},
        {6, 0}, {6, 1}, {6, 2},
        {7, 0}, {7, 1}, {7, 2},
        {8, 0}, {8, 1}, {8, 2},
        /* 姓名 — block 24~33 → sector 9~12 */
        {9, 0}, {9, 1}, {9, 2},
        {10, 0}, {10, 1}, {10, 2},
        {11, 0}, {11, 1}, {11, 2},
        {12, 0},
        /* 部门 — block 34~43 → sector 12~15 */
        {12, 1}, {12, 2},
        {13, 0}, {13, 1}, {13, 2},
        {14, 0}, {14, 1}, {14, 2},
        {15, 0}, {15, 1},
    };

    *sector = map[payloadBlock].sector;
    *block  = map[payloadBlock].block;

    /* 安全检查：不应映射到块 3 或扇区 0 块 0 */
    if (*block == 3 || (*sector == 0 && *block == 0))
    {
        return 0;
    }

    return 1;
}

/**
  * @brief  写入账户头 + 44 个 payload block 到 M1 卡
  * @note   调用方需确保已认证扇区 0
  * @param  uid: 4 字节卡片物理 UID
  * @retval ISSUE_ERR_NONE (0) 成功
  * @retval ISSUE_ERR_AUTH     认证某扇区失败
  * @retval ISSUE_ERR_WRITE    写某块失败
  */
static uint8_t Issue_WriteCardWithPayload(const uint8_t uid[4])
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t buf[16];

    /* ---- 1. 构造账户头 ---- */
    Issue_BuildCardHeader(buf, uid);

    /* ---- 2. 认证扇区 0 并写块 1（账户头） ---- */
    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        return ISSUE_ERR_AUTH;
    }
    if (RC522_WriteBlock(0, 1, buf) != RC522_OK)
    {
        return ISSUE_ERR_WRITE;
    }

    /* ---- 3. 写入 44 个 payload block ---- */
    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            return ISSUE_ERR_FAIL;  /* 映射表错误，不应发生 */
        }

        /* 认证目标扇区（若首次进入此扇区） */
        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            return ISSUE_ERR_AUTH;
        }

        /* 写 block */
        memcpy(buf, &s_issuePayload[pb * 16], 16);
        if (RC522_WriteBlock(sector, block, buf) != RC522_OK)
        {
            return ISSUE_ERR_WRITE;
        }
    }

    return ISSUE_ERR_NONE;
}

/**
  * @brief  读回并校验账户头 + 44 个 payload block
  * @note   逐块读回并与写入数据比对
  * @param  uid: 4 字节卡片物理 UID
  * @retval ISSUE_ERR_NONE (0) 全部校验通过
  * @retval ISSUE_ERR_AUTH     认证某扇区失败
  * @retval ISSUE_ERR_VERIFY   某块读回数据不匹配
  */
static uint8_t Issue_VerifyCardWithPayload(const uint8_t uid[4])
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t expected[16];
    uint8_t actual[16];

    /* ---- 1. 读回账户头 ---- */
    Issue_BuildCardHeader(expected, uid);

    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        return ISSUE_ERR_AUTH;
    }
    if (RC522_ReadBlock(0, 1, actual) != RC522_OK)
    {
        return ISSUE_ERR_VERIFY;
    }
    if (memcmp(expected, actual, 16) != 0)
    {
        return ISSUE_ERR_VERIFY;
    }

    /* ---- 2. 读回 44 个 payload block ---- */
    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            return ISSUE_ERR_FAIL;
        }

        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            return ISSUE_ERR_AUTH;
        }

        if (RC522_ReadBlock(sector, block, actual) != RC522_OK)
        {
            return ISSUE_ERR_VERIFY;
        }

        memcpy(expected, &s_issuePayload[pb * 16], 16);
        if (memcmp(expected, actual, 16) != 0)
        {
            return ISSUE_ERR_VERIFY;
        }
    }

    return ISSUE_ERR_NONE;
}

/**
  * @brief  等待放卡并完成写卡 + 读回校验
  * @note   由 nfcTask 调用，内部循环等待卡片 10 秒
  *         包含：寻卡 → 读 UID → 写账户头 → 写 44 block → 读回校验
  * @retval ISSUE_ERR_NONE (0) 成功
  * @retval ISSUE_ERR_NO_CARD  10 秒内未检测到卡片
  * @retval ISSUE_ERR_AUTH     认证失败
  * @retval ISSUE_ERR_WRITE    写块失败
  * @retval ISSUE_ERR_VERIFY   读回校验失败
  * @retval ISSUE_ERR_FAIL     其他错误
  */
static uint8_t Issue_WaitAndWriteCard(void)
{
    const uint32_t timeoutTicks = pdMS_TO_TICKS(10000U);  /* 10 秒 */
    uint32_t startTick = osKernelGetTickCount();
    uint8_t uid[4] = {0};

    /* ---- 等待卡片（循环寻卡） ---- */
    while (1)
    {
        if (RC522_ScanCard(uid) == RC522_OK)
        {
            break;  /* 检测到卡片 */
        }

        /* 超时检查 */
        if ((osKernelGetTickCount() - startTick) >= timeoutTicks)
        {
            return ISSUE_ERR_NO_CARD;
        }

        osDelay(100);  /* 100ms 寻卡间隔 */
    }

    /* ---- 写卡 ---- */
    uint8_t err = Issue_WriteCardWithPayload(uid);
    if (err != ISSUE_ERR_NONE)
    {
        RC522_Halt();
        return err;
    }

    /* ---- 读回校验 ---- */
    err = Issue_VerifyCardWithPayload(uid);
    if (err != ISSUE_ERR_NONE)
    {
        RC522_Halt();
        return err;
    }

    RC522_Halt();
    return ISSUE_ERR_NONE;
}

/* ====== 读卡/清卡操作辅助函数 ====== */

/**
  * @brief  重置读卡/清卡状态和缓存
  */
static void CardOp_Reset(void)
{
    s_cardOpState = CARD_OP_IDLE;
    s_cardOpErr = CARD_OP_ERR_NONE;
    memset(s_cardReadPayload, 0, sizeof(s_cardReadPayload));
    memset(s_cardReadUid, 0, sizeof(s_cardReadUid));
    s_cardReadWorkerId = 0;
    s_cardReadCardType = 0;
    s_cardReadStatus = 0;
    s_cardReadPayloadCrc = 0;
}

/**
  * @brief  解析账户头 (sector 0 block 1)
  * @note   与 NFC_ValidateCard 校验规则完全一致
  *         bytes  0~3: cardId (UID)
  *         bytes  4~7: magic (0xA5A5A5A5)
  *         bytes 8~11: workerId (大端)
  *         byte  12:   cardType
  *         byte  13:   status
  *         bytes 14~15: checksum (小端，前14字节累加和低16位)
  * @param  buf: 16 字节账户头数据
  * @param  uid: 4 字节物理 UID
  * @param  workerId: 输出工号
  * @param  cardType: 输出卡类型
  * @param  status: 输出状态
  * @retval 1 校验通过, 0 校验失败（magic 或 checksum 不匹配）
  */
static uint8_t CardOp_ParseAccountHeader(const uint8_t buf[16], const uint8_t uid[4], uint32_t *workerId, uint8_t *cardType, uint8_t *status)
{
    /* 验证卡号匹配物理UID */
    if (memcmp(buf, uid, 4) != 0)
    {
        return 0;
    }

    /* 验证 magic = 0xA5A5A5A5 (大端) */
    if (buf[4] != 0xA5 || buf[5] != 0xA5 || buf[6] != 0xA5 || buf[7] != 0xA5)
    {
        return 0;
    }

    /* 前14字节累加和校验（与 NFC_ValidateCard 一致） */
    {
        uint16_t sum = 0;
        for (uint8_t i = 0; i < 14; i++)
        {
            sum += buf[i];
        }
        uint16_t storedChecksum = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
        if (sum != storedChecksum)
        {
            return 0;
        }
    }

    /* 解析 workerId (大端) */
    *workerId = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];

    /* 解析 cardType 和 status */
    *cardType = buf[12];
    *status   = buf[13];

    /* 卡类型只允许 0/1/2 */
    if (*cardType > 2)
    {
        return 0;
    }

    return 1;
}

/**
  * @brief  执行读卡操作（由 nfcTask 调用）
  * @note   完整流程：等待放卡(10s) → 读 UID → 读账户头 → 校验 → 读 44 payload block → CRC
  *         结果写入 s_cardOpState / s_cardOpErr
  */
static void CardOp_ReadCard(void)
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint32_t timeoutTicks = pdMS_TO_TICKS(10000U);
    uint32_t startTick = osKernelGetTickCount();
    uint8_t uid[4] = {0};
    uint8_t buf[16];

    CARD_DBG("DBG READ ENTER\r\n");

    s_cardOpErr = CARD_OP_ERR_NONE;
    memset(s_cardReadPayload, 0, sizeof(s_cardReadPayload));

    /* ---- 1. 等待放卡 10 秒 ---- */
    while (1)
    {
        if (RC522_ScanCard(uid) == RC522_OK)
        {
            break;
        }
        if ((osKernelGetTickCount() - startTick) >= timeoutTicks)
        {
            CARD_DBG("DBG READ ERR NO_CARD\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_NO_CARD;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
        osDelay(100);
    }

    /* ---- 2. 读取扇区 0 块 1（账户头） ---- */
    CARD_DBG("DBG READ GOT_CARD\r\n");

    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        CARD_DBG("DBG READ ERR AUTH\r\n");
        RC522_Halt();
        s_cardOpErr = CARD_OP_ERR_AUTH;
        s_cardOpState = CARD_OP_ERROR;
        return;
    }
    if (RC522_ReadBlock(0, 1, buf) != RC522_OK)
    {
        CARD_DBG("DBG READ ERR READ\r\n");
        RC522_Halt();
        s_cardOpErr = CARD_OP_ERR_READ;
        s_cardOpState = CARD_OP_ERROR;
        return;
    }

    /* ---- 3. 校验账户头 ---- */
    {
        uint32_t wid;
        uint8_t ct, st;
        if (!CardOp_ParseAccountHeader(buf, uid, &wid, &ct, &st))
        {
            CARD_DBG("DBG READ ERR INVALID\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_INVALID;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        /* 保存 UID 和账户头信息 */
        memcpy(s_cardReadUid, uid, 4);
        s_cardReadWorkerId = wid;
        s_cardReadCardType = ct;
        s_cardReadStatus  = st;
    }

    /* ---- 4. 读取 44 个 payload block ---- */
    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            CARD_DBG("DBG READ ERR FAIL\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_FAIL;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_AUTH;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        if (RC522_ReadBlock(sector, block, buf) != RC522_OK)
        {
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_READ;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        memcpy(&s_cardReadPayload[pb * 16], buf, 16);
    }

    /* ---- 5. 计算 payload CRC-16 ---- */
    s_cardReadPayloadCrc = Issue_Crc16Xmodem(s_cardReadPayload, 704);

    /* ---- 6. 成功 ---- */
    CARD_DBG("DBG READ DONE\r\n");
    RC522_Halt();
    s_cardOpErr = CARD_OP_ERR_NONE;
    s_cardOpState = CARD_OP_READ_DONE;
}

/**
  * @brief  执行清卡操作（由 nfcTask 调用）
  * @note   完整流程：等待放卡(10s) → 读 UID → 写 0x00 到账户头+44 payload block → 读回校验
  *         结果写入 s_cardOpState / s_cardOpErr
  */
static void CardOp_ClearCard(void)
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint32_t timeoutTicks = pdMS_TO_TICKS(10000U);
    uint32_t startTick = osKernelGetTickCount();
    uint8_t uid[4] = {0};
    uint8_t zeroBuf[16];
    uint8_t verifyBuf[16];

    memset(zeroBuf, 0x00, 16);

    CARD_DBG("DBG CLEAR ENTER\r\n");

    s_cardOpErr = CARD_OP_ERR_NONE;

    /* ---- 1. 等待放卡 10 秒 ---- */
    while (1)
    {
        if (RC522_ScanCard(uid) == RC522_OK)
        {
            CARD_DBG("DBG CLEAR GOT_CARD\r\n");
            break;
        }
        if ((osKernelGetTickCount() - startTick) >= timeoutTicks)
        {
            CARD_DBG("DBG CLEAR ERR NO_CARD\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_NO_CARD;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
        osDelay(100);
    }

    /* ---- 2. 认证扇区 0 并写块 1（账户头）全 0x00 ---- */
    CARD_DBG("DBG CLEAR HEADER\r\n");
    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        CARD_DBG("DBG CLEAR ERR AUTH\r\n");
        RC522_Halt();
        s_cardOpErr = CARD_OP_ERR_AUTH;
        s_cardOpState = CARD_OP_ERROR;
        return;
    }
    if (RC522_WriteBlock(0, 1, zeroBuf) != RC522_OK)
    {
        CARD_DBG("DBG CLEAR ERR WRITE\r\n");
        RC522_Halt();
        s_cardOpErr = CARD_OP_ERR_WRITE;
        s_cardOpState = CARD_OP_ERROR;
        return;
    }

    /* ---- 3. 写 44 个 payload block 全 0x00 ---- */
    CARD_DBG("DBG CLEAR PAYLOAD BEGIN\r\n");
    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            CARD_DBG("DBG CLEAR ERR FAIL\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_FAIL;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            CARD_DBG("DBG CLEAR ERR AUTH\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_AUTH;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        if (RC522_WriteBlock(sector, block, zeroBuf) != RC522_OK)
        {
            CARD_DBG("DBG CLEAR ERR WRITE\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_WRITE;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
    }

    /* ---- 4. 读回校验：验证账户头块 1 全 0x00 ---- */
    {
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            CARD_DBG("DBG CLEAR ERR AUTH\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_AUTH;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
        if (RC522_ReadBlock(0, 1, verifyBuf) != RC522_OK)
        {
            CARD_DBG("DBG CLEAR ERR VERIFY\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_VERIFY;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
        if (memcmp(zeroBuf, verifyBuf, 16) != 0)
        {
            CARD_DBG("DBG CLEAR ERR VERIFY\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_VERIFY;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
    }

    /* ---- 5. 读回校验：验证 44 个 payload block 全 0x00 ---- */
    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            CARD_DBG("DBG CLEAR ERR FAIL\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_FAIL;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            CARD_DBG("DBG CLEAR ERR AUTH\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_AUTH;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        if (RC522_ReadBlock(sector, block, verifyBuf) != RC522_OK)
        {
            CARD_DBG("DBG CLEAR ERR VERIFY\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_VERIFY;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }

        if (memcmp(zeroBuf, verifyBuf, 16) != 0)
        {
            CARD_DBG("DBG CLEAR ERR VERIFY\r\n");
            RC522_Halt();
            s_cardOpErr = CARD_OP_ERR_VERIFY;
            s_cardOpState = CARD_OP_ERROR;
            return;
        }
    }

    /* ---- 6. 成功 ---- */
    CARD_DBG("DBG CLEAR DONE\r\n");
    RC522_Halt();
    s_cardOpErr = CARD_OP_ERR_NONE;
    s_cardOpState = CARD_OP_CLEAR_DONE;
}

/* USER CODE BEGIN Header_StartTaskUart */
/**
* @brief Function implementing the uartTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskUart */
void StartTaskUart(void *argument)
{
  /* USER CODE BEGIN StartTaskUart */

  /* 等待系统初始化完成 */
  while (!g_systemReady)
  {
    osDelay(50);
  }

  /* --- USART1 命令接收缓冲区 ---
   * 增大到 128 以容纳 ISSUE_IMAGE_DATA block=43 hex=<32 hex chars> 等长命令
   */
  #define UART_CMD_BUF_SIZE   128U

  uint8_t cmdBuf[UART_CMD_BUF_SIZE];
  uint8_t cmdLen = 0;

  /* 初始化发卡协议上下文 */
  Issue_ResetContext();

  /* Infinite loop */
  for(;;)
  {
    /* --- 发卡写卡结果轮询 ---
     * ISSUE_COMMIT 后，uartTask 设置 ISSUE_WAIT_CARD，
     * nfcTask 检测并执行写卡，完成后设置 ISSUE_DONE 或 ISSUE_ERROR。
     * uartTask 在此轮询并回复上层。
     */
    {
      IssueState_t st = s_issueState;
      if (st == ISSUE_DONE || st == ISSUE_ERROR)
      {
        if (st == ISSUE_DONE)
        {
          UartDrv_SendStr(&s_uart1Drv, "OK ISSUE DONE\r\n");
        }
        else
        {
          switch (s_issueWriteErr)
          {
          case ISSUE_ERR_NO_CARD:  UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE NO_CARD\r\n");  break;
          case ISSUE_ERR_AUTH:     UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE AUTH\r\n");      break;
          case ISSUE_ERR_WRITE:    UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE WRITE\r\n");     break;
          case ISSUE_ERR_VERIFY:   UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE VERIFY\r\n");    break;
          default:                 UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE FAIL\r\n");       break;
          }
        }
        /* 重置发卡上下文，允许下次 ISSUE_BEGIN */
        Issue_ResetContext();
        cmdLen = 0;
        continue;  /* 跳过本轮队列读取 */
      }
    }

    /* --- 非阻塞从 UartDrv 接收队列读取一帧数据, timeout=50ms ---
     * UartDrv 内部使用 HAL_UARTEx_ReceiveToIdle_IT, 空闲时自动回调并
     * 通过此队列发送 UartDrv_QueueEvent_t 数据帧副本 */
    UartDrv_QueueEvent_t evt;
    osStatus_t qstat = osMessageQueueGet(s_uart1RxQueue, &evt, NULL, 50U);

    if (qstat == osOK)
    {
      /* 遍历本帧数据中每个字节, 逐字节拼命令 */
      for (uint16_t i = 0; i < evt.len; i++)
      {
        uint8_t ch = evt.data[i];

        /* 忽略单独的回车符 '\r' — 等待 '\n' 作为行结束 */
        if (ch == '\r')
        {
          continue;
        }

        /* 行结束符：收到 '\n' */
        if (ch == '\n')
        {
          /* 去掉命令末尾可能的空白及'\r' */
          while (cmdLen > 0 && (cmdBuf[cmdLen - 1] == '\r' || cmdBuf[cmdLen - 1] == ' '))
          {
            cmdLen--;
          }

          /* 零终止 */
          cmdBuf[cmdLen] = '\0';

          /* --- 命令匹配（不区分大小写，仅比较已知命令） --- */
          {
            int matched = 0;

            /* ---- PING ---- */
            if (cmdLen == 4 &&
                ((cmdBuf[0] == 'P' || cmdBuf[0] == 'p') &&
                 (cmdBuf[1] == 'I' || cmdBuf[1] == 'i') &&
                 (cmdBuf[2] == 'N' || cmdBuf[2] == 'n') &&
                 (cmdBuf[3] == 'G' || cmdBuf[3] == 'g')))
            {
              UartDrv_SendStr(&s_uart1Drv, "PONG\r\n");
              matched = 1;
            }

            /* ---- HELP ---- */
            if (!matched && cmdLen == 4 &&
                ((cmdBuf[0] == 'H' || cmdBuf[0] == 'h') &&
                 (cmdBuf[1] == 'E' || cmdBuf[1] == 'e') &&
                 (cmdBuf[2] == 'L' || cmdBuf[2] == 'l') &&
                 (cmdBuf[3] == 'P' || cmdBuf[3] == 'p')))
            {
              UartDrv_SendStr(&s_uart1Drv,
                "OK CMDS: PING, HELP, ISSUE_BEGIN, ISSUE_IMAGE_BEGIN, "
                "ISSUE_IMAGE_DATA, ISSUE_IMAGE_END, ISSUE_COMMIT, ISSUE_CANCEL, "
                "CARD_READ, CARD_CLEAR, CONFIG_GET, CONFIG_SET_MODE, TEMP_GET, "
                "LOG_CLEAR, LOG_TEST_APPEND, LOG_LIST, LIST [n=<count>]\r\n");
              matched = 1;
            }

            /* ---- ISSUE_BEGIN type=<normal|image|admin> id=<工号> ---- */
            if (!matched && cmdLen >= 12 &&
                ((cmdBuf[0] == 'I' || cmdBuf[0] == 'i') &&
                 (cmdBuf[1] == 'S' || cmdBuf[1] == 's') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'U' || cmdBuf[3] == 'u') &&
                 (cmdBuf[4] == 'E' || cmdBuf[4] == 'e') &&
                 (cmdBuf[5] == '_' || cmdBuf[5] == '_') &&
                 (cmdBuf[6] == 'B' || cmdBuf[6] == 'b') &&
                 (cmdBuf[7] == 'E' || cmdBuf[7] == 'e') &&
                 (cmdBuf[8] == 'G' || cmdBuf[8] == 'g') &&
                 (cmdBuf[9] == 'I' || cmdBuf[9] == 'i') &&
                 (cmdBuf[10] == 'N' || cmdBuf[10] == 'n')))
            {
              matched = 1;

              if (s_issueState != ISSUE_IDLE)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE STATE\r\n");
              }
              else
              {
                /* 解析 type= 和 id= */
                const char *p = (const char *)&cmdBuf[11]; /* 跳过 "ISSUE_BEGIN" */
                const char *typeStr = NULL;
                const char *idStr = NULL;

                while (*p == ' ') p++;  /* 跳过空白 */

                /* 解析 type= */
                if (strncmp(p, "type=", 5) == 0)
                {
                  p += 5;
                  typeStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }
                else
                {
                  /* 兼容大小写: TYPE= */
                  if (strncmp(p, "TYPE=", 5) == 0)
                  {
                    p += 5;
                    typeStr = p;
                    while (*p != ' ' && *p != '\0') p++;
                  }
                }

                /* 解析 id= */
                while (*p == ' ') p++;
                if (strncmp(p, "id=", 3) == 0)
                {
                  p += 3;
                  idStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }
                else if (strncmp(p, "ID=", 3) == 0)
                {
                  p += 3;
                  idStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }

                if (typeStr == NULL || idStr == NULL)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE PARAM\r\n");
                }
                else
                {
                  /* 空终止 type 和 id 字符串（已在末尾空白处结束） */
                  /* 解析 type */
                  uint8_t ct;
                  if (strncmp(typeStr, "normal", 6) == 0)      ct = 0;
                  else if (strncmp(typeStr, "image", 5) == 0)  ct = 1;
                  else if (strncmp(typeStr, "admin", 5) == 0)  ct = 2;
                  else
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE PARAM\r\n");
                    cmdLen = 0;
                    break; /* 跳出 for 循环 */
                  }

                  /* 解析 id */
                  uint32_t wid;
                  if (!Issue_ParseDec(idStr, &wid))
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE PARAM\r\n");
                    cmdLen = 0;
                    break;
                  }

                  /* 记录并进入等待图像状态 */
                  s_issueCardType = ct;
                  s_issueWorkerId = wid;
                  s_issueState = ISSUE_WAIT_IMAGE;
                  UartDrv_SendStr(&s_uart1Drv, "OK ISSUE READY\r\n");
                }
              }
            }

            /* ---- ISSUE_IMAGE_BEGIN size=704 crc=<4HEX> ---- */
            if (!matched && cmdLen >= 18 &&
                ((cmdBuf[0] == 'I' || cmdBuf[0] == 'i') &&
                 (cmdBuf[1] == 'S' || cmdBuf[1] == 's') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'U' || cmdBuf[3] == 'u') &&
                 (cmdBuf[4] == 'E' || cmdBuf[4] == 'e') &&
                 (cmdBuf[5] == '_' || cmdBuf[5] == '_') &&
                 (cmdBuf[6] == 'I' || cmdBuf[6] == 'i') &&
                 (cmdBuf[7] == 'M' || cmdBuf[7] == 'm') &&
                 (cmdBuf[8] == 'A' || cmdBuf[8] == 'a') &&
                 (cmdBuf[9] == 'G' || cmdBuf[9] == 'g') &&
                 (cmdBuf[10] == 'E' || cmdBuf[10] == 'e') &&
                 (cmdBuf[11] == '_' || cmdBuf[11] == '_') &&
                 (cmdBuf[12] == 'B' || cmdBuf[12] == 'b') &&
                 (cmdBuf[13] == 'E' || cmdBuf[13] == 'e') &&
                 (cmdBuf[14] == 'G' || cmdBuf[14] == 'g') &&
                 (cmdBuf[15] == 'I' || cmdBuf[15] == 'i') &&
                 (cmdBuf[16] == 'N' || cmdBuf[16] == 'n')))
            {
              matched = 1;

              if (s_issueState != ISSUE_WAIT_IMAGE)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE STATE\r\n");
              }
              else
              {
                const char *p = (const char *)&cmdBuf[17]; /* 跳过 "ISSUE_IMAGE_BEGIN" */
                const char *sizeStr = NULL;
                const char *crcStr = NULL;

                while (*p == ' ') p++;

                /* 解析 size= */
                if (strncmp(p, "size=", 5) == 0)
                {
                  p += 5;
                  sizeStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }
                else if (strncmp(p, "SIZE=", 5) == 0)
                {
                  p += 5;
                  sizeStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }

                /* 解析 crc= */
                while (*p == ' ') p++;
                if (strncmp(p, "crc=", 4) == 0)
                {
                  p += 4;
                  crcStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }
                else if (strncmp(p, "CRC=", 4) == 0)
                {
                  p += 4;
                  crcStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }

                if (sizeStr == NULL || crcStr == NULL)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE PARAM\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 解析 size */
                uint32_t sz;
                if (!Issue_ParseDec(sizeStr, &sz))
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE PARAM\r\n");
                  cmdLen = 0;
                  break;
                }
                if (sz != 704)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE SIZE\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 解析 crc (4 字符 hex, 大小写不敏感) */
                uint16_t expCrc;
                /* 确保 crc 恰好 4 字符 */
                {
                  const char *cr = crcStr;
                  uint8_t clen = 0;
                  while (*cr != ' ' && *cr != '\0') { cr++; clen++; }
                  if (clen != 4 || !Issue_ParseCrc16Hex4(crcStr, &expCrc))
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE PARAM\r\n");
                    cmdLen = 0;
                    break;
                  }
                }

                /* 清空 payload 和 bitmask */
                memset(s_issuePayload, 0, sizeof(s_issuePayload));
                memset(s_issueBlockMask, 0, sizeof(s_issueBlockMask));
                s_issueBlockCount = 0;
                s_issueExpectedCrc = expCrc;
                /* 状态仍为 ISSUE_WAIT_IMAGE */

                UartDrv_SendStr(&s_uart1Drv, "OK IMAGE READY\r\n");
              }
            }

            /* ---- ISSUE_IMAGE_DATA block=<0~43> hex=<32 hex chars> ---- */
            if (!matched && cmdLen >= 15 &&
                ((cmdBuf[0] == 'I' || cmdBuf[0] == 'i') &&
                 (cmdBuf[1] == 'S' || cmdBuf[1] == 's') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'U' || cmdBuf[3] == 'u') &&
                 (cmdBuf[4] == 'E' || cmdBuf[4] == 'e') &&
                 (cmdBuf[5] == '_' || cmdBuf[5] == '_') &&
                 (cmdBuf[6] == 'I' || cmdBuf[6] == 'i') &&
                 (cmdBuf[7] == 'M' || cmdBuf[7] == 'm') &&
                 (cmdBuf[8] == 'A' || cmdBuf[8] == 'a') &&
                 (cmdBuf[9] == 'G' || cmdBuf[9] == 'g') &&
                 (cmdBuf[10] == 'E' || cmdBuf[10] == 'e') &&
                 (cmdBuf[11] == '_' || cmdBuf[11] == '_') &&
                 (cmdBuf[12] == 'D' || cmdBuf[12] == 'd') &&
                 (cmdBuf[13] == 'A' || cmdBuf[13] == 'a') &&
                 (cmdBuf[14] == 'T' || cmdBuf[14] == 't') &&
                 (cmdBuf[15] == 'A' || cmdBuf[15] == 'a')))
            {
              matched = 1;

              if (s_issueState != ISSUE_WAIT_IMAGE)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA STATE\r\n");
              }
              else
              {
                const char *p = (const char *)&cmdBuf[16]; /* 跳过 "ISSUE_IMAGE_DATA" */
                const char *blockStr = NULL;
                const char *hexStr = NULL;

                while (*p == ' ') p++;

                /* 解析 block= */
                if (strncmp(p, "block=", 6) == 0)
                {
                  p += 6;
                  blockStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }
                else if (strncmp(p, "BLOCK=", 6) == 0)
                {
                  p += 6;
                  blockStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }

                /* 解析 hex= */
                while (*p == ' ') p++;
                if (strncmp(p, "hex=", 4) == 0)
                {
                  p += 4;
                  hexStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }
                else if (strncmp(p, "HEX=", 4) == 0)
                {
                  p += 4;
                  hexStr = p;
                  while (*p != ' ' && *p != '\0') p++;
                }

                if (blockStr == NULL || hexStr == NULL)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA HEX\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 解析 block 编号 */
                uint32_t blk;
                if (!Issue_ParseDec(blockStr, &blk))
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA BLOCK\r\n");
                  cmdLen = 0;
                  break;
                }
                if (blk > 43)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA BLOCK\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 检查 hex 长度是否为 32 */
                {
                  const char *h = hexStr;
                  uint8_t hlen = 0;
                  while (*h != ' ' && *h != '\0') { h++; hlen++; }
                  if (hlen != 32)
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA HEX\r\n");
                    cmdLen = 0;
                    break;
                  }
                }

                /* 检查 block 重复 */
                if (Issue_IsBlockReceived((uint8_t)blk))
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA DUP\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 解析 hex → 16 字节 */
                uint8_t data16[16];
                if (!Issue_ParseHex16(hexStr, data16))
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE DATA HEX\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 写入 payload */
                memcpy(&s_issuePayload[blk * 16], data16, 16);
                Issue_SetBlockReceived((uint8_t)blk);
                s_issueBlockCount++;

                /* 回复 */
                char resp[64];
                snprintf(resp, sizeof(resp), "OK IMAGE DATA %u\r\n", (unsigned)blk);
                UartDrv_SendStr(&s_uart1Drv, resp);
              }
            }

            /* ---- ISSUE_IMAGE_END ---- */
            if (!matched && cmdLen >= 15 &&
                ((cmdBuf[0] == 'I' || cmdBuf[0] == 'i') &&
                 (cmdBuf[1] == 'S' || cmdBuf[1] == 's') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'U' || cmdBuf[3] == 'u') &&
                 (cmdBuf[4] == 'E' || cmdBuf[4] == 'e') &&
                 (cmdBuf[5] == '_' || cmdBuf[5] == '_') &&
                 (cmdBuf[6] == 'I' || cmdBuf[6] == 'i') &&
                 (cmdBuf[7] == 'M' || cmdBuf[7] == 'm') &&
                 (cmdBuf[8] == 'A' || cmdBuf[8] == 'a') &&
                 (cmdBuf[9] == 'G' || cmdBuf[9] == 'g') &&
                 (cmdBuf[10] == 'E' || cmdBuf[10] == 'e') &&
                 (cmdBuf[11] == '_' || cmdBuf[11] == '_') &&
                 (cmdBuf[12] == 'E' || cmdBuf[12] == 'e') &&
                 (cmdBuf[13] == 'N' || cmdBuf[13] == 'n') &&
                 (cmdBuf[14] == 'D' || cmdBuf[14] == 'd')))
            {
              matched = 1;

              if (s_issueState != ISSUE_WAIT_IMAGE)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE STATE\r\n");
              }
              else
              {
                /* 检查完整性 */
                if (s_issueBlockCount < 44)
                {
                  char resp[64];
                  snprintf(resp, sizeof(resp),
                    "ERR IMAGE INCOMPLETE %u/44\r\n", s_issueBlockCount);
                  UartDrv_SendStr(&s_uart1Drv, resp);
                }
                else
                {
                  /* 校验 CRC */
                  uint16_t calcCrc = Issue_Crc16Xmodem(s_issuePayload, 704);
                  if (calcCrc == s_issueExpectedCrc)
                  {
                    s_issueState = ISSUE_IMAGE_READY;
                    UartDrv_SendStr(&s_uart1Drv, "OK IMAGE END\r\n");
                  }
                  else
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR IMAGE CRC\r\n");
                  }
                }
              }
            }

            /* ---- ISSUE_COMMIT ---- */
            if (!matched && cmdLen >= 12 &&
                ((cmdBuf[0] == 'I' || cmdBuf[0] == 'i') &&
                 (cmdBuf[1] == 'S' || cmdBuf[1] == 's') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'U' || cmdBuf[3] == 'u') &&
                 (cmdBuf[4] == 'E' || cmdBuf[4] == 'e') &&
                 (cmdBuf[5] == '_' || cmdBuf[5] == '_') &&
                 (cmdBuf[6] == 'C' || cmdBuf[6] == 'c') &&
                 (cmdBuf[7] == 'O' || cmdBuf[7] == 'o') &&
                 (cmdBuf[8] == 'M' || cmdBuf[8] == 'm') &&
                 (cmdBuf[9] == 'M' || cmdBuf[9] == 'm') &&
                 (cmdBuf[10] == 'I' || cmdBuf[10] == 'i') &&
                 (cmdBuf[11] == 'T' || cmdBuf[11] == 't')))
            {
              matched = 1;

              if (s_issueState != ISSUE_IMAGE_READY)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR ISSUE NO_IMAGE\r\n");
              }
              else
              {
                /* 进入等待放卡状态 — nfcTask 将检测并执行写卡流程 */
                s_issueState = ISSUE_WAIT_CARD;
                /* 不立即回复 — 等待 nfcTask 完成写卡后通过状态变更通知 */
              }
            }

            /* ---- ISSUE_CANCEL ---- */
            if (!matched && cmdLen >= 12 &&
                ((cmdBuf[0] == 'I' || cmdBuf[0] == 'i') &&
                 (cmdBuf[1] == 'S' || cmdBuf[1] == 's') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'U' || cmdBuf[3] == 'u') &&
                 (cmdBuf[4] == 'E' || cmdBuf[4] == 'e') &&
                 (cmdBuf[5] == '_' || cmdBuf[5] == '_') &&
                 (cmdBuf[6] == 'C' || cmdBuf[6] == 'c') &&
                 (cmdBuf[7] == 'A' || cmdBuf[7] == 'a') &&
                 (cmdBuf[8] == 'N' || cmdBuf[8] == 'n') &&
                 (cmdBuf[9] == 'C' || cmdBuf[9] == 'c') &&
                 (cmdBuf[10] == 'E' || cmdBuf[10] == 'e') &&
                 (cmdBuf[11] == 'L' || cmdBuf[11] == 'l')))
            {
              matched = 1;

              Issue_ResetContext();
              UartDrv_SendStr(&s_uart1Drv, "OK CANCEL\r\n");
            }

            /* ---- CARD_READ ---- */
            if (!matched && cmdLen == 9 &&
                ((cmdBuf[0] == 'C' || cmdBuf[0] == 'c') &&
                 (cmdBuf[1] == 'A' || cmdBuf[1] == 'a') &&
                 (cmdBuf[2] == 'R' || cmdBuf[2] == 'r') &&
                 (cmdBuf[3] == 'D' || cmdBuf[3] == 'd') &&
                 (cmdBuf[4] == '_' || cmdBuf[4] == '_') &&
                 (cmdBuf[5] == 'R' || cmdBuf[5] == 'r') &&
                 (cmdBuf[6] == 'E' || cmdBuf[6] == 'e') &&
                 (cmdBuf[7] == 'A' || cmdBuf[7] == 'a') &&
                 (cmdBuf[8] == 'D' || cmdBuf[8] == 'd')))
            {
              matched = 1;

              CARD_DBG("DBG CARD_READ CMD\r\n");

              /* 检查状态：发卡过程中不可读卡 */
              if (s_cardOpState != CARD_OP_IDLE || s_issueState != ISSUE_IDLE)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR CARD BUSY\r\n");
                cmdLen = 0;
                break;
              }

              /* 通知 nfcTask 执行读卡 */
              s_cardOpState = CARD_OP_READ_WAIT_CARD;

              /* 轮询等待结果（最长 12 秒，给 nfcTask 10 秒等待放卡 + 2 秒操作） */
              {
                uint32_t pollStart = osKernelGetTickCount();
                const uint32_t pollTimeout = pdMS_TO_TICKS(20000U);
                CardOpState_t st;
                do {
                  osDelay(10);
                  st = s_cardOpState;
                } while ((st != CARD_OP_READ_DONE) && (st != CARD_OP_ERROR) &&
                         (osKernelGetTickCount() - pollStart) < pollTimeout);

                CARD_DBG("DBG UART READ RESULT\r\n");

                if (st != CARD_OP_READ_DONE && st != CARD_OP_ERROR)
                {
                  /* 超时或意外状态 */
                  CardOp_Reset();
                  UartDrv_SendStr(&s_uart1Drv, "ERR CARD FAIL\r\n");
                }
                else if (st == CARD_OP_ERROR)
                {
                  /* 返回错误 */
                  switch (s_cardOpErr)
                  {
                  case CARD_OP_ERR_NO_CARD: UartDrv_SendStr(&s_uart1Drv, "ERR CARD NO_CARD\r\n");  break;
                  case CARD_OP_ERR_AUTH:    UartDrv_SendStr(&s_uart1Drv, "ERR CARD AUTH\r\n");      break;
                  case CARD_OP_ERR_READ:    UartDrv_SendStr(&s_uart1Drv, "ERR CARD READ\r\n");      break;
                  case CARD_OP_ERR_INVALID: UartDrv_SendStr(&s_uart1Drv, "ERR CARD INVALID\r\n");   break;
                  default:                  UartDrv_SendStr(&s_uart1Drv, "ERR CARD FAIL\r\n");       break;
                  }
                  /* 重置，允许重试 */
                  CardOp_Reset();
                }
                else if (st == CARD_OP_READ_DONE)
                {
                  /* 成功 — 返回账户头 + 44 block payload */
                  char resp[128];
                  const char *typeStr = "unknown";
                  switch (s_cardReadCardType)
                  {
                  case 0: typeStr = "normal"; break;
                  case 1: typeStr = "image";  break;
                  case 2: typeStr = "admin";  break;
                  default: break;
                  }

                  /* OK CARD READ BEGIN */
                  snprintf(resp, sizeof(resp),
                    "OK CARD READ BEGIN uid=%02X%02X%02X%02X id=%lu type=%s status=%u size=704 crc=%04X\r\n",
                    s_cardReadUid[0], s_cardReadUid[1], s_cardReadUid[2], s_cardReadUid[3],
                    (unsigned long)s_cardReadWorkerId,
                    typeStr,
                    s_cardReadStatus,
                    s_cardReadPayloadCrc);
                  UartDrv_SendStr(&s_uart1Drv, resp);

                  /* OK CARD READ DATA block=00~43 */
                  for (uint8_t blk = 0; blk < 44; blk++)
                  {
                    snprintf(resp, sizeof(resp),
                      "OK CARD READ DATA block=%02u hex=", blk);
                    UartDrv_SendStr(&s_uart1Drv, resp);

                    /* 32 个大写 HEX 字符 */
                    char hexLine[33];
                    for (uint8_t k = 0; k < 16; k++)
                    {
                      uint8_t b = s_cardReadPayload[blk * 16 + k];
                      hexLine[k * 2]     = "0123456789ABCDEF"[b >> 4];
                      hexLine[k * 2 + 1] = "0123456789ABCDEF"[b & 0x0F];
                    }
                    hexLine[32] = '\0';
                    UartDrv_SendStr(&s_uart1Drv, hexLine);
                    UartDrv_SendStr(&s_uart1Drv, "\r\n");
                  }

                  /* OK CARD READ END */
                  UartDrv_SendStr(&s_uart1Drv, "OK CARD READ END\r\n");

                  /* 重置，允许下次操作 */
                  CardOp_Reset();
                }
              }
            }

            /* ---- CARD_CLEAR ---- */
            if (!matched && cmdLen == 10 &&
                ((cmdBuf[0] == 'C' || cmdBuf[0] == 'c') &&
                 (cmdBuf[1] == 'A' || cmdBuf[1] == 'a') &&
                 (cmdBuf[2] == 'R' || cmdBuf[2] == 'r') &&
                 (cmdBuf[3] == 'D' || cmdBuf[3] == 'd') &&
                 (cmdBuf[4] == '_' || cmdBuf[4] == '_') &&
                 (cmdBuf[5] == 'C' || cmdBuf[5] == 'c') &&
                 (cmdBuf[6] == 'L' || cmdBuf[6] == 'l') &&
                 (cmdBuf[7] == 'E' || cmdBuf[7] == 'e') &&
                 (cmdBuf[8] == 'A' || cmdBuf[8] == 'a') &&
                 (cmdBuf[9] == 'R' || cmdBuf[9] == 'r')))
            {
              matched = 1;

              CARD_DBG("DBG CARD_CLEAR CMD\r\n");

              /* 检查状态：发卡过程中不可清卡 */
              if (s_cardOpState != CARD_OP_IDLE || s_issueState != ISSUE_IDLE)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR CARD BUSY\r\n");
                cmdLen = 0;
                break;
              }

              /* 通知 nfcTask 执行清卡 */
              s_cardOpState = CARD_OP_CLEAR_WAIT_CARD;

              /* 轮询等待结果（最长 15 秒，写+校验耗时更多） */
              {
                uint32_t pollStart = osKernelGetTickCount();
                const uint32_t pollTimeout = pdMS_TO_TICKS(20000U);
                CardOpState_t st;
                do {
                  osDelay(10);
                  st = s_cardOpState;
                } while ((st != CARD_OP_CLEAR_DONE) && (st != CARD_OP_ERROR) &&
                         (osKernelGetTickCount() - pollStart) < pollTimeout);

                CARD_DBG("DBG UART CLEAR RESULT\r\n");

                if (st != CARD_OP_CLEAR_DONE && st != CARD_OP_ERROR)
                {
                  /* 超时或意外状态 */
                  CardOp_Reset();
                  UartDrv_SendStr(&s_uart1Drv, "ERR CARD FAIL\r\n");
                }
                else if (st == CARD_OP_ERROR)
                {
                  /* 返回错误 */
                  switch (s_cardOpErr)
                  {
                  case CARD_OP_ERR_NO_CARD: UartDrv_SendStr(&s_uart1Drv, "ERR CARD NO_CARD\r\n");  break;
                  case CARD_OP_ERR_AUTH:    UartDrv_SendStr(&s_uart1Drv, "ERR CARD AUTH\r\n");      break;
                  case CARD_OP_ERR_WRITE:   UartDrv_SendStr(&s_uart1Drv, "ERR CARD WRITE\r\n");     break;
                  case CARD_OP_ERR_VERIFY:  UartDrv_SendStr(&s_uart1Drv, "ERR CARD VERIFY\r\n");    break;
                  default:                  UartDrv_SendStr(&s_uart1Drv, "ERR CARD FAIL\r\n");       break;
                  }
                  CardOp_Reset();
                }
                else if (st == CARD_OP_CLEAR_DONE)
                {
                  UartDrv_SendStr(&s_uart1Drv, "OK CARD CLEAR DONE\r\n");
                  CardOp_Reset();
                }
              }
            }

            /* ---- CONFIG_GET ---- */
            if (!matched && cmdLen == 10 &&
                ((cmdBuf[0] == 'C' || cmdBuf[0] == 'c') &&
                 (cmdBuf[1] == 'O' || cmdBuf[1] == 'o') &&
                 (cmdBuf[2] == 'N' || cmdBuf[2] == 'n') &&
                 (cmdBuf[3] == 'F' || cmdBuf[3] == 'f') &&
                 (cmdBuf[4] == 'I' || cmdBuf[4] == 'i') &&
                 (cmdBuf[5] == 'G' || cmdBuf[5] == 'g') &&
                 (cmdBuf[6] == '_' || cmdBuf[6] == '_') &&
                 (cmdBuf[7] == 'G' || cmdBuf[7] == 'g') &&
                 (cmdBuf[8] == 'E' || cmdBuf[8] == 'e') &&
                 (cmdBuf[9] == 'T' || cmdBuf[9] == 't')))
            {
              matched = 1;

              const DeviceConfig_t *cfg = AppConfig_Get();
              if (cfg == NULL)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR CONFIG FAIL\r\n");
              }
              else
              {
                const char *modeStr = "BOTH";
                if (cfg->attendanceMode == ATT_MODE_ENTRY) modeStr = "ENTRY";
                else if (cfg->attendanceMode == ATT_MODE_EXIT) modeStr = "EXIT";

                char resp[64];
                snprintf(resp, sizeof(resp),
                  "OK CONFIG device=%lu mode=%s\r\n",
                  (unsigned long)cfg->deviceId, modeStr);
                UartDrv_SendStr(&s_uart1Drv, resp);
              }
            }

            /* ---- CONFIG_SET_MODE <0|1|2> ---- */
            if (!matched && cmdLen >= 14 &&
                ((cmdBuf[0] == 'C' || cmdBuf[0] == 'c') &&
                 (cmdBuf[1] == 'O' || cmdBuf[1] == 'o') &&
                 (cmdBuf[2] == 'N' || cmdBuf[2] == 'n') &&
                 (cmdBuf[3] == 'F' || cmdBuf[3] == 'f') &&
                 (cmdBuf[4] == 'I' || cmdBuf[4] == 'i') &&
                 (cmdBuf[5] == 'G' || cmdBuf[5] == 'g') &&
                 (cmdBuf[6] == '_' || cmdBuf[6] == '_') &&
                 (cmdBuf[7] == 'S' || cmdBuf[7] == 's') &&
                 (cmdBuf[8] == 'E' || cmdBuf[8] == 'e') &&
                 (cmdBuf[9] == 'T' || cmdBuf[9] == 't') &&
                 (cmdBuf[10] == '_' || cmdBuf[10] == '_') &&
                 (cmdBuf[11] == 'M' || cmdBuf[11] == 'm') &&
                 (cmdBuf[12] == 'O' || cmdBuf[12] == 'o') &&
                 (cmdBuf[13] == 'D' || cmdBuf[13] == 'd') &&
                 (cmdBuf[14] == 'E' || cmdBuf[14] == 'e')))
            {
              matched = 1;

              /* 提取空格后的数字参数 */
              const char *p = (const char *)&cmdBuf[15]; /* 跳过 "CONFIG_SET_MODE" */
              while (*p == ' ') p++;

              if (*p < '0' || *p > '2' || (p[1] != ' ' && p[1] != '\0'))
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR CONFIG PARAM\r\n");
              }
              else
              {
                uint8_t mode = (uint8_t)(*p - '0');
                uint8_t ret = AppConfig_SetMode(mode);
                if (ret == 0)
                {
                  UartDrv_SendStr(&s_uart1Drv, "OK CONFIG SET\r\n");
                }
                else if (ret == 2)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR CONFIG PARAM\r\n");
                }
                else
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR CONFIG FAIL\r\n");
                }
              }
            }

            /* ---- TEMP_GET 温度传感器诊断命令 ---- */
            if (!matched && cmdLen == 8 &&
                ((cmdBuf[0] == 'T' || cmdBuf[0] == 't') &&
                 (cmdBuf[1] == 'E' || cmdBuf[1] == 'e') &&
                 (cmdBuf[2] == 'M' || cmdBuf[2] == 'm') &&
                 (cmdBuf[3] == 'P' || cmdBuf[3] == 'p') &&
                 (cmdBuf[4] == '_' || cmdBuf[4] == '_') &&
                 (cmdBuf[5] == 'G' || cmdBuf[5] == 'g') &&
                 (cmdBuf[6] == 'E' || cmdBuf[6] == 'e') &&
                 (cmdBuf[7] == 'T' || cmdBuf[7] == 't')))
            {
              matched = 1;

              char tempBuf[128];
              const char *tempStr = "--.-";
              if (g_tempValid)
              {
                snprintf(tempBuf, sizeof(tempBuf), "%.1f", (double)g_temperature);
                tempStr = tempBuf;
              }

              snprintf(tempBuf, sizeof(tempBuf),
                "OK TEMP init=%u valid=%u lastRet=%u raw=%.1f temp=%s "
                "reads=%lu oks=%lu retry=%lu\r\n",
                (unsigned)g_ds18b20InitOk,
                (unsigned)g_tempValid,
                (unsigned)g_tempLastRet,
                (double)g_tempLastRaw,
                tempStr,
                (unsigned long)g_tempReadCount,
                (unsigned long)g_tempOkCount,
                (unsigned long)g_tempInitRetryCount);
              UartDrv_SendStr(&s_uart1Drv, tempBuf);
            }

            /* ---- LOG_CLEAR ---- */
            if (!matched && cmdLen == 9 &&
                ((cmdBuf[0] == 'L' || cmdBuf[0] == 'l') &&
                 (cmdBuf[1] == 'O' || cmdBuf[1] == 'o') &&
                 (cmdBuf[2] == 'G' || cmdBuf[2] == 'g') &&
                 (cmdBuf[3] == '_' || cmdBuf[3] == '_') &&
                 (cmdBuf[4] == 'C' || cmdBuf[4] == 'c') &&
                 (cmdBuf[5] == 'L' || cmdBuf[5] == 'l') &&
                 (cmdBuf[6] == 'E' || cmdBuf[6] == 'e') &&
                 (cmdBuf[7] == 'A' || cmdBuf[7] == 'a') &&
                 (cmdBuf[8] == 'R' || cmdBuf[8] == 'r')))
            {
              matched = 1;
              uint8_t ret = AttendanceLog_Clear();
              if (ret == 0)
              {
                UartDrv_SendStr(&s_uart1Drv, "OK LOG CLEAR\r\n");
              }
              else
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR LOG FAIL\r\n");
              }
            }

            /* ---- LOG_TEST_APPEND id=<workerId> event=<IN|OUT|DENY> duration=<seconds> ---- */
            if (!matched && cmdLen >= 14 &&
                ((cmdBuf[0] == 'L' || cmdBuf[0] == 'l') &&
                 (cmdBuf[1] == 'O' || cmdBuf[1] == 'o') &&
                 (cmdBuf[2] == 'G' || cmdBuf[2] == 'g') &&
                 (cmdBuf[3] == '_' || cmdBuf[3] == '_') &&
                 (cmdBuf[4] == 'T' || cmdBuf[4] == 't') &&
                 (cmdBuf[5] == 'E' || cmdBuf[5] == 'e') &&
                 (cmdBuf[6] == 'S' || cmdBuf[6] == 's') &&
                 (cmdBuf[7] == 'T' || cmdBuf[7] == 't') &&
                 (cmdBuf[8] == '_' || cmdBuf[8] == '_') &&
                 (cmdBuf[9] == 'A' || cmdBuf[9] == 'a') &&
                 (cmdBuf[10] == 'P' || cmdBuf[10] == 'p') &&
                 (cmdBuf[11] == 'P' || cmdBuf[11] == 'p') &&
                 (cmdBuf[12] == 'E' || cmdBuf[12] == 'e') &&
                 (cmdBuf[13] == 'N' || cmdBuf[13] == 'n') &&
                 (cmdBuf[14] == 'D' || cmdBuf[14] == 'd')))
            {
              matched = 1;

              /* 解析 id= event= duration= */
              const char *lp = (const char *)&cmdBuf[15]; /* 跳过 "LOG_TEST_APPEND" */
              const char *lidStr = NULL;
              const char *levtStr = NULL;
              const char *ldurStr = NULL;
              uint8_t lparamOk = 1;

              while (*lp == ' ') lp++;

              /* 解析 id= */
              if (strncmp(lp, "id=", 3) == 0 || strncmp(lp, "ID=", 3) == 0)
              {
                lp += 3;
                lidStr = lp;
                while (*lp != ' ' && *lp != '\0') lp++;
              }
              else
              {
                lparamOk = 0;
              }

              /* 解析 event= */
              while (*lp == ' ') lp++;
              if (lparamOk && (strncmp(lp, "event=", 6) == 0 || strncmp(lp, "EVENT=", 6) == 0))
              {
                lp += 6;
                levtStr = lp;
                while (*lp != ' ' && *lp != '\0') lp++;
              }
              else
              {
                lparamOk = 0;
              }

              /* 解析 duration= (可选) */
              while (*lp == ' ') lp++;
              if (lparamOk && *lp != '\0')
              {
                if (strncmp(lp, "duration=", 9) == 0 || strncmp(lp, "DURATION=", 9) == 0)
                {
                  lp += 9;
                  ldurStr = lp;
                  while (*lp != ' ' && *lp != '\0') lp++;
                }
                else
                {
                  /* duration 不是必须的，忽略未知参数 */
                  while (*lp != ' ' && *lp != '\0') lp++;
                }
              }

              if (!lparamOk || lidStr == NULL || levtStr == NULL)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR LOG PARAM\r\n");
              }
              else
              {
                /* 解析 workerId */
                uint32_t lwid;
                if (!Issue_ParseDec(lidStr, &lwid))
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR LOG PARAM\r\n");
                  cmdLen = 0;
                  break;
                }

                /* 解析 eventType */
                uint8_t levt;
                {
                  /* 先获取事件字符串长度 */
                  const char *es = levtStr;
                  uint8_t esLen = 0;
                  while (*es != ' ' && *es != '\0') { es++; esLen++; }

                  if ((esLen == 2 && (levtStr[0] == 'I' || levtStr[0] == 'i') &&
                                      (levtStr[1] == 'N' || levtStr[1] == 'n')))
                  {
                    levt = ATT_LOG_EVENT_IN;
                  }
                  else if ((esLen == 3 && (levtStr[0] == 'O' || levtStr[0] == 'o') &&
                                           (levtStr[1] == 'U' || levtStr[1] == 'u') &&
                                           (levtStr[2] == 'T' || levtStr[2] == 't')))
                  {
                    levt = ATT_LOG_EVENT_OUT;
                  }
                  else if ((esLen == 4 && (levtStr[0] == 'D' || levtStr[0] == 'd') &&
                                           (levtStr[1] == 'E' || levtStr[1] == 'e') &&
                                           (levtStr[2] == 'N' || levtStr[2] == 'n') &&
                                           (levtStr[3] == 'Y' || levtStr[3] == 'y')))
                  {
                    levt = ATT_LOG_EVENT_DENY;
                  }
                  else if (esLen == 1 && levtStr[0] == '1')
                  {
                    levt = ATT_LOG_EVENT_IN;
                  }
                  else if (esLen == 1 && levtStr[0] == '2')
                  {
                    levt = ATT_LOG_EVENT_OUT;
                  }
                  else if (esLen == 1 && levtStr[0] == '3')
                  {
                    levt = ATT_LOG_EVENT_DENY;
                  }
                  else
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR LOG PARAM\r\n");
                    cmdLen = 0;
                    break;
                  }
                }

                /* 解析 duration */
                uint32_t ldur = 0;
                if (ldurStr != NULL)
                {
                  if (!Issue_ParseDec(ldurStr, &ldur))
                  {
                    UartDrv_SendStr(&s_uart1Drv, "ERR LOG PARAM\r\n");
                    cmdLen = 0;
                    break;
                  }
                }

                /* 调用 AttendanceLog_AppendTest */
                uint8_t lret = AttendanceLog_AppendTest(lwid, levt, ldur);
                if (lret == 0)
                {
                  UartDrv_SendStr(&s_uart1Drv, "OK LOG APPEND\r\n");
                }
                else
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR LOG FAIL\r\n");
                }
              }
            }

            /* ---- LOG_LIST n=<count> ---- */
            if (!matched && cmdLen >= 8 &&
                ((cmdBuf[0] == 'L' || cmdBuf[0] == 'l') &&
                 (cmdBuf[1] == 'O' || cmdBuf[1] == 'o') &&
                 (cmdBuf[2] == 'G' || cmdBuf[2] == 'g') &&
                 (cmdBuf[3] == '_' || cmdBuf[3] == '_') &&
                 (cmdBuf[4] == 'L' || cmdBuf[4] == 'l') &&
                 (cmdBuf[5] == 'I' || cmdBuf[5] == 'i') &&
                 (cmdBuf[6] == 'S' || cmdBuf[6] == 's') &&
                 (cmdBuf[7] == 'T' || cmdBuf[7] == 't')))
            {
              matched = 1;

              uint16_t lcount = 10; /* 默认 10 条 */

              /* 解析 n= */
              const char *lq = (const char *)&cmdBuf[8]; /* 跳过 "LOG_LIST" */
              while (*lq == ' ') lq++;

              if (*lq != '\0')
              {
                if (strncmp(lq, "n=", 2) == 0 || strncmp(lq, "N=", 2) == 0)
                {
                  lq += 2;
                  const char *lnStr = lq;
                  while (*lq != ' ' && *lq != '\0') lq++;

                  uint32_t lnum;
                  if (Issue_ParseDec(lnStr, &lnum))
                  {
                    if (lnum < 1) lnum = 1;
                    if (lnum > 20) lnum = 20;
                    lcount = (uint16_t)lnum;
                  }
                  else
                  {
                    /* 参数格式错误，使用默认 10 */
                    lcount = 10;
                  }
                }
                else
                {
                  /* 直接尝试解析为数字 */
                  const char *lnStr = lq;
                  while (*lq != ' ' && *lq != '\0') lq++;
                  uint32_t lnum;
                  if (Issue_ParseDec(lnStr, &lnum))
                  {
                    if (lnum < 1) lnum = 1;
                    if (lnum > 20) lnum = 20;
                    lcount = (uint16_t)lnum;
                  }
                }
              }

              /* 读取记录 */
              #define LOG_LIST_BUF_SIZE 20
              AttendanceRecord_t lrecords[LOG_LIST_BUF_SIZE];
              uint16_t loutCount = 0;
              uint8_t lret2 = AttendanceLog_GetRecent(lcount, lrecords, &loutCount);

              if (lret2 != 0)
              {
                UartDrv_SendStr(&s_uart1Drv, "ERR LOG FAIL\r\n");
              }
              else
              {
                char lresp[128];
                snprintf(lresp, sizeof(lresp), "OK LOG BEGIN count=%u\r\n", loutCount);
                UartDrv_SendStr(&s_uart1Drv, lresp);

                for (uint16_t li = 0; li < loutCount; li++)
                {
                  const char *evtStr = "UNKNOWN";
                  switch (lrecords[li].eventType)
                  {
                  case ATT_LOG_EVENT_IN:   evtStr = "IN";   break;
                  case ATT_LOG_EVENT_OUT:  evtStr = "OUT";  break;
                  case ATT_LOG_EVENT_DENY: evtStr = "DENY"; break;
                  default: break;
                  }

                  const char *modeStr = "BOTH";
                  switch (lrecords[li].mode)
                  {
                  case ATT_MODE_ENTRY: modeStr = "ENTRY"; break;
                  case ATT_MODE_EXIT:  modeStr = "EXIT";  break;
                  case ATT_MODE_BOTH:  modeStr = "BOTH";  break;
                  default: break;
                  }

                  snprintf(lresp, sizeof(lresp),
                    "OK LOG seq=%lu id=%lu event=%s mode=%s time=%04u-%02u-%02u %02u:%02u:%02u duration=%lu\r\n",
                    (unsigned long)lrecords[li].seq,
                    (unsigned long)lrecords[li].workerId,
                    evtStr,
                    modeStr,
                    lrecords[li].year, lrecords[li].month, lrecords[li].day,
                    lrecords[li].hour, lrecords[li].minute, lrecords[li].second,
                    (unsigned long)lrecords[li].durationSec);
                  UartDrv_SendStr(&s_uart1Drv, lresp);
                }

                UartDrv_SendStr(&s_uart1Drv, "OK LOG END\r\n");
              }
            }

            /* ---- LIST [n=<count>] ---- */
            if (!matched && cmdLen >= 4 &&
                ((cmdBuf[0] == 'L' || cmdBuf[0] == 'l') &&
                 (cmdBuf[1] == 'I' || cmdBuf[1] == 'i') &&
                 (cmdBuf[2] == 'S' || cmdBuf[2] == 's') &&
                 (cmdBuf[3] == 'T' || cmdBuf[3] == 't')))
            {
              const char *la = (const char *)&cmdBuf[4];
              while (*la == ' ') la++;
              if (*la == '\0' || strncmp(la, "n=", 2) == 0 || strncmp(la, "N=", 2) == 0)
              {
                matched = 1;

                uint16_t lcnt = 20;
                if (*la != '\0')
                {
                  if (strncmp(la, "n=", 2) == 0 || strncmp(la, "N=", 2) == 0)
                  {
                    la += 2;
                    const char *lnStr = la;
                    while (*la != ' ' && *la != '\0') la++;

                    uint32_t lnum;
                    if (Issue_ParseDec(lnStr, &lnum))
                    {
                      if (lnum < 1) lnum = 1;
                      if (lnum > 20) lnum = 20;
                      lcnt = (uint16_t)lnum;
                    }
                  }
                }

                const DeviceConfig_t *cfg = AppConfig_Get();
                uint32_t devId = cfg ? cfg->deviceId : 0;

                #define LIST_BUF_SIZE 20
                AttendanceRecord_t lr[LIST_BUF_SIZE];
                uint16_t loutC = 0;
                uint8_t lrRet = AttendanceLog_GetRecent(lcnt, lr, &loutC);

                if (lrRet != 0)
                {
                  UartDrv_SendStr(&s_uart1Drv, "ERR LIST FAIL\r\n");
                }
                else
                {
                  char lrsp[128];
                  snprintf(lrsp, sizeof(lrsp), "OK LIST BEGIN count=%u\r\n", loutC);
                  UartDrv_SendStr(&s_uart1Drv, lrsp);

                  for (uint16_t li = 0; li < loutC; li++)
                  {
                    const char *evtStr = "UNK";
                    switch (lr[li].eventType)
                    {
                    case ATT_LOG_EVENT_IN:   evtStr = "IN";   break;
                    case ATT_LOG_EVENT_OUT:  evtStr = "OUT";  break;
                    case ATT_LOG_EVENT_DENY: evtStr = "DENY"; break;
                    default: break;
                    }

                    if (lr[li].eventType == ATT_LOG_EVENT_OUT && lr[li].durationSec > 0)
                    {
                      snprintf(lrsp, sizeof(lrsp),
                        "OK LIST %04u-%02u-%02u %02u:%02u:%02u DEV=%lu ID=%lu %s DUR=%lus\r\n",
                        lr[li].year, lr[li].month, lr[li].day,
                        lr[li].hour, lr[li].minute, lr[li].second,
                        (unsigned long)devId,
                        (unsigned long)lr[li].workerId,
                        evtStr,
                        (unsigned long)lr[li].durationSec);
                    }
                    else
                    {
                      snprintf(lrsp, sizeof(lrsp),
                        "OK LIST %04u-%02u-%02u %02u:%02u:%02u DEV=%lu ID=%lu %s\r\n",
                        lr[li].year, lr[li].month, lr[li].day,
                        lr[li].hour, lr[li].minute, lr[li].second,
                        (unsigned long)devId,
                        (unsigned long)lr[li].workerId,
                        evtStr);
                    }
                    UartDrv_SendStr(&s_uart1Drv, lrsp);
                  }

                  UartDrv_SendStr(&s_uart1Drv, "OK LIST END\r\n");
                }
              }
            }

            /* 未知命令 */
            if (!matched && cmdLen > 0)
            {
              UartDrv_SendStr(&s_uart1Drv, "ERR UNKNOWN CMD\r\n");
            }
          }

          /* 清空缓冲区 */
          cmdLen = 0;
        }
        else
        {
          /* 普通字符 */
          if (cmdLen < (UART_CMD_BUF_SIZE - 1))
          {
            cmdBuf[cmdLen++] = ch;
          }
          else
          {
            /* 缓冲区溢出：丢弃并重置 */
            cmdLen = 0;
          }
        }
      }
    }
  }
  /* USER CODE END StartTaskUart */
}

/* ================================================================
 *  阶段一·第五步：声光反馈任务 (otherTask)
 *  LED 跑马灯 / 闪烁 + 蜂鸣器提示
 * ================================================================ */
/* USER CODE BEGIN Header_StartTaskOther */
/**
* @brief Function implementing the otherTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskOther */
void StartTaskOther(void *argument)
{
  /* USER CODE BEGIN StartTaskOther */

  /* 等待系统初始化完成（非阻塞轮询，确保不会永久卡死） */
  while (!g_systemReady)
  {
    /* 同时消费可能已投递的 INIT_DONE 消息，防止队列堆积 */
    AppMessage_t msg;
    if (osMessageQueueGet(myQueue04Handle, &msg, NULL, 0U) == osOK)
    {
      /* 已收到初始化完成消息，但仍检查 g_systemReady 确认 */
    }
    osDelay(50);
  }

  /* ====== 蜂鸣器发声宏 ======
   * PB4 -> TIM3 CH1 PWM 输出，无源蜂鸣器需要 PWM 方波驱动
   * TIM3: 时钟 1MHz, Period=1000-1 -> PWM 频率 1kHz, Pulse=500 -> 占空比 50%
   */
#define BEEP_ON()   HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1)
#define BEEP_OFF()  HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1)

  /* ---- DS18B20 温度读取状态 ---- */
  uint32_t tempLastCheckTick = 0;
  #define TEMP_CHECK_INTERVAL_MS  2000U

  /* Infinite loop */
  for(;;)
  {
    /* ---- 非阻塞接收反馈消息（timeout=20ms，保证温度检查周期） ---- */
    AppMessage_t msg;
    if (osMessageQueueGet(myQueue04Handle, &msg, NULL, 20U) == osOK)
    {
      switch (msg.type)
      {
      case MSG_FEEDBACK_VALID:
      {
        /* 有效卡（入场/离场）：短鸣（L1 由 nfcTask 状态灯统一控制） */
        BEEP_ON();
        osDelay(50);
        BEEP_OFF();
        break;
      }

      case MSG_FEEDBACK_INVALID:
      {
        /* 无效卡：L7 闪 2 次，每次 100ms + 短鸣 */
        for (uint8_t i = 0; i < 2; i++)
        {
          LED_On(6);
          BEEP_ON();
          osDelay(50);
          BEEP_OFF();
          osDelay(50);
          LED_Off(6);
          osDelay(50);
        }
        break;
      }

      case MSG_FEEDBACK_DUP:
      {
        /* 重复刷卡：仅短鸣（L4 不再控制，避免与 ENTRY IN 状态灯冲突） */
        BEEP_ON();
        osDelay(50);
        BEEP_OFF();
        break;
      }

      case MSG_FEEDBACK_ADMIN:
      {
        /* 管理员卡：L1~L7 同时亮 300ms + 长鸣 */
        LED_SetLeds(0x7F);
        BEEP_ON();
        osDelay(300);
        BEEP_OFF();
        LED_SetLeds(0x00);
        break;
      }

      default:
        break;
      }
    }

    /* ---- 周期温度读取（每 2 秒） ---- */
    if (g_ds18b20InitOk)
    {
      if ((osKernelGetTickCount() - tempLastCheckTick) >= pdMS_TO_TICKS(TEMP_CHECK_INTERVAL_MS))
      {
        tempLastCheckTick = osKernelGetTickCount();

        float temp;
        uint8_t ret = ds18b20_read_temperature(&temp);
        g_tempLastRet = ret;
        g_tempReadCount++;
        g_tempLastRaw = ds18b20_get_last_raw();

        if (ret == 0)
        {
          /* 有效温度，更新缓存 */
          g_temperature = temp;
          g_tempValid = 1;
          g_tempOkCount++;
        }
        /* 返回非 0：保留上次有效温度，g_tempValid 不变 */
      }
    }
    else
    {
      /* DS18B20 初始化失败，每 2 秒重试一次 */
      if ((osKernelGetTickCount() - tempLastCheckTick) >= pdMS_TO_TICKS(TEMP_CHECK_INTERVAL_MS))
      {
        tempLastCheckTick = osKernelGetTickCount();
        g_tempInitRetryCount++;
        if (ds18b20_init() == 0)
        {
          g_ds18b20InitOk = 1;
        }
      }
    }
  }
  /* USER CODE END StartTaskOther */
}

/* ================================================================
 *  NFC 卡片校验：读取扇区0块1（账户头）校验魔数
 *  返回: 0=无效卡, 1=普通卡, 2=图像卡, 3=管理员卡
 * ================================================================ */
static uint8_t NFC_ValidateCard(uint8_t *uid)
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t buf[16];
    uint16_t sum = 0;

    /* 认证扇区0 */
    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        return 0;
    }

    /* 读取块1（账户头） */
    if (RC522_Read(1, buf) != RC522_OK)
    {
        return 0;
    }

    /* 前14字节逐字节累加，取低16位 */
    for (uint8_t i = 0; i < 14; i++)
    {
        sum += buf[i];
    }

    /* 校验和（小端：低字节在前） */
    uint16_t storedChecksum = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
    if (sum != storedChecksum)
    {
        return 0;  /* 无效卡：校验和不匹配 */
    }

    /* 验证卡号匹配物理UID */
    if (memcmp(buf, uid, 4) != 0)
    {
        return 0;  /* 无效卡：卡号不匹配 */
    }

    /* buf[12] = 卡类型：0=普通卡, 1=图像卡, 2=管理员卡 */
    uint8_t cardType = buf[12];
    if (cardType > 2)
    {
        return 0;  /* 无效卡类型 */
    }

    return cardType + 1;  /* 返回 1=普通卡, 2=图像卡, 3=管理员卡 */
}

/* ================================================================
 *  阶段一·第三步：NFC 刷卡任务 (nfcTask)
 *  RC522 寻卡 -> 读 UID -> 防重复 -> 发送消息
 * ================================================================ */
/* USER CODE BEGIN Header_StartTaskNFC */
/**
* @brief Function implementing the nfcTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskNFC */
void StartTaskNFC(void *argument)
{
  /* USER CODE BEGIN StartTaskNFC */

  /* 等待系统初始化完成 */
  while (!g_systemReady)
  {
    osDelay(50);
  }

  uint8_t lastUID[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t cardPresent = 0;
  uint8_t cardStable  = 0;

  const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  /* Infinite loop */
  for(;;)
  {
    /* ====== 读卡/清卡操作：检测 CARD_OP 状态 ======
     * uartTask 设置 s_cardOpState 为 WAIT_CARD 状态后，
     * nfcTask 在此检测并执行完整读卡/清卡流程。
     * 优先级高于 ISSUE_WAIT_CARD 和普通刷卡。
     */
    if (s_cardOpState == CARD_OP_READ_WAIT_CARD)
    {
      CARD_DBG("DBG NFC READ START\r\n");
      CardOp_ReadCard();
      /* 继续循环，uartTask 将读取状态并回复 */
      osDelay(50);
      continue;
    }

    if (s_cardOpState == CARD_OP_CLEAR_WAIT_CARD)
    {
      CARD_DBG("DBG NFC CLEAR START\r\n");
      CardOp_ClearCard();
      /* 继续循环，uartTask 将读取状态并回复 */
      osDelay(50);
      continue;
    }

    /* ====== 发卡写卡：检测 ISSUE_WAIT_CARD 状态 ======
     * uartTask 在收到 ISSUE_COMMIT 后设置 s_issueState = ISSUE_WAIT_CARD。
     * nfcTask 在此检测并执行完整写卡流程，期间暂停普通刷卡逻辑。
     */
    if (s_issueState == ISSUE_WAIT_CARD)
    {
      s_issueState = ISSUE_WRITING;
      s_issueWriteErr = Issue_WaitAndWriteCard();

      if (s_issueWriteErr == ISSUE_ERR_NONE)
      {
        s_issueState = ISSUE_DONE;
      }
      else
      {
        s_issueState = ISSUE_ERROR;
      }

      /* 继续循环，uartTask 将读取状态并回复 */
      osDelay(50);
      continue;
    }

    /* ---- 正常刷卡逻辑 ---- */
    uint8_t uid[4] = {0};

    if (RC522_ScanCard(uid) == RC522_OK)
    {
      if (cardStable < 3)
      {
        if (memcmp(uid, lastUID, 4) == 0)
        {
          cardStable++;
        }
        else
        {
          memcpy(lastUID, uid, 4);
          cardStable = 1;
        }
      }

      if (cardStable >= 3)
      {
        /* 重复刷卡：同一张卡未离开再次刷到 */
        if (cardPresent && memcmp(uid, lastUID, 4) == 0)
        {
          AppMessage_t fb;
          fb.type = MSG_FEEDBACK_DUP;
          /* 使用 50ms timeout，避免静默丢弃反馈消息 */
          osMessageQueuePut(myQueue04Handle, &fb, 0, 50U);
          RC522_Halt();
          osDelay(150);
          continue;
        }

        if (!cardPresent || memcmp(uid, lastUID, 4) != 0)
        {
        cardPresent = 1;
        LED_On(0);   /* L1 亮：检测到卡 */
        memcpy(lastUID, uid, 4);
        g_cardDataReady = 0;
        g_cardValidated = 0;
        g_cardType = 0;

        /* ---- 卡有效性校验（扇区0块1魔数） ---- */
        {
          uint8_t cardValidationResult = NFC_ValidateCard(uid);
          if (cardValidationResult == 0)
          {
            /* 无效卡：发送错误反馈 */
            g_cardValidated = 0;
            AppMessage_t fb;
            fb.type = MSG_FEEDBACK_INVALID;
            osMessageQueuePut(myQueue04Handle, &fb, 0, 50U);
            RC522_Halt();
            osDelay(150);
            continue;
          }
          g_cardValidated = 1;
          g_cardType = cardValidationResult - 1;  /* 0=普通, 1=图像, 2=管理员 */
        }

        /* ---- 读取扇区 1~8: 头像 48×64×1bpp = 384 字节 ---- */
        {
          int ok = 1;
          for (uint8_t sec = 1; sec <= 8 && ok; sec++)
          {
            if (RC522_AuthState(RC522_PICC_AUTHENT1A, (uint8_t)(sec * 4 + 3),
                                (uint8_t *)defaultKey, uid) != RC522_OK)
            {
              ok = 0; break;
            }
            for (uint8_t blk = 0; blk < 3 && ok; blk++)
            {
              uint8_t buf[16];
              if (RC522_ReadBlock(sec, blk, buf) != RC522_OK)
              {
                ok = 0; break;
              }
              memcpy(&g_cardAvatar[(sec - 1) * 48 + blk * 16], buf, 16);
            }
          }
          if (ok)
          {
            g_cardDataReady = 1;
          }
        }

        /* ---- 读取扇区 9~15: 姓名 80×16 + 学号 80×16 = 320 字节 ---- */
        if (g_cardDataReady)
        {
          int ok = 1;
          for (uint8_t sec = 9; sec <= 15 && ok; sec++)
          {
            if (RC522_AuthState(RC522_PICC_AUTHENT1A, (uint8_t)(sec * 4 + 3),
                                (uint8_t *)defaultKey, uid) != RC522_OK)
            {
              ok = 0; break;
            }
            for (uint8_t blk = 0; blk < 3 && ok; blk++)
            {
              uint8_t buf[16];
              if (RC522_ReadBlock(sec, blk, buf) != RC522_OK)
              {
                ok = 0; break;
              }
              uint16_t offset = (uint16_t)(sec - 9) * 48 + blk * 16;
              if (offset < 160)
              {
                memcpy(&g_cardNameImg[offset], buf, 16);
              }
              else if (offset < 320)
              {
                memcpy(&g_cardStuIDImg[offset - 160], buf, 16);
              }
            }
          }
        }

        /* ---- L2/L3 状态灯 — 根据卡类型设置 ---- */
        if (g_cardType == 0)
        {
            LED_On(1);    /* L2 亮：普通卡 */
            LED_Off(2);   /* L3 灭 */
        }
        else if (g_cardType == 1)
        {
            LED_Off(1);   /* L2 灭 */
            LED_On(2);    /* L3 亮：图像卡 */
        }
        else
        {
            /* 管理员卡或未知类型：L2/L3 都灭 */
            LED_Off(1);
            LED_Off(2);
        }

        /* ---- 记录刷卡时间 ---- */
        BSP_RTC_GetDateTime(&g_lastSwipeTime);

        /* ---- 考勤记录写入（仅普通卡 cardType == 0） ---- */
        if (g_cardType == 0)
        {
          /* 读取扇区 0 块 1 获取 workerId（与 NFC_ValidateCard 同一块） */
          uint8_t sector0Block1[16];
          int parseOk = 0;
          uint32_t attWorkerId = 0;

          if (RC522_AuthState(RC522_PICC_AUTHENT1A, 3, (uint8_t *)defaultKey, uid) == RC522_OK)
          {
            if (RC522_ReadBlock(0, 1, sector0Block1) == RC522_OK)
            {
              /* 校验前14字节累加和 */
              uint16_t sum = 0;
              for (uint8_t k = 0; k < 14; k++) sum += sector0Block1[k];
              uint16_t storedCS = (uint16_t)sector0Block1[14] | ((uint16_t)sector0Block1[15] << 8);
              if (sum == storedCS)
              {
                attWorkerId = ((uint32_t)sector0Block1[8] << 24) |
                              ((uint32_t)sector0Block1[9] << 16) |
                              ((uint32_t)sector0Block1[10] << 8) |
                              (uint32_t)sector0Block1[11];
                parseOk = 1;
              }
            }
          }

          /* 初始化考勤结果状态 */
          g_lastAttendValid       = 0;
          g_lastAttendRejected    = 0;
          g_lastAttendEvent       = 0;
          g_lastAttendMode        = 0;
          g_lastAttendDurationSec = 0;

          if (parseOk)
          {
            const DeviceConfig_t *cfg = AppConfig_Get();
            uint8_t attMode = (cfg != NULL) ? cfg->attendanceMode : ATT_MODE_BOTH;
            uint8_t eventType      = 0;  /* 0=不写日志 */
            uint32_t durationSec   = 0;
            uint8_t attendAllowed  = 0;  /* 1=允许写日志 */
            uint8_t attendRejected = 0;  /* 1=拒绝（重复签到/无效离开） */

            /* 查找同 workerId 的最后一条有效记录 */
            AttendanceRecord_t lastRec;
            uint8_t hasLast = (AttendanceLog_FindLastByWorker(attWorkerId, &lastRec) == 0);

            /* ---- 考勤模式判定 ---- */
            if (attMode == ATT_MODE_ENTRY)
            {
              /* ENTRY 模式：只允许签到 IN */
              if (!hasLast || lastRec.eventType == ATT_LOG_EVENT_OUT)
              {
                /* 无记录 或 上次是 OUT：本次写 IN */
                eventType     = ATT_LOG_EVENT_IN;
                durationSec   = 0;
                attendAllowed = 1;
              }
              else
              {
                /* 上次是 IN：重复签到，拒绝 */
                eventType       = ATT_LOG_EVENT_IN;
                attendRejected  = 1;
              }
            }
            else if (attMode == ATT_MODE_EXIT)
            {
              /* EXIT 模式：只允许离开 OUT */
              if (hasLast && lastRec.eventType == ATT_LOG_EVENT_IN)
              {
                /* 有 IN 记录：本次写 OUT */
                eventType     = ATT_LOG_EVENT_OUT;
                durationSec   = AttendanceLog_CalcDurationToNow(&lastRec);
                attendAllowed = 1;
              }
              else
              {
                /* 无 IN 记录 或 上次已是 OUT：无效离开/重复离开 */
                eventType      = ATT_LOG_EVENT_OUT;
                attendRejected = 1;
              }
            }
            else
            {
              /* BOTH 模式（默认）或非法模式：交替 IN/OUT */
              if (!hasLast || lastRec.eventType == ATT_LOG_EVENT_OUT)
              {
                /* 无记录 或 上次是 OUT：本次写 IN */
                eventType     = ATT_LOG_EVENT_IN;
                durationSec   = 0;
                attendAllowed = 1;
              }
              else
              {
                /* 上次是 IN：本次写 OUT */
                eventType     = ATT_LOG_EVENT_OUT;
                durationSec   = AttendanceLog_CalcDurationToNow(&lastRec);
                attendAllowed = 1;
              }
            }

            /* ---- 写日志（仅有效考勤） ---- */
            if (attendAllowed)
            {
              AttendanceLog_AppendRecord(attWorkerId, 0, eventType, attMode, durationSec);
              g_lastAttendValid       = 1;
              g_lastAttendRejected    = 0;
              g_lastAttendEvent       = eventType;
              g_lastAttendMode        = attMode;
              g_lastAttendDurationSec = durationSec;
            }
            else if (attendRejected)
            {
              g_lastAttendValid       = 0;
              g_lastAttendRejected    = 1;
              g_lastAttendEvent       = eventType;
              g_lastAttendMode        = attMode;
              g_lastAttendDurationSec = 0;
            }

            /* ---- L4/L5/L6/L7 状态灯控制 ---- */
            /* 先关闭 L4~L7 */
            LED_Off(3);
            LED_Off(4);
            LED_Off(5);
            LED_Off(6);

            if (g_lastAttendValid)
            {
              if (attMode == ATT_MODE_ENTRY && eventType == ATT_LOG_EVENT_IN)
              {
                LED_On(3);  /* L4: ENTRY 成功签到 */
              }
              else if (attMode == ATT_MODE_EXIT && eventType == ATT_LOG_EVENT_OUT)
              {
                LED_On(4);  /* L5: EXIT 成功离开 */
              }
              else if (attMode == ATT_MODE_BOTH && eventType == ATT_LOG_EVENT_IN)
              {
                LED_On(5);  /* L6: BOTH 成功签到 */
              }
              else if (attMode == ATT_MODE_BOTH && eventType == ATT_LOG_EVENT_OUT)
              {
                LED_On(6);  /* L7: BOTH 成功离开 */
              }
              /* 非法模式按 BOTH 处理，L6/L7 覆盖 */
            }
          }
          else
          {
            /* workerId 解析失败：清除考勤状态 */
            g_lastAttendValid       = 0;
            g_lastAttendRejected    = 0;
            g_lastAttendEvent       = 0;
            g_lastAttendMode        = 0;
            g_lastAttendDurationSec = 0;
            LED_Off(3);
            LED_Off(4);
            LED_Off(5);
            LED_Off(6);
          }
        }
        else
        {
          /* 非普通卡（图像卡/管理员卡）：清除考勤显示状态 */
          g_lastAttendValid       = 0;
          g_lastAttendRejected    = 0;
          g_lastAttendEvent       = 0;
          g_lastAttendMode        = 0;
          g_lastAttendDurationSec = 0;
        }

        /* 发送消息到 guiTask（使用 50ms timeout，防止消息静默丢失） */
        {
          AppMessage_t msg;
          msg.type = MSG_NFC_CARD;
          memcpy(msg.param.uid, uid, 4);
          osMessageQueuePut(myQueue01Handle, &msg, 0, 50U);
        }

        /* 触发声光反馈（按卡类型和考勤结果区分） */
        {
          AppMessage_t fb;
          if (g_cardType == 2)
          {
            fb.type = MSG_FEEDBACK_ADMIN;
          }
          else if (g_cardType == 0 && g_lastAttendRejected)
          {
            /* 普通卡重复签到/无效离开：短鸣（不控制 L4/L7） */
            fb.type = MSG_FEEDBACK_DUP;
          }
          else
          {
            fb.type = MSG_FEEDBACK_VALID;
          }
          osMessageQueuePut(myQueue04Handle, &fb, 0, 50U);
        }
        } /* !cardPresent || uid changed */
      } /* cardStable >= 3 */

      RC522_Halt();
    }
    else
    {
      if (cardStable > 0) cardStable--;
      if (cardPresent && cardStable == 0)
      {
        cardPresent = 0;
        /* 卡离开：L1/L2/L3 全灭，L4~L7 也关闭 */
        LED_Off(0);
        LED_Off(1);
        LED_Off(2);
        LED_Off(3);
        LED_Off(4);
        LED_Off(5);
        LED_Off(6);
        AppMessage_t msg;
        msg.type = MSG_DISPLAY_CLOCK;
        osMessageQueuePut(myQueue01Handle, &msg, 0, 50U);
      }
    }

    osDelay(150);
  }
  /* USER CODE END StartTaskNFC */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/**
  * @brief  FreeRTOS 栈溢出钩子
  * @note   检测到栈溢出时进入死循环，可用调试器查看调用栈
  * @param  xTask: 溢出任务句柄
  * @param  pcTaskName: 溢出任务名称
  * @retval None
  */
/**
  * @brief  WiFi 连接任务
  * @note   使用 ESP01S 真实接口连接 WiFi，独立低优先级任务不阻塞其他功能
  *         本轮只验证 WiFi 连接，不做 NTP 和天气查询
  */
/**
  * @brief  天气文字缩写 — 将心知天气 API 返回的英文天气描述缩写为 ≤4 字符
  * @note   不分配内存，返回字符串常量或 "Unk"；NULL/空字符串返回 "Unk"
  * @param  text: 天气描述原文（如 "Overcast", "Sunny", "Cloudy"）
  * @retval 缩写字符串（静态常量，线程安全）
  */
static const char *Weather_AbbrevText(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return "Unk";
    }

    /* 取首字符便于快速判读（大小写敏感，匹配心知天气英文返回格式） */
    switch (text[0])
    {
    case 'S':
        /* Sunny, Shower, Snow */
        if (text[1] == 'u')  return "Sun";   /* Sunny */
        if (text[1] == 'h')  return "Shwr";  /* Shower */
        if (text[1] == 'n')  return "Snow";  /* Snow */
        break;
    case 'C':
        /* Clear, Cloudy */
        if (text[1] == 'l' && text[2] == 'e') return "Sun";  /* Clear → Sun */
        if (text[1] == 'l' && text[2] == 'o') return "Cld";  /* Cloudy */
        break;
    case 'O':
        /* Overcast */
        return "Ovc";
    case 'R':
        /* Rain */
        return "Rain";
    case 'D':
        /* Drizzle */
        return "Shwr";
    case 'F':
        /* Fog */
        return "Fog";
    case 'H':
        /* Haze */
        return "Haze";
    case 'T':
        /* Thunderstorm, Thunder */
        return "TStr";
    case 'P':
        /* Partly Cloudy / Partly Sunny */
        if (text[1] == 'a') return "Cld";  /* Partly → 按多云处理 */
        break;
    default:
        break;
    }

    /* 未命中已知映射：长度 ≤5 原样返回，否则返回 "Unk" */
    {
        uint8_t len = 0;
        const char *p = text;
        while (*p != '\0' && len < 6) { p++; len++; }
        if (len <= 5)
        {
            return text;
        }
    }

    return "Unk";
}

/**
  * @brief  天气查询辅助函数 — 由 wifiTask 调用
  * @note   仅在 WiFi 已连接后调用；
  *         成功时组合 g_weatherText，失败时写入 "WX unavailable"
  */
static void Weather_UpdateOnce(void)
{
#if WEATHER_ENABLED
    if (!g_wifiConnected)
    {
        g_weatherState = WEATHER_STATE_WAIT_WIFI;
        g_weatherValid = 0;
        snprintf(g_weatherText, sizeof(g_weatherText), "WX wait WiFi");
        return;
    }

    g_weatherState = WEATHER_STATE_QUERYING;
    g_weatherValid = 0;

    char city[16];
    char textDay[32];
    char high[8];
    char textNight[32];
    char low[8];
    char precip[8];

    memset(city, 0, sizeof(city));
    memset(textDay, 0, sizeof(textDay));
    memset(high, 0, sizeof(high));
    memset(textNight, 0, sizeof(textNight));
    memset(low, 0, sizeof(low));
    memset(precip, 0, sizeof(precip));

    int ret = ESP01S_QueryWeather(
        WEATHER_API_KEY,
        WEATHER_CITY,
        WEATHER_LANGUAGE,
        WEATHER_UNIT,
        city, sizeof(city),
        textDay, sizeof(textDay),
        high, sizeof(high),
        textNight, sizeof(textNight),
        low, sizeof(low),
        precip, sizeof(precip)
    );

    if (ret == 0)
    {
        /* 组合天气文本: city前2字母 + 缩写天气 + 高温/低温C
         * 例如 "Ha Ovc 28/20C" — 控制在16字符以内 */
        char cityShort[4];
        cityShort[0] = city[0];
        cityShort[1] = (city[1] != '\0') ? city[1] : ' ';
        cityShort[2] = '\0';

        const char *weatherShort = Weather_AbbrevText(textDay);

        /* high 或 low 为空时显示 -- */
        const char *h = (high[0] != '\0') ? high : "--";
        const char *l = (low[0]  != '\0') ? low  : "--";

        snprintf(g_weatherText, sizeof(g_weatherText),
                 "%s %s %s/%sC",
                 cityShort, weatherShort, h, l);

        g_weatherValid = 1;
        g_weatherState = WEATHER_STATE_VALID;
    }
    else
    {
        g_weatherValid = 0;
        g_weatherState = WEATHER_STATE_FAILED;
        snprintf(g_weatherText, sizeof(g_weatherText), "WX unavailable");
    }
#else
    g_weatherState = WEATHER_STATE_DISABLED;
    g_weatherValid = 0;
    g_weatherText[0] = '\0';
#endif
}

void StartWifiTask(void *argument)
{
#if WIFI_ENABLED
  /* 等待 USART6 驱动初始化完成 */
  while (!g_uart6DrvReady)
  {
    osDelay(100);
  }

  /* 等待系统稳定后再初始化 ESP-01 */
  osDelay(3000);

  for (;;)
  {
    g_wifiState = WIFI_STATE_INIT;
    g_wifiConnected = 0;

    ESP01S_Init(&s_uart6Drv);
    ESP01S_SetWiFi(WIFI_SSID, WIFI_PASSWORD);

    /* 清空 TCP 目标，避免 ESP01S_Start 在 WiFi 成功后尝试 TCP 连接 */
    ESP01S_SetTcpServer("", 0);

#if NTP_ENABLED
    /* 设置 NTP 服务器 — ESP01S_Start 内部会自动执行 NTP 同步 */
    ESP01S_SetNtpServer(NTP_SERVER, NTP_TIMEZONE);
    g_ntpState = NTP_STATE_WAIT_WIFI;
#else
    g_ntpState = NTP_STATE_DISABLED;
    g_ntpSynced = 0;
#endif

    g_wifiState = WIFI_STATE_CONNECTING;

    int ret = ESP01S_Start();

    if (ESP01S_IsWiFiConnected())
    {
      g_wifiState = WIFI_STATE_CONNECTED;
      g_wifiConnected = 1;

#if NTP_ENABLED
      /* WiFi 已连接，检查 ESP01S_Start 内部的 NTP 同步结果 */
      g_ntpState = NTP_STATE_SYNCING;

      if (ESP01S_IsNtpSynced())
      {
        int rtcRet = ESP01S_SetRtcFromNtp(&hrtc);
        if (rtcRet == 0)
        {
          g_ntpSynced = 1;
          g_ntpState = NTP_STATE_SYNCED;
        }
        else
        {
          g_ntpSynced = 0;
          g_ntpState = NTP_STATE_FAILED;
        }
      }
      else
      {
        g_ntpSynced = 0;
        g_ntpState = NTP_STATE_FAILED;
      }
#endif

#if WEATHER_ENABLED
      /* WiFi + NTP 完成后立即查询一次天气 */
      Weather_UpdateOnce();
#endif

      {
        TickType_t lastWeatherTick = xTaskGetTickCount();

        while (ESP01S_IsWiFiConnected())
        {
#if WEATHER_ENABLED
          TickType_t now = xTaskGetTickCount();
          uint32_t interval = g_weatherValid ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS;

          if ((now - lastWeatherTick) >= pdMS_TO_TICKS(interval))
          {
            lastWeatherTick = now;
            Weather_UpdateOnce();
          }
#endif
          osDelay(1000);
        }
      }

      g_wifiConnected = 0;
      g_wifiState = WIFI_STATE_FAILED;
    }
    else
    {
      (void)ret;
      g_wifiConnected = 0;
      g_wifiState = WIFI_STATE_FAILED;

#if NTP_ENABLED
      g_ntpSynced = 0;
      g_ntpState = NTP_STATE_FAILED;
#endif
    }

    osDelay(WIFI_RETRY_DELAY_MS);
  }
#else
  g_wifiState = WIFI_STATE_DISABLED;
  g_wifiConnected = 0;
  g_ntpState = NTP_STATE_DISABLED;
  g_ntpSynced = 0;
  for (;;)
  {
    osDelay(10000);
  }
#endif
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  (void)pcTaskName;
  /* 进入死循环 — 连接调试器后在此处设置断点，查看 pcTaskName */
  for (;;)
  {
    __NOP();
  }
}

/* USER CODE END Application */
