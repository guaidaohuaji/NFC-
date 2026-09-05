/**
 * @file    attendance_log.c
 * @brief   W25Q128 考勤记录最小日志模块实现
 * @note    阶段二第二步
 *          依赖：W25QXX_Read / W25QXX_Write / W25QXX_Erase_Sector
 *                bsp_rtc.h（RTC 时间读取）
 *                app_config.h（考勤模式）
 */

#include "attendance_log.h"
#include "w25qxx.h"
#include "bsp_rtc.h"
#include "app_config.h"
#include <string.h>

/* ================================================================
 *  模块内静态变量
 * ================================================================ */
static AttendanceLogHeader_t s_header;
static uint8_t s_initialized = 0;
static uint8_t s_lastError = 0;

/* ================================================================
 *  CRC-16/XMODEM
 *  poly = 0x1021, init = 0x0000, xorout = 0x0000
 *  参考：b"123456789" -> 0x31C3
 * ================================================================ */
static uint16_t crc16_xmodem(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0x0000;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ================================================================
 *  内部辅助函数：计算 header 前 30 字节的 CRC
 * ================================================================ */
static uint16_t calc_header_checksum(const AttendanceLogHeader_t *h)
{
    return crc16_xmodem((const uint8_t *)h, 30);
}

/* ================================================================
 *  内部辅助函数：计算 record 前 30 字节的 CRC
 * ================================================================ */
static uint16_t calc_record_checksum(const AttendanceRecord_t *r)
{
    return crc16_xmodem((const uint8_t *)r, 30);
}

/* ================================================================
 *  内部辅助函数：校验 header 是否有效
 *  检查 magic、recordSize、checksum
 * ================================================================ */
static uint8_t validate_header(const AttendanceLogHeader_t *h)
{
    if (h->magic != ATT_LOG_HEADER_MAGIC)
        return 0;
    if (h->recordSize != 32)
        return 0;
    uint16_t calc = calc_header_checksum(h);
    if (calc != h->checksum)
        return 0;
    return 1;
}

/* ================================================================
 *  内部辅助函数：校验 record 是否有效
 *  检查 magic 和 checksum
 * ================================================================ */
static uint8_t validate_record(const AttendanceRecord_t *r)
{
    if (r->magic != ATT_LOG_RECORD_MAGIC)
        return 0;
    uint16_t calc = calc_record_checksum(r);
    if (calc != r->checksum)
        return 0;
    return 1;
}

/* ================================================================
 *  内部辅助函数：写入 header 到 Flash 并读回校验
 * ================================================================ */
static uint8_t write_header(const AttendanceLogHeader_t *h)
{
    uint32_t addr = LOG_REGION_BASE;

    W25QXX_Write((uint8_t *)h, addr, sizeof(AttendanceLogHeader_t));

    /* 读回校验 */
    AttendanceLogHeader_t verify;
    W25QXX_Read((uint8_t *)&verify, addr, sizeof(AttendanceLogHeader_t));

    if (memcmp(h, &verify, sizeof(AttendanceLogHeader_t)) != 0)
    {
        s_lastError = 1;
        return 1;
    }
    return 0;
}

/* ================================================================
 *  AttendanceLog_Init()
 * ================================================================ */
uint8_t AttendanceLog_Init(void)
{
    uint32_t addr = LOG_REGION_BASE;

    /* 读取现有 header */
    W25QXX_Read((uint8_t *)&s_header, addr, sizeof(AttendanceLogHeader_t));

    if (validate_header(&s_header))
    {
        /* header 有效，直接使用 */
        s_initialized = 1;
        s_lastError = 0;
        return 0;
    }

    /* header 无效，初始化新 header */
    memset(&s_header, 0, sizeof(s_header));
    s_header.magic         = ATT_LOG_HEADER_MAGIC;
    s_header.version       = 1;
    s_header.recordSize    = 32;
    s_header.writeIndex    = 0;
    s_header.seqNext       = 1;
    s_header.recordCountMax = (LOG_REGION_SIZE - sizeof(AttendanceLogHeader_t)) /
                               sizeof(AttendanceRecord_t);
    s_header.totalWritten  = 0;
    s_header.checksum      = calc_header_checksum(&s_header);

    uint8_t ret = write_header(&s_header);
    if (ret != 0)
    {
        s_initialized = 0;
        s_lastError = 2;
        return 2;
    }

    s_initialized = 1;
    s_lastError = 0;
    return 0;
}

/* ================================================================
 *  AttendanceLog_GetHeader()
 * ================================================================ */
const AttendanceLogHeader_t *AttendanceLog_GetHeader(void)
{
    if (!s_initialized)
    {
        return NULL;
    }
    return &s_header;
}

/* ================================================================
 *  AttendanceLog_FindLastByWorker()
 *  从最新记录向前扫描，查找指定 workerId 的最后一条有效记录
 * ================================================================ */
uint8_t AttendanceLog_FindLastByWorker(uint32_t workerId, AttendanceRecord_t *outRecord)
{
    if (!s_initialized || outRecord == NULL)
    {
        s_lastError = 10;
        return 10;
    }

    /* 无记录 */
    if (s_header.totalWritten == 0)
    {
        s_lastError = 11;
        return 11;
    }

    /* 确定扫描范围：最多往回扫描 totalWritten 条 */
    uint32_t scanCount = s_header.totalWritten;
    if (scanCount > s_header.recordCountMax)
    {
        scanCount = s_header.recordCountMax;
    }

    uint32_t recordAreaBase = LOG_REGION_BASE + sizeof(AttendanceLogHeader_t);

    for (uint32_t i = 0; i < scanCount; i++)
    {
        /* 从最新往前读 */
        int32_t idx = (int32_t)s_header.writeIndex - 1 - (int32_t)i;
        if (idx < 0)
        {
            idx += (int32_t)s_header.recordCountMax;
        }
        uint32_t recordAddr = recordAreaBase + (uint32_t)idx * sizeof(AttendanceRecord_t);

        AttendanceRecord_t rec;
        W25QXX_Read((uint8_t *)&rec, recordAddr, sizeof(AttendanceRecord_t));

        /* 校验记录有效性 */
        if (!validate_record(&rec))
        {
            continue; /* 损坏记录跳过 */
        }

        if (rec.workerId == workerId)
        {
            *outRecord = rec;
            s_lastError = 0;
            return 0;
        }
    }

    /* 未找到匹配记录 */
    s_lastError = 12;
    return 12;
}

/* ================================================================
 *  AttendanceLog_CalcDurationToNow()
 *  计算从 startRecord 的时间到当前 RTC 时间的秒差
 * ================================================================ */
uint32_t AttendanceLog_CalcDurationToNow(const AttendanceRecord_t *startRecord)
{
    if (startRecord == NULL)
    {
        return 0;
    }

    /* 读取当前 RTC 时间 */
    BSP_RTC_DateTime_t now;
    if (BSP_RTC_GetDateTime(&now) != HAL_OK)
    {
        return 0;
    }

    /* 每月天数表（简化，不处理闰年） */
    static const uint16_t monthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    /* 将 startRecord 的时间转换为秒 */
    uint32_t startSec = (uint32_t)startRecord->second +
                        (uint32_t)startRecord->minute * 60UL +
                        (uint32_t)startRecord->hour   * 3600UL +
                        (uint32_t)startRecord->day    * 86400UL;
    {
        uint32_t cumDays = 0;
        for (uint8_t m = 1; m < startRecord->month && m <= 12; m++)
        {
            cumDays += monthDays[m - 1];
        }
        startSec += cumDays * 86400UL;
    }
    startSec += (uint32_t)startRecord->year * 365UL * 86400UL;

    /* 将当前 RTC 时间转换为秒 */
    uint32_t nowSec = (uint32_t)now.second +
                      (uint32_t)now.minute * 60UL +
                      (uint32_t)now.hour   * 3600UL +
                      (uint32_t)now.day    * 86400UL;
    {
        uint32_t cumDays = 0;
        for (uint8_t m = 1; m < now.month && m <= 12; m++)
        {
            cumDays += monthDays[m - 1];
        }
        nowSec += cumDays * 86400UL;
    }
    nowSec += (uint32_t)now.year * 365UL * 86400UL;

    /* 如果当前时间大于起始时间，返回差值 */
    if (nowSec > startSec)
    {
        return nowSec - startSec;
    }

    return 0;
}

/* ================================================================
 *  AttendanceLog_Clear()
 * ================================================================ */
uint8_t AttendanceLog_Clear(void)
{
    if (!s_initialized)
    {
        s_lastError = 3;
        return 3;
    }

    /* 擦除扇区 2 ~ 18 */
    for (uint16_t sec = LOG_SECTOR_START; sec <= LOG_SECTOR_END; sec++)
    {
        W25QXX_Erase_Sector(sec);
    }

    /* 重新初始化 header */
    memset(&s_header, 0, sizeof(s_header));
    s_header.magic          = ATT_LOG_HEADER_MAGIC;
    s_header.version        = 1;
    s_header.recordSize     = 32;
    s_header.writeIndex     = 0;
    s_header.seqNext        = 1;
    s_header.recordCountMax = (LOG_REGION_SIZE - sizeof(AttendanceLogHeader_t)) /
                               sizeof(AttendanceRecord_t);
    s_header.totalWritten   = 0;
    s_header.checksum       = calc_header_checksum(&s_header);

    uint8_t ret = write_header(&s_header);
    if (ret != 0)
    {
        s_lastError = 4;
        return 4;
    }

    s_lastError = 0;
    return 0;
}

/* ================================================================
 *  AttendanceLog_AppendRecord()
 * ================================================================ */
uint8_t AttendanceLog_AppendRecord(uint32_t workerId, uint8_t cardType,
    uint8_t eventType, uint8_t mode, uint32_t durationSec)
{
    if (!s_initialized)
    {
        s_lastError = 5;
        return 5;
    }

    /* 读取当前 RTC 时间 */
    BSP_RTC_DateTime_t dt;
    if (BSP_RTC_GetDateTime(&dt) != HAL_OK)
    {
        /* RTC 读取失败，使用默认值 0 */
        memset(&dt, 0, sizeof(dt));
    }

    /* 构建记录 */
    AttendanceRecord_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic       = ATT_LOG_RECORD_MAGIC;
    rec.seq         = s_header.seqNext;
    rec.workerId    = workerId;
    rec.cardType    = cardType;
    rec.eventType   = eventType;
    rec.mode        = mode;
    rec.status      = 0;
    rec.year        = dt.year;
    rec.month       = dt.month;
    rec.day         = dt.day;
    rec.hour        = dt.hour;
    rec.minute      = dt.minute;
    rec.second      = dt.second;
    rec.durationSec = durationSec;
    rec.checksum    = calc_record_checksum(&rec);

    /* 计算写入地址 */
    uint32_t recordAreaBase = LOG_REGION_BASE + sizeof(AttendanceLogHeader_t);
    uint32_t recordAddr = recordAreaBase +
                           (uint32_t)s_header.writeIndex * sizeof(AttendanceRecord_t);

    /* 写入记录 */
    W25QXX_Write((uint8_t *)&rec, recordAddr, sizeof(AttendanceRecord_t));

    /* 读回校验记录 */
    {
        AttendanceRecord_t verify;
        W25QXX_Read((uint8_t *)&verify, recordAddr, sizeof(AttendanceRecord_t));
        if (memcmp(&rec, &verify, sizeof(AttendanceRecord_t)) != 0)
        {
            s_lastError = 6;
            return 6;
        }
    }

    /* 更新 header */
    s_header.writeIndex = (s_header.writeIndex + 1) % s_header.recordCountMax;
    s_header.seqNext++;
    s_header.totalWritten++;
    s_header.checksum = calc_header_checksum(&s_header);

    uint8_t ret = write_header(&s_header);
    if (ret != 0)
    {
        s_lastError = 7;
        return 7;
    }

    s_lastError = 0;
    return 0;
}

/* ================================================================
 *  AttendanceLog_AppendTest()
 * ================================================================ */
uint8_t AttendanceLog_AppendTest(uint32_t workerId, uint8_t eventType, uint32_t durationSec)
{
    if (!s_initialized)
    {
        s_lastError = 8;
        return 8;
    }

    /* 从 AppConfig 获取当前考勤模式 */
    const DeviceConfig_t *cfg = AppConfig_Get();
    uint8_t mode = (cfg != NULL) ? cfg->attendanceMode : ATT_MODE_BOTH;

    return AttendanceLog_AppendRecord(workerId, 0, eventType, mode, durationSec);
}

/* ================================================================
 *  AttendanceLog_GetRecent()
 * ================================================================ */
uint8_t AttendanceLog_GetRecent(uint16_t maxCount,
    AttendanceRecord_t *outRecords, uint16_t *outCount)
{
    if (!s_initialized)
    {
        s_lastError = 9;
        if (outCount != NULL) *outCount = 0;
        return 9;
    }

    if (maxCount == 0 || maxCount > 20)
    {
        maxCount = 10; /* 默认 10 条 */
    }

    *outCount = 0;

    /* 计算实际可扫描的记录数 */
    uint32_t totalRecords = s_header.totalWritten;
    if (totalRecords == 0)
    {
        s_lastError = 0;
        return 0; /* 无记录，成功返回 0 条 */
    }

    /* 确定扫描范围：最多往回扫描 totalRecords 条，但不超过 recordCountMax */
    uint32_t scanCount = totalRecords;
    if (scanCount > s_header.recordCountMax)
    {
        scanCount = s_header.recordCountMax;
    }
    if (scanCount > (uint32_t)maxCount)
    {
        scanCount = (uint32_t)maxCount;
    }

    /* writeIndex 指向下一条要写入的位置，所以最新一条在 writeIndex - 1 */
    uint32_t recordAreaBase = LOG_REGION_BASE + sizeof(AttendanceLogHeader_t);

    for (uint32_t i = 0; i < scanCount && *outCount < maxCount; i++)
    {
        /* 从最新往前读 */
        int32_t idx = (int32_t)s_header.writeIndex - 1 - (int32_t)i;
        if (idx < 0)
        {
            idx += (int32_t)s_header.recordCountMax;
        }
        uint32_t recordAddr = recordAreaBase + (uint32_t)idx * sizeof(AttendanceRecord_t);

        AttendanceRecord_t rec;
        W25QXX_Read((uint8_t *)&rec, recordAddr, sizeof(AttendanceRecord_t));

        if (validate_record(&rec))
        {
            outRecords[*outCount] = rec;
            (*outCount)++;
        }
        /* 遇到无效记录跳过，继续尝试下一条 */
    }

    s_lastError = 0;
    return 0;
}