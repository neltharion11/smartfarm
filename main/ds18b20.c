/* DS18B20 水温探头驱动实现（ESP-IDF 1-Wire 位操作版）。
 *
 * 与厂商 STM32 例程的关键差异（必须如此，否则会破坏系统）：
 *   例程用 __disable_irq() / __enable_irq() 关全局中断来保护时序。
 *   在 ESP-IDF 双核 FreeRTOS 下这会让系统 tick、看门狗、WiFi 全部停摆。
 *   本实现改用 portENTER_CRITICAL + portMUX_TYPE，且持有时间受控：
 *     - 位时隙：≤60 µs
 *     - 复位脉冲：约 1 ms（每秒钟最多一次）
 *   绝不把 750 ms 的转换等待放进临界区——那段时间只做 vTaskDelay。
 *
 * 时序参数全部取自 Maxim 手册 19-7487 Rev 4（已在注释中标注来源）。
 */

#include "ds18b20.h"
#include "board_pins.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define TAG "ds18b20"

#define DS18B20_PIN ((gpio_num_t)BOARD_DS18B20_DQ)

/* 12 位分辨率转换时间上限（手册 Table 2：750 ms）。
 * 早于此时间读取会拿到上一次/上电值，故作为"结果是否就绪"的判据。 */
#define DS18B20_CONV_TIME_US 750000

/* 保护 1-Wire 位时序的临界区锁（短时持有） */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;
static bool s_started;            /* 是否已启动过至少一次转换（规避 +85 ℃ 上电值） */
static int64_t s_conv_start_us;   /* 上一次转换的启动时刻 */

/* ---------------- 时序常量（手册 Figure 15/16） ---------------- */
#define OW_RESET_LOW_US      480   /* 复位脉冲：主机拉低 ≥480 µs */
#define OW_PRESENCE_WAIT_US   70   /* 释放后等 15–60 µs 才出现存在脉冲，取 70 µs 采样 */
#define OW_RESET_TAIL_US     410   /* 复位时隙收尾，使低电平总时长可控 */
#define OW_READ_INIT_LOW_US    2   /* 读时隙：拉低 ≥1 µs */
#define OW_READ_SAMPLE_US     10   /* 采样点：须在时隙开始后 15 µs 内 */
#define OW_READ_TAIL_US       48   /* 读时隙总长 ≥60 µs */
#define OW_WRITE_ONE_HOLD_US   2   /* 写 1：须在 15 µs 内释放 */
#define OW_WRITE_ONE_TAIL_US  58
#define OW_WRITE_ZERO_HOLD_US 60   /* 写 0：保持低到整时隙（≥60 µs） */

/* ----------------------------------------------------------------
 * 上拉策略
 *   0 = 纯开漏：高电平完全靠上拉电阻（**手册推荐**，需要外部 ~4.7 kΩ）
 *   1 = 主动拉高：写时隙的高电平阶段用推挽驱动强拉，读时隙与存在脉冲
 *       窗口仍保持开漏
 *
 * 为什么选 1 也安全：写时隙期间器件**只在监听、不驱动总线**，主机推挽拉高
 * 不存在总线竞争；而读时隙、以及复位后器件发存在脉冲的窗口，是**器件在驱动**，
 * 那时必须开漏。这本质是手册“强上拉（strong pull-up）”的思路，只是用 MCU 的
 * 推挽驱动代替外部 MOSFET。
 *
 * 局限：仅解决“写命令时 1 位升不到 VIH”的问题，不改变手册要求的电气设计。
 *       补上 4.7 kΩ 外部上拉后，建议改回 0。 */
#define DS18B20_ACTIVE_PULLUP 1

static inline void ow_delay_us(uint32_t us) { esp_rom_delay_us(us); }
static inline int  ow_level(void) { return gpio_get_level(DS18B20_PIN); }

/* 高阻释放：关闭输出，仅靠（内部/外部）上拉维持高。
 * 读时隙与复位后的存在脉冲窗口**必须**用这个——那时是器件在驱动总线。 */
static inline void ow_release(void)
{
    gpio_set_direction(DS18B20_PIN, GPIO_MODE_INPUT);
}

/* 主动驱动低：写时隙起始或读时隙起始 */
static inline void ow_pull_low(void)
{
    gpio_set_level(DS18B20_PIN, 0);
    gpio_set_direction(DS18B20_PIN, GPIO_MODE_INPUT_OUTPUT);
}

/* 空闲高：按 DS18B20_ACTIVE_PULLUP 选择“主动拉高”或“松手等上拉”。
 * 只允许在**主机说话**的阶段（写时隙之间、命令与命令之间）使用。 */
static inline void ow_idle_high(void)
{
#if DS18B20_ACTIVE_PULLUP
    gpio_set_level(DS18B20_PIN, 1);
    gpio_set_direction(DS18B20_PIN, GPIO_MODE_INPUT_OUTPUT);
#else
    ow_release();
#endif
}

esp_err_t ds18b20_init(void)
{
    if (s_ready) return ESP_OK;

    /* 用推挽双向模式，由 ow_release() / ow_pull_low() / ow_idle_high() 通过
     * gpio_set_direction 动态决定“高阻”还是“驱动”，从而同时支持
     * 开漏语义（读时隙）与主动拉高（写时隙）。 */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_DS18B20_DQ,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* 高阻释放时靠它维持高（辅助外部上拉） */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    ow_idle_high();   /* 空闲态为高（手册：1-Wire 总线空闲为高） */

    ESP_LOGW(TAG, "pull-up strategy: %s",
#if DS18B20_ACTIVE_PULLUP
             "ACTIVE (write slots push-pull driven; read slots & presence remain open-drain)"
#else
             "open-drain only (requires external ~4.7k to 3V3)"
#endif
    );

    s_ready = true;
    s_started = false;
    s_conv_start_us = 0;
    ESP_LOGI(TAG, "ds18b20 up: DQ=GPIO%d (needs external pull-up to 3V3, ~%.1fk)",
             BOARD_DS18B20_DQ, BOARD_DS18B20_PULLUP_OHM / 1000.0f);

    /* 注意：此处**不**启动转换。调用方应先 probe / read_rom（会发复位脉冲，
     * 外部供电下虽不打断转换，但保持顺序清晰），最后再 start_conversion。 */
    return ESP_OK;
}

/* ---- 复位 + 存在脉冲 ---- */
static bool ow_reset_presence(void)
{
    bool presence;

    portENTER_CRITICAL(&s_mux);
    ow_pull_low();
    ow_delay_us(OW_RESET_LOW_US);
    ow_release();
    ow_delay_us(OW_PRESENCE_WAIT_US);
    presence = (ow_level() == 0);   /* 器件拉低 = 存在脉冲 */
    ow_delay_us(OW_RESET_TAIL_US);
    portEXIT_CRITICAL(&s_mux);

    return presence;
}

/* ---- 写一位：写 1 在 15 µs 内拉高；写 0 保持低到整时隙 ----
 * 高电平阶段用 ow_idle_high()（可能是主动拉高）。写时隙期间器件不驱动总线，
 * 因此主机推挽拉高是安全的。 */
static void ow_write_bit(int bit)
{
    portENTER_CRITICAL(&s_mux);
    ow_pull_low();
    if (bit) {
        ow_delay_us(OW_WRITE_ONE_HOLD_US);
        ow_idle_high();                    /* 必须在 15 µs 内把线拉起来 */
        ow_delay_us(OW_WRITE_ONE_TAIL_US);
    } else {
        ow_delay_us(OW_WRITE_ZERO_HOLD_US);
        ow_idle_high();
        ow_delay_us(1);                    /* 时隙间恢复 ≥1 µs */
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ---- 读一位：拉低 ≥1 µs 后释放，在 15 µs 内采样 ---- */
static int ow_read_bit(void)
{
    int level;

    portENTER_CRITICAL(&s_mux);
    ow_pull_low();
    ow_delay_us(OW_READ_INIT_LOW_US);
    ow_release();
    ow_delay_us(OW_READ_SAMPLE_US);
    level = ow_level();
    ow_delay_us(OW_READ_TAIL_US);
    portEXIT_CRITICAL(&s_mux);

    return level;
}

static void ow_write_byte(uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(value & 0x01);   /* 手册：所有数据低位在前 */
        value >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) value |= (uint8_t)(1u << i);
    }
    return value;
}

uint8_t ds18b20_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;   /* 手册 Figure 11：移位寄存器初值全 0 */
    for (size_t i = 0; i < len; i++) {
        uint8_t inbyte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (uint8_t)((crc ^ inbyte) & 0x01);
            crc >>= 1;
            if (mix) crc ^= 0x8C;   /* 反射形式的多项式 X^8+X^5+X^4+1 */
            inbyte >>= 1;
        }
    }
    return crc;
}

esp_err_t ds18b20_probe(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return ow_reset_presence() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t ds18b20_start_conversion(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    if (!ow_reset_presence()) return ESP_ERR_NOT_FOUND;

    ow_write_byte(BOARD_DS18B20_CMD_SKIP_ROM);    /* 0xCC：单器件无需匹配 ROM */
    ow_write_byte(BOARD_DS18B20_CMD_CONVERT_T);   /* 0x44：启动温度转换 */

    s_started = true;
    s_conv_start_us = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t ds18b20_read_rom(uint8_t rom[DS18B20_ROM_SIZE])
{
    if (!rom) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    if (!ow_reset_presence()) return ESP_ERR_NOT_FOUND;
    ow_write_byte(BOARD_DS18B20_CMD_READ_ROM);    /* 0x33：仅单器件可用 */
    for (int i = 0; i < DS18B20_ROM_SIZE; i++) {
        rom[i] = ow_read_byte();
    }

    if (rom[0] != 0x28) {
        ESP_LOGW(TAG, "unexpected family code 0x%02X (DS18B20 should be 0x28)", rom[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ds18b20_crc8(rom, 7) != rom[7]) {
        ESP_LOGW(TAG, "ROM CRC mismatch: calc=0x%02X recv=0x%02X", ds18b20_crc8(rom, 7), rom[7]);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/* 温度换算：raw 为 1/16 ℃ 的 16 位补码；输出 0.1 ℃。
 * raw×10/16 = raw×5/8，四舍五入；对负数向远离零方向取整，保证 ±0.05 对称。 */
static int16_t raw_to_c10(int16_t raw)
{
    int32_t n = (int32_t)raw * 5;
    n += (raw >= 0) ? 4 : -4;
    int32_t q = n / 8;            /* C 的整数除法向零截断 */
    return (int16_t)q;
}

esp_err_t ds18b20_read_temperature(int16_t *temp_c10)
{
    if (!temp_c10) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    *temp_c10 = DS18B20_TEMP_ERROR;

    /* 从未启动过转换：此刻暂存器里是上电复位值 +85.0 ℃（手册 Table 1），
     * 直接读会把 85 ℃ 当成真实水温，故先拒绝并补启一次转换。 */
    if (!s_started) {
        ESP_LOGW(TAG, "no conversion started yet; starting one (POR value is +85.0 C)");
        return ds18b20_start_conversion();
    }

    /* 转换尚未完成：暂存器里仍是上一次的值（首次则是 +85.0 ℃ 上电复位值）。
     * 手册 Table 2：12 位分辨率转换时间最大 750 ms。此处按该上限判定"未就绪"，
     * 避免把上电值或上一次的旧值当成新读数。 */
    int64_t elapsed_us = esp_timer_get_time() - s_conv_start_us;
    if (elapsed_us < DS18B20_CONV_TIME_US) {
        ESP_LOGW(TAG, "conversion not finished (%lld ms elapsed < 750 ms)",
                 (long long)(elapsed_us / 1000));
        return ESP_ERR_INVALID_STATE;
    }

    if (!ow_reset_presence()) return ESP_ERR_NOT_FOUND;

    ow_write_byte(BOARD_DS18B20_CMD_SKIP_ROM);
    ow_write_byte(BOARD_DS18B20_CMD_READ_SCRATCH);   /* 0xBE：读 9 字节暂存器 */

    uint8_t sp[DS18B20_SCRATCHPAD_SIZE];
    for (int i = 0; i < DS18B20_SCRATCHPAD_SIZE; i++) {
        sp[i] = ow_read_byte();
    }

    /* 立即启动下一次转换，使本次返回的读数总是"上一秒"的结果（非阻塞式） */
    ds18b20_start_conversion();

    /* 全零检测：DQ 被短接到地或总线异常时的典型特征（例程同样采用） */
    if (sp[0] == 0 && sp[1] == 0 && sp[8] == 0) {
        ESP_LOGW(TAG, "scratchpad all-zero -> bus shorted to GND?");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* CRC-8 校验：前 8 字节的 CRC 应等于第 9 字节（手册第 8 页） */
    if (ds18b20_crc8(sp, 8) != sp[8]) {
        ESP_LOGW(TAG, "scratchpad CRC mismatch: calc=0x%02X recv=0x%02X", ds18b20_crc8(sp, 8), sp[8]);
        return ESP_ERR_INVALID_CRC;
    }

    int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
    int16_t c10 = raw_to_c10(raw);

    /* 范围检测（手册：-55 ~ +125 ℃） */
    if (c10 < -550 || c10 > 1250) {
        ESP_LOGW(TAG, "temperature out of range: raw=%d c10=%d", raw, c10);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *temp_c10 = c10;
    return ESP_OK;
}

/* ---------------- 总线电气诊断 ---------------- */

typedef struct { int pu, pd, lo, hi; } pin_dc_t;

/* 对单个引脚做一组 DC 电气测量（毫秒级稳定，不受微秒抖动影响）：
 *   PU    = 输入 + 内部上拉时的电平
 *   PD    = 输入 + 内部下拉时的电平（外部强上拉会压过它 -> 读到高）
 *   drvLO = 推挽驱动低后的回读（期望 0）
 *   drvHI = 推挽驱动高后的回读（期望 1）
 *
 * 关键：必须同时对一个**已知空闲的对照引脚**跑完全相同的测量，
 * 否则无法区分"被测硬件异常"与"测量代码/配置本身有问题"。 */
static pin_dc_t diag_pin_dc(const char *label, int pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(20));
    int pu = gpio_get_level((gpio_num_t)pin);

    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(20));
    int pd = gpio_get_level((gpio_num_t)pin);

    cfg.mode = GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    int lo = gpio_get_level((gpio_num_t)pin);
    gpio_set_level((gpio_num_t)pin, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    int hi = gpio_get_level((gpio_num_t)pin);

    ESP_LOGW(TAG, "[%s GPIO%d] PU=%d PD=%d drvLO=%d drvHI=%d", label, pin, pu, pd, lo, hi);
    return (pin_dc_t){ pu, pd, lo, hi };
}

void ds18b20_diag(void)
{
    ESP_LOGW(TAG, "=== DS18B20 bus diagnosis on GPIO%d ===", BOARD_DS18B20_DQ);

    /* 1) 带**对照**的引脚电气测量：
     *    对照 = 已知空闲的 GPIO13（不接任何外设）；目标 = DS18B20 的 DQ。
     *    对照脚若也异常，说明是测量代码/配置的问题；只有对照正常，被测脚的
     *    异常才可信。这是上次诊断 SHT40 时用 OLED 做阳性对照的同一方法论。 */
    const int ctrl_pin = 13;
    ESP_LOGW(TAG, "--- pin DC measurement (with control pin) ---");
    pin_dc_t c = diag_pin_dc("control", ctrl_pin);
    pin_dc_t t = diag_pin_dc("DS18B20", BOARD_DS18B20_DQ);

    bool meas_ok = (c.lo == 0 && c.hi == 1);
    if (meas_ok) {
        ESP_LOGI(TAG, "control GPIO%d behaved correctly (drvLO=0 drvHI=1) -> measurements trustworthy",
                 ctrl_pin);
    } else {
        ESP_LOGE(TAG, "control GPIO%d is ALSO abnormal -> measurement code/config suspect; "
                      "do NOT draw hardware conclusions from the target numbers", ctrl_pin);
    }

    if (t.pd == 1) {
        ESP_LOGI(TAG, "target GPIO%d: external pull-up IS present", BOARD_DS18B20_DQ);
    } else {
        ESP_LOGW(TAG, "target GPIO%d: no external pull-up detected (PD reads low)", BOARD_DS18B20_DQ);
    }

    if (meas_ok && t.hi != 1) {
        ESP_LOGE(TAG, "target GPIO%d cannot be driven HIGH -> something sinks it hard "
                      "(device holding the line, or a short to GND)", BOARD_DS18B20_DQ);
    } else if (meas_ok && t.pd == 0 && t.pu == 1 && t.lo == 0 && t.hi == 1) {
        ESP_LOGE(TAG, "target GPIO%d looks like a FLOATING pin: drives both ways, no pull-up, "
                      "nothing sinks it", BOARD_DS18B20_DQ);
        ESP_LOGE(TAG, "  => the DQ wire is most likely NOT connected to GPIO%d "
                      "(same failure mode as the earlier SHT40 SDA case)", BOARD_DS18B20_DQ);
    } else if (meas_ok) {
        ESP_LOGW(TAG, "target GPIO%d drives normally but shows no pull-up -> if a module with an "
                      "onboard pull-up is connected, check the module's VCC/GND wiring",
                 BOARD_DS18B20_DQ);
    }

    /* 3) 切回总线模式（后续 1-Wire 操作由 ow_* 助手动态切换方向；
     *    注意不能用推挽长期驱动——器件驱动时必须已释放）。 */
    gpio_config_t od_cfg = {
        .pin_bit_mask = 1ULL << BOARD_DS18B20_DQ,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* 仅辅助，不能替代外部上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&od_cfg);

    /* 4) Search ROM 判别：把"写路径失败"与"读路径失败"分开。
     *    器件参与 Search ROM 时会同时发出 ROM 位与其**反码**。首个 ROM 位
     *    是家族码 0x28 的 LSB = 0，其反码应为 1。
     *      读到 0 然后 1  -> 器件确实收到了命令并回了数据：读写路径都通
     *      两个都读到 1   -> 器件根本没收到命令（写路径失败，典型为上拉不足）
     *    这是本次定位的关键判据。 */
    if (ow_reset_presence()) {
        ow_write_byte(BOARD_DS18B20_CMD_SEARCH_ROM);   /* 0xF0 */
        int b1 = ow_read_bit();
        int b2 = ow_read_bit();
        ESP_LOGW(TAG, "Search ROM probe: bit=%d complement=%d (expect 0 then 1)", b1, b2);
        if (b1 == 0 && b2 == 1) {
            ESP_LOGI(TAG, "  device answers commands -> write & read paths are OK");
        } else if (b1 == 1 && b2 == 1) {
            ESP_LOGE(TAG, "  both 1 -> device did NOT receive the command: WRITE PATH FAILING");
            ESP_LOGE(TAG, "  bus rises too slowly for the DS18B20 to sample '1' bits.");
            ESP_LOGE(TAG, "  => connect the external ~4.7k pull-up from DQ to 3V3");
        } else {
            ESP_LOGW(TAG, "  unexpected pattern -> bus contention or swapped wiring");
        }
    } else {
        ESP_LOGE(TAG, "no presence pulse -> device does not answer reset at all");
    }

    /* 5) 多次细粒度复位波形：这是区分"空脚"与"接了无上拉的器件"的关键。
     *    空脚在释放后只会单调上升；真实 DS18B20 会给出存在脉冲（释放后
     *    15–60 µs 拉低 60–240 µs）。真器件每次响应一致，干扰则每次不同。
     *    采样放在临界区内，避免 ISR 延迟造成 µs 级抖动。 */
    for (int rep = 0; rep < 3; rep++) {
        char tr[128];
        size_t used = 0;

        portENTER_CRITICAL(&s_mux);
        ow_pull_low();
        esp_rom_delay_us(OW_RESET_LOW_US);
        ow_release();

        uint32_t prev = 0;
        for (uint32_t t = 0; t <= 400; t += 20) {
            esp_rom_delay_us(t - prev);
            prev = t;
            int n = snprintf(tr + used, sizeof(tr) - used, "%s%lu:%d",
                             (used == 0) ? "" : ",", (unsigned long)t, ow_level());
            if (n <= 0 || (size_t)n >= sizeof(tr) - used) break;
            used += (size_t)n;
        }
        portEXIT_CRITICAL(&s_mux);

        ESP_LOGW(TAG, "reset trace #%d (us:level) %s", rep + 1, tr);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGW(TAG, "  device present -> same low window (within 15-300us) in ALL 3 traces");
    ESP_LOGW(TAG, "  floating pin    -> monotonic rise, no low window in any trace");
    ESP_LOGW(TAG, "  note: a DS18B20 on a pull-up-less probe ALSO looks 'floating' in the DC tests,");
    ESP_LOGW(TAG, "        because DQ is open-drain and draws only ~5uA at DC -- the reset");
    ESP_LOGW(TAG, "        waveform is what tells the two cases apart");
    ESP_LOGW(TAG, "=== diagnosis end ===");
}

/* ---------------- 算法自检（纯计算） ---------------- */

static bool check_u8(const char *what, uint8_t got, uint8_t want)
{
    if (got == want) return true;
    ESP_LOGE(TAG, "selftest FAIL %s: got 0x%02X want 0x%02X", what, got, want);
    return false;
}

static bool check_i16(const char *what, int16_t got, int16_t want)
{
    if (got == want) return true;
    ESP_LOGE(TAG, "selftest FAIL %s: got %d want %d", what, got, want);
    return false;
}

esp_err_t ds18b20_selftest(void)
{
    bool ok = true;

    /* CRC-8/MAXIM-DOW 标准目录 check 值：CRC("123456789") = 0xA1 */
    static const uint8_t check_str[9] = { '1','2','3','4','5','6','7','8','9' };
    ok &= check_u8("CRC(\"123456789\")", ds18b20_crc8(check_str, 9), 0xA1);

    /* 典型暂存器（温度 +25.0625 ℃）的 CRC，由两种独立实现交叉核对得到 */
    static const uint8_t sp_classic[8] = { 0x91,0x01,0x00,0x00,0x7F,0xFF,0xFF,0xFF };
    ok &= check_u8("CRC(scratchpad +25.0625C)", ds18b20_crc8(sp_classic, 8), 0xEC);

    /* 手册 Table 1 的 10 组 hex ↔ 温度对照（权威测试数据） */
    static const struct { int16_t raw; int16_t c10; } table1[] = {
        { (int16_t)0x07D0,  1250 },  /* +125      */
        { (int16_t)0x0550,   850 },  /* +85 (上电复位值) */
        { (int16_t)0x0191,   251 },  /* +25.0625  */
        { (int16_t)0x00A2,   101 },  /* +10.125   */
        { (int16_t)0x0008,     5 },  /* +0.5      */
        { (int16_t)0x0000,     0 },  /* 0         */
        { (int16_t)0xFFF8,    -5 },  /* -0.5      */
        { (int16_t)0xFF5E,  -101 },  /* -10.125   */
        { (int16_t)0xFE6F,  -251 },  /* -25.0625  */
        { (int16_t)0xFC90,  -550 },  /* -55       */
    };
    for (size_t i = 0; i < sizeof(table1) / sizeof(table1[0]); i++) {
        if (!check_i16("Table1 conversion", raw_to_c10(table1[i].raw), table1[i].c10)) {
            ok = false;
        }
    }

    if (!ok) {
        ESP_LOGE(TAG, "ds18b20 selftest FAILED");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "selftest OK (CRC check value 0xA1 + Table 1 x10 conversion)");
    return ESP_OK;
}
