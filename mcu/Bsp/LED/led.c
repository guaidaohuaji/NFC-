/**
 * @file    led.c
 * @brief   通用 LED 驱动实现
 */

#include "led.h"
#include <string.h>

/* ====== 私有变量 ====== */

static Led_Config_t s_configs[LED_MAX_COUNT];
static uint8_t      s_ledCount = 0;
static uint8_t      s_currentMask = 0;  /**< 当前 LED 状态位掩码 */

/* ====== 导出函数实现 ====== */

void LED_Init(const Led_Config_t *configs, uint8_t count)
{
    if (count > LED_MAX_COUNT)
        count = LED_MAX_COUNT;

    s_ledCount = count;
    memcpy(s_configs, configs, count * sizeof(Led_Config_t));
    s_currentMask = 0;

    /* 初始化时全部熄灭 */
    LED_SetLeds(0);
}

void LED_SetLeds(uint8_t mask)
{
    uint8_t i;
    s_currentMask = mask;

    for (i = 0; i < s_ledCount; i++)
    {
        const Led_Config_t *cfg = &s_configs[i];
        GPIO_PinState state;

        if (mask & (1 << i))
        {
            /* 点亮 */
            state = (cfg->onLevel == LED_ON_LOW) ? GPIO_PIN_RESET : GPIO_PIN_SET;
        }
        else
        {
            /* 熄灭 */
            state = (cfg->onLevel == LED_ON_LOW) ? GPIO_PIN_SET : GPIO_PIN_RESET;
        }

        HAL_GPIO_WritePin(cfg->port, cfg->pin, state);
    }
}

uint8_t LED_GetLeds(void)
{
    return s_currentMask;
}

void LED_On(uint8_t index)
{
    if (index >= s_ledCount)
        return;
    s_currentMask |= (1 << index);
    LED_SetLeds(s_currentMask);
}

void LED_Off(uint8_t index)
{
    if (index >= s_ledCount)
        return;
    s_currentMask &= ~(1 << index);
    LED_SetLeds(s_currentMask);
}

void LED_Toggle(uint8_t index)
{
    if (index >= s_ledCount)
        return;
    s_currentMask ^= (1 << index);
    LED_SetLeds(s_currentMask);
}
