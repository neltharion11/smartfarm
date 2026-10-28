#ifndef RELAY_H
#define RELAY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * 继电器输出（3 路）+ 测试输入（3 路）
 *
 * 用途：3 路输出驱动继电器模块；3 路测试输入（内部下拉、高电平触发）
 *       用于**手动验证继电器控制**——给某路输入加 3.3 V 高电平，
 *       对应继电器即吸合（见 relay_apply_inputs()）。
 *
 * 硬件约束（务必先读）：
 *   1. **继电器线圈不能直接接 GPIO**。典型 5 V 继电器线圈需 70–90 mA，
 *      而 ESP32 单脚只能给几十 mA。必须用带三极管/光耦驱动的继电器模块；
 *      若用裸继电器，需自加驱动管（如 S8050/ULN2003）+ 续流二极管（1N4148/1N4007）。
 *   2. **本项目采用高电平触发**：输出高 = 继电器吸合。
 *      即"测试输入检测到高电平 → 对应继电器输出高电平打开"。
 *      若你的模块是低电平触发（廉价模块常见），把 relay.c 的
 *      RELAY_ACTIVE_LOW 改为 1；配错的表现为逻辑相反（上电即吸合）。
 *   3. 模块 VCC 常需 5 V（逻辑输入可 3.3 V）；确认模块是否 3.3 V 兼容。
 *   4. 上电瞬间的安全性已处理：先把输出脚配成高阻、预置电平寄存器为"关"，
 *      再切成输出，避免低电平触发模块在上电瞬间误吸合。
 */

#define RELAY_COUNT            3
#define RELAY_TEST_INPUT_COUNT 3

/** 初始化 3 路输出（默认断开）与 3 路测试输入（内部下拉、高电平有效）。
 *  可重复调用（幂等）。 */
esp_err_t relay_init(void);

/** 设置第 index 路继电器（0..2）。on=true 表示吸合（自动按极性换算电平）。 */
void relay_set(int index, bool on);

/** 读取第 index 路继电器的逻辑状态（true = 吸合）。 */
bool relay_get(int index);

/** 一次性设置 3 路（mask 的 bit0..bit2 对应继电器 1..3）。 */
void relay_set_all(uint8_t mask);

/** 一次性读取 3 路的逻辑状态（bit0..bit2）。 */
uint8_t relay_get_all(void);

/** 读第 index 路测试输入。true = 高电平（已触发）。 */
bool relay_test_input(int index);

/** 读全部测试输入（bit0..bit2，1 = 高电平/已触发）。 */
uint8_t relay_test_inputs_all(void);

/** 按"输入高 → 对应继电器吸合"的映射刷新输出，并返回新的输出状态掩码。
 *
 *  ⚠️ **仅用于手动测试，切勿与其它控制通道（BLE / MQTT / 规则引擎）并用。**
 *  它会无条件把输出覆盖成输入的值——两个"主控"抢同一个输出时，后执行的那个赢。
 *  本项目已踩过这个坑：BLE 打开继电器后，主循环每秒调用它又按输入把继电器关掉，
 *  表现为"继电器开一两秒就自己关了"。
 *
 *  主循环现在只调用 relay_test_inputs_all()（只读，无副作用）来上报输入状态。 */
uint8_t relay_apply_inputs(void);

/** 输出极性的文字描述，用于日志。 */
const char *relay_polarity_str(void);

#endif /* RELAY_H */
