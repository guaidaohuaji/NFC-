/**
 * @file    ssd1306.c
 * @brief   SSD1306/SH1106 OLED显示驱动实现
 * @details 支持SSD1306和SH1106两种OLED控制芯片
 *          - 自动检测芯片类型（通过I2C读取验证）
 *          - 支持硬件I2C和软件模拟I2C
 *          - 提供完整的图形绘制API
 * @author  HDU.ZYU
 * @date    2026
 */

#include "ssd1306.h"
#include "cmsis_os.h"

#ifdef OLED_I2C_HARD
#include "i2c.h"
#endif

/* ==================== 显示方向配置 ==================== */

/** @brief X轴翻转配置 (0=正常, 1=翻转) */
#define OLED_FLIP_X    1

/** @brief Y轴翻转配置 (0=正常, 1=翻转) */
#define OLED_FLIP_Y    1

/* ==================== 宏定义 ==================== */

/** @brief 取绝对值 */
#define ABS(x)   ((x) > 0 ? (x) : -(x))

/* ==================== 私有变量 ==================== */

/** @brief 显存缓冲区 (128x64像素, 8页, 每页128字节) */
static volatile uint8_t SSD1306_Buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];

/** @brief 透明绘制标志 (1=透明模式, 背景色不绘制) */
static unsigned char SSD_Trans = 0;

/* ==================== 芯片类型定义 ==================== */

/**
 * @brief OLED芯片类型枚举
 */
typedef enum {
    CHIP_UNKNOWN = 0,   /**< 未识别 */
    CHIP_SSD1306  = 1,  /**< SSD1306芯片 */
    CHIP_SH1106   = 2   /**< SH1106芯片 */
} OLED_ChipTypeDef;

/** @brief 当前识别到的芯片类型 */
OLED_ChipTypeDef OLED_ChipType = CHIP_UNKNOWN;

/* ==================== 私有结构体 ==================== */

/**
 * @brief SSD1306状态结构体
 */
typedef struct {
    uint16_t CurrentX;      /**< 当前X坐标 */
    uint16_t CurrentY;      /**< 当前Y坐标 */
    uint8_t Inverted;       /**< 反色显示标志 */
    uint8_t Initialized;    /**< 初始化完成标志 */
} SSD1306_t;

/** @brief SSD1306实例 */
static SSD1306_t SSD1306;

/* ==================== 底层I/O函数 ==================== */

#ifdef OLED_I2C_HARD
/**
 * @brief   写入命令（硬件I2C版本）
 * @param   cmd: 命令字节
 */
static void OLED_WriteCmd(uint8_t cmd)
{
    HAL_I2C_Mem_Write(&hi2c1, SSD1306_I2C_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT, &cmd, 1, 100);
}

/**
 * @brief   写入数据（硬件I2C版本）
 * @param   dat: 数据字节
 */
static void OLED_WriteData(uint8_t dat)
{
    HAL_I2C_Mem_Write(&hi2c1, SSD1306_I2C_ADDR, 0x40, I2C_MEMADD_SIZE_8BIT, &dat, 1, 100);
}

/**
 * @brief   读取数据（硬件I2C版本）
 * @details SH1106需要先发送控制字节0x40，再读取数据
 * @param   out_data: 读取到的数据指针
 * @return  HAL状态
 */
static HAL_StatusTypeDef OLED_ReadData(uint8_t *out_data)
{
    return HAL_I2C_Mem_Read(&hi2c1, SSD1306_I2C_ADDR, 0x40, I2C_MEMADD_SIZE_8BIT, out_data, 1, 100);
}
#else
/**
 * @brief   写入命令（软件I2C版本）
 * @param   cmd: 命令字节
 */
static void OLED_WriteCmd(uint8_t cmd)
{
    u8 ack;

    OLED_IIC_Start();
    ack  = OLED_IIC_SendByte(SSD1306_I2C_ADDR);
    ack |= OLED_IIC_SendByte(0x00);
    ack |= OLED_IIC_SendByte(cmd);
    OLED_IIC_Stop();

    if (ack)
    {
        OLED_IIC_ResetBus();
    }
}

/**
 * @brief   写入数据（软件I2C版本）
 * @param   dat: 数据字节
 */
static void OLED_WriteData(uint8_t dat)
{
    u8 ack;

    OLED_IIC_Start();
    ack  = OLED_IIC_SendByte(SSD1306_I2C_ADDR);
    ack |= OLED_IIC_SendByte(0x40);
    ack |= OLED_IIC_SendByte(dat);
    OLED_IIC_Stop();

    if (ack)
    {
        OLED_IIC_ResetBus();
    }
}

/**
 * @brief   读取数据（软件I2C版本）
 * @details I2C读取时序：
 *          START → 设备地址(写) → 控制字节(0x40) → 
 *          RESTART → 设备地址(读) → 读取数据 → STOP
 * @return  读取到的数据字节
 */
static uint8_t OLED_ReadData(void)
{
    uint8_t dat;

    OLED_IIC_Start();
    OLED_IIC_SendByte(SSD1306_I2C_ADDR);
    OLED_IIC_SendByte(0x40);
    OLED_IIC_Start();
    OLED_IIC_SendByte(SSD1306_I2C_ADDR | 0x01);
    dat = OLED_IIC_ReadByte(0);
    OLED_IIC_Stop();

    return dat;
}
#endif

/* ==================== 芯片检测函数 ==================== */

/**
 * @brief   检测OLED芯片类型
 * @details 检测原理：
 *          - SH1106支持I2C读取显存数据
 *          - SSD1306不支持I2C读取显存
 *          
 *          检测方法：
 *          1. 向显存写入测试数据(0xA5, 0x5A)
 *          2. 尝试读取显存数据
 *          3. SH1106能读回写入的数据（第一个字节是dummy byte）
 *          4. SSD1306无法读取，返回不匹配的数据
 *          
 * @return  1: SH1106芯片
 * @return  0: SSD1306芯片
 */
uint8_t OLED_DetectChip(void)
{
    uint8_t dummy, read_val1, read_val2;
    uint8_t test1 = 0xA5;
    uint8_t test2 = 0x5A;

    osDelay(20);

    OLED_WriteCmd(0xAE);
    osDelay(10);

    OLED_WriteCmd(0xB0);
    OLED_WriteCmd(0x00);
    OLED_WriteCmd(0x10);

    OLED_WriteData(test1);
    osDelay(2);
    OLED_WriteData(test2);
    osDelay(5);

    OLED_WriteCmd(0xB0);
    OLED_WriteCmd(0x00);
    OLED_WriteCmd(0x10);
    osDelay(2);

#ifdef OLED_I2C_HARD
    if (OLED_ReadData(&dummy) != HAL_OK) {
        return 0;
    }
    osDelay(2);

    if (OLED_ReadData(&read_val1) != HAL_OK) {
        return 0;
    }
    osDelay(2);

    if (OLED_ReadData(&read_val2) != HAL_OK) {
        return 0;
    }
#else
    dummy = OLED_ReadData();
    osDelay(2);
    read_val1 = OLED_ReadData();
    osDelay(2);
    read_val2 = OLED_ReadData();
#endif

    if (read_val1 == test1 && read_val2 == test2) {
        return 1;
    }

    return 0;
}

/**
 * @brief   OLED上电自检
 * @details 自动检测连接的OLED芯片类型并保存结果
 */
void OLED_SelfDetect(void)
{
    osDelay(50);

    if (OLED_DetectChip()) {
        OLED_ChipType = CHIP_SH1106;
    } else {
        OLED_ChipType = CHIP_SSD1306;
    }
}

/* ==================== 基本操作函数 ==================== */

/**
 * @brief   设置透明绘制模式
 * @param   btran: 1=透明模式, 0=正常模式
 * @note    透明模式下，背景色(COLOR_B)不会被绘制
 */
void SetSSDTrans(unsigned char btran)
{
    SSD_Trans = btran ? 1 : 0;
}

/**
 * @brief   写入单字节数据或命令
 * @param   dat: 数据字节
 * @param   cmd: OLED_CMD(命令) 或 OLED_DATA(数据)
 */
void SSD1306_WriteByte(uint8_t dat, uint8_t cmd)
{
#ifndef OLED_I2C_HARD
    u8 ack;

    OLED_IIC_Start();
    ack  = OLED_IIC_SendByte(SSD1306_I2C_ADDR);
    ack |= OLED_IIC_SendByte(cmd);
    ack |= OLED_IIC_SendByte(dat);
    OLED_IIC_Stop();

    /* ACK 失败时复位 I2C 总线，防止后续通信永久卡死 */
    if (ack)
    {
        OLED_IIC_ResetBus();
    }
#else
    /* 硬件 I2C: 100ms 超时已合理，HAL_I2C_Mem_Write 返回 HAL_BUSY/ERROR 时 HAL 内部会处理 */
    HAL_I2C_Mem_Write(&hi2c1, SSD1306_I2C_ADDR, cmd, I2C_MEMADD_SIZE_8BIT, &dat, 1, 100);
#endif
}

/**
 * @brief   写入多字节数据
 * @param   data: 数据指针
 * @param   count: 数据长度
 * @param   cmd: OLED_CMD(命令) 或 OLED_DATA(数据)
 */
void SSD1306_WriteMuliByte(uint8_t* data, uint16_t count, uint8_t cmd)
{
#ifndef OLED_I2C_HARD
    uint16_t i;
    u8 ack = 0;

    OLED_IIC_Start();
    ack  = OLED_IIC_SendByte(SSD1306_I2C_ADDR);
    ack |= OLED_IIC_SendByte(cmd);

    if (ack == 0)
    {
        for (i = 0; i < count; i++)
        {
            ack = OLED_IIC_SendByte(data[i]);
            if (ack)
            {
                /* 检测到 NACK，停止继续发送 */
                break;
            }
        }
    }

    OLED_IIC_Stop();

    /* ACK 失败时复位 I2C 总线 */
    if (ack)
    {
        OLED_IIC_ResetBus();
    }
#else
    HAL_I2C_Mem_Write(&hi2c1, SSD1306_I2C_ADDR, cmd, I2C_MEMADD_SIZE_8BIT, data, count, 100);
#endif
}

/**
 * @brief   初始化OLED显示屏
 * @details 自动检测芯片类型并完成初始化配置
 *          - 支持SSD1306和SH1106两种芯片
 *          - 配置显示参数、电荷泵、对比度等
 */
void SSD1306_init(void)
{
    int tick = HAL_GetTick();
    if (tick < 100)
        osDelay(100 - tick);

    uint8_t seg_com[2];

    osDelay(100);

    seg_com[0] = OLED_FLIP_X ? 0xA0 : 0xA1;
    seg_com[1] = OLED_FLIP_Y ? 0xC0 : 0xC8;

    OLED_SelfDetect();

    SSD1306_WriteByte(0xAE, OLED_CMD);
    osDelay(10);

    SSD1306_WriteByte(0xD5, OLED_CMD);
    SSD1306_WriteByte(0xF0, OLED_CMD);

    SSD1306_WriteByte(0xA8, OLED_CMD);
    SSD1306_WriteByte(0x3F, OLED_CMD);

    SSD1306_WriteByte(0xD3, OLED_CMD);
    SSD1306_WriteByte(0x00, OLED_CMD);

    SSD1306_WriteByte(0x40, OLED_CMD);

    SSD1306_WriteByte(seg_com[0], OLED_CMD);
    SSD1306_WriteByte(seg_com[1], OLED_CMD);

    SSD1306_WriteByte(0x81, OLED_CMD);
    SSD1306_WriteByte(200, OLED_CMD);

    SSD1306_WriteByte(0xA4, OLED_CMD);
    SSD1306_WriteByte(0xA6, OLED_CMD);

    SSD1306_WriteByte(0x20, OLED_CMD);
    SSD1306_WriteByte(0x02, OLED_CMD);

    SSD1306_WriteByte(0xDA, OLED_CMD);
    SSD1306_WriteByte(0x12, OLED_CMD);

    SSD1306_WriteByte(0xD9, OLED_CMD);
    SSD1306_WriteByte(0xF1, OLED_CMD);

    SSD1306_WriteByte(0xDB, OLED_CMD);
    SSD1306_WriteByte(0x20, OLED_CMD);

    SSD1306_WriteByte(0x8D, OLED_CMD);
    SSD1306_WriteByte(0x14, OLED_CMD);

    SSD1306_WriteByte(0xAD, OLED_CMD);
    SSD1306_WriteByte(0x8B, OLED_CMD);
    SSD1306_WriteByte(0x33, OLED_CMD);

    SSD1306_Fill(GUI_COLOR_BLACK);
    SSD1306_UpdateScreen();
    SSD1306_WriteByte(0xAF, OLED_CMD);

    SSD1306.CurrentX = SSD1306.CurrentY = 0;
    SSD1306.Initialized = 1;
}

/**
 * @brief   更新屏幕显示
 * @details 将显存缓冲区的内容发送到OLED屏幕
 *          根据芯片类型使用不同的列起始地址：
 *          - SSD1306: 列起始地址为0
 *          - SH1106: 列起始地址为2
 */
void SSD1306_UpdateScreen(void)
{
    uint8_t m, st;
    st = (OLED_ChipType == CHIP_SH1106) ? 0x02 : 0x00;

    for (m = 0; m < 8; m++)
    {
        SSD1306_WriteByte(0xb0 + m, OLED_CMD);
        SSD1306_WriteByte(st, OLED_CMD);
        SSD1306_WriteByte(0x10, OLED_CMD);
        SSD1306_WriteMuliByte((uint8_t *)&SSD1306_Buffer[128 * m], 128, OLED_DATA);
    }
}

/**
 * @brief   填充整个屏幕
 * @param   color: 填充颜色 (GUI_COLOR_BLACK=黑色, GUI_COLOR_WHITE=白色)
 */
void SSD1306_Fill(GUI_COLOR color)
{
    memset((void *)SSD1306_Buffer, (color == GUI_COLOR_BLACK) ? 0x00 : 0xFF, sizeof(SSD1306_Buffer));
}

/**
 * @brief   切换显示反色状态
 * @details 将显存缓冲区所有数据取反，实现反色效果
 */
void SSD1306_ToggleInvert(void)
{
    uint16_t i;

    SSD1306.Inverted = !SSD1306.Inverted;

    for (i = 0; i < sizeof(SSD1306_Buffer); i++) {
        SSD1306_Buffer[i] = ~SSD1306_Buffer[i];
    }
}

/**
 * @brief   绘制单个像素点
 * @param   x: X坐标 (0 ~ 127)
 * @param   y: Y坐标 (0 ~ 63)
 * @param   color: 像素颜色
 * @note    显存结构：每8行为一页，共8页
 *          字节内低位对应上面的像素
 */
void SSD1306_DrawPixel(uint16_t x, uint16_t y, GUI_COLOR color)
{
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return;
    }
    if (SSD_Trans && color == COLOR_B)
        return;

    if (SSD1306.Inverted) {
        color = (GUI_COLOR)!color;
    }

    if (color == GUI_COLOR_WHITE) {
        SSD1306_Buffer[x + (y / 8) * SSD1306_WIDTH] |= 1 << (y % 8);
    } else {
        SSD1306_Buffer[x + (y / 8) * SSD1306_WIDTH] &= ~(1 << (y % 8));
    }
}

/**
 * @brief   设置绘图光标位置
 * @param   x: X坐标
 * @param   y: Y坐标
 */
void SSD1306_GotoXY(uint16_t x, uint16_t y)
{
    SSD1306.CurrentX = x;
    SSD1306.CurrentY = y;
}

/**
 * @brief   绘制直线
 * @param   x0: 起点X坐标
 * @param   y0: 起点Y坐标
 * @param   x1: 终点X坐标
 * @param   y1: 终点Y坐标
 * @param   c: 线条颜色
 * @note    使用Bresenham算法
 */
void SSD1306_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, GUI_COLOR c)
{
    int16_t dx, dy, sx, sy, err, e2, i, tmp;

    if (x0 >= SSD1306_WIDTH) {
        x0 = SSD1306_WIDTH - 1;
    }
    if (x1 >= SSD1306_WIDTH) {
        x1 = SSD1306_WIDTH - 1;
    }
    if (y0 >= SSD1306_HEIGHT) {
        y0 = SSD1306_HEIGHT - 1;
    }
    if (y1 >= SSD1306_HEIGHT) {
        y1 = SSD1306_HEIGHT - 1;
    }

    dx = (x0 < x1) ? (x1 - x0) : (x0 - x1);
    dy = (y0 < y1) ? (y1 - y0) : (y0 - y1);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = ((dx > dy) ? dx : -dy) / 2;

    if (dx == 0) {
        if (y1 < y0) {
            tmp = y1;
            y1 = y0;
            y0 = tmp;
        }

        if (x1 < x0) {
            tmp = x1;
            x1 = x0;
            x0 = tmp;
        }

        for (i = y0; i <= y1; i++) {
            SSD1306_DrawPixel(x0, i, c);
        }

        return;
    }

    if (dy == 0) {
        if (y1 < y0) {
            tmp = y1;
            y1 = y0;
            y0 = tmp;
        }

        if (x1 < x0) {
            tmp = x1;
            x1 = x0;
            x0 = tmp;
        }

        for (i = x0; i <= x1; i++) {
            SSD1306_DrawPixel(i, y0, c);
        }

        return;
    }

    while (1) {
        SSD1306_DrawPixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = err;
        if (e2 > -dx) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dy) {
            err += dx;
            y0 += sy;
        }
    }
}

/**
 * @brief   绘制矩形边框
 * @param   x: 左上角X坐标
 * @param   y: 左上角Y坐标
 * @param   w: 宽度
 * @param   h: 高度
 * @param   c: 边框颜色
 */
void SSD1306_DrawRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, GUI_COLOR c)
{
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return;
    }

    if ((x + w) >= SSD1306_WIDTH) {
        w = SSD1306_WIDTH - x;
    }
    if ((y + h) >= SSD1306_HEIGHT) {
        h = SSD1306_HEIGHT - y;
    }

    SSD1306_DrawLine(x, y, x + w, y, c);
    SSD1306_DrawLine(x, y + h, x + w, y + h, c);
    SSD1306_DrawLine(x, y, x, y + h, c);
    SSD1306_DrawLine(x + w, y, x + w, y + h, c);
}

/**
 * @brief   绘制填充矩形
 * @param   x: 左上角X坐标
 * @param   y: 左上角Y坐标
 * @param   w: 宽度
 * @param   h: 高度
 * @param   c: 填充颜色
 */
void SSD1306_DrawFilledRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, GUI_COLOR c)
{
    uint8_t i;

    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return;
    }

    if ((x + w) >= SSD1306_WIDTH) {
        w = SSD1306_WIDTH - x;
    }
    if ((y + h) >= SSD1306_HEIGHT) {
        h = SSD1306_HEIGHT - y;
    }

    for (i = 0; i <= h; i++) {
        SSD1306_DrawLine(x, y + i, x + w, y + i, c);
    }
}

/**
 * @brief   绘制三角形边框
 * @param   x1, y1: 第一个顶点坐标
 * @param   x2, y2: 第二个顶点坐标
 * @param   x3, y3: 第三个顶点坐标
 * @param   color: 边框颜色
 */
void SSD1306_DrawTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, GUI_COLOR color)
{
    SSD1306_DrawLine(x1, y1, x2, y2, color);
    SSD1306_DrawLine(x2, y2, x3, y3, color);
    SSD1306_DrawLine(x3, y3, x1, y1, color);
}

/**
 * @brief   绘制填充三角形
 * @param   x1, y1: 第一个顶点坐标
 * @param   x2, y2: 第二个顶点坐标
 * @param   x3, y3: 第三个顶点坐标
 * @param   color: 填充颜色
 */
void SSD1306_DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, GUI_COLOR color)
{
    int16_t deltax = 0, deltay = 0, x = 0, y = 0, xinc1 = 0, xinc2 = 0,
    yinc1 = 0, yinc2 = 0, den = 0, num = 0, numadd = 0, numpixels = 0,
    curpixel = 0;

    deltax = ABS(x2 - x1);
    deltay = ABS(y2 - y1);
    x = x1;
    y = y1;

    if (x2 >= x1) {
        xinc1 = 1;
        xinc2 = 1;
    } else {
        xinc1 = -1;
        xinc2 = -1;
    }

    if (y2 >= y1) {
        yinc1 = 1;
        yinc2 = 1;
    } else {
        yinc1 = -1;
        yinc2 = -1;
    }

    if (deltax >= deltay) {
        xinc1 = 0;
        yinc2 = 0;
        den = deltax;
        num = deltax / 2;
        numadd = deltay;
        numpixels = deltax;
    } else {
        xinc2 = 0;
        yinc1 = 0;
        den = deltay;
        num = deltay / 2;
        numadd = deltax;
        numpixels = deltay;
    }

    for (curpixel = 0; curpixel <= numpixels; curpixel++) {
        SSD1306_DrawLine(x, y, x3, y3, color);

        num += numadd;
        if (num >= den) {
            num -= den;
            x += xinc1;
            y += yinc1;
        }
        x += xinc2;
        y += yinc2;
    }
}

/**
 * @brief   绘制圆形边框
 * @param   x0: 圆心X坐标
 * @param   y0: 圆心Y坐标
 * @param   r: 半径
 * @param   c: 边框颜色
 * @note    使用中点圆算法(Midpoint Circle Algorithm)
 */
void SSD1306_DrawCircle(int16_t x0, int16_t y0, int16_t r, GUI_COLOR c)
{
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    SSD1306_DrawPixel(x0, y0 + r, c);
    SSD1306_DrawPixel(x0, y0 - r, c);
    SSD1306_DrawPixel(x0 + r, y0, c);
    SSD1306_DrawPixel(x0 - r, y0, c);

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        SSD1306_DrawPixel(x0 + x, y0 + y, c);
        SSD1306_DrawPixel(x0 - x, y0 + y, c);
        SSD1306_DrawPixel(x0 + x, y0 - y, c);
        SSD1306_DrawPixel(x0 - x, y0 - y, c);

        SSD1306_DrawPixel(x0 + y, y0 + x, c);
        SSD1306_DrawPixel(x0 - y, y0 + x, c);
        SSD1306_DrawPixel(x0 + y, y0 - x, c);
        SSD1306_DrawPixel(x0 - y, y0 - x, c);
    }
}

/**
 * @brief   绘制填充圆形
 * @param   x0: 圆心X坐标
 * @param   y0: 圆心Y坐标
 * @param   r: 半径
 * @param   c: 填充颜色
 */
void SSD1306_DrawFilledCircle(int16_t x0, int16_t y0, int16_t r, GUI_COLOR c)
{
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    SSD1306_DrawPixel(x0, y0 + r, c);
    SSD1306_DrawPixel(x0, y0 - r, c);
    SSD1306_DrawPixel(x0 + r, y0, c);
    SSD1306_DrawPixel(x0 - r, y0, c);
    SSD1306_DrawLine(x0 - r, y0, x0 + r, y0, c);

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        SSD1306_DrawLine(x0 - x, y0 + y, x0 + x, y0 + y, c);
        SSD1306_DrawLine(x0 + x, y0 - y, x0 - x, y0 - y, c);

        SSD1306_DrawLine(x0 + y, y0 + x, x0 - y, y0 + x, c);
        SSD1306_DrawLine(x0 + y, y0 - x, x0 - y, y0 - x, c);
    }
}

/**
 * @brief   开启OLED显示
 * @details 开启电荷泵并打开显示
 */
void SSD1306_ON(void)
{
    SSD1306_WriteByte(0x8D, OLED_CMD);
    SSD1306_WriteByte(0x14, OLED_CMD);
    SSD1306_WriteByte(0xAF, OLED_CMD);
}

/**
 * @brief   关闭OLED显示
 * @details 关闭电荷泵并关闭显示
 */
void SSD1306_OFF(void)
{
    SSD1306_WriteByte(0x8D, OLED_CMD);
    SSD1306_WriteByte(0x10, OLED_CMD);
    SSD1306_WriteByte(0xAE, OLED_CMD);
}
