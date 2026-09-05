/**
 * @file    rc522.c
 * @brief   MFRC522/RC522 RFID读写芯片驱动 - 核心实现
 * @details 本文件实现 RC522 芯片的所有操作函数，通过 RC522_IO_t
 *          接口与硬件完全解耦，不依赖任何特定平台或 HAL 库。
 *
 *          内部函数 (static)：
 *          - rc522_write_reg()    : 写寄存器
 *          - rc522_read_reg()     : 读寄存器
 *          - rc522_set_bit_mask() : 置位寄存器位
 *          - rc522_clear_bit_mask(): 清零寄存器位
 *          - rc522_calculate_crc(): CRC16 计算
 *          - rc522_communicate()  : 与芯片核心通讯
 *
 * @note    编码: UTF-8
 */

#include "rc522.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* ======================================================
 *  内部常量
 * ====================================================== */

/** 内部通讯缓冲区长度 (FIFO 最大 64 字节 + 余量) */
#define RC522_INTERNAL_BUF_LEN     18

/** 通讯超时循环次数 (与参考工程一致, 约保证足够等待时间) */
#define RC522_COMM_TIMEOUT         2000

/* ======================================================
 *  全局变量
 * ====================================================== */

/** 平台 IO 接口指针 (全局静态，驱动初始化时赋值) */
static RC522_IO_t *s_io = NULL;

/* ======================================================
 *  内部函数: 寄存器读写 (底层 SPI 通讯)
 * ====================================================== */

/**
 * @brief 向 RC522 指定寄存器写入数据
 * @param addr  寄存器地址 (8bit)
 * @param value 要写入的值
 * @note  SPI 写寄存器协议:
 *        地址格式 = (addr << 1) & 0x7E，最低位为 0 表示写操作
 */
static void rc522_write_reg(uint8_t addr, uint8_t value)
{
    if (!s_io) return;

    s_io->cs_control(0);                              /* 拉低片选，选中芯片 */
    s_io->spi_transfer(((addr << 1) & 0x7E));         /* 发送地址字节 (写) */
    s_io->spi_transfer(value);                        /* 发送数据字节 */
    s_io->cs_control(1);                              /* 拉高片选，释放芯片 */
}

/**
 * @brief 从 RC522 指定寄存器读取数据
 * @param  addr 寄存器地址 (8bit)
 * @return 读取到的值
 * @note  SPI 读寄存器协议:
 *        地址格式 = ((addr << 1) & 0x7E) | 0x80，最低位为 1 表示读操作
 */
static uint8_t rc522_read_reg(uint8_t addr)
{
    if (!s_io) return 0;

    uint8_t value;
    s_io->cs_control(0);                              /* 拉低片选，选中芯片 */
    s_io->spi_transfer(((addr << 1) & 0x7E) | 0x80); /* 发送地址字节 (读) */
    value = s_io->spi_transfer(0x00);                 /* 发送哑数据，同时接收数据 */
    s_io->cs_control(1);                              /* 拉高片选，释放芯片 */
    return value;
}

/* ======================================================
 *  内部函数: 位操作
 * ====================================================== */

/**
 * @brief 置位寄存器中指定的位
 * @param reg  寄存器地址
 * @param mask 位掩码 (要置 1 的位)
 */
static void rc522_set_bit_mask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = rc522_read_reg(reg);
    rc522_write_reg(reg, tmp | mask);
}

/**
 * @brief 清零寄存器中指定的位
 * @param reg  寄存器地址
 * @param mask 位掩码 (要清零的位)
 */
static void rc522_clear_bit_mask(uint8_t reg, uint8_t mask)
{
    uint8_t tmp = rc522_read_reg(reg);
    rc522_write_reg(reg, tmp & ~mask);
}

/* ======================================================
 *  内部函数: CRC16 计算
 *  (利用 RC522 片内硬件 CRC 协处理器)
 * ====================================================== */

/**
 * @brief 使用 RC522 硬件计算 CRC16
 * @param pInData  输入数据缓冲区
 * @param len      输入数据长度
 * @param pOutData 输出 CRC 结果 (2 字节: [0]=低位, [1]=高位)
 */
static void rc522_calculate_crc(const uint8_t *pInData, uint8_t len, uint8_t *pOutData)
{
    uint8_t i, n;

    rc522_clear_bit_mask(RC522_REG_DIVIRQ, 0x04);     /* 清除 CRC IRQ 标志 */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);/* 取消前一个命令 */
    rc522_set_bit_mask(RC522_REG_FIFOLEVEL, 0x80);    /* 清空 FIFO */

    /* 将数据逐字节写入 FIFO */
    for (i = 0; i < len; i++) {
        rc522_write_reg(RC522_REG_FIFODATA, pInData[i]);
    }

    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_CALCCRC); /* 启动 CRC 计算 */

    /* 等待 CRC 计算完成 (超时 255 次) */
    i = 0xFF;
    do {
        n = rc522_read_reg(RC522_REG_DIVIRQ);
        i--;
    } while ((i != 0) && !(n & 0x04));

    /* 读取 CRC 结果 (低字节在前) */
    pOutData[0] = rc522_read_reg(RC522_REG_CRCRESULTL);
    pOutData[1] = rc522_read_reg(RC522_REG_CRCRESULTM);
}

/* ======================================================
 *  内部函数: 与 RC522 核心通讯
 *  (封装了命令发送、FIFO 操作和中断状态处理)
 * ====================================================== */

/**
 * @brief 通过 RC522 与 Mifare 卡进行数据通讯
 * @param Command    RC522 命令 (PCD_TRANSCEIVE / PCD_AUTHENT 等)
 * @param pInData    发送给卡的数据缓冲区
 * @param InLenByte  发送数据长度 (字节)
 * @param pOutData   接收数据缓冲区
 * @param pOutLenBit 输出参数，接收数据的位长度
 * @return RC522_OK 成功，RC522_ERR 失败，RC522_ERR_NO_TAG 无卡响应
 */
static char rc522_communicate(uint8_t Command,
                              const uint8_t *pInData,
                              uint8_t InLenByte,
                              uint8_t *pOutData,
                              uint32_t *pOutLenBit)
{
    char status = RC522_ERR;
    uint8_t irqEn   = 0x00;
    uint8_t waitFor = 0x00;
    uint8_t lastBits, n;
    uint32_t i;

    /* 根据命令类型选择中断使能和等待标志 */
    switch (Command) {
        case RC522_CMD_AUTHENT:
            irqEn   = 0x12;
            waitFor = 0x10;
            break;

        case RC522_CMD_TRANSCEIVE:
            irqEn   = 0x77;
            waitFor = 0x30;
            break;

        default:
            break;
    }

    /* 配置中断和 FIFO */
    rc522_write_reg(RC522_REG_COMIEN, irqEn | 0x80);  /* 使能相关中断 */
    rc522_clear_bit_mask(RC522_REG_COMIRQ, 0x80);      /* 清除所有中断标志 */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);/* 取消当前命令 */
    rc522_set_bit_mask(RC522_REG_FIFOLEVEL, 0x80);     /* 清空 FIFO */

    /* 将要发送的数据写入 FIFO */
    for (i = 0; i < InLenByte; i++) {
        rc522_write_reg(RC522_REG_FIFODATA, pInData[i]);
    }

    /* 启动命令 */
    rc522_write_reg(RC522_REG_COMMAND, Command);

    /* 如果是收发命令，在发送最后一个字节后自动开启接收 */
    if (Command == RC522_CMD_TRANSCEIVE) {
        rc522_set_bit_mask(RC522_REG_BITFRAMING, 0x80);
    }

    /* 等待命令完成或超时 */
    i = RC522_COMM_TIMEOUT;
    do {
        n = rc522_read_reg(RC522_REG_COMIRQ);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & waitFor));

    rc522_clear_bit_mask(RC522_REG_BITFRAMING, 0x80); /* 清除位帧调整 */

    if (i != 0) {
        /* 检查错误寄存器 (TimerErr | CRCErr | ParityErr | ProtocolErr) */
        if (!(rc522_read_reg(RC522_REG_ERROR) & 0x1B)) {
            status = RC522_OK;

            /* 检查是否收到 Set1 (表示无标签响应) */
            if (n & irqEn & 0x01) {
                status = RC522_ERR_NO_TAG;
            }

            /* 收发命令需要读取返回数据 */
            if (Command == RC522_CMD_TRANSCEIVE) {
                n = rc522_read_reg(RC522_REG_FIFOLEVEL);
                lastBits = rc522_read_reg(RC522_REG_CONTROL) & 0x07;

                /* 计算接收到的位长度 */
                if (lastBits) {
                    *pOutLenBit = (n - 1) * 8 + lastBits;
                } else {
                    *pOutLenBit = n * 8;
                }

                /* 处理 FIFO 空的情况 */
                if (n == 0) {
                    n = 1;
                }
                if (n > RC522_INTERNAL_BUF_LEN) {
                    n = RC522_INTERNAL_BUF_LEN;
                }

                /* 从 FIFO 读取数据 */
                for (i = 0; i < n; i++) {
                    pOutData[i] = rc522_read_reg(RC522_REG_FIFODATA);
                }
            }
        } else {
            status = RC522_ERR;
        }
    }

    /* 停止定时器并取消命令 */
    rc522_set_bit_mask(RC522_REG_CONTROL, 0x80);
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);

    return status;
}

/* ======================================================
 *  公开 API 实现
 * ====================================================== */

/* ---------- 初始化与配置 ---------- */

void RC522_Init(RC522_IO_t *io)
{
    if (!io) return;
    s_io = io;

    /* 硬件复位序列 */
    s_io->rst_control(1);       /* RST 拉高 */
    s_io->delay_ms(10);
    s_io->rst_control(0);       /* RST 拉低 */
    s_io->delay_ms(10);
    s_io->rst_control(1);       /* RST 拉高 */
    s_io->delay_ms(20);

    /* 检查芯片是否就绪 (读 ComIEnReg 预期值为 0x80) */
    if (rc522_read_reg(RC522_REG_COMIEN) == 0x80) {
        s_io->delay_ms(20);
    }

    /* 软复位芯片 */
    RC522_Reset();

    /* 配置工作参数 */
    rc522_write_reg(RC522_REG_MODE, 0x3D);             /* 与 Mifare 通讯，CRC 初始值 0x6363 */
    rc522_write_reg(RC522_REG_TRELOADL, 30);           /* 定时器重载值低位 */
    rc522_write_reg(RC522_REG_TRELOADH, 0);            /* 定时器重载值高位 */
    rc522_write_reg(RC522_REG_TMODE, 0x8D);            /* 定时器模式 */
    rc522_write_reg(RC522_REG_TPRESCALER, 0x3E);       /* 定时器预分频 */
    rc522_write_reg(RC522_REG_TXAUTO, 0x40);           /* 发送自动模式 */
}

void RC522_Reset(void)
{
    /* 硬件复位序列 */
    s_io->rst_control(1);
    s_io->delay_ms(10);
    s_io->rst_control(0);
    s_io->delay_ms(10);
    s_io->rst_control(1);
    s_io->delay_ms(20);

    /* 软复位芯片 */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_RESETPHASE);
    s_io->delay_ms(10);

    /* 软复位后寄存器恢复默认值，需重新配置工作参数 */
    rc522_write_reg(RC522_REG_MODE, 0x3D);
    rc522_write_reg(RC522_REG_TRELOADL, 30);
    rc522_write_reg(RC522_REG_TRELOADH, 0);
    rc522_write_reg(RC522_REG_TMODE, 0x8D);
    rc522_write_reg(RC522_REG_TPRESCALER, 0x3E);
    rc522_write_reg(RC522_REG_TXAUTO, 0x40);
}

void RC522_ConfigISOType(char type)
{
    if (type == 'A') {
        rc522_clear_bit_mask(RC522_REG_STATUS2, 0x08);

        rc522_write_reg(RC522_REG_MODE, 0x3D);
        rc522_write_reg(RC522_REG_RXSEL, 0x86);
        rc522_write_reg(RC522_REG_RFCFG, 0x7F);
        rc522_write_reg(RC522_REG_TRELOADL, 30);
        rc522_write_reg(RC522_REG_TRELOADH, 0);
        rc522_write_reg(RC522_REG_TMODE, 0x8D);
        rc522_write_reg(RC522_REG_TPRESCALER, 0x3E);
        rc522_write_reg(RC522_REG_TXAUTO, 0x40);       /* 使能内部 RF 发生器（软复位后会丢失，需重新设置） */

        s_io->delay_ms(10);
        RC522_AntennaOn();
    }
}

void RC522_AntennaOn(void)
{
    uint8_t i = rc522_read_reg(RC522_REG_TXCONTROL);
    if (!(i & 0x03)) {
        rc522_set_bit_mask(RC522_REG_TXCONTROL, 0x03);
    }
}

void RC522_AntennaOff(void)
{
    rc522_clear_bit_mask(RC522_REG_TXCONTROL, 0x03);
}

/* ---------- 卡片基本操作 ---------- */

char RC522_Request(uint8_t req_code, uint8_t *pTagType)
{
    char status;
    uint32_t unLen;
    uint8_t buf[RC522_INTERNAL_BUF_LEN];

    rc522_clear_bit_mask(RC522_REG_STATUS2, 0x08);
    rc522_write_reg(RC522_REG_BITFRAMING, 0x07);
    rc522_set_bit_mask(RC522_REG_TXCONTROL, 0x03);

    buf[0] = req_code;
    status = rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 1, buf, &unLen);

    if ((status == RC522_OK) && (unLen == 0x10)) {
        *pTagType     = buf[0];
        *(pTagType + 1) = buf[1];
    } else {
        status = RC522_ERR;
    }

    return status;
}

char RC522_Anticoll(uint8_t *pSnr)
{
    char status;
    uint8_t i, snr_check = 0;
    uint32_t unLen;
    uint8_t buf[RC522_INTERNAL_BUF_LEN];

    rc522_clear_bit_mask(RC522_REG_STATUS2, 0x08);
    rc522_write_reg(RC522_REG_BITFRAMING, 0x00);
    rc522_clear_bit_mask(RC522_REG_COLL, 0x80);

    buf[0] = RC522_PICC_ANTICOLL1;
    buf[1] = 0x20;

    status = rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 2, buf, &unLen);

    if (status == RC522_OK) {
        for (i = 0; i < 4; i++) {
            *(pSnr + i) = buf[i];
            snr_check  ^= buf[i];
        }
        /* 校验 UID 校验字节 */
        if (snr_check != buf[i]) {
            status = RC522_ERR;
        }
    }

    rc522_set_bit_mask(RC522_REG_COLL, 0x80);
    return status;
}

char RC522_Select(uint8_t *pSnr)
{
    char status;
    uint8_t i;
    uint32_t unLen;
    uint8_t buf[RC522_INTERNAL_BUF_LEN];

    buf[0] = RC522_PICC_ANTICOLL1;
    buf[1] = 0x70;
    buf[6] = 0;

    for (i = 0; i < 4; i++) {
        buf[i + 2] = *(pSnr + i);
        buf[6]    ^= *(pSnr + i);
    }

    rc522_calculate_crc(buf, 7, &buf[7]);

    rc522_clear_bit_mask(RC522_REG_STATUS2, 0x08);
    status = rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 9, buf, &unLen);

    if ((status == RC522_OK) && (unLen == 0x18)) {
        /* 选卡成功 */
    } else {
        status = RC522_ERR;
    }

    return status;
}

char RC522_AuthState(uint8_t auth_mode, uint8_t addr, uint8_t *pKey, uint8_t *pSnr)
{
    char status;
    uint32_t unLen;
    uint8_t i, buf[RC522_INTERNAL_BUF_LEN];

    buf[0] = auth_mode;
    buf[1] = addr;

    for (i = 0; i < 6; i++) {
        buf[i + 2] = *(pKey + i);
    }
    for (i = 0; i < 4; i++) {
        buf[i + 8] = *(pSnr + i);
    }

    status = rc522_communicate(RC522_CMD_AUTHENT, buf, 12, buf, &unLen);

    /* 验证通过后 Status2Reg 的 bit3 会被置 1 */
    if ((status != RC522_OK) || (!(rc522_read_reg(RC522_REG_STATUS2) & 0x08))) {
        status = RC522_ERR;
    }

    return status;
}

char RC522_Read(uint8_t addr, uint8_t *pData)
{
    char status;
    uint32_t unLen;
    uint8_t i, buf[RC522_INTERNAL_BUF_LEN];

    buf[0] = RC522_PICC_READ;
    buf[1] = addr;
    rc522_calculate_crc(buf, 2, &buf[2]);

    status = rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 4, buf, &unLen);

    if ((status == RC522_OK) && (unLen == 0x90)) {
        for (i = 0; i < 16; i++) {
            *(pData + i) = buf[i];
        }
    } else {
        status = RC522_ERR;
    }

    return status;
}

char RC522_Write(uint8_t addr, uint8_t *pData)
{
    char status;
    uint32_t unLen;
    uint8_t i, buf[RC522_INTERNAL_BUF_LEN];

    buf[0] = RC522_PICC_WRITE;
    buf[1] = addr;
    rc522_calculate_crc(buf, 2, &buf[2]);

    /* 发送写命令 */
    status = rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 4, buf, &unLen);

    /* 检查卡是否准备接收数据 (应答 0x0A) */
    if ((status != RC522_OK) || (unLen != 4) || ((buf[0] & 0x0F) != 0x0A)) {
        return RC522_ERR;
    }

    /* 发送数据块 */
    for (i = 0; i < 16; i++) {
        buf[i] = *(pData + i);
    }
    rc522_calculate_crc(buf, 16, &buf[16]);

    status = rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 18, buf, &unLen);

    if ((status != RC522_OK) || (unLen != 4) || ((buf[0] & 0x0F) != 0x0A)) {
        status = RC522_ERR;
    }

    return status;
}

char RC522_Halt(void)
{
    uint32_t unLen;
    uint8_t buf[RC522_INTERNAL_BUF_LEN];

    buf[0] = RC522_PICC_HALT;
    buf[1] = 0;
    rc522_calculate_crc(buf, 2, &buf[2]);

    rc522_communicate(RC522_CMD_TRANSCEIVE, buf, 4, buf, &unLen);

    return RC522_OK;
}

void RC522_WaitCardOff(void)
{
    uint8_t tagType[2];

    /* 连续 3 次寻卡失败即认为卡已离开 */
    while (1) {
        if (RC522_Request(RC522_PICC_REQALL, tagType) != RC522_OK) {
            if (RC522_Request(RC522_PICC_REQALL, tagType) != RC522_OK) {
                if (RC522_Request(RC522_PICC_REQALL, tagType) != RC522_OK) {
                    return;
                }
            }
        }
        s_io->delay_ms(1000);
    }
}

/* ---------- 高级数据操作 ---------- */

char RC522_ReadBlock(uint8_t secNum, uint8_t blkNum, uint8_t buf[16])
{
    if (secNum > 15 || blkNum > 3) {
        return RC522_ERR;
    }
    return RC522_Read((secNum * 4 + blkNum), buf);
}

char RC522_WriteBlock(uint8_t secNum, uint8_t blkNum, uint8_t buf[16])
{
    if (secNum > 15 || blkNum > 3) {
        return RC522_ERR;
    }
    /* 扇区 0 块 0 为厂商数据块，通常不可写 */
    if (secNum == 0 && blkNum == 0) {
        return RC522_ERR;
    }
    return RC522_Write((secNum * 4 + blkNum), buf);
}

char RC522_ReadSector(uint8_t secNum, uint8_t buf[64])
{
    char status;

    for (int i = 0; i < 4; i++) {
        status = RC522_ReadBlock(secNum, i, &buf[i * 16]);
        if (status != RC522_OK) {
            break;
        }
    }

    return status;
}

char RC522_WriteSector(uint8_t secNum, uint8_t buf[48])
{
    char status;

    for (int i = 0; i < 3; i++) {   /* 只写前 3 块，第 4 块为密钥区 */
        status = RC522_WriteBlock(secNum, i, &buf[i * 16]);
        if (status != RC522_OK) {
            break;
        }
    }

    return status;
}

char RC522_ScanCard(uint8_t cn[4])
{
    char status;
    uint8_t tagType[2];

    /* 第 1 步：寻卡 */
    status = RC522_Request(RC522_PICC_REQALL, tagType);
    if (status != RC522_OK) {
        return status;
    }

    /* 第 2 步：防碰撞，获取 UID */
    status = RC522_Anticoll(cn);
    if (status != RC522_OK) {
        return status;
    }

    /* 第 3 步：选卡 */
    status = RC522_Select(cn);
    if (status != RC522_OK) {
        return status;
    }

    return RC522_OK;
}

char RC522_ReadAllSectors(uint8_t data[64][16])
{
    char status;
    uint8_t cardID[4];
    uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int sectorsRead = 0;

    /* ScanCard 已将卡置于 ACTIVE 状态，ACTIVE 下不响应 REQA/WUPA。
     * 先 Halt 使卡进入 HALT 状态，再用 WUPA(0x52) 唤醒，确保从已知状态开始。 */
    RC522_Halt();
    s_io->delay_ms(5);

    /* 寻卡 + 防碰撞 + 选卡 */
    status = RC522_Request(RC522_PICC_REQALL, cardID);
    if (status != RC522_OK) {
        return status;
    }

    status = RC522_Anticoll(cardID);
    if (status != RC522_OK) {
        return status;
    }

    status = RC522_Select(cardID);
    if (status != RC522_OK) {
        return status;
    }

    /* 逐个扇区读取 */
    for (int sec = 0; sec < 16; sec++) {
        /* 验证扇区密钥 (使用默认密钥 A) */
        status = RC522_AuthState(RC522_PICC_AUTHENT1A,
                                  (uint8_t)(sec * 4 + 3),
                                  defaultKey, cardID);
        if (status != RC522_OK) {
            /* 密钥验证失败，跳过此扇区，清零数据。
             * 认证失败后卡片安全状态不确定，需 Halt + WUPA 重新激活。 */
            memset(data[sec * 4], 0, 64);
            RC522_Halt();
            s_io->delay_ms(2);
            status = RC522_Request(RC522_PICC_REQALL, cardID);
            if (status != RC522_OK) break;
            status = RC522_Anticoll(cardID);
            if (status != RC522_OK) break;
            status = RC522_Select(cardID);
            if (status != RC522_OK) break;
            continue;
        }

        /* 读取扇区的 4 个块 */
        for (int blk = 0; blk < 4; blk++) {
            status = RC522_ReadBlock((uint8_t)sec, (uint8_t)blk, data[sec * 4 + blk]);
            if (status != RC522_OK) {
                memset(data[sec * 4 + blk], 0, 16);
            }
        }
        sectorsRead++;
    }

    /* 使卡进入休眠状态，方便后续重新检测 */
    RC522_Halt();

    return (sectorsRead > 0) ? RC522_OK : RC522_ERR;
}

char RC522_WriteAllSectors(uint8_t data[64][16])
{
    char status;
    uint8_t cardID[4];
    uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int sectorsWritten = 0;

    /* 寻卡 + 防碰撞 + 选卡 */
    status = RC522_Request(RC522_PICC_REQALL, cardID);
    if (status != RC522_OK) {
        return status;
    }

    status = RC522_Anticoll(cardID);
    if (status != RC522_OK) {
        return status;
    }

    status = RC522_Select(cardID);
    if (status != RC522_OK) {
        return status;
    }

    /* 逐个扇区写入 */
    for (int sec = 0; sec < 16; sec++) {
        /* 验证扇区密钥 */
        status = RC522_AuthState(RC522_PICC_AUTHENT1A,
                                  (uint8_t)(sec * 4 + 3),
                                  defaultKey, cardID);
        if (status != RC522_OK) {
            /* 密钥验证失败，跳过此扇区 */
            status = RC522_Request(RC522_PICC_REQALL, cardID);
            if (status != RC522_OK) break;
            status = RC522_Anticoll(cardID);
            if (status != RC522_OK) break;
            status = RC522_Select(cardID);
            if (status != RC522_OK) break;
            continue;
        }

        /* 写入扇区前 3 块 (跳过第 4 块密钥区) */
        for (int blk = 0; blk < 3; blk++) {
            uint8_t bidx = (uint8_t)(sec * 4 + blk);
            /* 跳过扇区 0 块 0 (厂商块) */
            if (bidx == 0) continue;
            status = RC522_WriteBlock((uint8_t)sec, (uint8_t)blk, data[bidx]);
            if (status != RC522_OK) {
                break;
            }
        }
        if (status == RC522_OK) {
            sectorsWritten++;
        }
    }

    RC522_Halt();

    return (sectorsWritten > 0) ? RC522_OK : RC522_ERR;
}
