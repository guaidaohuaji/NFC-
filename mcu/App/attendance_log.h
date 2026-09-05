/**
 * @file    attendance_log.h
 * @brief   W25Q128 考勤记录最小日志模块
 * @note    阶段二第二步：实现考勤记录 Flash 日志的最小闭环
 *          - 32 字节 Header + 32 字节 Record
 *          - CRC-16/XMODEM 校验
 *          - 扇区 2 ~ 18（共 17 个扇区，69632 字节）
 */

#ifndef __ATTENDANCE_LOG_H__
#define __ATTENDANCE_LOG_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  Flash 区域规划
 * ================================================================ */
#define LOG_REGION_BASE     0x002000U
#define LOG_REGION_SIZE     69632U          /* 17 * 4096 */
#define LOG_REGION_END      (LOG_REGION_BASE + LOG_REGION_SIZE)

#define LOG_SECTOR_START    2U
#define LOG_SECTOR_END      18U
#define LOG_SECTOR_COUNT    17U

/* ================================================================
 *  Magic 常量
 * ================================================================ */
#define ATT_LOG_RECORD_MAGIC  0x4154544EUL   /* "ATTN" */
#define ATT_LOG_HEADER_MAGIC  0x4C4F4700UL   /* "LOG\0" */

/* ================================================================
 *  事件类型
 * ================================================================ */
#define ATT_LOG_EVENT_IN      1U
#define ATT_LOG_EVENT_OUT     2U
#define ATT_LOG_EVENT_DENY    3U

/* ================================================================
 *  考勤模式（与 AppConfig 对齐）
 * ================================================================ */
#define ATT_MODE_ENTRY        0U
#define ATT_MODE_EXIT         1U
#define ATT_MODE_BOTH         2U

/* ================================================================
 *  日志 Header 结构体（32 字节 packed）
 * ================================================================ */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* ATT_LOG_HEADER_MAGIC           */
    uint16_t version;        /* 版本号，当前为 1                */
    uint16_t recordSize;     /* 记录大小，固定 32               */
    uint32_t writeIndex;     /* 下一条写入位置（循环索引）       */
    uint32_t seqNext;        /* 下一条记录的序号                */
    uint32_t recordCountMax; /* 最大可容纳记录数                */
    uint32_t totalWritten;   /* 已写入记录总数（不循环）         */
    uint8_t  reserved[6];    /* 保留字段                       */
    uint16_t checksum;       /* CRC-16/XMODEM，对前 30 字节计算  */
} AttendanceLogHeader_t;

/* ================================================================
 *  考勤记录结构体（32 字节 packed）
 * ================================================================ */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* ATT_LOG_RECORD_MAGIC           */
    uint32_t seq;            /* 递增序号，从 1 开始            */
    uint32_t workerId;       /* 工号                           */
    uint8_t  cardType;       /* 0=normal, 1=image, 2=admin     */
    uint8_t  eventType;      /* 1=IN, 2=OUT, 3=DENY            */
    uint8_t  mode;           /* 0=ENTRY, 1=EXIT, 2=BOTH        */
    uint8_t  status;         /* 保留，默认 0                    */
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  reserved[3];
    uint32_t durationSec;    /* OUT 有效，IN 为 0               */
    uint16_t checksum;       /* CRC-16/XMODEM，对前 30 字节计算  */
} AttendanceRecord_t;

/* ================================================================
 *  编译期大小检查
 * ================================================================ */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(AttendanceLogHeader_t) == 32,
    "AttendanceLogHeader_t must be 32 bytes");
_Static_assert(sizeof(AttendanceRecord_t) == 32,
    "AttendanceRecord_t must be 32 bytes");
#endif

/* ================================================================
 *  公共接口
 * ================================================================ */

/**
 * @brief  初始化考勤日志区
 *         - 读取 LOG_REGION_BASE 的 header
 *         - 有效则加载到模块内
 *         - 无效则写入默认 header
 * @retval 0 = 成功, 非0 = 失败
 */
uint8_t AttendanceLog_Init(void);

/**
 * @brief  清空考勤日志区
 *         - 擦除扇区 2 ~ 18
 *         - 重新写入默认 header
 * @retval 0 = 成功, 非0 = 失败
 */
uint8_t AttendanceLog_Clear(void);

/**
 * @brief  追加一条考勤记录（供串口测试使用）
 * @param  workerId:   工号
 * @param  eventType:  事件类型 (IN=1 / OUT=2 / DENY=3)
 * @param  durationSec: 时长（秒），OUT 有效，其他为 0
 * @retval 0 = 成功, 非0 = 失败
 * @note   cardType 固定为 0，mode 从 AppConfig 获取
 */
uint8_t AttendanceLog_AppendTest(uint32_t workerId, uint8_t eventType, uint32_t durationSec);

/**
 * @brief  追加一条考勤记录（完整参数）
 * @param  workerId:    工号
 * @param  cardType:    卡类型
 * @param  eventType:   事件类型
 * @param  mode:        考勤模式
 * @param  durationSec: 时长（秒）
 * @retval 0 = 成功, 非0 = 失败
 */
uint8_t AttendanceLog_AppendRecord(uint32_t workerId, uint8_t cardType,
    uint8_t eventType, uint8_t mode, uint32_t durationSec);

/**
 * @brief  获取最近 maxCount 条记录
 * @param  maxCount:   期望获取的最大记录数（1~20）
 * @param  outRecords: 输出缓冲区，调用者分配
 * @param  outCount:   实际返回的记录数
 * @retval 0 = 成功, 非0 = 失败
 */
uint8_t AttendanceLog_GetRecent(uint16_t maxCount,
    AttendanceRecord_t *outRecords, uint16_t *outCount);

/**
 * @brief  获取当前日志 Header（只读）
 * @retval 指向模块内静态 header 的指针，未初始化时返回 NULL
 */
const AttendanceLogHeader_t *AttendanceLog_GetHeader(void);

/**
 * @brief  查找指定 workerId 的最后一条有效记录
 * @param  workerId:   工号
 * @param  outRecord:  输出记录缓冲区
 * @retval 0 = 找到, 非0 = 未找到
 * @note   从最新记录向前扫描，校验 magic 和 checksum
 *         损坏记录自动跳过，记录区为空返回非0
 */
uint8_t AttendanceLog_FindLastByWorker(uint32_t workerId, AttendanceRecord_t *outRecord);

/**
 * @brief  计算从 startRecord 的时间点到当前 RTC 时间的秒差
 * @param  startRecord: 起始考勤记录
 * @retval 秒差，如果当前时间异常或小于起始时间返回 0
 * @note   使用简化日期算法（每月固定天数表，365天/年，不处理闰年）
 */
uint32_t AttendanceLog_CalcDurationToNow(const AttendanceRecord_t *startRecord);

#ifdef __cplusplus
}
#endif

#endif /* __ATTENDANCE_LOG_H__ */
