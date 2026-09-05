/**
 * @file    esp01s.c
 * @brief   ESP-01S WiFi模块驱动实现 - 基于UartDrv通用串口驱动
 * @version 1.0
 *
 * @par 架构说明
 *   驱动内部维护单例实例 s_esp01s,用户无需定义ESP01S_t变量。
 *   默认配置见下方 s_defaultConfig,可直接在此修改。
 *
 * @par NTP授时双模式自动回退
 *   1. AT指令SNTP (AT+CIPSNTPCFG + AT+CIPSNTPTIME) - 需要固件v1.7.0+
 *   2. UDP NTP协议 - 通过UDP直连NTP服务器发送/解析NTP报文,通用方案
 *   先尝试方式1,若返回的年份<=2000(说明固件不支持),则自动回退到方式2。
 *
 * @par ESP8266单连接限制
 *   ESP8266单连接模式(CIPMUX=0)下只能维持一条连接,
 *   UDP NTP需要先关闭已有连接,获取时间后再建立TCP连接。
 *   因此NTP授时必须在TCP连接之前进行(ESP01S_Start已自动处理)。
 *
 * @par 状态机
 *   IDLE → AT_OK → WIFI_CONNECTING → WIFI_CONNECTED
 *        → TCP_CONNECTING → TCP_CONNECTED → TRANSPARENT
 *   状态由AT响应在回调中推动,ESP01S_Start中通过比较状态值判断各阶段是否成功。
 */

#include "esp01s.h"
#include "rtc.h"

#ifndef ESP01S_NO_FREERTOS
#include "cmsis_os.h"
#define ESP01S_DELAY(ms)    osDelay(ms)
#else
#define ESP01S_DELAY(ms)    HAL_Delay(ms)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====== 内部实例与默认配置 ====== */

/**
 * @brief  ESP01S驱动实例结构(内部使用,不暴露给用户)
 * @note   采用单例模式,整个驱动只有一个实例 s_esp01s。
 *         pUartDrv: 底层串口驱动,通过它发送AT指令和接收响应
 *         config:   当前生效的配置(WiFi/TCP/NTP参数)
 *         state:    状态机当前状态
 *         strAPName/strESPName: 缓存查询到的热点名/AP名,用于判断是否需要重新连接
 *         pDataCb/pUserCtx: 透传数据接收回调
 *         ntpTime/ntpTimeValid: 最近一次NTP授时结果
 *         ntpUdpPending: UDP NTP等待标志(回调中根据此标志优先解析+IPD数据)
 *         ntpTick: NTP授时成功时的HAL_Tick值,用于时间推算和延迟补偿
 *         ntpUnixTimestamp: NTP授时时的UTC Unix时间戳,用于推算当前时间
 */
typedef struct {
    UartDrv_t            *pUartDrv;        /**< 底层串口驱动指针(ESP01S通信串口) */
    ESP01S_Config_t       config;          /**< 当前生效的配置参数 */
    ESP01S_State_t        state;           /**< 状态机当前状态 */
    char                  strAPName[32];   /**< 缓存: 当前连接的热点名称 */
    char                  strESPName[32];  /**< 缓存: 模块自身AP名称 */
    ESP01S_DataCallback_t pDataCb;         /**< 透传数据接收回调函数 */
    void                 *pUserCtx;        /**< 透传回调用户上下文 */
#ifndef ESP01S_NO_FREERTOS
    osMessageQueueId_t    rxQueue;         /**< 透传数据接收队列句柄 */
#endif
    ESP01S_NtpTime_t      ntpTime;         /**< 最近一次NTP授时时间(本地时间) */
    uint8_t               ntpTimeValid;    /**< NTP时间是否有效(1=有效) */
    uint8_t               ntpUdpPending;   /**< UDP NTP等待标志(1=正在等待NTP响应) */
    uint32_t              ntpTick;         /**< NTP授时成功瞬间的HAL_Tick值 */
    uint32_t              ntpUnixTimestamp; /**< NTP授时时的UTC Unix时间戳 */
    uint8_t               weatherPending;  /**< 天气HTTP查询等待标志(1=正在等待响应) */
    char                  weatherRxBuf[1024]; /**< 天气HTTP响应接收缓冲区 */
    uint16_t              weatherRxLen;    /**< 天气响应已接收字节数 */
    char                  cipdomainResult[20]; /**< AT+CIPDOMAIN解析结果(点分十进制IP) */
} ESP01S_Instance_t;

/**
 * @brief  默认配置 - 用户可直接在此修改WiFi热点、TCP服务器、NTP服务器等参数
 * @note   也可以通过 ESP01S_SetConfig/SetWiFi/SetTcpServer/SetNtpServer 运行时修改
 */
static ESP01S_Config_t s_defaultConfig = {
    .ssid        = "Your2GHotspot",           /**< WiFi热点名称 */
    .password    = "YourPassword",        /**< WiFi密码 */
    .tcpServerIP = "192.168.1.100",    /**< TCP服务器IP地址 */
    .tcpPort     = 8081,              /**< TCP服务器端口号 */
    .ntpServer   = "ntp.aliyun.com",   /**< NTP服务器(阿里云NTP) */
    .ntpTimezone = 8,                 /**< 时区(东八区) */
};

/** 驱动内部单例实例(全局唯一,用户无需定义) */
static ESP01S_Instance_t s_esp01s;

/**
 * @brief  NTP协议常量: 1900-01-01到1970-01-01的秒数
 * @note   NTP时间戳从1900年起算,Unix时间戳从1970年起算,
 *         两者相差70年的秒数=2208988800。转换时需减去此偏移量。
 */
#define NTP_UNIX_OFFSET     2208988800UL

/* ====== 私有函数声明 ====== */
static void ESP01S_RxCallback(UartDrv_RxData_t *pData, void *pUserCtx);     /**< 串口接收回调(由UartDrv分发调用) */
static int  ESP01S_RestoreTcpTransparent(void);                              /**< 恢复TCP连接并进入透传模式 */
static void ESP01S_ProcessATResponse(const char *pstr, uint16_t len);        /**< AT响应解析与状态推进 */
static void ESP01S_ParseUdpNtpData(const uint8_t *pBuf, uint16_t len);      /**< +IPD中的UDP NTP数据解析 */
static void UnixToDateTime(uint32_t timestamp, int timezoneHrs, ESP01S_NtpTime_t *pTime); /**< Unix时间戳转日期时间 */
static uint32_t DateTimeToUnix(const ESP01S_NtpTime_t *pTime, int timezoneHrs);           /**< 日期时间转Unix时间戳 */
static uint8_t DecToBcd(uint8_t dec);   /**< 十进制转BCD */
static uint8_t BcdToDec(uint8_t bcd);   /**< BCD转十进制 */
static uint8_t ParseWeekday(const char *str);  /**< 英文星期名转数字 */
static void FormatDateTime(const ESP01S_NtpTime_t *pTime, DT_Format_t format, char *pBuf, uint16_t bufLen); /**< 格式化时间到字符串 */

/* ====== 初始化与启停 ====== */

/**
 * @brief  初始化ESP01S驱动(使用默认配置)
 * @param  pUartDrv 底层串口驱动指针(需先调用UartDrv_Init初始化)
 * @note   初始化流程:
 *         1. 清零内部单例实例
 *         2. 绑定串口驱动(ESP01S的所有通信都通过此串口)
 *         3. 拷贝默认配置到实例(运行时可通过Set*函数修改)
 *         4. 设置初始状态为IDLE
 *         5. 注册串口接收回调(ESP01S_RxCallback),回调中传入实例指针作为用户上下文
 *         初始化后状态为ESP01S_STATE_IDLE,需调用ESP01S_Start()开始连接
 */
void ESP01S_Init(UartDrv_t *pUartDrv)
{
    if (pUartDrv == NULL)
        return;

    /* 清零实例,确保从干净状态开始 */
    memset(&s_esp01s, 0, sizeof(ESP01S_Instance_t));

    /* 绑定串口驱动 */
    s_esp01s.pUartDrv = pUartDrv;

    /* 拷贝默认配置到实例(后续可通过Set*API修改) */
    memcpy(&s_esp01s.config, &s_defaultConfig, sizeof(ESP01S_Config_t));

    /* 设置初始状态 */
    s_esp01s.state = ESP01S_STATE_IDLE;

    /* 注册串口接收回调,回调中传入实例指针作为用户上下文,
     * 这样回调函数可以访问实例的所有字段 */
    UartDrv_RegisterRxCb(pUartDrv, ESP01S_RxCallback, &s_esp01s);
}

#ifndef ESP01S_NO_FREERTOS
/**
 * @brief  注册ESP01S透传数据接收队列
 */
void ESP01S_RegisterRxQueue(osMessageQueueId_t queue)
{
    s_esp01s.rxQueue = queue;
}
#endif

/**
 * @brief  在任务上下文中分发透传数据到用户回调
 */
void ESP01S_DispatchTransparentData(const uint8_t *pData, uint16_t len)
{
    if (pData == NULL || len == 0)
        return;

    if (s_esp01s.pDataCb)
    {
        s_esp01s.pDataCb(pData, len, s_esp01s.pUserCtx);
    }
}

/**
 * @brief  启动ESP01S完整流程: AT测试 → WiFi → NTP → TCP → 透传
 * @retval 0:成功  -1:AT通信失败  -2:WiFi连接失败  -3:TCP连接失败
 * @note   本函数为阻塞式,通过HAL_Delay等待AT响应,整个流程约需10~20秒。
 *         每个AT指令发送后等待固定时间,回调中解析响应并推进状态。
 */
int ESP01S_Start(void)
{
    if (s_esp01s.pUartDrv == NULL)
        return -1;

    char buf[128];

    /* 等待模块上电稳定 */
    ESP01S_DELAY(100);

    /* 启动串口接收(空闲中断模式) */
    UartDrv_StartRecv(s_esp01s.pUartDrv);

    s_esp01s.state = ESP01S_STATE_IDLE;

    /* ---- 第1步: 退出透传模式 ----
     * 防止上次运行遗留的透传状态,+++不加换行符(ESP8266透传退出协议) */
    ESP01S_SendATCmd("+++", 500);

    /* ---- 第2步: AT通信测试 ----
     * 发送AT,如果模块正常则回复OK,状态推进到AT_OK */
    ESP01S_SendATCmd("AT\r\n", 1000);

    if (s_esp01s.state < ESP01S_STATE_AT_OK)
    {
        printf("[ESP01S] AT通信失败!\r\n");
        return -1;
    }

    /* ---- 第3步: 关闭回显 ----
     * ATE0: 关闭AT指令回显,减少串口数据量,便于解析响应 */
    ESP01S_SendATCmd("ATE0\r\n", 500);

    /* ---- 第4步: 关闭已有连接 ----
     * 上次运行可能遗留TCP/UDP连接,必须先关闭才能新建连接。
     * 忽略错误(如果没有连接会返回ERROR,属正常情况) */
    ESP01S_SendATCmd("AT+CIPCLOSE\r\n", 500);

    /* ---- 第5步: 关闭透传模式设置 ----
     * AT+CIPMODE=0: 上次运行可能遗留CIPMODE=1(透传模式),
     * 如果不关闭,后续CIPSEND会报错(透传模式下不能用指定长度发送) */
    ESP01S_SendATCmd("AT+CIPMODE=0\r\n", 500);

    /* ---- 第6步: 设置单连接模式 ----
     * AT+CIPMUX=0: 透传模式必须在单连接模式下工作。
     * 多连接模式(CIPMUX=1)不支持透传(CIPMODE=1) */
    ESP01S_SendATCmd("AT+CIPMUX=0\r\n", 500);

    /* ---- 第7步: 查看模块版本 ----
     * AT+GMR: 用于确认固件版本,判断是否支持AT SNTP等功能 */
    ESP01S_SendATCmd("AT+GMR\r\n", 1000);

    /* ---- 第8步: 开启AP+STA模式 ----
     * AT+CWMODE=3: 同时开启AP(热点)和STA(客户端)模式,
     * STA模式用于连接路由器,AP模式允许其他设备连接本模块 */
    ESP01S_SendATCmd("AT+CWMODE=3\r\n", 500);

    /* ---- 第9步: 查询模块自身AP名称 ----
     * AT+CWSAP?: 返回当前AP配置,解析出AP名称缓存到strESPName */
    ESP01S_SendATCmd("AT+CWSAP?\r\n", 500);

    /* ---- 第10步: 查询当前连接的热点 ----
     * AT+CWJAP?: 返回当前连接的WiFi热点名称,缓存到strAPName,
     * 用于判断是否已连接到目标热点(避免重复连接) */
    ESP01S_SendATCmd("AT+CWJAP?\r\n", 500);

    /* ---- 第11步: 连接WiFi热点 ----
     * 如果当前已连接到目标热点(与config.ssid相同),跳过连接步骤
     * 否则发送AT+CWJAP连接,等待最多10秒 */
    if (strcmp(s_esp01s.strAPName, s_esp01s.config.ssid) != 0)
    {
        s_esp01s.state = ESP01S_STATE_WIFI_CONNECTING;
        sprintf(buf, "AT+CWJAP=\"%s\",\"%s\"\r\n", s_esp01s.config.ssid, s_esp01s.config.password);
        ESP01S_SendATCmd(buf, 10000);
    }
    else
    {
        /* 已连接到目标热点,直接设置状态 */
        s_esp01s.state = ESP01S_STATE_WIFI_CONNECTED;
    }

    /* 检查WiFi是否连接成功 */
    if (s_esp01s.state < ESP01S_STATE_WIFI_CONNECTED)
    {
        printf("[ESP01S] WiFi连接失败!\r\n");
        return -2;
    }

    /* ---- 第12步: 查询设备IP ----
     * AT+CIFSR: 获取STA模式下的IP地址 */
    ESP01S_SendATCmd("AT+CIFSR\r\n", 500);

    /* ---- 第13步: NTP授时(可选) ----
     * config.ntpServer非空时自动授时,内部支持AT SNTP和UDP NTP双模式回退 */
    ESP01S_SyncNtpTime();

    /* ---- 第14~15步: 连接TCP服务器并进入透传模式 ---- */
    return ESP01S_RestoreTcpTransparent();
}

/**
 * @brief  退出透传模式
 * @note   ESP8266透传退出协议要求:
 *         1. 前导静默: 至少500ms不发送任何数据
 *         2. 发送"+++": 不加\r\n(否则会被当作透传数据发送到服务器)
 *         3. 后续静默: 至少500ms不发送任何数据
 *         只有满足以上时序,模块才会识别为退出透传命令
 *         退出后状态回退到TCP_CONNECTED,可重新发AT指令或修改配置后重新Start
 */
void ESP01S_ExitTransparent(void)
{
    /* 前导静默: 确保之前的数据发送已完成 */
    ESP01S_DELAY(500);

    /* 发送退出命令: 不加换行符! */
    ESP01S_SendATCmd("+++", 500);

    /* 后续静默: 等待模块识别退出命令 */
    ESP01S_DELAY(500);

    /* 状态回退到TCP已连接 */
    s_esp01s.state = ESP01S_STATE_TCP_CONNECTED;
}

/* ====== 配置API ====== */

/**
 * @brief  一次性设置全部配置参数
 * @param  pConfig 配置参数指针(内容会被memcpy拷贝,调用后可释放)
 * @note   修改配置后需调用ESP01S_Start()重新连接才生效
 */
void ESP01S_SetConfig(const ESP01S_Config_t *pConfig)
{
    if (pConfig == NULL)
        return;

    memcpy(&s_esp01s.config, pConfig, sizeof(ESP01S_Config_t));
}

/**
 * @brief  获取当前配置(只读)
 * @retval 当前配置指针,指向驱动内部配置(不要修改返回的指针内容)
 */
const ESP01S_Config_t *ESP01S_GetConfig(void)
{
    return &s_esp01s.config;
}

/**
 * @brief  设置WiFi热点参数
 * @param  ssid     WiFi热点名称(最长31字符,超出截断)
 * @param  password WiFi密码(最长63字符,超出截断)
 * @note   使用strncpy确保不会越界,并手动添加字符串结束符
 */
void ESP01S_SetWiFi(const char *ssid, const char *password)
{
    if (ssid != NULL)
    {
        strncpy(s_esp01s.config.ssid, ssid, sizeof(s_esp01s.config.ssid) - 1);
        s_esp01s.config.ssid[sizeof(s_esp01s.config.ssid) - 1] = '\0';
    }
    if (password != NULL)
    {
        strncpy(s_esp01s.config.password, password, sizeof(s_esp01s.config.password) - 1);
        s_esp01s.config.password[sizeof(s_esp01s.config.password) - 1] = '\0';
    }
}

/**
 * @brief  设置TCP服务器参数
 * @param  ip   TCP服务器IP地址(点分十进制,如"192.168.1.100")
 * @param  port TCP服务器端口号(1~65535)
 */
void ESP01S_SetTcpServer(const char *ip, uint16_t port)
{
    if (ip != NULL)
    {
        strncpy(s_esp01s.config.tcpServerIP, ip, sizeof(s_esp01s.config.tcpServerIP) - 1);
        s_esp01s.config.tcpServerIP[sizeof(s_esp01s.config.tcpServerIP) - 1] = '\0';
    }
    s_esp01s.config.tcpPort = port;
}

/**
 * @brief  设置NTP授时服务器参数
 * @param  server    NTP服务器域名(如"ntp.aliyun.com"),传NULL则清空(禁用NTP)
 * @param  timezone  时区小时数(东八区=8),范围-12~14
 */
void ESP01S_SetNtpServer(const char *server, int8_t timezone)
{
    if (server == NULL)
    {
        /* 传NULL则清空NTP服务器,禁用NTP授时 */
        s_esp01s.config.ntpServer[0] = '\0';
    }
    else
    {
        strncpy(s_esp01s.config.ntpServer, server, sizeof(s_esp01s.config.ntpServer) - 1);
        s_esp01s.config.ntpServer[sizeof(s_esp01s.config.ntpServer) - 1] = '\0';
    }
    s_esp01s.config.ntpTimezone = timezone;
}

/* ====== 数据收发 ====== */

/**
 * @brief  发送AT指令并等待
 * @param  cmd     AT指令字符串(需包含\r\n结尾,如"AT\r\n")
 * @param  waitMs  发送后等待时间(毫秒),0则不等
 * @note   透传模式下请勿调用此函数(数据会被当作透传数据发送)
 *         内部调用 UartDrv_SendStr + HAL_Delay
 */
void ESP01S_SendATCmd(const char *cmd, uint32_t waitMs)
{
    if (cmd == NULL || s_esp01s.pUartDrv == NULL)
        return;

    /* 通过串口发送AT指令 */
    UartDrv_SendStr(s_esp01s.pUartDrv, cmd);

    /* 等待模块响应(阻塞) */
    if (waitMs > 0)
        ESP01S_DELAY(waitMs);
}

/**
 * @brief  发送字符串(透传模式)
 * @param  str 以'\0'结尾的字符串
 * @note   仅在透传模式下使用,数据将直接发送到TCP服务器
 */
void ESP01S_SendStr(const char *str)
{
    if (str == NULL || s_esp01s.pUartDrv == NULL)
        return;

    UartDrv_SendStr(s_esp01s.pUartDrv, str);
}

/**
 * @brief  发送数据(透传模式)
 * @param  pData 数据指针
 * @param  len   数据长度(字节)
 * @note   仅在透传模式下使用,可发送含\0的二进制数据
 */
void ESP01S_SendData(const uint8_t *pData, uint16_t len)
{
    if (pData == NULL || len == 0 || s_esp01s.pUartDrv == NULL)
        return;

    UartDrv_Send(s_esp01s.pUartDrv, pData, len);
}

/**
 * @brief  注册透传数据接收回调
 * @param  pCb       回调函数指针
 * @param  pUserCtx  用户上下文指针,回调时原样传回
 */
void ESP01S_RegisterDataCb(ESP01S_DataCallback_t pCb, void *pUserCtx)
{
    s_esp01s.pDataCb = pCb;
    s_esp01s.pUserCtx = pUserCtx;
}

/* ====== 状态查询 ====== */

/**
 * @brief  获取当前连接状态
 * @retval 当前状态(见 ESP01S_State_t 枚举)
 */
ESP01S_State_t ESP01S_GetState(void)
{
    return s_esp01s.state;
}

/* ====== NTP网络授时 ====== */

/**
 * @brief  配置SNTP服务器并获取NTP网络时间
 * @note   需在WiFi已连接、未进入透传模式时调用。
 *         ESP01S_Start()中已自动调用此函数,一般无需手动调用。
 *         工作流程(双模式自动回退):
 *         1. 尝试AT指令SNTP: AT+CIPSNTPCFG配置时区和服务器 → AT+CIPSNTPTIME?查询时间
 *            若固件支持(v1.7.0+),返回包含年份的有效时间,直接成功
 *         2. 若AT SNTP返回年份<=2000(固件不支持),回退到UDP NTP:
 *            关闭已有连接 → 建立UDP连接到NTP服务器123端口 → 发送48字节NTP请求包
 *            → 等待3秒接收响应 → 回调中解析+IPD数据提取时间戳 → 关闭UDP连接
 *         如果config.ntpServer为空字符串则跳过
 */
void ESP01S_SyncNtpTime(void)
{
    if (s_esp01s.pUartDrv == NULL)
        return;

    /* NTP服务器为空则跳过授时 */
    if (s_esp01s.config.ntpServer[0] == '\0')
        return;

    char buf[96];

    /* ---- 方式1: 尝试AT指令SNTP (需要ESP8266固件v1.7.0+支持) ---- */
    /* 配置SNTP时区和服务器 */
    sprintf(buf, "AT+CIPSNTPCFG=%d,\"%s\"\r\n", s_esp01s.config.ntpTimezone, s_esp01s.config.ntpServer);
    ESP01S_SendATCmd(buf, 500);

    /* 查询SNTP时间(回调中解析+CIPSNTPTIME响应) */
    ESP01S_SendATCmd("AT+CIPSNTPTIME?\r\n", 2000);

    /* 检查: 如果AT SNTP返回有效时间(年份>2000),直接成功 */
    if (s_esp01s.ntpTimeValid && s_esp01s.ntpTime.year > 2000)
    {
        printf("[ESP01S] NTP授时成功(AT SNTP): %04d-%02d-%02d %02d:%02d:%02d\r\n",
               s_esp01s.ntpTime.year, s_esp01s.ntpTime.month, s_esp01s.ntpTime.day,
               s_esp01s.ntpTime.hour, s_esp01s.ntpTime.minute, s_esp01s.ntpTime.second);
        return;
    }

    /* AT SNTP不可用(返回1970年或无响应),回退到UDP方式 */
    s_esp01s.ntpTimeValid = 0;

    /* ---- 方式2: UDP NTP协议 (通用方案,任何固件版本均可用) ----
     *
     * NTP协议工作原理:
     * - NTP服务器监听UDP 123端口
     * - 客户端发送48字节请求包(首字节0x1B=LI0/VN3/Mode3/Client)
     * - 服务器返回48字节响应包,其中bytes 40-43为发送时间戳(大端序,自1900年秒数)
     * - 将NTP时间戳减去NTP_UNIX_OFFSET(2208988800)得到Unix时间戳
     */

    /* 关闭已有连接(单连接模式下必须先关闭才能新建UDP) */
    ESP01S_SendATCmd("AT+CIPCLOSE\r\n", 500);

    /* 构造NTP请求包: 48字节全零,首字节设为0x1B
     * 0x1B = 0001 1011:
     *   LI=0 (无闰秒警告), VN=3 (NTP版本3), Mode=3 (Client) */
    uint8_t ntpReq[48];
    memset(ntpReq, 0, 48);
    ntpReq[0] = 0x1B;

    /* 设置UDP NTP等待标志,回调中将优先检查+IPD数据(可能含二进制) */
    s_esp01s.ntpUdpPending = 1;

    /* 建立UDP连接到NTP服务器123端口 */
    sprintf(buf, "AT+CIPSTART=\"UDP\",\"%s\",123\r\n", s_esp01s.config.ntpServer);
    ESP01S_SendATCmd(buf, 2000);

    /* 发送NTP请求: 先发AT+CIPSEND=48告知模块要发送48字节,
     * 等待200ms让模块返回">"提示符,再发48字节数据 */
    ESP01S_SendATCmd("AT+CIPSEND=48\r\n", 200);
    UartDrv_Send(s_esp01s.pUartDrv, ntpReq, 48);

    /* 等待NTP响应(回调中解析+IPD数据并填充ntpTime) */
    ESP01S_DELAY(3000);

    /* 关闭UDP连接(为后续TCP连接腾出单连接通道) */
    ESP01S_SendATCmd("AT+CIPCLOSE\r\n", 500);
    s_esp01s.ntpUdpPending = 0;

    if (s_esp01s.ntpTimeValid)
    {
        printf("[ESP01S] NTP授时成功(UDP): %04d-%02d-%02d %02d:%02d:%02d\r\n",
               s_esp01s.ntpTime.year, s_esp01s.ntpTime.month, s_esp01s.ntpTime.day,
               s_esp01s.ntpTime.hour, s_esp01s.ntpTime.minute, s_esp01s.ntpTime.second);
    }
    else
    {
        printf("[ESP01S] NTP授时失败!\r\n");
    }
}

/**
 * @brief  获取最近一次NTP授时结果
 * @param  pTime 输出时间结构指针(由调用者分配)
 * @retval 0:时间有效  -1:时间无效(未获取或获取失败)
 * @note   返回的是NTP授时时的瞬间时间,不是当前时间。
 *         如需获取当前时间请使用 ESP01S_GetDateTime()
 */
int ESP01S_GetNtpTime(ESP01S_NtpTime_t *pTime)
{
    if (pTime == NULL)
        return -1;

    if (!s_esp01s.ntpTimeValid)
        return -1;

    memcpy(pTime, &s_esp01s.ntpTime, sizeof(ESP01S_NtpTime_t));
    return 0;
}

/**
 * @brief  检查NTP时间是否已同步成功
 * @retval 1:已同步  0:未同步
 */
uint8_t ESP01S_IsNtpSynced(void)
{
    return s_esp01s.ntpTimeValid;
}

/**
 * @brief  检查当前是否已连接WiFi
 * @retval 1:已连接  0:未连接
 */
uint8_t ESP01S_IsWiFiConnected(void)
{
    return (s_esp01s.state >= ESP01S_STATE_WIFI_CONNECTED) ? 1 : 0;
}

/**
 * @brief  将NTP时间写入STM32的RTC(BCD格式)
 * @param  pRtc STM32 RTC句柄指针,传NULL则跳过RTC写入
 * @retval 0:成功  -1:NTP时间无效  -2:RTC写入失败
 * @note   内部会补偿从NTP授时到当前的时间差:
 *         - ntpTick: NTP授时成功瞬间记录的HAL_Tick值
 *         - ntpUnixTimestamp: NTP授时时的UTC Unix时间戳
 *         - 当前时间 = ntpUnixTimestamp + (HAL_GetTick() - ntpTick) / 1000
 *         这样消除了NTP响应后到写入RTC之间(TCP连接等操作)的延迟误差
 */
int ESP01S_SetRtcFromNtp(RTC_HandleTypeDef *pRtc)
{
    if (pRtc == NULL)
        return -1;

    if (!s_esp01s.ntpTimeValid)
        return -1;

    /* 补偿从NTP授时到当前的经过时间,消除后续操作(TCP连接等)带来的延迟 */
    ESP01S_NtpTime_t now;
    uint32_t elapsedMs = HAL_GetTick() - s_esp01s.ntpTick;
    uint32_t elapsedSec = elapsedMs / 1000;
    uint32_t currentTs = s_esp01s.ntpUnixTimestamp + elapsedSec;
    UnixToDateTime(currentTs, s_esp01s.config.ntpTimezone, &now);

    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    /* 将十进制转为BCD格式写入RTC
     * STM32 HAL RTC要求BCD格式,如23点=0x23,45分=0x45 */
    sTime.Hours   = DecToBcd(now.hour);
    sTime.Minutes = DecToBcd(now.minute);
    sTime.Seconds = DecToBcd(now.second);

    if (HAL_RTC_SetTime(pRtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
        return -2;

    /* 设置日期: WeekDay直接使用数字(1=Mon~7=Sun),月日年转BCD */
    sDate.WeekDay = now.weekday;
    sDate.Month   = DecToBcd(now.month);
    sDate.Date    = DecToBcd(now.day);
    /* STM32 RTC Year = 实际年份 - 2000,如2024年存储为24(BCD:0x24) */
    sDate.Year    = DecToBcd((uint8_t)(now.year - 2000));

    if (HAL_RTC_SetDate(pRtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
        return -2;

    /* STM32F1的RTC日期存在hrtc->DateToUpdate(RAM)中, 复位后丢失
     * 将日期备份到BKP后备寄存器(由VBAT供电, 复位不丢失), 供上电恢复 */
    HAL_RTCEx_BKUPWrite(pRtc, RTC_BKP_DR2, (uint32_t)(now.year - 2000));
    HAL_RTCEx_BKUPWrite(pRtc, RTC_BKP_DR3, (uint32_t)now.month);
    HAL_RTCEx_BKUPWrite(pRtc, RTC_BKP_DR4, (uint32_t)now.day);
    HAL_RTCEx_BKUPWrite(pRtc, RTC_BKP_DR5, (uint32_t)now.weekday);

    printf("[ESP01S] RTC已校准: %04d-%02d-%02d %02d:%02d:%02d\r\n",
           now.year, now.month, now.day,
           now.hour, now.minute, now.second);

    return 0;
}

/* ====== 天气查询 ====== */

/**
 * @brief  从JSON字符串中提取指定key的值(假设值为字符串,且不嵌套)
 * @param  json  JSON字符串
 * @param  key   要提取的键名
 * @param  out   输出缓冲区
 * @param  outSize 输出缓冲区大小
 * @retval 成功返回out指针,失败返回NULL
 */
static const char* ESP01S_JsonExtractString(const char *json, const char *key, char *out, uint16_t outSize)
{
    char pattern[24];
    const char *p;
    const char *end;
    uint16_t len;

    if (json == NULL || key == NULL || out == NULL || outSize == 0)
        return NULL;

    /* 先定位 key,允许 key 与 ':' 之间、':' 与 value 之间有空格 */
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (p == NULL)
        return NULL;

    p += strlen(pattern);

    /* 跳过空白 */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;

    if (*p != '"')
        return NULL;
    p++;

    end = strchr(p, '"');
    if (end == NULL)
        return NULL;

    len = (uint16_t)(end - p);
    if (len >= outSize)
        len = outSize - 1;

    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

/**
 * @brief  恢复TCP服务器连接并进入透传模式
 * @retval 0:成功  -3:TCP连接失败
 * @note   将ESP01S_Start()中的TCP连接与透传开启步骤提取出来,
 *         供天气查询后恢复透传连接使用。
 */
static int ESP01S_RestoreTcpTransparent(void)
{
    char buf[128];

    /* 连接配置的中继TCP服务器 */
    s_esp01s.state = ESP01S_STATE_TCP_CONNECTING;
    sprintf(buf, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", s_esp01s.config.tcpServerIP, s_esp01s.config.tcpPort);
    ESP01S_SendATCmd(buf, 2000);

    if (s_esp01s.state < ESP01S_STATE_TCP_CONNECTED)
    {
        printf("[ESP01S] TCP服务器连接失败!\r\n");
        return -3;
    }

    /* 开启透传模式 */
    ESP01S_SendATCmd("AT+CIPMODE=1\r\n", 500);
    ESP01S_SendATCmd("AT+CIPSEND\r\n", 500);
    s_esp01s.state = ESP01S_STATE_TRANSPARENT;

    return 0;
}

/**
 * @brief  查询心知天气日报并解析出城市、白天/夜间天气、温度及降雨概率
 * @retval 0:成功  -1:参数错误  -2:未连接WiFi  -3:网络请求失败  -4:解析失败
 * @note   本函数会临时退出透传模式、关闭当前TCP连接,查询结束后自动恢复
 *         到配置的中继TCP服务器并重新进入透传模式(若配置了TCP服务器)。
 *         使用 API: /v3/weather/daily.json?start=0&days=1
 */
int ESP01S_QueryWeather(const char *apiKey, const char *location,
                        const char *language, const char *unit,
                        char *outCity, uint16_t cityBufSize,
                        char *outTextDay, uint16_t textDayBufSize,
                        char *outHigh, uint16_t highBufSize,
                        char *outTextNight, uint16_t textNightBufSize,
                        char *outLow, uint16_t lowBufSize,
                        char *outPrecip, uint16_t precipBufSize)
{
    char req[256];
    char sendCmd[32];
    uint16_t reqLen;
    const char *body;
    int ret = -4;

    if (apiKey == NULL || location == NULL || language == NULL || unit == NULL)
        return -1;

    if (outTextDay == NULL || textDayBufSize == 0 ||
        outHigh == NULL || highBufSize == 0 ||
        outTextNight == NULL || textNightBufSize == 0 ||
        outLow == NULL || lowBufSize == 0 ||
        outPrecip == NULL || precipBufSize == 0)
    {
        return -1;
    }

    if (!ESP01S_IsWiFiConnected())
    {
        return -2;
    }

    /* 如果当前在透传模式,先退出 */
    if (s_esp01s.state == ESP01S_STATE_TRANSPARENT)
    {
        ESP01S_ExitTransparent();
    }

    /* 关闭已有连接,为新建HTTP连接腾出单连接通道 */
    ESP01S_SendATCmd("AT+CIPCLOSE\r\n", 500);

    /* 建立到心知天气服务器的TCP连接(HTTP端口80)
     *
     * 策略: 先尝试域名连接,如果DNS解析失败则回退到AT+CIPDOMAIN
     * 解析IP后再用IP连接。因为ESP8266 AT固件在某些网络环境下
     * 内置DNS可能超时或失败,但AT+CIPDOMAIN命令可能正常工作。 */
    s_esp01s.state = ESP01S_STATE_TCP_CONNECTING;
    ESP01S_SendATCmd("AT+CIPSTART=\"TCP\",\"api.seniverse.com\",80\r\n", 5000);
    if (s_esp01s.state < ESP01S_STATE_TCP_CONNECTED)
    {
        /* 域名连接失败: 尝试AT+CIPDOMAIN解析IP后重试 */
        printf("[ESP01S] 天气域名连接失败,尝试CIPDOMAIN解析IP...\r\n");
        s_esp01s.cipdomainResult[0] = '\0';
        ESP01S_SendATCmd("AT+CIPDOMAIN=\"api.seniverse.com\"\r\n", 3000);
        if (s_esp01s.cipdomainResult[0] != '\0')
        {
            char cipBuf[96];
            s_esp01s.state = ESP01S_STATE_TCP_CONNECTING;
            snprintf(cipBuf, sizeof(cipBuf),
                     "AT+CIPSTART=\"TCP\",\"%s\",80\r\n", s_esp01s.cipdomainResult);
            ESP01S_SendATCmd(cipBuf, 5000);
        }
        if (s_esp01s.state < ESP01S_STATE_TCP_CONNECTED)
        {
            printf("[ESP01S] 天气服务器连接失败!\r\n");
            goto WEATHER_RESTORE;
        }
    }

    /* 构造HTTP GET请求 (日报, 当天) */
    snprintf(req, sizeof(req),
             "GET /v3/weather/daily.json?key=%s&location=%s&language=%s&unit=%s&start=0&days=1 HTTP/1.1\r\n"
             "Host: api.seniverse.com\r\n"
             "Connection: close\r\n"
             "\r\n",
             apiKey, location, language, unit);
    reqLen = (uint16_t)strlen(req);

    /* 设置天气查询等待标志,回调将缓存所有原始响应数据 */
    s_esp01s.weatherPending = 1;
    s_esp01s.weatherRxLen = 0;
    s_esp01s.weatherRxBuf[0] = '\0';

    /* 通过AT+CIPSEND发送HTTP请求 */
    snprintf(sendCmd, sizeof(sendCmd), "AT+CIPSEND=%d\r\n", reqLen);
    ESP01S_SendATCmd(sendCmd, 200);
    UartDrv_Send(s_esp01s.pUartDrv, (uint8_t *)req, reqLen);
    ESP01S_DELAY(500);

    /* 等待HTTP响应(心知天气响应通常在1~3秒内返回) */
    ESP01S_DELAY(4000);

    s_esp01s.weatherPending = 0;

    /* 解析HTTP响应体中的JSON字段 */
    body = strstr(s_esp01s.weatherRxBuf, "\r\n\r\n");
    if (body != NULL)
    {
        body += 4; /* 跳过HTTP头部分隔符 */

        /* 检测API错误响应: {"status":"...","status_code":"APxxxxxx"} */
        {
            char apiErr[64];
            if (ESP01S_JsonExtractString(body, "status_code", apiErr, sizeof(apiErr)) != NULL)
            {
                char apiMsg[64];
                if (ESP01S_JsonExtractString(body, "status", apiMsg, sizeof(apiMsg)) == NULL)
                    apiMsg[0] = '\0';
                printf("[ESP01S] 天气API错误: [%s] %s\r\n", apiErr, apiMsg);
                ret = -4;
                goto WEATHER_RESTORE;
            }
        }

        /* 定位 daily 数组(只取第1天), 在其中解析白天/夜间天气、温度、降雨概率 */
        const char *daily = strstr(body, "\"daily\"");
        if (daily == NULL)
        {
            printf("[ESP01S] 天气JSON解析失败(未找到daily), 原始响应:\r\n%.120s\r\n", body);
            ret = -4;
            goto WEATHER_RESTORE;
        }

        if (ESP01S_JsonExtractString(daily, "text_day", outTextDay, textDayBufSize) == NULL ||
            ESP01S_JsonExtractString(daily, "high", outHigh, highBufSize) == NULL ||
            ESP01S_JsonExtractString(daily, "text_night", outTextNight, textNightBufSize) == NULL ||
            ESP01S_JsonExtractString(daily, "low", outLow, lowBufSize) == NULL ||
            ESP01S_JsonExtractString(daily, "precip", outPrecip, precipBufSize) == NULL)
        {
            printf("[ESP01S] 天气JSON解析失败(daily字段), 原始响应:\r\n%.120s\r\n", body);
            ret = -4;
            goto WEATHER_RESTORE;
        }

        /* 提取城市名：在 "location" 块中搜索 "name" */
        if (outCity && cityBufSize > 0)
        {
            const char *loc = strstr(body, "\"location\"");
            if (loc)
            {
                ESP01S_JsonExtractString(loc, "name", outCity, cityBufSize);
            }
            else
            {
                outCity[0] = '\0';
            }
        }

        printf("[ESP01S] 天气查询成功: %s 白天%s %sC 夜间%s %sC 降雨概率%s\r\n",
               outCity, outTextDay, outHigh, outTextNight, outLow, outPrecip);
        ret = 0;
    }
    else
    {
        printf("[ESP01S] 天气HTTP响应解析失败\r\n");
        ret = -4;
    }

WEATHER_RESTORE:
    /* 关闭天气HTTP连接 */
    ESP01S_SendATCmd("AT+CIPCLOSE\r\n", 500);

    /* 恢复原有TCP透传连接 */
    if (s_esp01s.config.tcpServerIP[0] != '\0' && s_esp01s.config.tcpPort != 0)
    {
        ESP01S_RestoreTcpTransparent();
    }
    else
    {
        s_esp01s.state = ESP01S_STATE_WIFI_CONNECTED;
    }

    return ret;
}

/* ====== 时间获取 ====== */

/**
 * @brief  获取当前时间字符串
 * @param  pRtc    STM32 RTC句柄指针,传NULL则不从RTC读取
 * @param  format  时间格式: DT_DATE/DT_TIME/DT_ALL
 * @param  pBuf    输出字符串缓冲区(由调用者分配)
 * @param  bufLen  缓冲区长度(建议>=24字节)
 * @retval 0:成功  -1:无可用时间源
 * @note   时间来源优先级:
 *         1. RTC(如果pRtc非NULL且RTC年份>2000) — 最精确,掉电可保持
 *         2. NTP时间+HAL_Tick差值推算(如果NTP已授时) — 有轻微累积误差
 *         两种来源互为备份: RTC掉电后可由NTP校准,NTP未授时时RTC仍可提供时间
 */
int ESP01S_GetDateTime(RTC_HandleTypeDef *pRtc, DT_Format_t format, char *pBuf, uint16_t bufLen)
{
    if (pBuf == NULL || bufLen == 0)
        return -1;

    ESP01S_NtpTime_t now;
    memset(&now, 0, sizeof(now));

    /* 优先级1: 尝试从RTC读取(硬件实时时钟,最精确) */
    if (pRtc != NULL)
    {
        RTC_TimeTypeDef sTime = {0};
        RTC_DateTypeDef sDate = {0};

        /* 注意: HAL_RTC_GetTime必须先于GetDate调用,这是HAL库的约束 */
        if (HAL_RTC_GetTime(pRtc, &sTime, RTC_FORMAT_BCD) == HAL_OK &&
            HAL_RTC_GetDate(pRtc, &sDate, RTC_FORMAT_BCD) == HAL_OK)
        {
            /* BCD转十进制 */
            now.year    = (uint16_t)BcdToDec(sDate.Year) + 2000;
            now.month   = BcdToDec(sDate.Month);
            now.day     = BcdToDec(sDate.Date);
            now.hour    = BcdToDec(sTime.Hours);
            now.minute  = BcdToDec(sTime.Minutes);
            now.second  = BcdToDec(sTime.Seconds);
            now.weekday = sDate.WeekDay;

            /* RTC年份>2000才认为有效(未初始化的RTC年份通常为0或2000) */
            if (now.year > 2000)
            {
                FormatDateTime(&now, format, pBuf, bufLen);
                return 0;
            }
        }
    }

    /* 优先级2: NTP时间 + HAL_Tick差值推算
     * 原理: NTP授时成功时记录了当时的Unix时间戳和HAL_Tick值,
     * 当前时间 = ntpUnixTimestamp + (当前Tick - ntpTick) / 1000
     * 这种方法有轻微累积误差(HAL_Tick精度为1ms),长时间运行需重新授时 */
    if (s_esp01s.ntpTimeValid)
    {
        uint32_t elapsedMs = HAL_GetTick() - s_esp01s.ntpTick;
        uint32_t elapsedSec = elapsedMs / 1000;

        /* 当前UTC时间戳 = NTP时UTC时间戳 + 经过的秒数,再加时区偏移 */
        uint32_t currentTs = s_esp01s.ntpUnixTimestamp + elapsedSec;
        UnixToDateTime(currentTs, s_esp01s.config.ntpTimezone, &now);

        FormatDateTime(&now, format, pBuf, bufLen);
        return 0;
    }

    /* 无可用时间源: RTC无效且NTP未授时 */
    pBuf[0] = '\0';
    return -1;
}

/* ====== 私有函数实现 ====== */

/**
 * @brief  十进制转BCD(Binary-Coded Decimal)
 * @param  dec 十进制数(0~99)
 * @retval BCD编码值,如23 → 0x23
 * @note   STM32 HAL RTC要求时间日期为BCD格式
 *         例: 23点 → DecToBcd(23) = 0x23
 */
static uint8_t DecToBcd(uint8_t dec)
{
    return (uint8_t)((dec / 10) << 4) | (dec % 10);
}

/**
 * @brief  BCD转十进制
 * @param  bcd BCD编码值(0x00~0x99)
 * @retval 十进制数,如0x23 → 23
 */
static uint8_t BcdToDec(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

/**
 * @brief  解析英文星期名缩写到数字
 * @param  str 星期名缩写(如"Mon","Tue",至少3字节)
 * @retval 1=Mon, 2=Tue, 3=Wed, 4=Thu, 5=Fri, 6=Sat, 7=Sun, 0=解析失败
 * @note   ESP8266 AT+CIPSNTPTIME响应格式: +CIPSNTPTIME:Thu Jul 18 10:30:45 2024
 */
static uint8_t ParseWeekday(const char *str)
{
    if (strncmp(str, "Mon", 3) == 0) return 1;
    if (strncmp(str, "Tue", 3) == 0) return 2;
    if (strncmp(str, "Wed", 3) == 0) return 3;
    if (strncmp(str, "Thu", 3) == 0) return 4;
    if (strncmp(str, "Fri", 3) == 0) return 5;
    if (strncmp(str, "Sat", 3) == 0) return 6;
    if (strncmp(str, "Sun", 3) == 0) return 7;
    return 0;  /* 未知星期 */
}

/**
 * @brief  将ESP01S_NtpTime_t转为Unix时间戳(UTC)
 * @param  pTime        时间结构(已含时区偏移后的本地时间)
 * @param  timezoneHrs  时区小时数(用于还原为UTC)
 * @retval Unix时间戳(UTC秒数,自1970-01-01 00:00:00)
 * @note   计算步骤:
 *         1. 从1970年起逐年累加天数
 *         2. 加上当年已过月的天数
 *         3. 加上当月天数和时分秒
 *         4. 本地时间减去时区偏移得到UTC时间戳
 */
static uint32_t DateTimeToUnix(const ESP01S_NtpTime_t *pTime, int timezoneHrs)
{
    /* 第1步: 计算从1970-01-01到pTime->year-01-01的总天数 */
    uint32_t days = 0;
    uint16_t y;
    for (y = 1970; y < pTime->year; y++)
    {
        /* 闰年判断: 能被4整除且(不能被100整除或能被400整除) */
        days += ((y % 4 == 0) && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }

    /* 第2步: 加上当年已过月的天数
     * dim[0]: 平年每月天数, dim[1]: 闰年每月天数(2月29天) */
    static const uint8_t dim[2][12] = {
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
        {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    };
    uint8_t leap = ((pTime->year % 4 == 0) && (pTime->year % 100 != 0 || pTime->year % 400 == 0)) ? 1 : 0;
    uint8_t m;
    for (m = 0; m < (uint8_t)(pTime->month - 1); m++)
    {
        days += dim[leap][m];
    }

    /* 第3步: 加上当月天数(日-1,因为当天还未结束) */
    days += (uint32_t)(pTime->day - 1);

    /* 第4步: 总秒数 = 天数*86400 + 时分秒 */
    uint32_t ts = days * 86400
                + (uint32_t)pTime->hour * 3600
                + (uint32_t)pTime->minute * 60
                + (uint32_t)pTime->second;

    /* 第5步: 本地时间减去时区偏移得到UTC
     * 例如东八区: 本地10:00 → UTC 10:00-8:00 = 02:00 */
    if (timezoneHrs >= 0)
        ts -= (uint32_t)timezoneHrs * 3600;
    else
        ts += (uint32_t)(-timezoneHrs) * 3600;

    return ts;
}

/**
 * @brief  格式化时间到字符串
 * @param  pTime   时间结构指针
 * @param  format  格式类型: DT_DATE/DT_TIME/DT_ALL
 * @param  pBuf    输出缓冲区
 * @param  bufLen  缓冲区长度
 * @note   输出示例:
 *         DT_DATE: "2026-04-12"
 *         DT_TIME: "14:30:45"
 *         DT_ALL:  "2026-04-12 14:30:45"
 */
static void FormatDateTime(const ESP01S_NtpTime_t *pTime, DT_Format_t format, char *pBuf, uint16_t bufLen)
{
    switch (format)
    {
    case DT_DATE:
        snprintf(pBuf, bufLen, "%04d-%02d-%02d",
                 pTime->year, pTime->month, pTime->day);
        break;
    case DT_TIME:
        snprintf(pBuf, bufLen, "%02d:%02d:%02d",
                 pTime->hour, pTime->minute, pTime->second);
        break;
    case DT_ALL:
    default:
        snprintf(pBuf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
                 pTime->year, pTime->month, pTime->day,
                 pTime->hour, pTime->minute, pTime->second);
        break;
    }
}

/**
 * @brief  Unix时间戳(UTC)转日期时间(含时区偏移)
 * @param  timestamp   Unix时间戳(UTC秒数,自1970-01-01 00:00:00)
 * @param  timezoneHrs 时区小时数(东八区=8,负数表示西时区)
 * @param  pTime       输出时间结构(本地时间)
 * @note   算法步骤:
 *         1. UTC时间戳加时区偏移得到本地时间戳
 *         2. 用除法和模运算分离出天数和当天秒数
 *         3. 由当天秒数算出时分秒
 *         4. 由天数算出年月日和星期
 *         星期计算: 1970-01-01是周四,weekday=(days+3)%7+1 → 1=Mon,7=Sun
 */
static void UnixToDateTime(uint32_t timestamp, int timezoneHrs, ESP01S_NtpTime_t *pTime)
{
    /* 第1步: 时区偏移(UTC → 本地时间) */
    if (timezoneHrs >= 0)
        timestamp += (uint32_t)timezoneHrs * 3600;
    else
        timestamp -= (uint32_t)(-timezoneHrs) * 3600;

    /* 第2步: 分离天数和当天剩余秒数 */
    uint32_t days = timestamp / 86400;    /* 自1970-01-01以来的天数 */
    uint32_t secs = timestamp % 86400;    /* 当天剩余秒数 */

    /* 第3步: 计算时分秒 */
    pTime->hour   = (uint8_t)(secs / 3600);
    pTime->minute = (uint8_t)((secs % 3600) / 60);
    pTime->second = (uint8_t)(secs % 60);

    /* 第4步: 计算星期
     * 1970-01-01是周四,偏移3天使得周一=1,周日=7 */
    pTime->weekday = (uint8_t)((days + 3) % 7 + 1);

    /* 第5步: 逐年减去天数,得到年份 */
    uint32_t year = 1970;
    uint32_t daysInYear;
    while (1)
    {
        daysInYear = ((year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
        if (days < daysInYear)
            break;
        days -= daysInYear;
        year++;
    }
    pTime->year = (uint16_t)year;

    /* 第6步: 逐月减去天数,得到月份和日期 */
    static const uint8_t dimFmt[2][12] = {
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
        {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    };
    uint8_t leap = ((year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0;
    uint8_t month = 0;
    while (days >= dimFmt[leap][month])
    {
        days -= dimFmt[leap][month];
        month++;
    }
    pTime->month = (uint8_t)(month + 1);  /* 月份从1开始 */
    pTime->day   = (uint8_t)(days + 1);    /* 日期从1开始 */
}

/**
 * @brief  解析+IPD中的UDP NTP响应数据
 * @param  pBuf 接收缓冲区(可能含二进制数据)
 * @param  len  数据长度
 * @note   +IPD格式: +IPD,<len>:<data>
 *         NTP响应为48字节,时间戳在bytes 40-43(大端序,自1900-01-01的秒数)
 *         解析流程:
 *         1. 在二进制数据中搜索"+IPD,"前缀
 *         2. 解析数据长度字段
 *         3. 跳过':'分隔符
 *         4. 提取bytes 40-43的NTP时间戳(大端序转uint32_t)
 *         5. 减去NTP_UNIX_OFFSET得到Unix时间戳
 *         6. 转换为日期时间并记录ntpTick(即时记录,消除延迟误差)
 */
static void ESP01S_ParseUdpNtpData(const uint8_t *pBuf, uint16_t len)
{
    /* 第1步: 在二进制数据中搜索"+IPD,"前缀
     * 因为UDP NTP响应可能包含二进制数据,不能用strstr,
     * 只能逐字节搜索前缀 */
    uint16_t i;
    for (i = 0; i + 5 <= len; i++)
    {
        if (memcmp(pBuf + i, "+IPD,", 5) == 0)
            break;
    }
    if (i + 5 > len)
        return;  /* 未找到+IPD前缀 */

    const uint8_t *p = pBuf + i + 5; /* 跳过"+IPD," */

    /* 第2步: 解析数据长度(ASCII数字转整数) */
    uint16_t dataLen = 0;
    while (p < pBuf + len && *p >= '0' && *p <= '9')
    {
        dataLen = dataLen * 10 + (*p - '0');
        p++;
    }

    /* 第3步: 跳过':'分隔符 */
    if (p >= pBuf + len || *p != ':')
        return;
    p++;

    /* 第4步: 检查数据长度: NTP响应必须至少48字节 */
    uint16_t remain = (uint16_t)(pBuf + len - p);
    if (dataLen < 48 || remain < 48)
        return;

    /* 第5步: 提取NTP发送时间戳 (bytes 40-43, 大端序)
     * NTP报文结构: Header(4B) + Root Delay(4B) + Root Dispersion(4B) + Reference ID(4B)
     *             + Reference Timestamp(8B) + Originate Timestamp(8B) + Receive Timestamp(8B)
     *             + Transmit Timestamp(8B)
     * Transmit Timestamp从byte 40开始,前4字节为秒数(大端序) */
    uint32_t ntpSeconds = ((uint32_t)p[40] << 24) |
                          ((uint32_t)p[41] << 16) |
                          ((uint32_t)p[42] << 8)  |
                          ((uint32_t)p[43]);

    /* 第6步: 转换为Unix时间戳
     * NTP纪元从1900年起算,Unix纪元从1970年起算,差70年=2208988800秒 */
    if (ntpSeconds <= NTP_UNIX_OFFSET)
        return;  /* 无效时间(时间戳太小) */

    uint32_t unixTimestamp = ntpSeconds - NTP_UNIX_OFFSET;

    /* 第7步: 转换为本地日期时间 */
    UnixToDateTime(unixTimestamp, s_esp01s.config.ntpTimezone, &s_esp01s.ntpTime);
    s_esp01s.ntpTimeValid = 1;
    s_esp01s.ntpUdpPending = 0;

    /* 即时记录授时时间戳和Tick值,消除从接收到处理之间的延迟误差。
     * 后续 ESP01S_GetDateTime/ESP01S_SetRtcFromNtp 通过
     * ntpUnixTimestamp + (HAL_GetTick() - ntpTick) / 1000 推算当前时间 */
    s_esp01s.ntpTick = HAL_GetTick();
    s_esp01s.ntpUnixTimestamp = unixTimestamp;
}

/**
 * @brief  串口接收回调(由UartDrv分发调用)
 * @param  pData     接收数据指针(由UartDrv_RxEventDispatch传入)
 * @param  pUserCtx  用户上下文(即ESP01S_Instance_t实例指针)
 * @note   此函数在UartDrv的中断回调中被调用,处于中断上下文,应尽快返回。
 *         处理逻辑:
 *         1. 如果正在等待UDP NTP响应,优先解析+IPD数据(可能含二进制)
 *         2. 透传模式下:直接将数据传给用户回调
 *         3. AT指令模式下:调用ESP01S_ProcessATResponse解析响应并推进状态机
 */
static void ESP01S_RxCallback(UartDrv_RxData_t *pData, void *pUserCtx)
{
    if (pData == NULL || pUserCtx == NULL)
        return;

    ESP01S_Instance_t *pInst = (ESP01S_Instance_t *)pUserCtx;

    if (pData->rx_len > 0)
    {
        /* UDP NTP等待中: 优先检查+IPD数据(可能含二进制,不能用字符串函数处理) */
        if (pInst->ntpUdpPending)
        {
            ESP01S_ParseUdpNtpData(pData->rx_buf, pData->rx_len);
            if (pInst->ntpTimeValid)
                return;  /* NTP数据已解析,不再做字符串处理 */
        }

        /* 天气HTTP查询等待中: 缓存原始响应数据供后续解析 */
        if (pInst->weatherPending)
        {
            uint16_t copyLen = pData->rx_len;
            if (pInst->weatherRxLen + copyLen > sizeof(pInst->weatherRxBuf) - 1)
                copyLen = (uint16_t)(sizeof(pInst->weatherRxBuf) - 1 - pInst->weatherRxLen);
            if (copyLen > 0)
            {
                memcpy(pInst->weatherRxBuf + pInst->weatherRxLen, pData->rx_buf, copyLen);
                pInst->weatherRxLen += copyLen;
                pInst->weatherRxBuf[pInst->weatherRxLen] = '\0';
            }
            return;  /* 天气查询期间不处理AT响应/透传数据 */
        }

        char *pstr = (char *)pData->rx_buf;

        if (pInst->state == ESP01S_STATE_TRANSPARENT)
        {
            /* 透传模式: 优先通过消息队列发送到任务上下文处理 */
#ifndef ESP01S_NO_FREERTOS
            if (pInst->rxQueue != NULL)
            {
                UartDrv_QueueEvent_t evt;
                evt.pDrv = pInst->pUartDrv;
                evt.len  = (pData->rx_len > UART_DRV_QUEUE_DATA_SIZE) ? UART_DRV_QUEUE_DATA_SIZE : pData->rx_len;
                memcpy(evt.data, pData->rx_buf, evt.len);
                osMessageQueuePut(pInst->rxQueue, &evt, 0, 0);
            }
            else
#endif
            {
                /* 未注册队列时,直接在回调中调用用户回调 */
                if (pInst->pDataCb)
                {
                    pInst->pDataCb(pData->rx_buf, pData->rx_len, pInst->pUserCtx);
                }
            }
        }
        else
        {
            /* AT指令模式: 解析AT响应,推动状态机 */
            ESP01S_ProcessATResponse(pstr, pData->rx_len);
        }
    }
}

/**
 * @brief  解析ESP01S的AT响应,推动状态机
 * @param  pstr 接收到的AT响应字符串(已由UartDrv添加'\0'结尾)
 * @param  len  数据长度
 * @note   本函数是状态机的核心,根据AT响应中的关键字推进状态:
 *         - "OK" (行首): 通用成功响应,用于AT测试和WiFi连接确认
 *         - "WIFI CONNECTED": WiFi正在连接(中间状态)
 *         - "\r\nOK" (行首): WiFi连接成功(收到CWJAP的最终OK)
 *         - "CONNECT\r\n\r\nOK" 或 "ALREADY CONNECTED": TCP连接成功
 *         - "+CWSAP:": 解析模块自身AP名称
 *         - "+CWJAP:": 解析当前连接的热点名称
 *         - "+CIPSNTPTIME:": 解析AT SNTP时间
 */
static void ESP01S_ProcessATResponse(const char *pstr, uint16_t len)
{
    /* ---- 检测OK响应 ----
     * AT指令执行成功通常会返回"OK"。
     * 当状态为IDLE时,收到OK说明AT通信正常,推进到AT_OK。 */
    if (strstr(pstr, "OK") >= pstr)
    {
        if (s_esp01s.state == ESP01S_STATE_IDLE)
        {
            s_esp01s.state = ESP01S_STATE_AT_OK;
        }
    }

    /* 仅在AT通信正常后继续解析后续响应 */
    if (s_esp01s.state < ESP01S_STATE_AT_OK)
        return;

    /* ---- WIFI CONNECTED ----
     * AT+CWJAP连接WiFi时的中间状态,表示正在关联AP,
     * 还需要等待最终的\r\nOK才算连接完成 */
    if (strstr(pstr, "WIFI CONNECTED") == pstr)
    {
        s_esp01s.state = ESP01S_STATE_WIFI_CONNECTING;
    }

    /* ---- WiFi连接成功 ----
     * AT+CWJAP最终返回\r\nOK,表示连接过程完成。
     * 用"\r\nOK"而非"OK"来匹配,避免误判其他含OK的响应 */
    if (strstr(pstr, "\r\nOK") == pstr)
    {
        if (s_esp01s.state == ESP01S_STATE_WIFI_CONNECTING)
        {
            s_esp01s.state = ESP01S_STATE_WIFI_CONNECTED;
        }
    }

    /* ---- TCP连接成功 ----
     * AT+CIPSTART成功返回: "CONNECT\r\n\r\nOK"
     * 如果已连接则返回: "ALREADY CONNECTED"
     *
     * 使用 != NULL 而非 == pstr,因为此前AT+CIPCLOSE的响应
     * (CLOSED\r\n\r\nOK)可能与CIPSTART响应在同一UART空闲中断帧中,
     * 导致缓冲区开头不是"CONNECT"。 */
    if (strstr(pstr, "CONNECT\r\n\r\nOK") != NULL && s_esp01s.state == ESP01S_STATE_TCP_CONNECTING)
    {
        s_esp01s.state = ESP01S_STATE_TCP_CONNECTED;
    }
    else if (strstr(pstr, "ALREADY CONNECTED") != NULL && s_esp01s.state == ESP01S_STATE_TCP_CONNECTING)
    {
        s_esp01s.state = ESP01S_STATE_TCP_CONNECTED;
    }

    /* ---- DNS/TCP连接失败 ----
     * AT+CIPSTART使用域名时,如果DNS解析失败,ESP8266返回:
     * "+CIPSTART: DNS fail"
     * 如果TCP连接被拒绝,返回:
     * "+CIPSTART: CONNECT FAIL" */
    if (s_esp01s.state == ESP01S_STATE_TCP_CONNECTING &&
        (strstr(pstr, "DNS fail") != NULL || strstr(pstr, "DNS FAIL") != NULL ||
         strstr(pstr, "CONNECT FAIL") != NULL))
    {
        printf("[ESP01S] CIPSTART失败: %s\r\n", pstr);
    }

    /* ---- 解析+CIPDOMAIN响应 ----
     * AT+CIPDOMAIN="hostname" 返回: "+CIPDOMAIN:123.45.67.89"
     * 提取IP地址缓存到cipdomainResult供后续CIPSTART使用 */
    if (strstr(pstr, "+CIPDOMAIN:") != NULL)
    {
        const char *match = strstr(pstr, "+CIPDOMAIN:");
        const char *ipStart = match + 11; /* 跳过"+CIPDOMAIN:" */
        while (*ipStart == ' ' || *ipStart == '\r' || *ipStart == '\n')
            ipStart++;
        /* 提取IP地址: 遇到非数字/点/冒号(IPv6)字符停止 */
        const char *p = ipStart;
        while (*p && (*p == '.' || (*p >= '0' && *p <= '9') || *p == ':'))
            p++;
        uint16_t ipLen = (uint16_t)(p - ipStart);
        if (ipLen > 0 && ipLen < sizeof(s_esp01s.cipdomainResult))
        {
            strncpy(s_esp01s.cipdomainResult, ipStart, ipLen);
            s_esp01s.cipdomainResult[ipLen] = '\0';
            printf("[ESP01S] DNS解析成功: %s -> %s\r\n", "api.seniverse.com", s_esp01s.cipdomainResult);
        }
    }

    /* ---- 解析+CWSAP响应 - 模块自身AP名称 ----
     * 响应格式: +CWSAP:"ESP_XXXXXX","password",1,0
     * 提取第一个引号内的AP名称,缓存到strESPName */
    if (strstr(pstr, "+CWSAP:") == pstr)
    {
        const char *pStart = pstr + 7;  /* 跳过"+CWSAP:" */
        while (*pStart == ' ')
            pStart++;                   /* 跳过空格 */
        if (*pStart == '"')
            pStart++;                   /* 跳过开引号 */
        const char *pEnd = strstr(pStart, "\"");
        if (pEnd != NULL && pEnd > pStart)
        {
            uint16_t nameLen = (uint16_t)(pEnd - pStart);
            if (nameLen < sizeof(s_esp01s.strESPName))
            {
                strncpy(s_esp01s.strESPName, pStart, nameLen);
                s_esp01s.strESPName[nameLen] = '\0';
            }
        }
    }

    /* ---- 解析+CWJAP响应 - 已连接的热点名称 ----
     * 响应格式: +CWJAP:"MyWiFi","b0:be:76:xx:xx:xx",6,-48
     * 提取第一个引号内的热点名称,缓存到strAPName
     * 用于判断是否已连接到目标热点,避免重复连接 */
    if (strstr(pstr, "+CWJAP:") == pstr)
    {
        const char *pStart = pstr + 7;  /* 跳过"+CWJAP:" */
        if (*pStart == '"')
            pStart++;                   /* 跳过开引号 */
        const char *pEnd = strstr(pStart, "\"");
        if (pEnd != NULL && pEnd > pStart)
        {
            uint16_t nameLen = (uint16_t)(pEnd - pStart);
            if (nameLen < sizeof(s_esp01s.strAPName))
            {
                strncpy(s_esp01s.strAPName, pStart, nameLen);
                s_esp01s.strAPName[nameLen] = '\0';
            }
        }
    }

    /* ---- 解析+CIPSNTPTIME响应 - AT指令SNTP时间 ----
     * ESP8266 AT响应格式示例:
     * +CIPSNTPTIME:Thu Jul 18 10:30:45 2024
     *
     * 注意: 如果固件不支持SNTP,会返回1970年,后续会自动回退到UDP NTP。
     * 只有年份>2000时才记录ntpTick和ntpUnixTimestamp(避免记录无效时间)。
     */
    const char *pNtp = strstr(pstr, "+CIPSNTPTIME:");
    if (pNtp != NULL)
    {
        pNtp += 13; /* 跳过"+CIPSNTPTIME:" */
        char weekday[4] = {0};
        char month[4] = {0};
        int day = 0, hour = 0, minute = 0, second = 0, year = 0;

        int parsed = sscanf(pNtp, "%3s %3s %d %d:%d:%d %d",
                            weekday, month, &day, &hour, &minute, &second, &year);

        if (parsed == 7)
        {
            s_esp01s.ntpTime.year = (uint16_t)year;
            s_esp01s.ntpTime.day = (uint8_t)day;
            s_esp01s.ntpTime.hour = (uint8_t)hour;
            s_esp01s.ntpTime.minute = (uint8_t)minute;
            s_esp01s.ntpTime.second = (uint8_t)second;
            s_esp01s.ntpTime.weekday = ParseWeekday(weekday);

            /* 月份英文名转数字: Jan=1, Feb=2, ..., Dec=12 */
            if (strncmp(month, "Jan", 3) == 0)      s_esp01s.ntpTime.month = 1;
            else if (strncmp(month, "Feb", 3) == 0)  s_esp01s.ntpTime.month = 2;
            else if (strncmp(month, "Mar", 3) == 0)  s_esp01s.ntpTime.month = 3;
            else if (strncmp(month, "Apr", 3) == 0)  s_esp01s.ntpTime.month = 4;
            else if (strncmp(month, "May", 3) == 0)  s_esp01s.ntpTime.month = 5;
            else if (strncmp(month, "Jun", 3) == 0)  s_esp01s.ntpTime.month = 6;
            else if (strncmp(month, "Jul", 3) == 0)  s_esp01s.ntpTime.month = 7;
            else if (strncmp(month, "Aug", 3) == 0)  s_esp01s.ntpTime.month = 8;
            else if (strncmp(month, "Sep", 3) == 0)  s_esp01s.ntpTime.month = 9;
            else if (strncmp(month, "Oct", 3) == 0)  s_esp01s.ntpTime.month = 10;
            else if (strncmp(month, "Nov", 3) == 0)  s_esp01s.ntpTime.month = 11;
            else if (strncmp(month, "Dec", 3) == 0)  s_esp01s.ntpTime.month = 12;

            s_esp01s.ntpTimeValid = 1;

            /* 即时记录授时时间戳(仅AT SNTP返回有效年份时),
             * 用于后续时间推算和RTC校准延迟补偿。
             * 年份<=2000说明固件不支持SNTP,不应记录 */
            if (s_esp01s.ntpTime.year > 2000)
            {
                s_esp01s.ntpTick = HAL_GetTick();
                s_esp01s.ntpUnixTimestamp = DateTimeToUnix(&s_esp01s.ntpTime, s_esp01s.config.ntpTimezone);
            }
        }
        else
        {
            s_esp01s.ntpTimeValid = 0;
            printf("[ESP01S] NTP时间解析失败\r\n");
        }
    }
}
