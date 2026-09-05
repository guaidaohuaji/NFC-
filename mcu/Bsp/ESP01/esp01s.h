/**
 * @file    esp01s.h
 * @brief   ESP-01S WiFi模块驱动 - 基于UartDrv通用串口驱动的上层应用驱动
 * @version 1.0
 *
 * @par 架构概述
 *   本驱动采用"单例实例 + 状态机"架构,封装ESP-01S模块的完整工作流程:
 *   - 驱动内部维护唯一的单例实例 s_esp01s,用户无需定义ESP01S_t变量
 *   - 通过状态机(ESP01S_State_t)管理连接生命周期: IDLE→AT_OK→WiFi→TCP→透传
 *   - 上层依赖UartDrv串口驱动,通过回调机制接收AT响应和数据
 *   - 支持NTP网络授时(AT SNTP / UDP NTP双模式自动回退)
 *   - 支持RTC校时和时间字符串格式化输出
 *
 * @par 连接流程
 *   ESP01S_Start() 完成以下步骤:
 *   1. 退出透传模式(防止上次遗留)
 *   2. AT通信测试
 *   3. 关闭回显和残留连接
 *   4. 配置单连接模式(CIPMUX=0,透传前提)
 *   5. 查询模块信息和当前连接状态
 *   6. 连接WiFi热点
 *   7. NTP授时(可选)
 *   8. 连接TCP服务器
 *   9. 开启透传模式
 *
 * @par 使用方法
 *   1. 调用 UartDrv_Init() 初始化底层串口驱动
 *   2. 调用 ESP01S_Init() 初始化ESP01S驱动(使用默认配置)
 *   3. (可选) 调用 ESP01S_SetWiFi/SetTcpServer/SetNtpServer 修改配置
 *   4. 调用 ESP01S_RegisterDataCb() 注册数据接收回调(可选)
 *   5. 调用 ESP01S_Start() 启动WiFi连接和TCP连接
 *   6. 透传模式下调用 ESP01S_SendStr()/ESP01S_SendData() 发送数据
 *   7. 调用 ESP01S_GetDateTime() 获取当前时间字符串
 *   8. 调用 ESP01S_SetRtcFromNtp() 将NTP时间写入RTC
 *
 * @par 配置方式
 *   - 方式1: 直接修改 esp01s.c 中的 s_defaultConfig 默认值(最简单)
 *   - 方式2: 调用 ESP01S_SetConfig() 一次性覆盖全部配置
 *   - 方式3: 调用 ESP01S_SetWiFi/SetTcpServer/SetNtpServer 分项修改
 *   注意: 修改配置后需重新调用 ESP01S_Start() 才会生效
 *
 * @par NTP授时说明
 *   支持两种NTP授时方式(自动回退):
 *   - 方式1: AT指令SNTP (AT+CIPSNTPCFG + AT+CIPSNTPTIME),需固件v1.7.0+
 *   - 方式2: UDP NTP协议,通过UDP直连NTP服务器发送/解析NTP报文,通用方案
 *   如果config.ntpServer为空字符串,则跳过NTP授时
 *
 * @par 透传模式注意事项
 *   - 透传模式下只能通过 ESP01S_SendStr/SendData 发送原始数据
 *   - 退出透传模式需调用 ESP01S_ExitTransparent(),内部发送"+++"命令
 *   - "+++"前后需各500ms静默,不加换行符(ESP8266透传退出协议要求)
 *   - 退出后可重新发送AT指令或修改配置后重新Start
 */

#ifndef __ESP01S_H
#define __ESP01S_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "uart_drv.h"

#ifndef ESP01S_NO_FREERTOS
#include "cmsis_os.h"
#endif

/* ====== 导出类型定义 ====== */

/**
 * @brief  ESP01S连接状态枚举
 * @note   状态转移顺序(正常流程):
 *         IDLE → AT_OK → WIFI_CONNECTING → WIFI_CONNECTED
 *              → TCP_CONNECTING → TCP_CONNECTED → TRANSPARENT
 *
 *         状态判断逻辑:
 *         - ESP01S_Start()中通过 "state < ESP01S_STATE_xxx" 判断是否达到某阶段
 *         - 回调中根据AT响应字符串推动状态前进
 *         - 任何阶段失败则停留在当前状态,Start()返回对应错误码
 */
typedef enum {
    ESP01S_STATE_IDLE = 0,          /**< 空闲,未初始化或刚初始化 */
    ESP01S_STATE_AT_OK,             /**< AT通信正常(模块响应AT指令) */
    ESP01S_STATE_WIFI_CONNECTING,   /**< 正在连接WiFi(AT+CWJAP已发送) */
    ESP01S_STATE_WIFI_CONNECTED,    /**< WiFi已连接(收到WIFI CONNECTED+OK) */
    ESP01S_STATE_TCP_CONNECTING,    /**< 正在连接TCP服务器(AT+CIPSTART已发送) */
    ESP01S_STATE_TCP_CONNECTED,     /**< TCP已连接(收到CONNECT+OK) */
    ESP01S_STATE_TRANSPARENT,       /**< 透传模式(AT+CIPSEND已发送并成功) */
} ESP01S_State_t;

/**
 * @brief  ESP01S配置结构
 * @note   配置方法:
 *         - 直接修改 esp01s.c 中的 s_defaultConfig
 *         - 调用 ESP01S_SetConfig() / ESP01S_SetWiFi() / ESP01S_SetTcpServer() / ESP01S_SetNtpServer()
 *         修改后需调用 ESP01S_Start() 重新连接才生效
 */
typedef struct {
    char     ssid[32];              /**< WiFi热点名称(SSID),最长31字符 */
    char     password[64];          /**< WiFi密码,最长63字符 */
    char     tcpServerIP[48];       /**< TCP服务器IP地址(点分十进制或域名) */
    uint16_t tcpPort;               /**< TCP服务器端口号(1~65535) */
    char     ntpServer[48];         /**< NTP服务器域名(如"ntp.aliyun.com"),空字符串则不启用NTP */
    int8_t   ntpTimezone;           /**< 时区小时数(中国东八区=8),范围-12~14 */
} ESP01S_Config_t;

/**
 * @brief  NTP时间结构
 * @note   时间为本地时间(已含时区偏移),由NTP响应或UDP NTP报文解析得到。
 *         通过 ESP01S_GetNtpTime() 获取,或由 ESP01S_GetDateTime() 间接使用。
 */
typedef struct {
    uint16_t year;      /**< 年 (2024~) */
    uint8_t  month;     /**< 月 (1~12) */
    uint8_t  day;       /**< 日 (1~31) */
    uint8_t  hour;      /**< 时 (0~23, 已含时区偏移) */
    uint8_t  minute;    /**< 分 (0~59) */
    uint8_t  second;    /**< 秒 (0~59) */
    uint8_t  weekday;   /**< 星期 (1=Mon, 2=Tue, ..., 7=Sun) */
} ESP01S_NtpTime_t;

/**
 * @brief  日期时间格式枚举
 * @note   用于 ESP01S_GetDateTime() 的 format 参数,控制输出格式
 */
typedef enum {
    DT_DATE = 0,        /**< 仅日期: "2026-04-12" */
    DT_TIME,            /**< 仅时间: "14:30:45" */
    DT_ALL,             /**< 日期+时间: "2026-04-12 14:30:45" */
} DT_Format_t;

/**
 * @brief  ESP01S透传数据接收回调函数类型
 * @param  pData      接收到的原始数据指针(透传模式下TCP服务器发来的数据)
 * @param  len        接收数据长度
 * @param  pUserCtx   用户上下文指针(注册时传入,回调时原样传回)
 * @note   回调在中断上下文中执行,应尽快返回
 */
typedef void (*ESP01S_DataCallback_t)(const uint8_t *pData, uint16_t len, void *pUserCtx);

/* ====== 导出函数声明 ====== */

/* ---- 初始化与启停 ---- */

/**
 * @brief  初始化ESP01S驱动(使用esp01s.c中的默认配置)
 * @param  pUartDrv 底层串口驱动指针(需先初始化,如 UartDrv_Init(&g_uart4Drv, &huart4))
 * @note   初始化流程:
 *         1. 清零内部单例实例
 *         2. 绑定串口驱动
 *         3. 拷贝默认配置到实例
 *         4. 注册串口接收回调(ESP01S_RxCallback)
 *         调用后状态为 ESP01S_STATE_IDLE,需调用 ESP01S_Start() 开始连接
 */
void ESP01S_Init(UartDrv_t *pUartDrv);

/**
 * @brief  启动ESP01S完整流程: AT测试 → 连WiFi → NTP授时 → 连TCP → 透传
 * @retval 0:成功  -1:AT通信失败  -2:WiFi连接失败  -3:TCP连接失败
 * @note   本函数为阻塞式,内部通过HAL_Delay等待AT响应,整个流程约需10~20秒。
 *         调用前需确保:
 *         - ESP01S_Init() 已调用
 *         - 配置参数(SSID/密码/TCP服务器)已正确设置
 *         - 底层串口已初始化并启动接收
 *         成功后进入透传模式,可直接使用 SendStr/SendData 发送数据
 */
int ESP01S_Start(void);

/**
 * @brief  退出透传模式
 * @note   内部发送"+++"命令退出透传,前后各等待500ms静默(ESP8266协议要求):
 *         - 前导500ms: 确保之前的数据发送已完成
 *         - 发送"+++": 不加换行符(否则会被当作数据发送)
 *         - 后续500ms: 等待模块识别退出命令
 *         退出后状态回退到 ESP01S_STATE_TCP_CONNECTED,可重新发AT指令
 *         或修改配置后调用 ESP01S_Start() 重新连接
 */
void ESP01S_ExitTransparent(void);

/* ---- 配置API ---- */

/**
 * @brief  一次性设置全部配置参数
 * @param  pConfig 配置参数指针(内容会被拷贝,调用后可释放)
 * @note   修改配置后需调用ESP01S_Start()重新连接才生效
 *         使用示例:
 *         @code
 *         ESP01S_Config_t cfg = {
 *             .ssid = "MyWiFi", .password = "12345678",
 *             .tcpServerIP = "192.168.1.100", .tcpPort = 8080,
 *             .ntpServer = "ntp.aliyun.com", .ntpTimezone = 8
 *         };
 *         ESP01S_SetConfig(&cfg);
 *         @endcode
 */
void ESP01S_SetConfig(const ESP01S_Config_t *pConfig);

/**
 * @brief  获取当前配置(只读)
 * @retval 当前配置指针,指向驱动内部配置(不要修改返回的指针内容)
 * @note   返回的是内部配置的地址,不要直接修改返回指针的内容,
 *         应使用 ESP01S_SetConfig/SetWiFi/SetTcpServer/SetNtpServer 来修改
 */
const ESP01S_Config_t *ESP01S_GetConfig(void);

/**
 * @brief  设置WiFi热点参数
 * @param  ssid     WiFi热点名称(最长31字符,超出截断)
 * @param  password WiFi密码(最长63字符,超出截断)
 * @note   修改后需调用ESP01S_Start()重新连接
 */
void ESP01S_SetWiFi(const char *ssid, const char *password);

/**
 * @brief  设置TCP服务器参数
 * @param  ip   TCP服务器IP地址(点分十进制,如"192.168.1.100")
 * @param  port TCP服务器端口号(1~65535)
 * @note   修改后需调用ESP01S_Start()重新连接
 */
void ESP01S_SetTcpServer(const char *ip, uint16_t port);

/**
 * @brief  设置NTP授时服务器参数
 * @param  server    NTP服务器域名(如"ntp.aliyun.com"),传""或NULL则禁用NTP授时
 * @param  timezone  时区小时数(中国东八区=8),范围-12~14
 * @note   修改后需调用ESP01S_Start()重新连接
 */
void ESP01S_SetNtpServer(const char *server, int8_t timezone);

/* ---- 数据收发 ---- */

/**
 * @brief  发送AT指令并等待
 * @param  cmd     AT指令字符串(需包含\r\n结尾,如"AT\r\n")
 * @param  waitMs  发送后等待时间(毫秒),用于等待模块响应,0则不等
 * @note   本函数为阻塞式,内部调用 UartDrv_SendStr 发送 + HAL_Delay 等待
 *         透传模式下请勿调用此函数(数据会被当作透传数据发送)
 */
void ESP01S_SendATCmd(const char *cmd, uint32_t waitMs);

/**
 * @brief  发送字符串(透传模式)
 * @param  str 以'\0'结尾的字符串
 * @note   仅在透传模式下使用,数据将直接发送到TCP服务器
 */
void ESP01S_SendStr(const char *str);

/**
 * @brief  发送数据(透传模式)
 * @param  pData 数据指针
 * @param  len   数据长度(字节)
 * @note   仅在透传模式下使用,可发送二进制数据(含\0的字节)
 */
void ESP01S_SendData(const uint8_t *pData, uint16_t len);

/**
 * @brief  注册透传数据接收回调
 * @param  pCb       回调函数指针(函数原型见 ESP01S_DataCallback_t)
 * @param  pUserCtx  用户上下文指针,回调时原样传回,可传NULL
 * @note   透传模式下收到TCP服务器数据时触发回调
 *         使用示例:
 *         @code
 *         void MyDataCb(const uint8_t *pData, uint16_t len, void *pCtx) {
 *             // 处理接收到的TCP数据
 *         }
 *         ESP01S_RegisterDataCb(MyDataCb, NULL);
 *         @endcode
 */
void ESP01S_RegisterDataCb(ESP01S_DataCallback_t pCb, void *pUserCtx);

/* ---- 状态查询 ---- */

/**
 * @brief  获取当前连接状态
 * @retval 当前状态(见 ESP01S_State_t 枚举定义)
 * @note   可用于判断模块是否就绪、是否在透传模式等
 */
ESP01S_State_t ESP01S_GetState(void);

/* ---- NTP授时 ---- */

/**
 * @brief  配置SNTP服务器并获取NTP网络时间
 * @note   需在WiFi已连接、未进入透传模式时调用。
 *         ESP01S_Start()中已自动调用此函数,一般无需手动调用。
 *         工作流程:
 *         1. 尝试AT指令SNTP (AT+CIPSNTPCFG + AT+CIPSNTPTIME)
 *         2. 若AT SNTP失败(固件不支持),回退到UDP NTP协议
 *         3. UDP方式: 关闭已有连接→建UDP→发NTP请求→解析响应→关UDP
 *         如果config.ntpServer为空字符串则跳过
 */
void ESP01S_SyncNtpTime(void);

/**
 * @brief  获取最近一次NTP授时结果
 * @param  pTime 输出时间结构指针(由调用者分配)
 * @retval 0:时间有效  -1:时间无效(未获取或获取失败)
 * @note   返回的是NTP授时时的瞬间时间,不是当前时间。
 *         如需获取当前时间请使用 ESP01S_GetDateTime()
 */
int ESP01S_GetNtpTime(ESP01S_NtpTime_t *pTime);

/**
 * @brief  检查NTP时间是否已同步成功
 * @retval 1:已同步  0:未同步
 */
uint8_t ESP01S_IsNtpSynced(void);

/**
 * @brief  检查当前是否已连接WiFi(即ESP01S状态机已到达WIFI_CONNECTED及以上)
 * @retval 1:已连接  0:未连接
 */
uint8_t ESP01S_IsWiFiConnected(void);

/**
 * @brief  将NTP时间写入STM32的RTC(BCD格式)
 * @param  pRtc STM32 RTC句柄指针,传NULL则跳过RTC写入
 * @retval 0:成功  -1:NTP时间无效  -2:RTC写入失败
 * @note   调用前需确保NTP时间已有效(先调用ESP01S_Start或ESP01S_SyncNtpTime)
 *         内部会补偿从NTP授时到当前的时间差(通过HAL_Tick计算),
 *         消除后续操作(TCP连接等)带来的延迟误差。
 *         写入后可通过 HAL_RTC_GetTime/GetDate 读取RTC时间
 */
int ESP01S_SetRtcFromNtp(RTC_HandleTypeDef *pRtc);

/**
 * @brief  查询心知天气日报并解析出城市、白天/夜间天气、温度及降雨量
 * @param  apiKey          心知天气API Key
 * @param  location        城市,如 "hangzhou"
 * @param  language        语言,如 "zh-Hans"
 * @param  unit            温度单位,"c"或"f"
 * @param  outCity         输出城市名缓冲区
 * @param  cityBufSize     outCity缓冲区大小(建议>=16)
 * @param  outTextDay      输出白天天气现象文字缓冲区
 * @param  textDayBufSize  outTextDay缓冲区大小(建议>=32)
 * @param  outHigh         输出白天最高温度字符串缓冲区
 * @param  highBufSize     outHigh缓冲区大小(建议>=8)
 * @param  outTextNight    输出夜间天气现象文字缓冲区
 * @param  textNightBufSize outTextNight缓冲区大小(建议>=32)
 * @param  outLow          输出夜间最低温度字符串缓冲区
 * @param  lowBufSize      outLow缓冲区大小(建议>=8)
 * @param  outPrecip       输出降雨概率字符串缓冲区
 * @param  precipBufSize   outPrecip缓冲区大小(建议>=8)
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
                        char *outPrecip, uint16_t precipBufSize);

/* ---- 时间获取 ---- */

/**
 * @brief  获取当前时间字符串
 * @param  pRtc    STM32 RTC句柄指针,传NULL则不从RTC读取
 * @param  format  时间格式: DT_DATE/DT_TIME/DT_ALL
 * @param  pBuf    输出字符串缓冲区(由调用者分配)
 * @param  bufLen  缓冲区长度(建议>=24字节)
 * @retval 0:成功  -1:无可用时间源(NTP无效且RTC无效或未传)
 * @note   时间来源优先级:
 *         1. RTC(如果pRtc非NULL且RTC年份>2000) — 最精确,掉电可保持
 *         2. NTP时间+HAL_Tick差值推算(如果NTP已授时) — 有累积误差
 *         格式示例:
 *         DT_DATE: "2026-04-12"
 *         DT_TIME: "14:30:45"
 *         DT_ALL:  "2026-04-12 14:30:45"
 *         使用示例:
 *         @code
 *         char buf[24];
 *         if (ESP01S_GetDateTime(&hrtc, DT_ALL, buf, sizeof(buf)) == 0)
 *             printf("当前时间: %s\r\n", buf);
 *         @endcode
 */
int ESP01S_GetDateTime(RTC_HandleTypeDef *pRtc, DT_Format_t format, char *pBuf, uint16_t bufLen);

#ifdef __cplusplus
}
#endif

#ifndef ESP01S_NO_FREERTOS
/**
 * @brief  注册ESP01S透传数据接收队列
 * @param  queue CMSIS-RTOS2 消息队列句柄
 * @note   注册队列后,透传模式下接收到的TCP数据将直接发送到该队列,
 *         不再通过 ESP01S_RegisterDataCb 注册的回调处理。
 *         AT指令模式的响应仍由内部回调在ISR中处理。
 */
void ESP01S_RegisterRxQueue(osMessageQueueId_t queue);
#endif

/**
 * @brief  在任务上下文中分发透传数据到用户回调
 * @param  pData 接收到的TCP数据指针
 * @param  len   数据长度
 * @note   当使用消息队列接收透传数据时,应在 NetworkTask 中读取队列后调用本函数,
 *         或自行处理数据。本函数会调用 ESP01S_RegisterDataCb 注册的用户回调。
 */
void ESP01S_DispatchTransparentData(const uint8_t *pData, uint16_t len);
#endif /* __ESP01S_H */

