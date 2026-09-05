/**
 * @file    uart_drv.h
 * @brief   通用串口驱动 - 基于STM32 HAL库的串口收发及回调分发
 * @version 1.0
 *
 * @par 架构概述
 *   本驱动采用"实例注册 + 回调分发"架构,支持多个串口外设同时工作:
 *   - 每个串口外设对应一个 UartDrv_t 实例
 *   - 所有实例通过静态注册表管理(最多 UART_DRV_MAX_INSTANCES 个)
 *   - 串口接收事件通过 HAL_UARTEx_RxEventCallback 自动分发到对应实例的回调
 *   - 每个实例可独立注册接收回调函数和用户上下文
 *
 * @par 接收机制
 *   采用 HAL_UARTEx_ReceiveToIdle_IT 空闲中断接收模式:
 *   - 当串口总线空闲一个字符时间后触发中断,一次性返回整帧数据
 *   - 相比固定长度接收,无需预知数据长度,适合AT指令等变长协议
 *   - 接收缓冲区大小由 UART_DRV_RX_BUF_SIZE 决定(默认512字节)
 *
 * @par 使用方法
 *   1. 定义 UartDrv_t 实例:   UartDrv_t g_uart1Drv;
 *   2. 调用 UartDrv_Init() 初始化:  UartDrv_Init(&g_uart1Drv, &huart1);
 *   3. 调用 UartDrv_RegisterRxCb() 注册接收回调
 *   4. 调用 UartDrv_StartRecv() 开始接收
 *   5. 在回调函数中处理接收到的数据
 *   6. (可选) 调用 UartDrv_SetDebugPort() 设置printf调试输出口
 *
 * @par 自定义回调行为
 *   - 轻度自定义: 在自己的.c文件中重写 UartDrv_OnRxEvent() 弱钩子函数,
 *     在分发完成后追加额外处理
 *   - 完全自定义: 在编译选项中定义 UART_DRV_NO_DEFAULT_CALLBACK 宏,
 *     然后在自己的.c文件中重写 HAL_UARTEx_RxEventCallback()
 *
 * @par 线程安全
 *   本驱动未使用互斥锁,发送函数采用阻塞式HAL调用:
 *   - UartDrv_Send/UartDrv_SendStr 内部调用 HAL_UART_Transmit(阻塞)
 *   - 不建议在中断上下文中调用发送函数(可能长时间阻塞)
 *   - 接收回调在中断上下文中执行,回调函数应尽快返回
 */

#ifndef __UART_DRV_H
#define __UART_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

#ifndef UART_DRV_NO_FREERTOS
#include "cmsis_os.h"
#endif

/* ====== 编译配置宏 ====== */

/**
 * @def   UART_DRV_MAX_INSTANCES
 * @brief 静态注册表最大实例数,即系统最多可同时管理的串口驱动实例数量
 * @note  增大此值会增加静态内存占用(每个指针4/8字节),
 *        一般STM32F1项目使用2~4个串口,8个已足够
 */
#define UART_DRV_MAX_INSTANCES  8

/**
 * @def   UART_DRV_RX_BUF_SIZE
 * @brief 每个串口实例的接收缓冲区大小(字节)
 * @note  此值决定单次空闲中断能接收的最大帧长度。
 *        AT指令响应通常在200字节以内,NTP数据包48字节,
 *        512字节可满足大多数场景。若接收数据被截断,可适当增大。
 */
#define UART_DRV_RX_BUF_SIZE    512

/* ====== 导出类型定义 ====== */

/**
 * @brief  串口接收数据结构
 * @note   每次空闲中断触发后,接收到的数据将存入此结构:
 *         - rx_buf: 存储原始接收字节,回调结束后会被下一帧数据覆盖
 *         - rx_len: 本帧接收到的有效字节数
 *         回调函数中应尽快处理数据,或将其拷贝到用户自己的缓冲区
 */
typedef struct {
    uint16_t rx_len;                        /**< 本帧接收数据长度(字节数) */
    uint8_t  rx_buf[UART_DRV_RX_BUF_SIZE];  /**< 接收数据缓冲区(回调结束后可能被覆盖) */
} UartDrv_RxData_t;

/**
 * @brief  串口接收回调函数类型
 * @param  pData     指向本帧接收数据的指针,数据在回调返回后可能被覆盖
 * @param  pUserCtx  用户在 UartDrv_RegisterRxCb() 中传入的上下文指针,
 *                   用于回调函数访问外部变量,避免使用全局变量
 * @note   回调在中断上下文中执行,应尽快返回,不要做耗时操作
 */
typedef void (*UartDrv_RxCallback_t)(UartDrv_RxData_t *pData, void *pUserCtx);

/**
 * @brief  串口驱动实例结构
 * @note   每个串口外设对应一个此结构实例,如:
 *         UartDrv_t g_uart1Drv;  // USART1的驱动实例
 *         UartDrv_t g_uart4Drv;  // UART4的驱动实例
 *         初始化后实例会自动注册到内部静态表,用于回调分发时查找
 */
typedef struct {
    UART_HandleTypeDef    *pUartHandle;     /**< HAL UART句柄指针(由UartDrv_Init设置) */
    UartDrv_RxData_t      rxData;           /**< 接收数据(中断中填充,回调中读取) */
    UartDrv_RxCallback_t  pRxCb;            /**< 用户注册的接收回调函数(可为NULL) */
    void                  *pUserCtx;        /**< 回调用户上下文(回调时原样传回) */
#ifndef UART_DRV_NO_FREERTOS
    osMessageQueueId_t    rxQueue;          /**< 接收消息队列句柄,为NULL时不使用队列 */
#endif
    uint8_t               initialized;      /**< 初始化标志(1=已初始化,0=未初始化) */
} UartDrv_t;

#ifndef UART_DRV_NO_FREERTOS

/**
 * @brief  队列消息中每帧数据的最大长度,应与UART_DRV_RX_BUF_SIZE一致
 * @note   入队时若帧长度超过此值会截断,因此必须 >= UART_DRV_RX_BUF_SIZE。
 *         若需调整,同时修改 UART_DRV_RX_BUF_SIZE 保持一致。
 */
#define UART_DRV_QUEUE_DATA_SIZE  UART_DRV_RX_BUF_SIZE

/**
 * @brief  通过消息队列传递的串口接收事件(数据副本,非指针)
 * @note   数据在入队时拷贝到data[]中,消费者可安全使用,不怕被下一帧覆盖。
 *         消息大小 = sizeof(UartDrv_QueueEvent_t) ≈ 4+2+256 = 262字节,
 *         队列创建时 msg_size 必须等于此值。
 */
typedef struct {
    UartDrv_t   *pDrv;                          /**< 来源串口驱动实例 */
    uint16_t     len;                           /**< 接收数据长度 */
    uint8_t      data[UART_DRV_QUEUE_DATA_SIZE]; /**< 数据副本 */
} UartDrv_QueueEvent_t;

#endif /* UART_DRV_NO_FREERTOS */

/* ====== 导出函数声明 ====== */

/* ---- 初始化与启停 ---- */

/**
 * @brief  初始化串口驱动实例
 * @param  pDrv        串口驱动实例指针(由用户定义的UartDrv_t变量)
 * @param  pUartHandle HAL UART句柄指针(如&huart1, 由CubeMX生成)
 * @note   初始化会:
 *         1. 清零整个实例结构
 *         2. 绑定UART句柄
 *         3. 设置初始化标志
 *         4. 将实例注册到内部静态表(用于回调分发)
 *         调用前需确保HAL UART已初始化(MX_USARTx_UART_Init已完成)
 */
void UartDrv_Init(UartDrv_t *pDrv, UART_HandleTypeDef *pUartHandle);

/**
 * @brief  启动串口接收(空闲中断方式)
 * @param  pDrv 串口驱动实例指针
 * @note   内部执行流程:
 *         1. 先启动一次普通接收再中止,清除HAL内部可能残留的状态
 *         2. 启动 HAL_UARTEx_ReceiveToIdle_IT 空闲中断接收
 *         此后当串口总线空闲一个字符时间时,触发中断并回调
 *         接收回调结束后驱动会自动重启接收,无需用户手动操作
 */
void UartDrv_StartRecv(UartDrv_t *pDrv);

/**
 * @brief  发送数据(阻塞方式)
 * @param  pDrv  串口驱动实例指针
 * @param  pData 待发送数据指针
 * @param  len   待发送数据长度(字节)
 * @note   内部调用 HAL_UART_Transmit,阻塞直到发送完成或超时(0xFFFF ms)
 *         不建议在中断上下文中调用(可能长时间阻塞)
 */
void UartDrv_Send(UartDrv_t *pDrv, const uint8_t *pData, uint16_t len);

/**
 * @brief  发送字符串(阻塞方式)
 * @param  pDrv 串口驱动实例指针
 * @param  str  以'\0'结尾的字符串指针
 * @note   内部调用 UartDrv_Send,自动计算字符串长度(strlen)
 */
void UartDrv_SendStr(UartDrv_t *pDrv, const char *str);

/**
 * @brief  注册串口接收回调函数
 * @param  pDrv      串口驱动实例指针
 * @param  pCb       回调函数指针(函数原型见 UartDrv_RxCallback_t 定义)
 * @param  pUserCtx  用户上下文指针,回调时原样传回,可传入NULL
 * @note   使用示例:
 *         UartDrv_RegisterRxCb(&g_uart1Drv, MyRxCallback, &myContext);
 *         回调在中断上下文中执行,应尽快返回
 */
void UartDrv_RegisterRxCb(UartDrv_t *pDrv, UartDrv_RxCallback_t pCb, void *pUserCtx);

#ifndef UART_DRV_NO_FREERTOS
/**
 * @brief  注册串口接收消息队列
 * @param  pDrv  串口驱动实例指针
 * @param  queue CMSIS-RTOS2 消息队列句柄
 * @note   注册队列后,每次空闲中断收到一帧数据,除了调用用户回调外,
 *         还会将一个 UartDrv_QueueEvent_t 事件发送到该队列。
 *         事件中的 pData 指向驱动实例内部的接收缓冲区,消费者应尽快处理。
 */
void UartDrv_RegisterRxQueue(UartDrv_t *pDrv, osMessageQueueId_t queue);
#endif

/**
 * @brief  串口接收事件分发函数
 * @param  huart 触发事件的UART句柄(由HAL中断回调传入)
 * @param  Size  接收数据长度(由HAL中断回调传入)
 * @note   此函数由默认的 HAL_UARTEx_RxEventCallback 自动调用,
 *         用户通常不需要手动调用。处理流程:
 *         1. 根据huart句柄在注册表中查找对应的驱动实例
 *         2. 在接收缓冲区末尾添加'\0'结尾(方便字符串处理)
 *         3. 调用用户注册的回调函数
 *         4. 解锁UART句柄并重启接收(为下一帧数据做准备)
 */
void UartDrv_RxEventDispatch(UART_HandleTypeDef *huart, uint16_t Size);

/**
 * @brief  串口接收事件弱钩子函数,用户可重写以追加额外处理
 * @param  huart 触发事件的UART句柄
 * @param  Size  接收数据长度
 * @note   此函数在 UartDrv_RxEventDispatch 之后被调用(默认为空实现)。
 *         用户可在自己的.c文件中重写此函数来追加额外逻辑,例如:
 *         @code
 *         void UartDrv_OnRxEvent(UART_HandleTypeDef *huart, uint16_t Size) {
 *             // 在分发完成后追加的处理,如点亮LED指示灯等
 *         }
 *         @endcode
 *         重写后不需要声明,链接器会自动使用用户的强定义替换此弱定义
 */
void UartDrv_OnRxEvent(UART_HandleTypeDef *huart, uint16_t Size);

/* ---- 调试输出 ---- */

/**
 * @brief  设置printf调试输出使用的串口驱动实例
 * @param  pDrv 串口驱动实例指针,传入NULL则禁用printf输出
 * @note   调用后printf/fprintf等标准输出将重定向到该串口。
 *         同时支持MDK(ARMCC)和GCC(CMake)工具链:
 *         - MDK: 重写 fputc() 函数
 *         - GCC: 重写 _write() 系统调用
 *         使用示例:
 *         UartDrv_SetDebugPort(&g_uart1Drv);  // printf输出到UART1
 *         UartDrv_SetDebugPort(NULL);           // 禁用printf输出
 */
void UartDrv_SetDebugPort(UartDrv_t *pDrv);

/**
 * @brief  通过串口打印浮点数(纯整数运算,不依赖printf %f)
 * @param  pDrv          串口驱动实例指针
 * @param  value         要打印的浮点数
 * @param  decimalPlaces 小数位数(0~6,超过6自动截断为6)
 * @note   本函数使用纯整数运算实现浮点数打印,不依赖printf的%f格式化,
 *         即使在GCC newlib-nano环境下(默认不支持%f)也能正常工作。
 *         打印浮点数的两种方式:
 *         1. printf("%.2f", 3.14f) -- GCC工具链已自动链接 _printf_float;
 *            MDK(ARMCC/AC6)原生支持%f,无需额外配置
 *         2. UartDrv_PrintFloat(&drv, 3.14f, 2) -- 纯整数运算,无额外依赖
 *         如果GCC下不需要printf %f功能,可定义宏 UART_DRV_NO_PRINTF_FLOAT
 *         禁用 _printf_float 引用以减小代码体积,此时仍可使用本函数打印浮点数。
 *         使用示例:
 *         UartDrv_PrintFloat(&g_uart1Drv, 3.14159f, 2);  // 输出 "3.14"
 *         UartDrv_PrintFloat(&g_uart1Drv, -0.5f, 1);       // 输出 "-0.5"
 */
void UartDrv_PrintFloat(UartDrv_t *pDrv, float value, uint8_t decimalPlaces);

#ifdef __cplusplus
}
#endif

#endif /* __UART_DRV_H */
