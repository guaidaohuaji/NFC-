/**
 * @file    uart_drv.c
 * @brief   通用串口驱动实现 - 包含printf重定向(支持MDK和GCC/CMake)
 *
 * @par 模块职责
 *   1. 管理多个串口驱动实例的注册与查找
 *   2. 提供阻塞式发送和空闲中断式接收
 *   3. 在HAL中断回调中自动分发接收事件到对应实例的回调函数
 *   4. 提供printf重定向功能(调试输出)
 *   5. 提供纯整数浮点数打印(不依赖printf %f)
 *
 * @par 数据流
 *   串口硬件 → 空闲中断 → HAL_UARTEx_RxEventCallback
 *     → UartDrv_RxEventDispatch → FindInstance(查表)
 *     → 用户回调函数 pRxCb → 自动重启接收
 */

#include "uart_drv.h"

#ifndef UART_DRV_NO_FREERTOS
#include "cmsis_os.h"
#endif
#include <string.h>
#include <stdio.h>

/* ====== 私有变量 ====== */

/**
 * @brief  已注册的串口驱动实例表(静态数组)
 * @note   UartDrv_Init() 时将实例指针存入此表,
 *         UartDrv_RxEventDispatch() 时通过遍历此表查找huart对应的实例
 */
static UartDrv_t *s_drvInstances[UART_DRV_MAX_INSTANCES];

/**
 * @brief  已注册实例数量(同时也是下一个可用槽位索引)
 * @note   当 s_drvCount == UART_DRV_MAX_INSTANCES 时,不再接受新实例注册
 */
static uint8_t s_drvCount = 0;

/**
 * @brief  printf调试输出使用的串口驱动实例
 * @note   由 UartDrv_SetDebugPort() 设置, _write()/fputc() 中使用
 *         为NULL时printf输出将被丢弃(不会崩溃)
 */
static UartDrv_t *s_pDebugPort = NULL;

/* ====== 私有函数声明 ====== */
static UartDrv_t *FindInstance(UART_HandleTypeDef *huart);

/* ====== printf重定向 - MDK(ARMCC) / GCC(CMake) 兼容 ======
 *
 * 不同工具链的printf底层机制不同:
 * - GCC (newlib): printf最终调用 _write() 系统调用输出字符
 * - MDK (ARMCC):  printf最终调用 fputc() 逐字符输出
 *
 * 两种方式都是将字符通过HAL_UART_Transmit发送到调试串口,
 * 使用 s_pDebugPort 作为目标串口,未设置时输出被丢弃
 */

#if defined(__GNUC__) && !defined(__clang__)
/* ---- GCC工具链 (CMake / STM32CubeIDE / arm-none-eabi-gcc) ----
 * 注意: 使用 __GNUC__ && !__clang__ 排除 MDK AC6 (armclang)。
 * AC6 虽然也定义 __GNUC__,但它是基于 LLVM/Clang 的编译器,
 * 其标准库不使用 newlib-nano,不需要也不存在 _printf_float 符号。
 * 如果不加 __clang__ 排除,AC6 编译时会因 _printf_float 未定义而报错。 */

/**
 * @brief  GCC标准库_write系统调用,将printf输出重定向到串口
 * @param  fd       文件描述符(未使用,仅用于兼容标准接口)
 * @param  pBuffer  待输出字符缓冲区
 * @param  size     待输出字符数量
 * @retval 实际输出字节数(始终等于size,保持printf语义)
 * @note   GCC下printf最终调用_write,在此拦截并输出到调试串口。
 *         当 s_pDebugPort 为NULL或未初始化时,直接丢弃数据不输出。
 */
int _write(int fd, char *pBuffer, int size)
{
    (void)fd;  /* 抑制未使用参数警告 */
    if (s_pDebugPort != NULL && s_pDebugPort->initialized)
    {
        /* 阻塞式发送,超时0xFFFFms(约65秒) */
        HAL_UART_Transmit(s_pDebugPort->pUartHandle, (uint8_t *)pBuffer, (uint16_t)size, 0xFFFF);
    }
    return size;  /* 始终返回size,避免printf认为写入失败 */
}

/*
 * ====== GCC newlib-nano 浮点printf支持 ======
 *
 * arm-none-eabi-gcc 默认使用 newlib-nano 精简C库, printf不链接浮点格式化代码,
 * 导致 printf("%f", 3.14) 输出为空或乱码.
 *
 * 常规做法是在CMakeLists.txt中添加:
 *   target_link_options(${TARGET} PRIVATE -u _printf_float)
 *
 * 这里通过在C代码中强制引用 _printf_float 符号,让链接器必须从库中解析它,
 * 效果等价于 -u _printf_float, 无需修改任何构建配置文件.
 *
 * __attribute__((used)) 确保编译器不会优化掉这个引用,
 * volatile 确保链接器也无法在GC sections阶段丢弃它.
 *
 * 如果不需要printf的%f功能,可以定义宏 UART_DRV_NO_PRINTF_FLOAT
 * 来禁用此功能(减小代码体积),此时仍可使用 UartDrv_PrintFloat 打印浮点数。
 *
 * 注意: 此代码仅对纯GCC工具链生效(通过 __GNUC__ && !__clang__ 判断)。
 * MDK AC6 (armclang) 虽然也定义 __GNUC__,但它基于 LLVM/Clang,
 * 标准库不使用 newlib-nano,_printf_float 符号不存在,会导致链接错误。
 */
#ifndef UART_DRV_NO_PRINTF_FLOAT
extern void _printf_float(void);
__attribute__((used)) volatile void (*s_printfFloatForceRef)(void) = _printf_float;
#endif /* UART_DRV_NO_PRINTF_FLOAT */

#elif defined(__clang__)
/* ---- MDK AC6 (armclang) 工具链 ----
 * AC6 基于 LLVM/Clang,同时定义 __GNUC__ 和 __clang__,
 * 但其标准库不是 newlib-nano,printf原生支持 %f,无需 _printf_float。
 * fputc 重定向方式与 ARMCC 相同。 */
#ifndef __GNUC__		// 如果不使用ARM CC V6编译器
/* 禁用半主机模式 */
#pragma import(__use_no_semihosting)

/* MDK标准库需要的支持结构 */
struct __FILE { int handle; };
void _sys_exit(int x)
{
    x = x;
}
#endif
FILE __stdout;


/**
 * @brief  MDK AC6标准库fputc重定向,将printf输出重定向到串口
 * @param  ch 待输出字符(ASCII码)
 * @param  f  文件指针(未使用)
 * @retval 输出的字符
 * @note   AC6的printf原生支持%f,无需额外链接 _printf_float。
 */
int fputc(int ch, FILE *f)
{
    (void)f;
    if (s_pDebugPort != NULL && s_pDebugPort->initialized)
    {
        HAL_UART_Transmit(s_pDebugPort->pUartHandle, (uint8_t *)&ch, 1, 0xFFFF);
    }
    return ch;
}

#else
/* ---- MDK ARMCC (传统ARM编译器) 工具链 ---- */

/* 禁用半主机模式: ARM semihosting通过调试器访问主机文件系统,
 * 嵌入式独立运行时必须禁用,否则程序会在BKPT指令处卡死 */
#pragma import(__use_no_semihosting)

/* MDK标准库需要的支持结构: __FILE和__stdout是fputc的依赖 */
struct __FILE { int handle; };
FILE __stdout;

/* 半主机模式的退出函数(空实现,避免链接报错) */
void _sys_exit(int x)
{
    x = x;
}

/**
 * @brief  MDK ARMCC标准库fputc重定向,将printf输出重定向到串口
 * @param  ch 待输出字符(ASCII码)
 * @param  f  文件指针(未使用)
 * @retval 输出的字符
 * @note   ARMCC的printf原生支持%f,无需额外链接。
 *         MDK工程中需勾选 Options -> Target -> Use MicroLIB
 *         或确保标准库包含浮点格式化支持。
 */
int fputc(int ch, FILE *f)
{
    (void)f;  /* 抑制未使用参数警告 */
    if (s_pDebugPort != NULL && s_pDebugPort->initialized)
    {
        HAL_UART_Transmit(s_pDebugPort->pUartHandle, (uint8_t *)&ch, 1, 0xFFFF);
    }
    return ch;
}

#endif /* __GNUC__ / __clang__ / ARMCC 三分支结束 */

/* ====== 导出函数实现 ====== */

/**
 * @brief  初始化串口驱动实例
 * @param  pDrv        用户定义的UartDrv_t实例指针
 * @param  pUartHandle HAL UART句柄(由CubeMX生成,如&huart1)
 * @note   初始化流程:
 *         1. 参数合法性检查
 *         2. 清零实例结构(所有字段归零)
 *         3. 绑定UART句柄到实例
 *         4. 设置初始化标志
 *         5. 将实例注册到内部静态表(供回调分发时查找)
 */
void UartDrv_Init(UartDrv_t *pDrv, UART_HandleTypeDef *pUartHandle)
{
    /* 参数校验: 指针为空则直接返回 */
    if (pDrv == NULL || pUartHandle == NULL)
        return;

    /* 清零结构体,确保所有字段从干净状态开始 */
    memset(pDrv, 0, sizeof(UartDrv_t));

    /* 绑定HAL UART句柄 */
    pDrv->pUartHandle = pUartHandle;

    /* 标记已初始化 */
    pDrv->initialized = 1;

    /* 注册实例到静态表,供RxEventDispatch查找 */
    if (s_drvCount < UART_DRV_MAX_INSTANCES)
    {
        s_drvInstances[s_drvCount++] = pDrv;
    }
}

/**
 * @brief  启动串口接收(空闲中断方式)
 * @param  pDrv 串口驱动实例指针
 * @note   两步启动流程的原因:
 *         1. 先调用 HAL_UART_Receive_IT 启动普通接收,再调用 HAL_UART_AbortReceive_IT 中止
 *            这一步的目的是清除HAL内部可能残留的接收状态(如上次异常中断后遗留的),
 *            确保后续的 ReceiveToIdle_IT 能正常工作
 *         2. 再调用 HAL_UARTEx_ReceiveToIdle_IT 启动空闲中断接收
 *            当串口总线空闲一个字符时间后,触发中断,一次性返回整帧数据
 *
 *         接收回调结束后 UartDrv_RxEventDispatch 会自动重启接收,
 *         用户无需手动循环调用此函数(除非要重新启动接收)
 */
void UartDrv_StartRecv(UartDrv_t *pDrv)
{
    if (pDrv == NULL || !pDrv->initialized)
        return;

    /* 第1步: 先启动一次普通接收再中止,清除之前可能残留的状态 */
    HAL_UART_Receive_IT(pDrv->pUartHandle, pDrv->rxData.rx_buf, UART_DRV_RX_BUF_SIZE - 1);
    HAL_UART_AbortReceive_IT(pDrv->pUartHandle);

    /* 第2步: 启动空闲中断接收(真正的工作模式) */
    HAL_UARTEx_ReceiveToIdle_IT(pDrv->pUartHandle, pDrv->rxData.rx_buf, UART_DRV_RX_BUF_SIZE - 1);
}

/**
 * @brief  发送数据(阻塞方式)
 * @param  pDrv  串口驱动实例指针
 * @param  pData 待发送数据指针
 * @param  len   待发送数据长度(字节)
 * @note   内部调用 HAL_UART_Transmit,阻塞直到发送完成或超时。
 *         超时值为0xFFFF(约65秒),一般不会超时。
 *         不建议在中断上下文中调用(可能长时间阻塞)。
 */
void UartDrv_Send(UartDrv_t *pDrv, const uint8_t *pData, uint16_t len)
{
    /* 参数校验 */
    if (pDrv == NULL || !pDrv->initialized || pData == NULL || len == 0)
        return;

    HAL_UART_Transmit(pDrv->pUartHandle, (uint8_t *)pData, len, 0xFFFF);
}

/**
 * @brief  发送字符串(阻塞方式)
 * @param  pDrv 串口驱动实例指针
 * @param  str  以'\0'结尾的字符串
 * @note   自动计算字符串长度(strlen),内部调用UartDrv_Send
 */
void UartDrv_SendStr(UartDrv_t *pDrv, const char *str)
{
    if (pDrv == NULL || !pDrv->initialized || str == NULL)
        return;

    UartDrv_Send(pDrv, (const uint8_t *)str, (uint16_t)strlen(str));
}

/**
 * @brief  注册串口接收回调函数
 * @param  pDrv      串口驱动实例指针
 * @param  pCb       回调函数指针(函数原型见 UartDrv_RxCallback_t)
 * @param  pUserCtx  用户上下文指针,回调时原样传回,可传NULL
 */
void UartDrv_RegisterRxCb(UartDrv_t *pDrv, UartDrv_RxCallback_t pCb, void *pUserCtx)
{
    if (pDrv == NULL || !pDrv->initialized)
        return;

    pDrv->pRxCb = pCb;
    pDrv->pUserCtx = pUserCtx;
}

#ifndef UART_DRV_NO_FREERTOS
/**
 * @brief  注册串口接收消息队列
 * @param  pDrv  串口驱动实例指针
 * @param  queue CMSIS-RTOS2 消息队列句柄
 * @note   注册后,每次空闲中断收到数据都会向该队列发送一个 UartDrv_QueueEvent_t。
 */
void UartDrv_RegisterRxQueue(UartDrv_t *pDrv, osMessageQueueId_t queue)
{
    if (pDrv == NULL || !pDrv->initialized)
        return;

    pDrv->rxQueue = queue;
}
#endif

/**
 * @brief  串口接收事件分发函数
 * @param  huart 触发事件的UART句柄(由HAL中断回调传入)
 * @param  Size  接收数据长度(由HAL中断回调传入)
 * @note   处理流程:
 *         1. 根据huart句柄查找对应的驱动实例(FindInstance查表)
 *         2. 在缓冲区末尾添加'\0'结尾(方便用户作为字符串处理)
 *         3. 如果用户注册了回调,则调用回调函数传递数据
 *         4. 解锁UART句柄(防止HAL锁死)并重启接收(为下一帧做准备)
 *
 *         此函数由默认的 HAL_UARTEx_RxEventCallback 自动调用,
 *         用户通常不需要手动调用。
 */
void UartDrv_RxEventDispatch(UART_HandleTypeDef *huart, uint16_t Size)
{
    /* 根据UART句柄查找对应的驱动实例 */
    UartDrv_t *pDrv = FindInstance(huart);
    if (pDrv == NULL || !pDrv->initialized)
        return;

    if (Size > 0)
    {
        /* 在接收数据末尾添加字符串结束符,方便用户直接用strstr/strcmp等处理 */
        pDrv->rxData.rx_buf[Size] = '\0';
        pDrv->rxData.rx_len = Size;

        /* 调用用户注册的回调函数 */
#ifndef UART_DRV_NO_FREERTOS
        /* 如果注册了接收队列,将事件(含数据副本)发送到队列(中断中不等待) */
        if (pDrv->rxQueue != NULL)
        {
            UartDrv_QueueEvent_t evt;
            evt.pDrv = pDrv;
            evt.len  = (Size > UART_DRV_QUEUE_DATA_SIZE) ? UART_DRV_QUEUE_DATA_SIZE : Size;
            memcpy(evt.data, pDrv->rxData.rx_buf, evt.len);
            osMessageQueuePut(pDrv->rxQueue, &evt, 0, 0);
        }
#endif

        if (pDrv->pRxCb)
        {
            pDrv->pRxCb(&pDrv->rxData, pDrv->pUserCtx);
        }
    }

    /* 解锁UART句柄: HAL_UARTEx_ReceiveToIdle_IT完成接收后会锁住句柄,
     * 不解锁则无法重新启动接收。这是HAL库的一个特性,
     * 直接操作 __HAL_UNLOCK 宏来绕过HAL的状态检查 */
    __HAL_UNLOCK(huart);

    /* 重启接收: 为下一帧数据做准备,形成自动接收循环 */
    HAL_UARTEx_ReceiveToIdle_IT(pDrv->pUartHandle, pDrv->rxData.rx_buf, UART_DRV_RX_BUF_SIZE - 1);
}

/* ====== 私有函数实现 ====== */

/**
 * @brief  根据UART句柄查找对应的驱动实例
 * @param  huart UART句柄指针(由HAL中断回调传入)
 * @retval 驱动实例指针,未找到返回NULL
 * @note   遍历静态注册表 s_drvInstances,比较每个实例的 pUartHandle 与传入的 huart
 *         由于每个串口外设的句柄地址是唯一的,可以用指针比较来匹配
 */
static UartDrv_t *FindInstance(UART_HandleTypeDef *huart)
{
    for (uint8_t i = 0; i < s_drvCount; i++)
    {
        if (s_drvInstances[i] != NULL && s_drvInstances[i]->pUartHandle == huart)
        {
            return s_drvInstances[i];
        }
    }
    return NULL;  /* 未找到匹配的实例,可能是未注册的串口触发了中断 */
}

/**
 * @brief  设置printf调试输出使用的串口驱动实例
 * @param  pDrv 串口驱动实例指针,传入NULL则禁用printf输出
 * @note   设置后,所有printf/fputs等标准输出将重定向到该串口
 */
void UartDrv_SetDebugPort(UartDrv_t *pDrv)
{
    s_pDebugPort = pDrv;
}

/**
 * @brief  通过串口打印浮点数(纯整数运算实现,不依赖printf %f)
 * @param  pDrv          串口驱动实例指针
 * @param  value         要打印的浮点数(支持负数)
 * @param  decimalPlaces 小数位数(0~6,超过6自动截断为6)
 * @note   实现原理:
 *         1. 处理负数符号
 *         2. 加上舍入偏移(如2位小数加0.005)实现四舍五入
 *         3. 分离整数部分和小数部分
 *         4. 分别格式化输出整数和小数部分
 *         这是不依赖 newlib-nano _printf_float 的备选方案,
 *         即使GCC工具链未启用浮点printf也能正常打印浮点数.
 *         使用示例: UartDrv_PrintFloat(&g_uart1Drv, 3.14159f, 2) 输出 "3.14"
 */
void UartDrv_PrintFloat(UartDrv_t *pDrv, float value, uint8_t decimalPlaces)
{
    if (pDrv == NULL || !pDrv->initialized)
        return;

    /* 限制小数位数上限为6位(float有效精度约6~7位) */
    if (decimalPlaces > 6)
        decimalPlaces = 6;

    char buf[16];

    /* 第1步: 处理负数 - 先输出负号,再取绝对值 */
    if (value < 0.0f)
    {
        UartDrv_SendStr(pDrv, "-");
        value = -value;
    }

    /* 第2步: 计算舍入偏移,实现四舍五入
     * 例如2位小数: rounding = 0.5/10/10 = 0.005
     * 加上0.005后3.144变为3.149,截断小数得到3.14
     * 而加上0.005后3.145变为3.150,截断小数得到3.15 */
    float rounding = 0.5f;
    for (uint8_t i = 0; i < decimalPlaces; i++)
        rounding /= 10.0f;
    value += rounding;

    /* 第3步: 分离整数部分和小数部分
     * 例如3.149: intPart=3, fracPart=0.149 */
    uint32_t intPart = (uint32_t)value;
    float fracPart = value - (float)intPart;

    /* 第4步: 将小数部分转为整数
     * 例如2位小数: fracPart*10*10 = 14.9, fracInt=14 */
    for (uint8_t i = 0; i < decimalPlaces; i++)
        fracPart *= 10.0f;
    uint32_t fracInt = (uint32_t)fracPart;

    /* 第5步: 输出整数部分 */
    sprintf(buf, "%lu", (unsigned long)intPart);
    UartDrv_SendStr(pDrv, buf);

    /* 第6步: 输出小数部分(如有) */
    if (decimalPlaces > 0)
    {
        UartDrv_SendStr(pDrv, ".");
        /* 前导零补齐: 如 3.05 的小数部分应为 "05" 而非 "5"
         * %0*lu 中的 * 由 decimalPlaces 指定宽度,0表示补零 */
        sprintf(buf, "%0*lu", decimalPlaces, (unsigned long)fracInt);
        UartDrv_SendStr(pDrv, buf);
    }
}

/* ====== 默认串口接收回调 + 弱钩子 ======
 *
 * 默认回调 HAL_UARTEx_RxEventCallback 覆盖了HAL库中的__weak空实现,
 * 自动完成: 事件分发 → 弱钩子调用
 *
 * 用户自定义回调行为的两种方式:
 * 方式1(轻度): 重写 UartDrv_OnRxEvent() 弱钩子,在分发完成后追加处理
 * 方式2(完全): 定义 UART_DRV_NO_DEFAULT_CALLBACK 宏排除默认实现,
 *             然后在自己的.c文件中重写 HAL_UARTEx_RxEventCallback()
 */

/**
 * @brief  弱钩子函数,用户可在自己的.c文件中重写以追加处理逻辑
 * @param  huart 触发事件的UART句柄
 * @param  Size  接收数据长度
 * @note   默认空实现。用户重写示例:
 *         @code
 *         void UartDrv_OnRxEvent(UART_HandleTypeDef *huart, uint16_t Size) {
 *             // 追加的处理,如点亮LED、触发事件标志等
 *         }
 *         @endcode
 *         链接器会自动用用户的强定义替换此弱定义
 */
__WEAK void UartDrv_OnRxEvent(UART_HandleTypeDef *huart, uint16_t Size)
{
    (void)huart;  /* 抑制未使用参数警告 */
    (void)Size;
}

#ifndef UART_DRV_NO_DEFAULT_CALLBACK

/**
 * @brief  HAL串口接收事件回调函数的默认实现
 * @param  huart 触发事件的UART句柄(由HAL中断处理自动传入)
 * @param  Size  接收数据长度(由HAL中断处理自动传入)
 * @note   此函数为强定义,覆盖HAL库中的__weak空实现。
 *         处理流程:
 *         1. 调用 UartDrv_RxEventDispatch() 进行事件分发
 *            (查找实例→填充数据→调用用户回调→重启接收)
 *         2. 调用 UartDrv_OnRxEvent() 弱钩子供用户追加处理
 *
 *         如果用户需要完全自定义此回调(如多协议解析等),
 *         可在编译选项中定义 UART_DRV_NO_DEFAULT_CALLBACK 宏来排除此默认实现,
 *         然后在自己的.c文件中重写此函数。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    /* 第1步: 分发接收事件到对应实例的回调函数 */
    UartDrv_RxEventDispatch(huart, Size);
    /* 第2步: 调用弱钩子,允许用户追加额外处理 */
    UartDrv_OnRxEvent(huart, Size);
}

#endif /* UART_DRV_NO_DEFAULT_CALLBACK */
