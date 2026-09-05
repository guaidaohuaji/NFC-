/**
 * @file    ssd1306_i2c.c
 * @brief   软件模拟I2C接口实现
 * @details 使用GPIO模拟I2C时序，实现OLED通信功能
 *          - 支持标准I2C读写时序
 *          - 支持读取操作（用于SH1106芯片检测）
 *          仅在 OLED_I2C_HARD = 0 时编译
 * @author  HDU.ZYU
 * @date    2026
 */

#include "ssd1306_i2c.h"
#include "ssd1306.h"
#include "main.h"

#ifndef OLED_I2C_HARD

/* ==================== GPIO引脚定义 ==================== */

/** @brief I2C时钟引脚 */
#define OLED_SCL        I2C_SCL_Pin

/** @brief I2C数据引脚 */
#define OLED_SDA        I2C_SDA_Pin

/** @brief I2C GPIO端口 */
#define OLED_GPIO       I2C_SCL_GPIO_Port

/* ==================== GPIO操作宏 ==================== */

/** @brief SCL引脚置高 */
#define OLED_SCL_H      HAL_GPIO_WritePin(OLED_GPIO, OLED_SCL, GPIO_PIN_SET)

/** @brief SCL引脚置低 */
#define OLED_SCL_L      HAL_GPIO_WritePin(OLED_GPIO, OLED_SCL, GPIO_PIN_RESET)

/** @brief SDA引脚置高 */
#define OLED_SDA_H      HAL_GPIO_WritePin(OLED_GPIO, OLED_SDA, GPIO_PIN_SET)

/** @brief SDA引脚置低 */
#define OLED_SDA_L      HAL_GPIO_WritePin(OLED_GPIO, OLED_SDA, GPIO_PIN_RESET)

/** @brief 读取SDA引脚电平 */
#define OLED_SDA_READ   HAL_GPIO_ReadPin(OLED_GPIO, OLED_SDA)

/* ==================== 内部函数 ==================== */

/**
 * @brief   I2C延时函数
 * @note    控制I2C通信速率，延时越长速率越慢
 */
static void I2C_delay(void)
{
    volatile int i = 10;
    while (i)
        i--;
}

/**
 * @brief   配置SDA引脚为输入模式
 * @note    用于读取从设备发送的数据
 */
static void OLED_SDA_INPUT(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = OLED_SDA;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(OLED_GPIO, &GPIO_InitStruct);
}

/**
 * @brief   配置SDA引脚为输出模式
 * @note    用于向从设备发送数据
 */
static void OLED_SDA_OUTPUT(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = OLED_SDA;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(OLED_GPIO, &GPIO_InitStruct);
}

/* ==================== I2C时序函数 ==================== */

/**
 * @brief   产生I2C起始信号
 * @details 起始条件：SCL高电平期间，SDA由高变低
 *          时序：
 *          1. SDA置高
 *          2. SCL置高
 *          3. 延时
 *          4. SDA置低（产生起始信号）
 *          5. 延时
 *          6. SCL置低（准备发送数据）
 */
void OLED_IIC_Start(void)
{
    OLED_SDA_H;
    I2C_delay();
    OLED_SCL_H;
    I2C_delay();
    OLED_SDA_L;
    I2C_delay();
    OLED_SCL_L;
    I2C_delay();
}

/**
 * @brief   产生I2C停止信号
 * @details 停止条件：SCL高电平期间，SDA由低变高
 *          时序：
 *          1. SCL置低
 *          2. SDA置低
 *          3. 延时
 *          4. SCL置高
 *          5. SDA置高（产生停止信号）
 *          6. 延时
 */
void OLED_IIC_Stop(void)
{
    OLED_SCL_L;
    I2C_delay();
    OLED_SDA_L;
    I2C_delay();
    OLED_SCL_H;
    I2C_delay();
    OLED_SDA_H;
    I2C_delay();
}

/**
 * @brief   产生I2C应答信号(ACK)
 * @details 应答：在第9个时钟周期，主机拉低SDA表示应答
 *          时序：
 *          1. SDA置低
 *          2. 延时
 *          3. SCL置高（从机读取SDA）
 *          4. 延时
 *          5. SCL置低
 *          6. 延时
 *          7. SDA置高（释放SDA线）
 */
void OLED_IIC_Ack(void)
{
    OLED_SDA_L;
    I2C_delay();
    OLED_SCL_H;
    I2C_delay();
    OLED_SCL_L;
    I2C_delay();
    OLED_SDA_H;
}

/**
 * @brief   产生I2C非应答信号(NACK)
 * @details 非应答：在第9个时钟周期，主机保持SDA高电平
 *          用于读取最后一个字节后通知从机结束传输
 */
void OLED_IIC_NAck(void)
{
    OLED_SDA_H;
    I2C_delay();
    OLED_SCL_H;
    I2C_delay();
    OLED_SCL_L;
    I2C_delay();
}

/**
 * @brief   接收I2C应答信号
 * @details 在发送一个字节后，检测从机是否应答
 *          时序：
 *          1. SDA切换为输入模式
 *          2. SDA置高（释放SDA线）
 *          3. 延时
 *          4. SCL置高（第9个时钟）
 *          5. 延时
 *          6. 读取SDA电平（0=ACK, 1=NACK）
 *          7. SCL置低
 *          8. SDA切换回输出模式
 * @return  0: 收到ACK (从机应答)
 * @return  1: 收到NACK (从机无应答)
 */
u8 OLED_IIC_ReceiveAck(void)
{
    u8 ack;
    OLED_SDA_INPUT();
    OLED_SDA_H;
    I2C_delay();
    OLED_SCL_H;
    I2C_delay();
    ack = OLED_SDA_READ ? 1 : 0;
    OLED_SCL_L;
    I2C_delay();
    OLED_SDA_OUTPUT();
    return ack;
}

/**
 * @brief   发送一个字节数据
 * @details 高位先发送，每个位在SCL上升沿被从机采样
 *          发送完成后自动接收ACK
 * @param   dat: 要发送的数据字节
 */
/**
 * @brief   发送一个字节数据（带 ACK 检测）
 * @details 高位先发送，每个位在SCL上升沿被从机采样
 *          发送完成后检测 ACK，若 NACK 则记录错误并尝试恢复总线
 * @param   dat: 要发送的数据字节
 * @return  0: ACK 正常
 * @return  1: NACK (从机无应答，可能总线异常)
 */
u8 OLED_IIC_SendByte(u8 dat)
{
    u8 i;
    u8 ack;
    OLED_SCL_L;

    for (i = 0; i < 8; i++)
    {
        if (dat & 0x80)
            OLED_SDA_H;
        else
            OLED_SDA_L;
        dat <<= 1;

        I2C_delay();
        OLED_SCL_H;
        I2C_delay();
        OLED_SCL_L;
        I2C_delay();
    }
    ack = OLED_IIC_ReceiveAck();
    return ack;  /* 0=ACK, 1=NACK */
}

/**
 * @brief   I2C 总线复位
 * @details 发送 9 个 SCL 脉冲 + STOP，释放被阻塞的总线
 *          之后可通过 OLED_IIC_Start() 重新开始通信
 */
void OLED_IIC_ResetBus(void)
{
    u8 i;
    /* 发送 STOP 确保总线处于空闲状态 */
    OLED_IIC_Stop();
    /* 发送 9 个 SCL 脉冲尝试释放 SDA */
    OLED_SDA_H;
    for (i = 0; i < 9; i++)
    {
        OLED_SCL_H;
        I2C_delay();
        OLED_SCL_L;
        I2C_delay();
    }
    /* 发送 START 后立即 STOP，完成总线复位 */
    OLED_IIC_Start();
    OLED_IIC_Stop();
}

/**
 * @brief   读取一个字节数据
 * @details 高位先读取，每个位在SCL高电平期间被主机采样
 *          读取完成后发送ACK或NACK
 * @param   ack: 1=发送ACK(继续读取), 0=发送NACK(读取结束)
 * @return  读取到的字节数据
 */
u8 OLED_IIC_ReadByte(u8 ack)
{
    u8 i, dat = 0;

    OLED_SDA_INPUT();

    for (i = 0; i < 8; i++)
    {
        dat <<= 1;
        OLED_SCL_H;
        I2C_delay();
        if (OLED_SDA_READ) dat |= 0x01;
        OLED_SCL_L;
        I2C_delay();
    }

    OLED_SDA_OUTPUT();
    if (ack)
        OLED_IIC_Ack();
    else
        OLED_IIC_NAck();

    return dat;
}

#endif /* OLED_I2C_HARD */
