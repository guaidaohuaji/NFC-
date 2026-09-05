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

static AttendanceLogHeader_t s_header;
static uint8_t s_initialized = 0;
static uint8_t s_lastError = 0;

static uint16_t crc16_xmodem(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0x0000;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

static uint16_t calc_header_checksum(const AttendanceLogHeader_t *h)
{
    return crc16_xmodem((const uint8_t *)h, 30);
}

static uint16_t calc_record_checksum(const AttendanceRecord_t *r)
{
    return crc16_xmodem((const uint8_t *)r, 30);
}

static uint8_t validate_header(const AttendanceLogHeader_t *h)
{
    if (h->magic != ATT_LOG_HEADER_MAGIC) return 0;
    if (h->recordSize != 32) return 0;
    return calc_header_checksum(h) == h->checksum;
}

static uint8_t validate_record(const AttendanceRecord_t *r)
{
    if (r->magic != ATT_LOG_RECORD_MAGIC) return 0;
    return calc_record_checksum(r) == r->checksum;
}

static uint8_t write_header(const AttendanceLogHeader_t *h)
{
    uint32_t addr = LOG_REGION_BASE;
    W25QXX_Write((uint8_t *)h, addr, sizeof(AttendanceLogHeader_t));

    AttendanceLogHeader_t verify;
    W25QXX_Read((uint8_t *)&verify, addr, sizeof(AttendanceLogHeader_t));
    if (memcmp(h, &verify, sizeof(AttendanceLogHeader_t)) != 0)
    {
        s_lastError = 1;
        return 1;
    }
    return 0;
}

uint8_t AttendanceLog_Init(void)
{
    uint32_t addr = LOG_REGION_BASE;
    W25QXX_Read((uint8_t *)&s_header, addr, sizeof(AttendanceLogHeader_t));

    if (validate_header(&s_header))
    {
        s_initialized = 1;
        s_lastError = 0;
        return 0;
    }

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
        s_initialized = 0;
        s_lastError = 2;
        return 2;
    }

    s_initialized = 1;
    s_lastError = 0;
    return 0;
}

const AttendanceLogHeader_t *AttendanceLog_GetHeader(void)
{
    if (!s_initialized) return NULL;
    return &s_header;
}

uint8_t AttendanceLog_FindLastByWorker(uint32_t workerId, AttendanceRecord_t *outRecord)
{
    if (!s_initialized || outRecord == NULL)
    {
        s_lastError = 10;
        return 10;
    }
    if (s_header.totalWritten == 0)
    {
        s_lastError = 11;
        return 11;
    }

    uint32_t scanCount = s_header.totalWritten;
    if (scanCount > s_header.recordCountMax) scanCount = s_header.recordCountMax;
    uint32_t recordAreaBase = LOG_REGION_BASE + sizeof(AttendanceLogHeader_t);

    for (uint32_t i = 0; i < scanCount; i++)
    {
        int32_t idx = (int32_t)s_header.writeIndex - 1 - (int32_t)i;
        if (idx < 0) idx += (int32_t)s_header.recordCountMax;
        uint32_t recordAddr = recordAreaBase + (uint32_t)idx * sizeof(AttendanceRecord_t);

        AttendanceRecord_t rec;
        W25QXX_Read((uint8_t *)&rec, recordAddr, sizeof(AttendanceRecord_t));
        if (!validate_record(&rec)) continue;
        if (rec.workerId == workerId)
        {
            *outRecord = rec;
            s_lastError = 0;
            return 0;
        }
    }

    s_lastError = 12;
    return 12;
}

uint32_t AttendanceLog_CalcDurationToNow(const AttendanceRecord_t *startRecord)
{
    if (startRecord == NULL) return 0;

    BSP_RTC_DateTime_t now;
    if (BSP_RTC_GetDateTime(&now) != HAL_OK) return 0;

    static const uint16_t monthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    uint32_t startSec = (uint32_t)startRecord->second +
                        (uint32_t)startRecord->minute * 60UL +
                        (uint32_t)startRecord->hour   * 3600UL +
                        (uint32_t)startRecord->day    * 86400UL;
    {
        uint32_t cumDays = 0;
        for (uint8_t m = 1; m < startRecord->month && m <= 12; m++) cumDays += monthDays[m - 1];
        startSec += cumDays * 86400UL;
    }
    startSec += (uint32_t)startRecord->year * 365UL * 86400UL;

    uint32_t nowSec = (uint32_t)now.second +
                      (uint32_t)now.minute * 60UL +
                      (uint32_t)now.hour   * 3600UL +
                      (uint32_t)now.day    * 86400UL;
    {
        uint32_t cumDays = 0;
        for (uint8_t m = 1; m < now.month && m <= 12; m++) cumDays += monthDays[m - 1];
        nowSec += cumDays * 86400UL;
    }
    nowSec += (uint32_t)now.year * 365UL * 86400UL;

    return nowSec > startSec ? nowSec - startSec : 0;
}

uint8_t AttendanceLog_Clear(void)
{
    if (!s_initialized)
    {
        s_lastError = 3;
        return 3;
    }

    for (uint16_t sec = LOG_SECTOR_START; sec <= LOG_SECTOR_END; sec++)
    {
        W25QXX_Erase_Sector(sec);
    }

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

uint8_t AttendanceLog_AppendRecord(uint32_t workerId, uint8_t cardType,
    uint8_t eventType, uint8_t mode, uint32_t durationSec)
{
    if (!s_initialized)
    {
        s_lastError = 5;
        return 5;
    }

    BSP_RTC_DateTime_t dt;
    if (BSP_RTC_GetDateTime(&dt) != HAL_OK) memset(&dt, 0, sizeof(dt));

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

    uint32_t recordAreaBase = LOG_REGION_BASE + sizeof(AttendanceLogHeader_t);
    uint32_t recordAddr = recordAreaBase +
                           (uint32_t)s_header.writeIndex * sizeof(AttendanceRecord_t);

    W25QXX_Write((uint8_t *)&rec, recordAddr, sizeof(AttendanceRecord_t));

    {
        AttendanceRecord_t verify;
        W25QXX_Read((uint8_t *)&verify, recordAddr, sizeof(AttendanceRecord_t));
        if (memcmp(&rec, &verify, sizeof(AttendanceRecord_t)) != 0)
        {
            s_lastError = 6;
            return 6;
        }
    }

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

uint8_t AttendanceLog_AppendTest(uint32_t workerId, uint8_t eventType, uint32_t durationSec)
{
    if (!s_initialized)
    {
        s_lastError = 8;
        return 8;
    }

    const DeviceConfig_t *cfg = AppConfig_Get();
    uint8_t mode = (cfg != NULL) ? cfg->attendanceMode : ATT_MODE_BOTH;
    return AttendanceLog_AppendRecord(workerId, 0, eventType, mode, durationSec);
}

uint8_t AttendanceLog_GetRecent(uint16_t maxCount,
    AttendanceRecord_t *outRecords, uint16_t *outCount)
{
    if (!s_initialized)
    {
        s_lastError = 9;
        if (outCount != NULL) *outCount = 0;
        return 9;
    }

    if (maxCount == 0 || maxCount > 20) maxCount = 10;
    *outCount = 0;

    uint32_t totalRecords = s_header.totalWritten;
    if (totalRecords == 0)
    {
        s_lastError = 0;
        return 0;
    }

    uint32_t scanCount = totalRecords;
    if (scanCount > s_header.recordCountMax) scanCount = s_header.recordCountMax;
    if (scanCount > (uint32_t)maxCount) scanCount = (uint32_t)maxCount;

    uint32_t recordAreaBase = LOG_REGION_BASE + sizeof(AttendanceLogHeader_t);

    for (uint32_t i = 0; i < scanCount && *outCount < maxCount; i++)
    {
        int32_t idx = (int32_t)s_header.writeIndex - 1 - (int32_t)i;
        if (idx < 0) idx += (int32_t)s_header.recordCountMax;
        uint32_t recordAddr = recordAreaBase + (uint32_t)idx * sizeof(AttendanceRecord_t);

        AttendanceRecord_t rec;
        W25QXX_Read((uint8_t *)&rec, recordAddr, sizeof(AttendanceRecord_t));

        if (validate_record(&rec))
        {
            outRecords[*outCount] = rec;
            (*outCount)++;
        }
    }

    s_lastError = 0;
    return 0;
}
