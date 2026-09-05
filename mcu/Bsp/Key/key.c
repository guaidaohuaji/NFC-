/**
 * @file    key.c
 * @brief   通用按键驱动实现 - 计数消抖 + 短按/长按/连按检测
 */

#include "key.h"
#include <string.h>

/* ====== 私有变量 ====== */

static Key_Config_t s_configs[KEY_MAX_COUNT];
static Key_State_t  s_states[KEY_MAX_COUNT];
static uint8_t      s_keyCount = 0;

/* ====== 导出函数实现 ====== */

void Key_Init(const Key_Config_t *configs, uint8_t count)
{
    if (count > KEY_MAX_COUNT)
        count = KEY_MAX_COUNT;

    s_keyCount = count;
    memcpy(s_configs, configs, count * sizeof(Key_Config_t));
    memset(s_states, 0, count * sizeof(Key_State_t));
}

void Key_Scan(void)
{
    uint8_t i;

    for (i = 0; i < s_keyCount; i++)
    {
        Key_State_t *st = &s_states[i];
        const Key_Config_t *cfg = &s_configs[i];

        /* 读取原始电平并转换为逻辑状态 (1=按下, 0=释放) */
        GPIO_PinState raw = HAL_GPIO_ReadPin(cfg->port, cfg->pin);
        uint8_t logicState;
        if (cfg->activeLevel == KEY_ACTIVE_LOW)
            logicState = (raw == GPIO_PIN_RESET) ? 1 : 0;
        else
            logicState = (raw == GPIO_PIN_SET) ? 1 : 0;

        /* ---- 计数消抖 ---- */
        if (logicState == st->stableState)
        {
            /* 状态一致, 计数器可保持 (不超出阈值) */
            if (st->debounceCnt < 255)
                st->debounceCnt++;
        }
        else
        {
            /* 状态变化, 开始计数 */
            st->debounceCnt++;
            if (st->debounceCnt >= KEY_DEBOUNCE_COUNT)
            {
                /* 确认状态翻转 */
                st->stableState = logicState;
                st->debounceCnt = 0;
            }
        }

        /* ---- 事件判定 ---- */
        if (st->stableState == 1)
        {
            /* 按下中 */
            if (st->prevState == 0)
            {
                /* 刚按下 (下降沿/上升沿) */
                st->prevState = 1;
                st->pressDuration = 0;
                st->longPressed = 0;
                st->events |= KEY_EVENT_PRESSED;
            }
            else
            {
                /* 持续按下 */
                st->pressDuration += KEY_SCAN_INTERVAL_MS;

                /* 长按检测 */
                if (!st->longPressed && st->pressDuration >= KEY_LONG_PRESS_MS)
                {
                    st->longPressed = 1;
                    st->events |= KEY_EVENT_LONG_PRESS;
                }

                /* 长按连发 */
                if (st->longPressed && st->pressDuration >= KEY_LONG_PRESS_MS + KEY_REPEAT_INTERVAL_MS)
                {
                    st->events |= KEY_EVENT_REPEAT;
                    /* 重置持续时间以连发, 保留长按标志 */
                    st->pressDuration = KEY_LONG_PRESS_MS;
                }
            }
        }
        else
        {
            /* 释放中 */
            if (st->prevState == 1)
            {
                /* 刚释放 (上升沿/下降沿) */
                st->prevState = 0;

                /* 如果未触发长按, 则为短按 */
                if (!st->longPressed)
                {
                    st->events |= KEY_EVENT_SHORT_PRESS;
                }
                st->pressDuration = 0;
                st->longPressed = 0;
            }
        }
    }
}

uint8_t Key_IsPressed(uint8_t index)
{
    if (index >= s_keyCount)
        return 0;
    return s_states[index].stableState;
}

uint8_t Key_IsShortPressed(uint8_t index)
{
    if (index >= s_keyCount)
        return 0;
    if (s_states[index].events & KEY_EVENT_SHORT_PRESS)
    {
        s_states[index].events &= ~KEY_EVENT_SHORT_PRESS;
        return 1;
    }
    return 0;
}

uint8_t Key_IsLongPressed(uint8_t index)
{
    if (index >= s_keyCount)
        return 0;
    if (s_states[index].events & KEY_EVENT_LONG_PRESS)
    {
        s_states[index].events &= ~KEY_EVENT_LONG_PRESS;
        return 1;
    }
    return 0;
}

uint8_t Key_IsRepeat(uint8_t index)
{
    if (index >= s_keyCount)
        return 0;
    if (s_states[index].events & KEY_EVENT_REPEAT)
    {
        s_states[index].events &= ~KEY_EVENT_REPEAT;
        return 1;
    }
    return 0;
}

uint16_t Key_AnyPressed(void)
{
    uint8_t i;
    uint16_t mask = 0;
    for (i = 0; i < s_keyCount; i++)
    {
        if (s_states[i].stableState)
            mask |= (1 << i);
    }
    return mask;
}

void Key_ClearAllEvents(void)
{
    uint8_t i;
    for (i = 0; i < s_keyCount; i++)
        s_states[i].events = KEY_EVENT_NONE;
}
