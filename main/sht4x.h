#ifndef SHT4X_H
#define SHT4X_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * SHT4x（SHT40 / SHT41 / SHT45）数字温湿度传感器驱动 —— ESP-IDF i2c_master 版
 *
 * 代码来源与复用说明：
 *   - 芯片手册：Sensirion SHT4x Datasheet v2 (July 2021) —— 命令表、CRC 参数、
 *     换算公式、时序均取自手册，已写入本地知识库。
 *   - 例程算法：厂商 STM32 例程 sht4x.c / iic.c（软件 I2C）—— CRC-8 实现与
 *     定点换算写法复用于此，但 I/O 层换成本机 SDK 的 i2c_master 主机驱动。
 *   - LUXAR 驱动库：库内**没有** SHT4x 驱动（仅 sh1106_1p3_landscape），
 *     因此本文件是「例程移植 + 手册实现」，不是驱动库复用。
 *
 * 硬件：
 *   - I²C 从机地址 0x44（SHT40-AD1B / SHT41-AD1B / SHT45-AD1B；
 *     **SHT40-BD1B 为 0x45**，如有需要请改 SHT4X_I2C_ADDR）
 *   - 引脚与总线见 board_pins.h（SDA=GPIO25 / SCL=GPIO26，独立总线 I2C_NUM_1）
 *
 * 手册要点（直接影响本驱动的写法，见手册 4.1–4.3）：
 *   1. SHT4x **不支持时钟拉伸**：忙时对读头返回 NACK，
 *      所以必须「发命令 → 主机按 tMEAS 自行延时 → 再读」。
 *   2. 测量数据**只能被读取一次**：首个被 ACK 的读头之后数据即被删除，
 *      重试必须重新发测量命令（本驱动的重试逻辑即按此实现）。
 *   3. 返回 6 字节定序：[T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC]。
 */

/* I²C 从机地址 */
#define SHT4X_I2C_ADDR 0x44

/* ---------------- 命令（手册 Table 7） ---------------- */
#define SHT4X_CMD_MEAS_HIGH          0xFD  /* 高精度（高重复性） */
#define SHT4X_CMD_MEAS_MEDIUM        0xF6  /* 中等精度 */
#define SHT4X_CMD_MEAS_LOW           0xE0  /* 最低精度 */
#define SHT4X_CMD_READ_SERIAL        0x89  /* 读序列号 */
#define SHT4X_CMD_SOFT_RESET         0x94  /* 软复位 */
#define SHT4X_CMD_HEATER_200MW_1S    0x39  /* 200 mW / 1 s */
#define SHT4X_CMD_HEATER_200MW_100MS 0x32  /* 200 mW / 0.1 s */
#define SHT4X_CMD_HEATER_110MW_1S    0x2F  /* 110 mW / 1 s */
#define SHT4X_CMD_HEATER_110MW_100MS 0x24  /* 110 mW / 0.1 s */
#define SHT4X_CMD_HEATER_20MW_1S     0x1E  /* 20 mW / 1 s */
#define SHT4X_CMD_HEATER_20MW_100MS  0x15  /* 20 mW / 0.1 s */

/* 主机等待时长（手册 Table 4 的最大值 + 余量） */
#define SHT4X_WAIT_HIGH_MS  10  /* 高重复性最大 8.2 ms */
#define SHT4X_WAIT_MED_MS    5  /* 中等重复性最大 4.5 ms */
#define SHT4X_WAIT_LOW_MS    2  /* 低重复性最大 1.7 ms */
#define SHT4X_WAIT_RESET_MS  1  /* 软复位最大 1 ms */

/* 一次测量结果。c10/rh10 为放大 10 倍的整数（避免依赖浮点 printf），
 * 同时提供浮点值供上层计算使用。 */
typedef struct {
    int16_t  temperature_c10;  /* 温度 ×10（如 253 表示 25.3 °C） */
    uint16_t humidity_rh10;    /* 相对湿度 ×10（如 456 表示 45.6 %RH，已裁剪到 0–100 %） */
    float    temperature_c;
    float    humidity_rh;
    uint16_t t_raw;
    uint16_t rh_raw;
} sht4x_sample_t;

/** 建 I2C 主机总线并挂载 SHT4x 设备（不探测，不阻塞）。
 *  返回 ESP_OK 表示总线与设备已就绪。可重复调用（幂等）。 */
esp_err_t sht4x_init(void);

/** 释放 I2C 总线与设备句柄。用于把引脚让给总线诊断等需要独占的场景；
 *  释放后可再次调用 sht4x_init() 重新装回。 */
void sht4x_deinit(void);

/** 软复位（命令 0x94，等待 tSR）。 */
esp_err_t sht4x_soft_reset(void);

/** 探测总线上是否存在 SHT4x（i2c_master_probe）。
 *  存在返回 ESP_OK；不存在返回底层错误（通常是 ESP_ERR_TIMEOUT / ESP_FAIL）。 */
esp_err_t sht4x_probe(void);

/** 以高精度模式读一次温湿度（发 0xFD → 等待 → 读 6 字节 → CRC 校验 → 换算）。
 *  内部最多重试 3 次（每次重试都重发命令，因为数据只能读一次）。
 *  out 不可为 NULL。 */
esp_err_t sht4x_read(sht4x_sample_t *out);

/** 读 32-bit 序列号（命令 0x89，两个 16-bit 字各带 CRC）。
 *  高 16 位为先接收的第一个字。 */
esp_err_t sht4x_read_serial(uint32_t *serial);

/** SHT4x CRC-8：多项式 0x31、初值 0xFF、不反射、最终异或 0x00。
 *  手册测试向量：CRC(0xBEEF) == 0x92（已在开发机复现验证）。 */
uint8_t sht4x_crc8(const uint8_t *data, size_t len);

/** 算法自检（纯计算，不碰 I²C）：校验 CRC 与换算公式对数据手册给定期望值的一致性。
 *  本机无宿主编译器可运行 C 单元测试，故把自检放进固件，由目标硬件在启动时执行。
 *  返回 ESP_OK 表示全部通过；否则返回首个失败项的错误码并在日志中给出明细。
 *  自检向量（与手册对照）：
 *    CRC(0xBEEF)=0x92（手册 Table 6）、CRC(0x0000)=0x81、CRC(0xFFFF)=0xAC
 *    T(0)=-45.0 °C、T(65535)=130.0 °C、T(32768)=42.5 °C
 *    RH(0) 裁剪为 0.0 %、RH(65535) 裁剪为 100.0 %、RH(32768)=56.5 % */
esp_err_t sht4x_selftest(void);

#endif /* SHT4X_H */
