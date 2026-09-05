/**
 * @file    rc522.h
 * @brief   MFRC522/RC522 RFID读写芯片驱动 - 主头文件
 * @details 本驱动采用分层设计，通过 RC522_IO_t 接口与硬件平台解耦。
 *          用户只需实现 spi_transfer、cs_control、rst_control、delay_us/delay_ms
 *          五个回调函数即可将驱动移植到任意平台。
 *
 *          使用步骤：
 *          1. 实现 RC522_IO_t 接口中的回调函数
 *          2. 调用 RC522_Init() 初始化驱动
 *          3. 调用 RC522_ScanCard() 寻卡
 *          4. 调用 RC522_AuthState() 认证后读写卡数据
 *          5. 调用 RC522_Halt() 使卡进入休眠
 *
 * @note    编码：UTF-8
 */

#ifndef RC522_H
#define RC522_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================
 *  1. 平台抽象接口 (Platform IO Interface)
 *     用户必须实现此接口中的所有回调函数
 * ====================================================== */
typedef struct {
    /**
     * @brief 片选(CS/NSS)引脚控制
     * @param level 0=拉低(选中), 1=拉高(释放)
     */
    void (*cs_control)(uint8_t level);

    /**
     * @brief 复位(RST)引脚控制
     * @param level 0=拉低(复位), 1=拉高(正常工作)
     */
    void (*rst_control)(uint8_t level);

    /**
     * @brief SPI全双工字节传输
     * @param data 要发送的数据字节
     * @return 接收到的数据字节
     * @note 对于软件模拟SPI，此函数需自行控制 SCK 和 MOSI，
     *       并读取 MISO。数据在 SCK 上升沿发送，下降沿接收。
     *       时钟极性(CPOL)=0, 时钟相位(CPHA)=0 (Mode 0)。
     */
    uint8_t (*spi_transfer)(uint8_t data);

    /**
     * @brief 微秒级延时
     * @param us 延时微秒数
     */
    void (*delay_us)(uint32_t us);

    /**
     * @brief 毫秒级延时
     * @param ms 延时毫秒数
     */
    void (*delay_ms)(uint32_t ms);
} RC522_IO_t;

/* ======================================================
 *  2. 状态码 (Status Codes)
 * ====================================================== */
#define RC522_OK            ((char)0)      /**< 操作成功 */
#define RC522_ERR           ((char)(-1))   /**< 操作失败 */
#define RC522_ERR_NO_TAG    ((char)(-2))   /**< 无标签(卡)响应 */
#define RC522_ERR_COLLISION ((char)(-3))   /**< 防碰撞失败 */

/* ======================================================
 *  3. MF522 芯片命令 (PCD - Proximity Coupling Device Commands)
 * ====================================================== */
#define RC522_CMD_IDLE          0x00    /**< 取消当前命令 */
#define RC522_CMD_AUTHENT       0x0E    /**< 密码验证 */
#define RC522_CMD_RECEIVE       0x08    /**< 接收数据 */
#define RC522_CMD_TRANSMIT      0x04    /**< 发送数据 */
#define RC522_CMD_TRANSCEIVE    0x0C    /**< 发送并接收数据 */
#define RC522_CMD_RESETPHASE    0x0F    /**< 复位 */
#define RC522_CMD_CALCCRC       0x03    /**< CRC 计算 */

/* ======================================================
 *  4. Mifare 卡片命令 (PICC - Proximity Integrated Circuit Card Commands)
 * ====================================================== */
#define RC522_PICC_REQIDL       0x26    /**< 寻卡：只寻处于空闲状态的卡 */
#define RC522_PICC_REQALL       0x52    /**< 寻卡：寻所有卡 */
#define RC522_PICC_ANTICOLL1    0x93    /**< 防碰撞：一级 */
#define RC522_PICC_ANTICOLL2    0x95    /**< 防碰撞：二级 */
#define RC522_PICC_AUTHENT1A    0x60    /**< 验证 A 密钥 */
#define RC522_PICC_AUTHENT1B    0x61    /**< 验证 B 密钥 */
#define RC522_PICC_READ         0x30    /**< 读取块 */
#define RC522_PICC_WRITE        0xA0    /**< 写入块 */
#define RC522_PICC_DECREMENT    0xC0    /**< 扣款 */
#define RC522_PICC_INCREMENT    0xC1    /**< 充值 */
#define RC522_PICC_RESTORE      0xC2    /**< 恢复数据到缓冲区 */
#define RC522_PICC_TRANSFER     0xB0    /**< 将缓冲区数据存入 EEPROM */
#define RC522_PICC_HALT         0x50    /**< 休眠 */

/* ======================================================
 *  5. MF522 寄存器地址 (Register Addresses)
 *     - PAGE 0 ~ PAGE 3
 * ====================================================== */
// ------ PAGE 0 ------
#define RC522_REG_RFU00             0x00    /**< 保留 */
#define RC522_REG_COMMAND           0x01    /**< 命令寄存器 */
#define RC522_REG_COMIEN            0x02    /**< 中断使能 */
#define RC522_REG_DIVLEN            0x03    /**< 分频中断使能 */
#define RC522_REG_COMIRQ            0x04    /**< 中断请求 */
#define RC522_REG_DIVIRQ            0x05    /**< 分频中断请求 */
#define RC522_REG_ERROR             0x06    /**< 错误标志 */
#define RC522_REG_STATUS1           0x07    /**< 状态寄存器 1 */
#define RC522_REG_STATUS2           0x08    /**< 状态寄存器 2 */
#define RC522_REG_FIFODATA          0x09    /**< FIFO 数据寄存器 */
#define RC522_REG_FIFOLEVEL         0x0A    /**< FIFO 电平寄存器 */
#define RC522_REG_WATERLEVEL        0x0B    /**< FIFO 水线寄存器 */
#define RC522_REG_CONTROL           0x0C    /**< 控制寄存器 */
#define RC522_REG_BITFRAMING        0x0D    /**< 位帧调整寄存器 */
#define RC522_REG_COLL              0x0E    /**< 碰撞检测寄存器 */
#define RC522_REG_RFU0F             0x0F    /**< 保留 */
// ------ PAGE 1 ------
#define RC522_REG_RFU10             0x10    /**< 保留 */
#define RC522_REG_MODE              0x11    /**< 模式寄存器 */
#define RC522_REG_TXMODE            0x12    /**< 发送模式 */
#define RC522_REG_RXMODE            0x13    /**< 接收模式 */
#define RC522_REG_TXCONTROL         0x14    /**< 发送控制 */
#define RC522_REG_TXAUTO            0x15    /**< 发送自动 */
#define RC522_REG_TXSEL             0x16    /**< 发送通道选择 */
#define RC522_REG_RXSEL             0x17    /**< 接收通道选择 */
#define RC522_REG_RXTHRESHOLD       0x18    /**< 接收阈值 */
#define RC522_REG_DEMOD             0x19    /**< 解调控制 */
#define RC522_REG_RFU1A             0x1A    /**< 保留 */
#define RC522_REG_RFU1B             0x1B    /**< 保留 */
#define RC522_REG_MIFARE            0x1C    /**< Mifare 专用寄存器 */
#define RC522_REG_RFU1D             0x1D    /**< 保留 */
#define RC522_REG_RFU1E             0x1E    /**< 保留 */
#define RC522_REG_SERIALSPEED       0x1F    /**< 串行速度 */
// ------ PAGE 2 ------
#define RC522_REG_RFU20             0x20    /**< 保留 */
#define RC522_REG_CRCRESULTM        0x21    /**< CRC 结果高位 */
#define RC522_REG_CRCRESULTL        0x22    /**< CRC 结果低位 */
#define RC522_REG_RFU23             0x23    /**< 保留 */
#define RC522_REG_MODWIDTH          0x24    /**< 调制宽度 */
#define RC522_REG_RFCFG             0x26    /**< 射频配置 */
#define RC522_REG_GSN               0x27    /**< 发射器增益 */
#define RC522_REG_CWGSCFG           0x28    /**< CW 增益控制 */
#define RC522_REG_MODGSCFG          0x29    /**< 调制增益控制 */
#define RC522_REG_TMODE             0x2A    /**< 定时器模式 */
#define RC522_REG_TPRESCALER        0x2B    /**< 定时器预分频 */
#define RC522_REG_TRELOADH          0x2C    /**< 定时器重载值高位 */
#define RC522_REG_TRELOADL          0x2D    /**< 定时器重载值低位 */
#define RC522_REG_TCOUNTERVALUEH    0x2E    /**< 定时器计数值高位 */
#define RC522_REG_TCOUNTERVALUEL    0x2F    /**< 定时器计数值低位 */
// ------ PAGE 3 ------
#define RC522_REG_RFU30             0x30    /**< 保留 */
#define RC522_REG_TESTSEL1          0x31    /**< 测试选择 1 */
#define RC522_REG_TESTSEL2          0x32    /**< 测试选择 2 */
#define RC522_REG_TESTPINEN         0x33    /**< 测试引脚使能 */
#define RC522_REG_TESTPINVALUE      0x34    /**< 测试引脚值 */
#define RC522_REG_TESTBUS           0x35    /**< 测试总线 */
#define RC522_REG_AUTOTEST          0x36    /**< 自动测试 */
#define RC522_REG_VERSION           0x37    /**< 版本号 */
#define RC522_REG_ANALOGTEST        0x38    /**< 模拟测试 */
#define RC522_REG_TESTDAC1          0x39    /**< DAC 测试 1 */
#define RC522_REG_TESTDAC2          0x3A    /**< DAC 测试 2 */
#define RC522_REG_TESTADC           0x3B    /**< ADC 测试 */
#define RC522_REG_RFU3C             0x3C    /**< 保留 */
#define RC522_REG_RFU3D             0x3D    /**< 保留 */
#define RC522_REG_RFU3E             0x3E    /**< 保留 */
#define RC522_REG_RFU3F             0x3F    /**< 保留 */

/** FIFO 最大长度 */
#define RC522_FIFO_MAX_LEN          64

/** Mifare 1K 卡每块字节数 */
#define RC522_BLOCK_SIZE            16

/** Mifare 1K 卡扇区数 */
#define RC522_SECTOR_COUNT          16

/** Mifare 1K 卡每扇区块数 */
#define RC522_BLOCKS_PER_SECTOR     4

/* ======================================================
 *  6. 驱动 API (Application Programming Interface)
 * ====================================================== */

// ---------- 初始化与配置 ----------

/**
 * @brief 初始化 RC522 芯片
 * @param io 平台 IO 接口指针，必须指向有效的 RC522_IO_t 结构体
 * @note  调用此函数后，RC522 完成复位并进入初始状态。
 *        用户需确保 io 结构体中的所有回调函数已正确实现。
 */
void RC522_Init(RC522_IO_t *io);

/**
 * @brief 复位 RC522 芯片
 * @note  软复位芯片内部寄存器到默认值
 */
void RC522_Reset(void);

/**
 * @brief 配置 ISO14443 A 类通讯类型
 * @param type 类型，当前仅支持 'A'(ISO14443_A)
 */
void RC522_ConfigISOType(char type);

/**
 * @brief 开启天线
 * @note 每次通讯后天线会自动关闭，需要重新开启
 */
void RC522_AntennaOn(void);

/**
 * @brief 关闭天线
 */
void RC522_AntennaOff(void);

// ---------- 卡片基本操作 ----------

/**
 * @brief 寻卡 (Request)
 * @param req_code 寻卡模式：
 *        - RC522_PICC_REQALL (0x52): 寻所有卡
 *        - RC522_PICC_REQIDL (0x26): 只寻未休眠的卡
 * @param pTagType 输出参数，卡片类型码 (2 字节)
 *        - 0x4400 = Mifare UltraLight
 *        - 0x0400 = Mifare One(S50)
 *        - 0x0200 = Mifare One(S70)
 *        - 0x0800 = Mifare Pro(X)
 *        - 0x4403 = Mifare DESFire
 * @return RC522_OK 成功，RC522_ERR_NO_TAG 无卡，RC522_ERR 失败
 */
char RC522_Request(uint8_t req_code, uint8_t *pTagType);

/**
 * @brief 防碰撞 (Anticollision)
 * @param pSnr 输出参数，卡片序列号 (4 字节)
 * @return RC522_OK 成功，其他值失败
 * @note 当多张卡同时在场时，此函数通过算法选出一张卡
 */
char RC522_Anticoll(uint8_t *pSnr);

/**
 * @brief 选卡 (Select)
 * @param pSnr 卡片序列号 (4 字节，由 RC522_Anticoll 获取)
 * @return RC522_OK 成功，其他值失败
 * @note 选中卡后，后续操作只针对此卡
 */
char RC522_Select(uint8_t *pSnr);

/**
 * @brief 验证密码 (Authentication)
 * @param auth_mode 验证模式：RC522_PICC_AUTHENT1A (0x60) 或 RC522_PICC_AUTHENT1B (0x61)
 * @param addr     要验证的块地址
 * @param pKey     密钥 (6 字节)，默认出厂密钥为 {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}
 * @param pSnr     卡片序列号 (4 字节)
 * @return RC522_OK 成功，其他值失败
 * @note 验证通过后才能进行读写操作。密钥存储在每个扇区的第 3 块。
 */
char RC522_AuthState(uint8_t auth_mode, uint8_t addr, uint8_t *pKey, uint8_t *pSnr);

/**
 * @brief 读块数据 (Read)
 * @param addr  块地址 (Mifare 1K: 0~63)
 * @param pData 输出参数，读取的数据 (16 字节)
 * @return RC522_OK 成功，其他值失败
 */
char RC522_Read(uint8_t addr, uint8_t *pData);

/**
 * @brief 写块数据 (Write)
 * @param addr  块地址 (Mifare 1K: 0~63)
 * @param pData 要写入的数据 (16 字节)
 * @return RC522_OK 成功，其他值失败
 * @note 块 0 通常不可写（存有厂商数据），扇区第 3 块存储密钥需谨慎操作
 */
char RC522_Write(uint8_t addr, uint8_t *pData);

/**
 * @brief 使卡进入休眠状态 (Halt)
 * @return RC522_OK
 */
char RC522_Halt(void);

/**
 * @brief 等待卡离开射频场
 * @note 阻塞直到检测不到卡
 */
void RC522_WaitCardOff(void);

// ---------- 高级数据操作 ----------

/**
 * @brief 读取指定扇区中的某个块
 * @param secNum 扇区号 (0~15)
 * @param blkNum 块号 (0~3)
 * @param buf    输出缓冲区 (至少 16 字节)
 * @return RC522_OK 成功，其他值失败
 */
char RC522_ReadBlock(uint8_t secNum, uint8_t blkNum, uint8_t buf[16]);

/**
 * @brief 写入指定扇区中的某个块
 * @param secNum 扇区号 (0~15)
 * @param blkNum 块号 (0~3)，块0通常不可写
 * @param buf    要写入的数据 (16 字节)
 * @return RC522_OK 成功，其他值失败
 */
char RC522_WriteBlock(uint8_t secNum, uint8_t blkNum, uint8_t buf[16]);

/**
 * @brief 读取整个扇区数据
 * @param secNum 扇区号 (0~15)
 * @param buf    输出缓冲区 (64 字节 = 4块 x 16字节)
 * @return RC522_OK 成功，其他值失败
 */
char RC522_ReadSector(uint8_t secNum, uint8_t buf[64]);

/**
 * @brief 写入扇区数据（仅前3块，第4块为密钥区）
 * @param secNum 扇区号 (0~15)
 * @param buf    要写入的数据 (48 字节 = 3块 x 16字节)
 * @return RC522_OK 成功，其他值失败
 */
char RC522_WriteSector(uint8_t secNum, uint8_t buf[48]);

/**
 * @brief 快速寻卡：一次完成 Request + Anticollision + Select
 * @param cn 输出参数，卡片序列号 (4 字节)
 * @return RC522_OK 成功，其他值失败
 */
char RC522_ScanCard(uint8_t cn[4]);

/**
 * @brief 读取整张卡片所有扇区数据 (16扇区 x 64字节)
 * @param data 输出缓冲区 [64][16] 字节
 * @return RC522_OK 成功，其他值失败
 * @note 自动处理寻卡、防碰撞、选卡、密码验证等流程
 */
char RC522_ReadAllSectors(uint8_t data[64][16]);

/**
 * @brief 写入整张卡片所有扇区数据 (16扇区 x 48字节)
 * @param data 要写入的数据 [64][16] 字节
 * @return RC522_OK 成功，其他值失败
 * @note 只写入每个扇区的前3块，跳过密钥区
 */
char RC522_WriteAllSectors(uint8_t data[64][16]);

/**
 * @brief 初始化 RC522 STM32 平台
 * @note  此函数完成:
 *        1. DWT 定时器初始化 (微秒延时)
 *        2. NFC 模块接地引脚拉低 (如果有定义)
 *        3. 调用 RC522_Init() 初始化 RC522 芯片
 *
 *        在 FreeRTOS 任务中调用此函数前需确保 HAL_Init()
 *        和 MX_GPIO_Init() 等系统初始化已完成。
 */
void RC522_Platform_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* RC522_H */
