/**
 * @file    ssd1306_i2c.h
 * @brief   软件模拟I2C接口头文件
 * @details 使用GPIO模拟I2C时序，用于OLED通信
 *          仅在 OLED_I2C_HARD = 0 时编译
 * @author  HDU.ZYU
 * @date    2026
 */

#ifndef _SSD1306_I2C_H
#define _SSD1306_I2C_H

#include "main.h"
#include "ssd1306.h"

#ifndef OLED_I2C_HARD

/* ==================== 类型定义 ==================== */

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

/* ==================== 函数声明 ==================== */

/**
 * @brief   产生I2C起始信号
 * @note    SCL高电平期间，SDA由高变低
 */
void OLED_IIC_Start(void);

/**
 * @brief   产生I2C停止信号
 * @note    SCL高电平期间，SDA由低变高
 */
void OLED_IIC_Stop(void);

/**
 * @brief   产生I2C应答信号(ACK)
 * @note    拉低SDA表示应答
 */
void OLED_IIC_Ack(void);

/**
 * @brief   发送一个字节数据（带 ACK 检测）
 * @param   dat: 要发送的数据
 * @return  0: ACK 正常, 1: NACK (从机无应答)
 * @note    高位先发，发送完成后检测ACK
 */
u8 OLED_IIC_SendByte(u8 dat);

/**
 * @brief   I2C 总线复位
 * @note    发送9个SCL脉冲+STOP，释放被阻塞的总线
 */
void OLED_IIC_ResetBus(void);

/**
 * @brief   接收I2C应答信号
 * @return  0: 收到ACK (设备应答)
 * @return  1: 收到NACK (设备无应答)
 */
u8 OLED_IIC_ReceiveAck(void);

/**
 * @brief   读取一个字节数据
 * @param   ack: 1=发送ACK, 0=发送NACK
 * @return  读取到的字节数据
 * @note    高位先读
 */
u8 OLED_IIC_ReadByte(u8 ack);

/**
 * @brief   产生I2C非应答信号(NACK)
 * @note    拉高SDA表示非应答，用于读取最后一个字节后
 */
void OLED_IIC_NAck(void);

#endif /* OLED_I2C_HARD */

#endif /* _SSD1306_I2C_H */
