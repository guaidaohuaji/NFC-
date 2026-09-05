#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ================================================================
 * Flash 地址规划
 * ================================================================ */
#define CONFIG_PRIMARY_ADDR     0x000000UL
#define CONFIG_BACKUP_ADDR      0x001000UL
#define CONFIG_SECTOR_SIZE      4096UL

/* ================================================================
 * 魔数 / 版本 / 默认值
 * ================================================================ */
#define APP_CONFIG_MAGIC        0x4E464341UL   /* ASCII "NFCA" */
#define APP_CONFIG_VERSION      1U
#define APP_CONFIG_DEFAULT_ID   1UL

/* 考勤模式 */
#define ATT_MODE_ENTRY          0U
#define ATT_MODE_EXIT           1U
#define ATT_MODE_BOTH           2U
#define ATT_MODE_DEFAULT        ATT_MODE_BOTH

/* ================================================================
 * DeviceConfig_t — 32 字节 packed 结构体
 * ================================================================ */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* 0x4E464341, ASCII "NFCA" */
    uint16_t version;        /* 1 */
    uint16_t size;           /* sizeof(DeviceConfig_t), 应为 32 */
    uint32_t deviceId;       /* 默认 1 */
    uint8_t  attendanceMode; /* 0=ENTRY, 1=EXIT, 2=BOTH */
    uint8_t  reserved[17];   /* 填充到 32 字节 */
    uint16_t checksum;       /* CRC-16/XMODEM, 对前 30 字节计算 */
} DeviceConfig_t;

/* 编译期大小检查 — 如果编译器支持 _Static_assert，启用 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(DeviceConfig_t) == 32, "DeviceConfig_t must be 32 bytes");
#endif

/* ================================================================
 * API
 * ================================================================ */
void AppConfig_SetDefaults(DeviceConfig_t *cfg);
uint8_t AppConfig_IsValid(const DeviceConfig_t *cfg);
uint8_t AppConfig_Load(DeviceConfig_t *cfg);
uint8_t AppConfig_Save(const DeviceConfig_t *cfg);
uint8_t AppConfig_Init(void);
const DeviceConfig_t *AppConfig_Get(void);
uint8_t AppConfig_SetMode(uint8_t mode);
uint8_t AppConfig_Update(uint32_t deviceId, uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CONFIG_H */