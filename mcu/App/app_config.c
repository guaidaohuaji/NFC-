#include "app_config.h"
#include "w25qxx.h"
#include <string.h>

/* ================================================================
 * 模块内全局当前配置
 * ================================================================ */
static DeviceConfig_t s_config;
static uint8_t        s_initialized = 0;

/* ================================================================
 * CRC-16/XMODEM
 *   poly=0x1021, init=0x0000, refin=false, refout=false, xorout=0x0000
 *   校验: b"123456789" -> 0x31C3
 * ================================================================ */
static uint16_t Config_Crc16Xmodem(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0x0000;
    for (uint16_t i = 0; i < len; i++)
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
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/* ================================================================
 * AppConfig_SetDefaults
 * ================================================================ */
void AppConfig_SetDefaults(DeviceConfig_t *cfg)
{
    if (cfg == NULL) return;

    memset(cfg, 0, sizeof(DeviceConfig_t));
    cfg->magic          = APP_CONFIG_MAGIC;
    cfg->version        = APP_CONFIG_VERSION;
    cfg->size           = (uint16_t)sizeof(DeviceConfig_t);
    cfg->deviceId       = APP_CONFIG_DEFAULT_ID;
    cfg->attendanceMode = ATT_MODE_DEFAULT;
    /* reserved 已清零 */

    /* 计算前 30 字节 CRC-16/XMODEM */
    cfg->checksum = Config_Crc16Xmodem((const uint8_t *)cfg, 30);
}

/* ================================================================
 * AppConfig_IsValid
 * ================================================================ */
uint8_t AppConfig_IsValid(const DeviceConfig_t *cfg)
{
    if (cfg == NULL) return 0;

    /* 检查 magic */
    if (cfg->magic != APP_CONFIG_MAGIC) return 0;

    /* 检查 version */
    if (cfg->version != APP_CONFIG_VERSION) return 0;

    /* 检查 size */
    if (cfg->size != (uint16_t)sizeof(DeviceConfig_t)) return 0;

    /* 检查 attendanceMode 范围 */
    if (cfg->attendanceMode > 2) return 0;

    /* 检查 checksum — 对前 30 字节计算 */
    uint16_t calc = Config_Crc16Xmodem((const uint8_t *)cfg, 30);
    if (calc != cfg->checksum) return 0;

    return 1;
}

/* ================================================================
 * AppConfig_Load
 *   返回值: 0 = 成功, 非0 = 失败
 * ================================================================ */
uint8_t AppConfig_Load(DeviceConfig_t *cfg)
{
    if (cfg == NULL) return 1;

    DeviceConfig_t temp;

    /* 1. 先读主区 */
    W25QXX_Read((uint8_t *)&temp, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
    if (AppConfig_IsValid(&temp))
    {
        memcpy(cfg, &temp, sizeof(DeviceConfig_t));
        return 0;
    }

    /* 2. 主区无效，读备份区 */
    W25QXX_Read((uint8_t *)&temp, CONFIG_BACKUP_ADDR, sizeof(DeviceConfig_t));
    if (AppConfig_IsValid(&temp))
    {
        memcpy(cfg, &temp, sizeof(DeviceConfig_t));
        /* 尝试恢复主区 */
        W25QXX_Write((uint8_t *)&temp, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
        return 0;
    }

    /* 3. 两者都无效 */
    return 1;
}

/* ================================================================
 * AppConfig_Save
 *   先写主区，再写备份区；写后读回校验
 *   使用 W25QXX_Write() — 内部带扇区擦除和读改写
 *   返回值: 0 = 成功, 非0 = 失败
 * ================================================================ */
uint8_t AppConfig_Save(const DeviceConfig_t *cfg)
{
    if (cfg == NULL) return 1;

    DeviceConfig_t verify;

    /* 1. 写主区 */
    W25QXX_Write((uint8_t *)cfg, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));

    /* 2. 读回主区校验 */
    W25QXX_Read((uint8_t *)&verify, CONFIG_PRIMARY_ADDR, sizeof(DeviceConfig_t));
    if (memcmp(cfg, &verify, sizeof(DeviceConfig_t)) != 0)
    {
        return 2;  /* 主区写入校验失败 */
    }

    /* 3. 写备份区 */
    W25QXX_Write((uint8_t *)cfg, CONFIG_BACKUP_ADDR, sizeof(DeviceConfig_t));

    /* 4. 读回备份区校验 */
    W25QXX_Read((uint8_t *)&verify, CONFIG_BACKUP_ADDR, sizeof(DeviceConfig_t));
    if (memcmp(cfg, &verify, sizeof(DeviceConfig_t)) != 0)
    {
        return 3;  /* 备份区写入校验失败 */
    }

    return 0;
}

/* ================================================================
 * AppConfig_Init
 *   返回值: 0 = 成功, 非0 = 失败
 * ================================================================ */
uint8_t AppConfig_Init(void)
{
    if (AppConfig_Load(&s_config) == 0)
    {
        s_initialized = 1;
        return 0;
    }

    /* Load 失败 — 生成默认配置并保存 */
    AppConfig_SetDefaults(&s_config);
    if (AppConfig_Save(&s_config) == 0)
    {
        s_initialized = 1;
        return 0;
    }

    /* 保存也失败 — 模块未初始化 */
    s_initialized = 0;
    return 1;
}

/* ================================================================
 * AppConfig_Get
 * ================================================================ */
const DeviceConfig_t *AppConfig_Get(void)
{
    if (!s_initialized) return NULL;
    return &s_config;
}

/* ================================================================
 * AppConfig_SetMode
 *   mode: 0/1/2 (ATT_MODE_ENTRY / ATT_MODE_EXIT / ATT_MODE_BOTH)
 *   返回值: 0 = 成功, 非0 = 失败
 * ================================================================ */
uint8_t AppConfig_SetMode(uint8_t mode)
{
    if (!s_initialized) return 1;
    if (mode > 2) return 2;

    s_config.attendanceMode = mode;

    /* 重新计算 checksum */
    s_config.checksum = Config_Crc16Xmodem((const uint8_t *)&s_config, 30);

    /* 写入 Flash */
    if (AppConfig_Save(&s_config) != 0)
    {
        return 3;
    }

    return 0;
}

/* ================================================================
 * AppConfig_Update
 *   同时更新 deviceId 和 attendanceMode
 *   deviceId: 有效范围 1~9999（<=0 返回错误）
 *   mode:     有效范围 0~2（ >2 返回错误）
 *   返回值: 0 = 成功, 非0 = 失败
 *   保存失败时不会污染当前 RAM 配置
 * ================================================================ */
uint8_t AppConfig_Update(uint32_t deviceId, uint8_t mode)
{
    if (!s_initialized) return 1;
    if (deviceId < 1) return 2;
    if (mode > 2) return 3;

    /* 复制当前配置到临时变量 */
    DeviceConfig_t temp;
    memcpy(&temp, &s_config, sizeof(DeviceConfig_t));

    /* 修改临时变量 */
    temp.deviceId       = deviceId;
    temp.attendanceMode = mode;

    /* 重新计算 checksum */
    temp.checksum = Config_Crc16Xmodem((const uint8_t *)&temp, 30);

    /* 写入 Flash */
    if (AppConfig_Save(&temp) != 0)
    {
        return 4;  /* 写入失败，s_config 保持原值 */
    }

    /* 写入成功，更新 RAM 配置 */
    memcpy(&s_config, &temp, sizeof(DeviceConfig_t));

    return 0;
}
