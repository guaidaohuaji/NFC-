#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define CONFIG_PRIMARY_ADDR     0x000000UL
#define CONFIG_BACKUP_ADDR      0x001000UL
#define CONFIG_SECTOR_SIZE      4096UL

#define APP_CONFIG_MAGIC        0x4E464341UL
#define APP_CONFIG_VERSION      1U
#define APP_CONFIG_DEFAULT_ID   1UL

#define ATT_MODE_ENTRY          0U
#define ATT_MODE_EXIT           1U
#define ATT_MODE_BOTH           2U
#define ATT_MODE_DEFAULT        ATT_MODE_BOTH

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t deviceId;
    uint8_t  attendanceMode;
    uint8_t  reserved[17];
    uint16_t checksum;
} DeviceConfig_t;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(DeviceConfig_t) == 32, "DeviceConfig_t must be 32 bytes");
#endif

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
