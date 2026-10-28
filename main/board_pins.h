#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/**
 * 智慧农场 ESP32 板级引脚分配 —— 单一权威表
 *
 * 芯片：ESP32-D0WD-V3（WROOM-32 模组）
 *
 * 已占用 / 不可用（分配新增外设前请先读这段）：
 *   GPIO1  / GPIO3   控制台 UART0（启动日志已确认 "GPIO 3 and 1 are used as console UART I/O pins"）
 *   GPIO4            DS18B20 水温探头 1-Wire DQ
 *   GPIO6  – GPIO11  内部 SPI Flash，绝对不可用
 *   GPIO18 / GPIO19 / GPIO23   继电器输出 1/2/3
 *   GPIO21 / GPIO22  SH1106 OLED 的 I²C（见 sh1106.h）
 *   GPIO25 / GPIO26  SHT40 空气温湿度的 I²C
 *   GPIO27 / GPIO32 / GPIO33   继电器测试输入 1/2/3（内部下拉、高电平触发）
 *   GPIO34 – GPIO39  仅输入（input-only）**且无内部上拉/下拉**；既不能做输出，也不能配置内部下拉
 *   GPIO0/2/5/12/15  strapping 引脚，默认避开（影响启动模式/电压）
 *   GPIO16 / GPIO17  若模组带 PSRAM 则被占用，本板按避开处理
 *
 * 仍空闲可用：GPIO13、GPIO14
 *
 * 说明：本文件只做「分配记录」，是新增引脚时的查重依据；
 *       各外设驱动仍以自身头文件中的宏为准（drift 风险见下方 OLED 条目）。
 */

/* ---------------- SH1106 1.3" OLED（中景园横屏 128x64） ----------------
 * 控制器 SH1106 / SSD1106G，7 位 I²C 地址 0x3C。
 * 权威定义在 main/sh1106.h（SH1106_I2C_GPIO_SDA / _SCL / _I2C_ADDR）；
 * 此处仅为查重用途登记，修改请同步两处。 */
#define BOARD_OLED_I2C_SDA   21
#define BOARD_OLED_I2C_SCL   22
#define BOARD_OLED_I2C_ADDR  0x3C

/* ---------------- SHT40 温湿度传感器（Sensirion SHT4x） ----------------
 * 7 位 I²C 地址固定 0x44（与 OLED 的 0x3C 不冲突）。
 * 供电 1.08–3.6 V（典型 3.3 V）；SCL/SDA 均需上拉到 VDD；支持 400 kHz 快速模式。
 *
 * 分配方案：占用 GPIO25 / GPIO26 作为独立 I²C 总线（I2C_NUM_1），
 *           与 OLED 的 I2C_NUM_0 分离，避免共总线时的时序耦合与阻塞。
 *
 * 备选（引脚紧张时）：SHT40 可直接挂到 OLED 同一条总线
 *           （I2C_NUM_0，SDA=21/SCL=22），地址不冲突，无需额外引脚。
 *           届时只需把下方 PORT 改为 0、SDA/SCL 改为 21/22。
 *
 * 选择依据：GPIO25/26 非 strapping、非 Flash、非控制台、非仅输入，
 *           且在常见 ESP32 开发板上均已引出。 */
#define BOARD_SHT40_I2C_PORT 1     /* 1 = I2C_NUM_1（独立总线） */
#define BOARD_SHT40_I2C_SDA  25
#define BOARD_SHT40_I2C_SCL  26
#define BOARD_SHT40_I2C_ADDR 0x44

/* SHT4x 测量命令（来自 ST TN1426 Table 6；校验和与换算公式见 Sensirion SHT4x 数据手册）。
 * 仅登记备用，驱动实现在后续阶段加入。 */
#define BOARD_SHT40_CMD_MEAS_HIGH   0xFD  /* 最高精度（高重复性） */
#define BOARD_SHT40_CMD_MEAS_MEDIUM 0xF6  /* 中等精度 */
#define BOARD_SHT40_CMD_MEAS_LOW    0xE0  /* 最低精度 */
#define BOARD_SHT40_CMD_READ_SERIAL 0x89  /* 读序列号 */
#define BOARD_SHT40_CMD_SOFT_RESET  0x94  /* 软复位 */
#define BOARD_SHT40_CMD_HEATER_HIGH 0x39  /* 最高功率加热 1 s */

/* ---------------- DS18B20 水温探头（Maxim 1-Wire 数字温度计） ----------------
 * 用途分工：SHT40 测空气温湿度；DS18B20 测**水温**（防水探头）。
 *
 * 与 I²C 器件的关键差别：1-Wire 总线**只需 1 个数据引脚**（DQ），
 * 不像 I²C 需要 SDA+SCL 两个。加上 VDD/GND 即所谓“3 线制”。
 *
 * 分配：GPIO4 作为 DQ。选择依据：非 strapping、非 Flash、非控制台 UART、
 *       非仅输入，且在常见 ESP32 开发板上均已引出。
 *
 * 硬件要求（见 DS18B20 手册）：
 *   - DQ 需**外部上拉电阻到 3.3 V**（手册称约 5 kΩ，典型 4.7 kΩ）。
 *     总线空闲为高；被持续拉低超过 480 µs 会导致总线上所有器件复位。
 *   - 推荐**外部 3 线供电**（VDD=3.3 V、GND、DQ），不用寄生供电：
 *     寄生供电要求在 750 ms 转换期间保持强上拉且禁止总线活动，
 *     而水温探头线缆较长，寄生供电易不稳定。
 *   - 探头线序常见：红=VDD、黑=GND、黄/白=DQ（以实物为准）。
 *
 * 上电陷阱：温度寄存器上电复位值为 **+85 °C**（0x0550）。首次转换完成前
 *           读到的 85 °C 是假值，必须识别并丢弃。
 *
 * 扩展性：每颗 DS18B20 有唯一 64 位 ROM 码，**同一根 DQ 可挂多颗**
 *         （多个水箱共用一个引脚），用 Match ROM(0x55)/Search ROM(0xF0) 区分。 */
#define BOARD_DS18B20_DQ         4
#define BOARD_DS18B20_PULLUP_OHM 4700  /* 外部上拉标称值，文档用途 */

/* DS18B20 命令（Maxim 手册 Table 3 与 ROM 命令） */
#define BOARD_DS18B20_CMD_CONVERT_T     0x44  /* 启动温度转换 */
#define BOARD_DS18B20_CMD_READ_SCRATCH  0xBE  /* 读暂存器（9 字节） */
#define BOARD_DS18B20_CMD_WRITE_SCRATCH 0x4E  /* 写暂存器（TH/TL/配置） */
#define BOARD_DS18B20_CMD_COPY_SCRATCH  0x48  /* 暂存器拷入 EEPROM */
#define BOARD_DS18B20_CMD_RECALL_E2     0xB8  /* EEPROM 回召到暂存器 */
#define BOARD_DS18B20_CMD_READ_POWER    0xB4  /* 读供电方式 */
#define BOARD_DS18B20_CMD_SKIP_ROM      0xCC  /* 跳过 ROM（单器件） */
#define BOARD_DS18B20_CMD_MATCH_ROM     0x55  /* 匹配 ROM（多器件） */
#define BOARD_DS18B20_CMD_READ_ROM      0x33  /* 读 ROM（仅单器件） */
#define BOARD_DS18B20_CMD_SEARCH_ROM    0xF0  /* 搜索 ROM */
#define BOARD_DS18B20_CMD_ALARM_SEARCH  0xEC  /* 报警搜索 */

/* ---------------- 继电器输出（3 路）+ 测试输入（3 路） ----------------
 * 分工：3 路输出驱动继电器模块；3 路测试输入（内部下拉、高电平触发）
 *       用于手动验证继电器控制——给某路输入加 3.3 V，对应继电器即吸合。
 *
 * 选脚依据（重要，解释了为什么不能用某些脚）：
 *   - **必须避开 GPIO34–GPIO39**：它们是「仅输入」且**没有内部上拉/下拉电阻**，
 *     所以既不能做输出，也无法用于"配置内部下拉"的输入。
 *   - 6 个脚都取自「复位后无内部上下拉、且非 strapping」的普通 GPIO：
 *     GPIO18/19/23/27/32/33 —— 上电瞬间继电器不会被误触发。
 *   - 避开 strapping 脚（0/2/5/12/15）：上电电平受外部电路影响，可能误吸合。
 *
 * 硬件约束：
 *   - **继电器线圈不能直接接 GPIO**（典型 5 V 线圈 70–90 mA，GPIO 仅几十 mA）。
 *     必须用带三极管/光耦驱动的继电器模块；裸继电器需驱动管 + 续流二极管。
 *   - **本项目为高电平触发**：输出高 = 继电器吸合（测试输入高 → 输出高 → 打开）。
 *     若模块是低电平触发，改 relay.c 的 RELAY_ACTIVE_LOW 为 1；配错则逻辑相反。
 *   - 模块 VCC 常需 5 V（逻辑输入可 3.3 V），确认模块是否 3.3 V 兼容。 */
#define BOARD_RELAY1_OUT     18
#define BOARD_RELAY2_OUT     19
#define BOARD_RELAY3_OUT     23

#define BOARD_RELAY_TEST1_IN 27
#define BOARD_RELAY_TEST2_IN 32
#define BOARD_RELAY_TEST3_IN 33

#endif /* BOARD_PINS_H */
