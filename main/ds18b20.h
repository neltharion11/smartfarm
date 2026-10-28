#ifndef DS18B20_H
#define DS18B20_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * DS18B20 水温探头驱动（Maxim 1-Wire 数字温度计）—— ESP-IDF 版
 *
 * 代码来源：
 *   - 芯片手册：Maxim DS18B20 数据手册 19-7487 Rev 4 —— 1-Wire 时序、ROM/功能命令、
 *     暂存器布局、温度寄存器格式、CRC 多项式均取自手册（已写入本地知识库）。
 *   - 例程复用：厂商 STM32 例程 ds18b20.c 的 CRC-8（反射多项式 0x8C、初值 0x00）、
 *     “raw×5/8 四舍五入到 0.1 ℃”的定点换算，以及多重校验思路
 *     （全零检测 + CRC + 范围检测）。
 *   - I/O 层**重写**：例程用 __disable_irq()/__enable_irq() 关全局中断保护时序；
 *     在 ESP-IDF 双核 FreeRTOS 下这样做会破坏调度（系统 tick、看门狗、WiFi）。
 *     本实现改用 portENTER_CRITICAL 临界区，且只在**短时隙（≤60 µs）**与
 *     **复位脉冲（约 1 ms）**内持有，不长时间屏蔽中断。
 *
 * 硬件（见 board_pins.h）：
 *   - DQ = BOARD_DS18B20_DQ（GPIO4）
 *   - DQ 必须外接上拉电阻到 3.3 V（手册称约 5 kΩ，典型 4.7 kΩ）
 *   - 推荐外部 3 线供电（VDD/GND/DQ），不用寄生供电
 *
 * 用法（非阻塞式，适合 1 Hz 刷新）：
 *   ds18b20_init();                       // 内部启动首次转换
 *   vTaskDelay(1000ms);                   // 等首次转换完成
 *   ds18b20_read_temperature(&c10);       // 读上一次结果，并自动启动下一次转换
 */

#define DS18B20_ROM_SIZE        8
#define DS18B20_SCRATCHPAD_SIZE 9

/* 温度读取失败标记：超出 DS18B20 量程（-55.0 ~ +125.0 ℃） */
#define DS18B20_TEMP_ERROR ((int16_t)32767)

/** 初始化 DQ 引脚并启动首次温度转换。
 *  返回 ESP_OK 表示引脚配置完成（不代表探头一定在线，用 ds18b20_probe 判定）。 */
esp_err_t ds18b20_init(void);

/** 复位 + 存在脉冲检测。返回 ESP_OK 表示探头在线应答。 */
esp_err_t ds18b20_probe(void);

/** 读 64 位 ROM 码（命令 0x33）。
 *  仅适用于总线上只有一颗探头的情形；多颗时须改用 Search ROM(0xF0)。
 *  rom[0] 应为家族码 0x28，rom[7] 为前 56 字节的 CRC-8（本函数会校验）。 */
esp_err_t ds18b20_read_rom(uint8_t rom[DS18B20_ROM_SIZE]);

/** 启动一次温度转换（SKIP ROM + Convert T）。
 *  12 位分辨率需约 750 ms 完成（手册 Table 2）。 */
esp_err_t ds18b20_start_conversion(void);

/** 读取上一次转换结果，并**自动启动下一次转换**。
 *  内部流程：SKIP ROM + Read Scratchpad(9B) → 全零检测 → CRC-8 校验
 *            → 温度换算(0.1 ℃) → 范围检测 → 启动下一次转换。
 *  temp_c10 返回温度 ×10（例 243 表示 24.3 ℃）；失败时置 DS18B20_TEMP_ERROR。
 *
 *  注意（手册 Table 1）：温度寄存器上电复位值为 +85.0 ℃。若在首次转换完成前
 *  读取会得到该假值，本函数通过 s_started 标记拒绝“从未启动转换”时的读数。 */
esp_err_t ds18b20_read_temperature(int16_t *temp_c10);

/** Dallas 1-Wire CRC-8：反射多项式 0x8C、初值 0x00。
 *  等价于标准目录中的 CRC-8/MAXIM-DOW（check 值 0xA1，已实测复核）。 */
uint8_t ds18b20_crc8(const uint8_t *data, size_t len);

/** 算法自检（纯计算，不访问总线）。校验 CRC 与温度换算，
 *  测试数据取自手册 Table 1 的 10 组 hex↔温度对照。 */
esp_err_t ds18b20_selftest(void);

/** 总线电气诊断，用于定位"存在脉冲正常、但数据全读成 0xFF"这类故障。
 *
 *  做三件事：
 *   1. **内部下拉法**判断外部上拉是否存在（与 I²C 那次同一判据）：
 *      引脚配输入 + 内部下拉（约 45 kΩ）读电平，外部 4.7 kΩ 上拉会把它压成高。
 *   2. **释放后上升时间**多点采样，直接反映上拉强度。
 *   3. **复位后波形**多点采样，显示存在脉冲形状。
 *
 *  为什么这项检查关键：1-Wire 靠上拉把总线拉高来传"1"。若只有内部 45 kΩ 上拉，
 *  长线缆电容会让总线在器件采样窗口（拉低后 15–60 µs）内升不到 VIH，
 *  器件便把命令位全读成 0 → 命令无效 → 器件只回存在脉冲、永不回数据
 *  （表现为 ROM/暂存器读出全 0xFF）。 */
void ds18b20_diag(void);

#endif /* DS18B20_H */
