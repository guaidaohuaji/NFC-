/**
 * @file    ssd1306.h
 * @brief   SSD1306/SH1106 OLED显示驱动头文件
 * @details 支持SSD1306和SH1106两种OLED控制芯片的驱动
 *          - 自动检测芯片类型
 *          - 支持硬件I2C和软件模拟I2C
 *          - 提供基本图形绘制功能
 * @author  HDU.ZYU
 * @date    2026
 */

#ifndef _SSD1306_H
#define _SSD1306_H

#include <stdlib.h>
#include <string.h>
#include "ssd1306_i2c.h"
#include "gui.h"

/* ==================== 配置选项 ==================== */

/**
 * @brief I2C模式选择
 * @param 1: 使用硬件I2C (HAL库)
 * @param 0: 使用软件模拟I2C (GPIO)
 */
#define OLED_I2C_HARD

/* ==================== 命令/数据定义 ==================== */

/** @brief I2C命令控制字节 */
#define OLED_CMD            0X00

/** @brief I2C数据控制字节 */
#define OLED_DATA           0X40

/** @brief OLED设备I2C地址 (7位地址0x3C左移1位) */
#define SSD1306_I2C_ADDR    0x78

/** @brief OLED显示宽度(像素) */
#define SSD1306_WIDTH       128

/** @brief OLED显示高度(像素) */
#define SSD1306_HEIGHT      64

/* ==================== 函数声明 ==================== */

/**
 * @brief   填充整个屏幕
 * @param   color: 填充颜色 (GUI_COLOR_BLACK/GUI_COLOR_WHITE)
 */
void SSD1306_Fill(GUI_COLOR color);

/**
 * @brief   将显存缓冲区内容更新到屏幕
 * @note    调用此函数后，缓冲区的修改才会显示到屏幕上
 */
void SSD1306_UpdateScreen(void);

/**
 * @brief   写入单字节数据或命令
 * @param   dat: 要写入的数据
 * @param   cmd: OLED_CMD(命令) 或 OLED_DATA(数据)
 */
void SSD1306_WriteByte(uint8_t dat, uint8_t cmd);

/**
 * @brief   写入多字节数据
 * @param   data: 数据指针
 * @param   count: 数据长度
 * @param   cmd: OLED_CMD(命令) 或 OLED_DATA(数据)
 */
void SSD1306_WriteMuliByte(uint8_t* data, uint16_t count, uint8_t cmd);

/**
 * @brief   初始化OLED显示屏
 * @note    自动检测芯片类型(SSD1306/SH1106)并初始化
 */
void SSD1306_init(void);

/**
 * @brief   切换显示反色状态
 */
void SSD1306_ToggleInvert(void);

/**
 * @brief   绘制单个像素点
 * @param   x: X坐标 (0 ~ SSD1306_WIDTH-1)
 * @param   y: Y坐标 (0 ~ SSD1306_HEIGHT-1)
 * @param   color: 像素颜色
 */
void SSD1306_DrawPixel(uint16_t x, uint16_t y, GUI_COLOR color);

/**
 * @brief   设置绘图光标位置
 * @param   x: X坐标
 * @param   y: Y坐标
 */
void SSD1306_GotoXY(uint16_t x, uint16_t y);

/**
 * @brief   绘制直线
 * @param   x0: 起点X坐标
 * @param   y0: 起点Y坐标
 * @param   x1: 终点X坐标
 * @param   y1: 终点Y坐标
 * @param   c: 线条颜色
 */
void SSD1306_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, GUI_COLOR c);

/**
 * @brief   绘制矩形边框
 * @param   x: 左上角X坐标
 * @param   y: 左上角Y坐标
 * @param   w: 宽度
 * @param   h: 高度
 * @param   c: 边框颜色
 */
void SSD1306_DrawRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, GUI_COLOR c);

/**
 * @brief   绘制填充矩形
 * @param   x: 左上角X坐标
 * @param   y: 左上角Y坐标
 * @param   w: 宽度
 * @param   h: 高度
 * @param   c: 填充颜色
 */
void SSD1306_DrawFilledRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, GUI_COLOR c);

/**
 * @brief   绘制三角形边框
 * @param   x1, y1: 第一个顶点坐标
 * @param   x2, y2: 第二个顶点坐标
 * @param   x3, y3: 第三个顶点坐标
 * @param   color: 边框颜色
 */
void SSD1306_DrawTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, GUI_COLOR color);

/**
 * @brief   绘制填充三角形
 * @param   x1, y1: 第一个顶点坐标
 * @param   x2, y2: 第二个顶点坐标
 * @param   x3, y3: 第三个顶点坐标
 * @param   color: 填充颜色
 */
void SSD1306_DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, GUI_COLOR color);

/**
 * @brief   绘制圆形边框
 * @param   x0: 圆心X坐标
 * @param   y0: 圆心Y坐标
 * @param   r: 半径
 * @param   c: 边框颜色
 */
void SSD1306_DrawCircle(int16_t x0, int16_t y0, int16_t r, GUI_COLOR c);

/**
 * @brief   绘制填充圆形
 * @param   x0: 圆心X坐标
 * @param   y0: 圆心Y坐标
 * @param   r: 半径
 * @param   c: 填充颜色
 */
void SSD1306_DrawFilledCircle(int16_t x0, int16_t y0, int16_t r, GUI_COLOR c);

/**
 * @brief   开启OLED显示
 */
void SSD1306_ON(void);

/**
 * @brief   关闭OLED显示
 */
void SSD1306_OFF(void);

#endif /* _SSD1306_H */
