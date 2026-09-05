/**
 * @file    led.h
 * @brief   通用 LED 驱动 - 支持多 LED 独立配置和统一控制
 * @details 特性:
 *          - 每个 LED 可独立配置端口/引脚/亮灯电平
 *          - 提供 SetLeds() 统一控制函数, uint8_t 位掩码
 *          - 提供单个 LED 开/关/翻转操作
 *
 * @par 使用方法
 *         1. 定义 Led_Config_t 数组, 配置每个 LED
 *         2. 调用 LED_Init() 初始化
 *         3. 调用 LED_SetLeds(0x7F) 点亮全部 LED
 */

#ifndef __LED_H
#define __LED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/** 最大 LED 数量 */
#define LED_MAX_COUNT   7

/** LED 亮灯电平 */
typedef enum {
    LED_ON_LOW = 0,   /**< 低电平点亮 (灌电流, 共阳极) */
    LED_ON_HIGH = 1,  /**< 高电平点亮 (拉电流, 共阴极) */
} Led_OnLevel_t;

/** LED 配置结构 */
typedef struct {
    GPIO_TypeDef  *port;     /**< GPIO 端口 */
    uint16_t       pin;      /**< GPIO 引脚 */
    Led_OnLevel_t  onLevel;  /**< 亮灯电平 */
} Led_Config_t;

/* ====== 导出函数 ====== */

/**
 * @brief  初始化 LED 驱动
 * @param  configs  LED 配置数组
 * @param  count    LED 数量 (不超过 LED_MAX_COUNT)
 * @note   初始化后所有 LED 默认关闭
 */
void LED_Init(const Led_Config_t *configs, uint8_t count);

/**
 * @brief  统一设置 LED 状态
 * @param  mask  位掩码, bit0 对应 LED0, bit1 对应 LED1, ...
 *                1=亮, 0=灭
 * @note   例如 LED_SetLeds(0x05) 点亮 LED0 和 LED2
 */
void LED_SetLeds(uint8_t mask);

/**
 * @brief  获取当前 LED 状态
 * @retval 位掩码, 同 LED_SetLeds 参数格式
 */
uint8_t LED_GetLeds(void);

/**
 * @brief  点亮单个 LED
 * @param  index LED 索引 (0~count-1)
 */
void LED_On(uint8_t index);

/**
 * @brief  熄灭单个 LED
 * @param  index LED 索引
 */
void LED_Off(uint8_t index);

/**
 * @brief  翻转单个 LED
 * @param  index LED 索引
 */
void LED_Toggle(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* __LED_H */
