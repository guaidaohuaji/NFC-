#include "app_config.h"
#include "w25qxx.h"
#include <string.h>

static DeviceConfig_t s_config;
static uint8_t s_initialized = 0;

static uint16_t Config_Crc16Xmodem(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0x0000;
    for (uint16_t i = 0; i < len; i++)
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

void AppConfig_SetDefaults(DeviceConfig_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(DeviceConfig_t));
    cfg->magic = APP_CONFIG_MAGIC;
    cfg->version = APP_CONFIG_VERSION;
    cfg->size = (uint16_t)sizeof(DeviceConfig_t);
    cfg->deviceId = APP_CONFIG_DEFAULT_ID;
    cfg->attendanceMode = ATT_MODE_DEFAULT;
    cfg->checksum = Config_Crc16Xmodem((const uint8_t *)cfg, 30);
}

uint8_t AppConfig_IsValid(const DeviceConfig_t *cfg)
{
    if (cfg == NULL) return 0;
    if (cfg->magic != APP_CONFIG_MAGIC) return 0;
    if (cfg->version != APP_CONFIG_VERSION) return 0;
    if (cfg->size != (uint16_t)sizeof(DeviceConfig_t)) return 0;
    if (cfg->attendanceMode > 2) return 0;
    return Config_Crc16Xmodem((const uint8_t *)cfg, 30) == cfg->checksum;
}

uint8_t AppConfig_Load(DeviceConfig_t *cfg)
{
    if (cfg == NULL) return 1;
    DeviceConfig_t temp;
    W25QXX_Read((uint8_t *)&temp, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
    if (AppConfig_IsValid(&temp)) {
        memcpy(cfg, &temp, sizeof(DeviceConfig_t));
        return 0;
    }
    W25QXX_Read((uint8_t *)&temp, CONFIG_BACKUP_ADDR, sizeof(DeviceConfig_t));
    if (AppConfig_IsValid(&temp)) {
        memcpy(cfg, &temp, sizeof(DeviceConfig_t));
        W25QXX_Write((uint8_t *)&temp, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
        return 0;
    }
    return 1;
}

uint8_t AppConfig_Save(const DeviceConfig_t *cfg)
{
    if (cfg == NULL) return 1;
    DeviceConfig_t verify;
    W25QXX_Write((uint8_t *)cfg, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
    W25QXX_Read((uint8_t *)&verify, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
    if (memcmp(cfg, &verify, sizeof(DeviceConfig_t)) != 0) return 2;
    W25QXX_Write((uint8_t *)cfg, CONFIG_BACKUP_ADDR, sizeof(DeviceConfig_t));
    W25QXX_Read((uint8_t *)&verify, CONFIG_BACKUP_ADDR, sizeof(DeviceConfig_t));
    if (memcmp(cfg, &verify, sizeof(DeviceConfig_t)) != 0) return 3;
    return 0;
}

uint8_t AppConfig_Init(void)
{
    if (AppConfig_Load(&s_config) == 0) {
        s_initialized = 1;
        return 0;
    }
    AppConfig_SetDefaults(&s_config);
    if (AppConfig_Save(&s_config) == 0) {
        s_initialized = 1;
        return 0;
    }
    s_initialized = 0;
    return 1;
}

const DeviceConfig_t *AppConfig_Get(void)
{
    return s_initialized ? &s_config : NULL;
}

uint8_t AppConfig_SetMode(uint8_t mode)
{
    if (!s_initialized) return 1;
    if (mode > 2) return 2;
    s_config.attendanceMode = mode;
    s_config.checksum = Config_Crc16Xmodem((const uint8_t *)&s_config, 30);
    return AppConfig_Save(&s_config) == 0 ? 0 : 3;
}

uint8_t AppConfig_Update(uint32_t deviceId, uint8_t mode)
{
    if (!s_initialized) return 1;
    if (deviceId < 1) return 2;
    if (mode > 2) return 3;
    DeviceConfig_t temp;
    memcpy(&temp, &s_config, sizeof(DeviceConfig_t));
    temp.deviceId = deviceId;
    temp.attendanceMode = mode;
    temp.checksum = Config_Crc16Xmodem((const uint8_t *)&temp, 30);
    if (AppConfig_Save(&temp) != 0) return 4;
    memcpy(&s_config, &temp, sizeof(DeviceConfig_t));
    return 0;
}
