/**
 * @file    key.h
 * @brief   通用按键驱动 - 支持短按/长按/连按检测
 * @details 特性:
 *          - 支持多按键独立配置 (端口/引脚/有效电平)
 *          - 计数式消抖: 每隔 SCAN_INTERVAL_MS 检测一次,
 *            状态保持则计数+1, 否则清零, 计数 >= DEBOUNCE_COUNT 才有效
 *          - 短按: 按下并释放 (释放时触发)
 *          - 长按: 按下保持超过 LONG_PRESS_MS 触发
 *          - 连按: 长按后每隔 REPEAT_INTERVAL_MS 触发一次
 *          - 支持单键检测和合并检测 (任意键按下)
 *
 * @par 使用方法
 *         1. 定义 Key_Config_t 数组, 配置每个按键
 *         2. 调用 Key_Init() 初始化
 *         3. 在周期任务 (10ms) 中调用 Key_Scan()
 *         4. 调用 Key_IsPressed()/Key_IsShortPressed()/Key_IsLongPressed() 查询状态
 */

#ifndef __KEY_H
#define __KEY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ====== 编译配置 ====== */

/** 消抖扫描间隔 (ms), 建议 10ms */
#define KEY_SCAN_INTERVAL_MS    10

/** 消抖有效计数 (连续 N 次检测到同一状态才有效) */
#define KEY_DEBOUNCE_COUNT      2

/** 长按判定时长 (ms), 超过此时间视为长按 */
#define KEY_LONG_PRESS_MS       800

/** 长按后连续触发间隔 (ms) */
#define KEY_REPEAT_INTERVAL_MS  100

/** 最大按键数量 */
#define KEY_MAX_COUNT           6

/* ====== 类型定义 ====== */

/** 按键有效电平 */
typedef enum {
    KEY_ACTIVE_LOW = 0,   /**< 上拉输入, 低电平有效 (K1~K4) */
    KEY_ACTIVE_HIGH = 1,  /**< 下拉输入, 高电平有效 (K5~K6) */
} Key_ActiveLevel_t;

/** 按键事件类型 (位掩码, 可同时触发多个) */
typedef enum {
    KEY_EVENT_NONE        = 0x00,  /**< 无事件 */
    KEY_EVENT_PRESSED     = 0x01,  /**< 按下 (消抖后) */
    KEY_EVENT_SHORT_PRESS = 0x02,  /**< 短按 (按下并释放) */
    KEY_EVENT_LONG_PRESS  = 0x04,  /**< 长按 (首次触发) */
    KEY_EVENT_REPEAT      = 0x08,  /**< 长按连发 */
} Key_Event_t;

/** 按键配置结构 */
typedef struct {
    GPIO_TypeDef    *port;         /**< GPIO 端口 */
    uint16_t         pin;          /**< GPIO 引脚 */
    Key_ActiveLevel_t activeLevel; /**< 有效电平 */
} Key_Config_t;

/** 按键内部状态 */
typedef struct {
    uint8_t  debounceCnt;          /**< 消抖计数器 */
    uint8_t  stableState;          /**< 消抖后稳定状态 (0=释放, 1=按下) */
    uint8_t  prevState;            /**< 上一次稳定状态 */
    uint16_t pressDuration;        /**< 按下持续时间 (ms) */
    uint8_t  longPressed;          /**< 是否已触发长按 */
    uint8_t  events;               /**< 待处理事件 (Key_Event_t 位掩码) */
} Key_State_t;

/* ====== 导出函数 ====== */

/**
 * @brief  初始化按键驱动
 * @param  configs  按键配置数组
 * @param  count    按键数量 (不超过 KEY_MAX_COUNT)
 * @note   会清零内部状态, 保存配置指针
 */
void Key_Init(const Key_Config_t *configs, uint8_t count);

/**
 * @brief  按键扫描函数 (需周期调用, 间隔 KEY_SCAN_INTERVAL_MS)
 * @note   在 FreeRTOS 任务中每 10ms 调用一次
 *         内部完成消抖、短按/长按/连按判定
 */
void Key_Scan(void);

/**
 * @brief  查询按键是否处于按下状态
 * @param  index 按键索引 (0~count-1)
 * @retval 1=按下, 0=释放
 */
uint8_t Key_IsPressed(uint8_t index);

/**
 * @brief  查询并清除短按事件
 * @param  index 按键索引
 * @retval 1=短按发生 (调用后自动清除), 0=无
 */
uint8_t Key_IsShortPressed(uint8_t index);

/**
 * @brief  查询并清除长按事件 (首次触发)
 * @param  index 按键索引
 * @retval 1=长按发生 (调用后自动清除), 0=无
 */
uint8_t Key_IsLongPressed(uint8_t index);

/**
 * @brief  查询并清除长按连发事件
 * @param  index 按键索引
 * @retval 1=连发触发 (调用后自动清除), 0=无
 */
uint8_t Key_IsRepeat(uint8_t index);

/**
 * @brief  获取当前所有按键的组合状态
 * @retval 位掩码, bit0 对应按键0, bit1 对应按键1, ...
 *         1=按下, 0=释放
 * @note   例如返回 0x03 表示按键0和按键1同时按下 (组合键)
 *         返回值类型为 uint16_t, 最多支持 16 个按键
 */
uint16_t Key_AnyPressed(void);

/**
 * @brief  清除所有按键的待处理事件
 */
void Key_ClearAllEvents(void);

#ifdef __cplusplus
}
#endif

#endif /* __KEY_H */
