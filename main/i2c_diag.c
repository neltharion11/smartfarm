/* I²C 总线诊断实现。
 *
 * 必须在任何 i2c_master 总线安装之前调用（诊断会临时占用引脚）。
 * 每次扫描临时建一条总线，扫完立即删除。
 */

#include "i2c_diag.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "i2c_diag"

/* ---------------- 单引脚电气自检 ---------------- */

/* 输入模式 + 指定上/下拉，稳定后读电平 */
static int read_with_pull(int pin, gpio_pullup_t pu, gpio_pulldown_t pd)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pu,
        .pull_down_en = pd,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));   /* 等 RC 稳定 */
    return gpio_get_level((gpio_num_t)pin);
}

/* 双向推挽：驱动到 level 后读回 */
static int drive_and_read(int pin, int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)pin, level);
    vTaskDelay(pdMS_TO_TICKS(2));
    return gpio_get_level((gpio_num_t)pin);
}

bool i2c_diag_pin_selftest(int pin, const char *label)
{
    int pu = read_with_pull(pin, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE);
    int pd = read_with_pull(pin, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE);
    int drv_lo = drive_and_read(pin, 0);
    int drv_hi = drive_and_read(pin, 1);

    ESP_LOGW(TAG, "[%s GPIO%d] PU=%d PD=%d drvLO=%d drvHI=%d", label, pin, pu, pd, drv_lo, drv_hi);

    bool healthy = true;

    /* 上拉能读到高、下拉能读到低：引脚最基本的输入功能 */
    if (pu != 1) {
        ESP_LOGE(TAG, "[%s GPIO%d] cannot read HIGH even with internal pull-up -> "
                      "pin shorted to GND or damaged", label, pin);
        healthy = false;
    }
    /* 驱动能力 */
    if (drv_lo != 0) {
        ESP_LOGE(TAG, "[%s GPIO%d] cannot drive LOW -> open circuit or damaged pin", label, pin);
        healthy = false;
    }
    if (drv_hi != 1) {
        ESP_LOGE(TAG, "[%s GPIO%d] cannot drive HIGH -> shorted to GND", label, pin);
        healthy = false;
    }

    /* 关键判据：外部上拉是否存在于此引脚 */
    if (pd == 1) {
        ESP_LOGI(TAG, "[%s GPIO%d] PD reads HIGH -> an EXTERNAL pull-up is connected to THIS pin "
                      "(internal 45k pulldown is overridden)", label, pin);
    } else {
        ESP_LOGE(TAG, "[%s GPIO%d] PD reads LOW -> NO external pull-up on this pin. "
                      "If you wired SDA/SCL pull-ups to 3.3V, they are NOT landing on this pin.",
                 label, pin);
    }

    /* 收尾：恢复为干净的输入态（无内部上拉），交由后续 I2C 驱动接管 */
    gpio_reset_pin((gpio_num_t)pin);
    return healthy;
}

/* 扫描探测超时：112 个地址全失败时的最坏耗时 = 112 * 该值。
 * 取 20 ms 以兼顾"够长到能等到 ACK"与"启动不被拖太久"。 */
#define PROBE_TIMEOUT_MS 20

void i2c_diag_lines(int sda, int scl, const char *label)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));   /* 等上拉把线拉稳 */

    int sda_level = gpio_get_level((gpio_num_t)sda);
    int scl_level = gpio_get_level((gpio_num_t)scl);

    ESP_LOGW(TAG, "[%s] idle levels SDA(GPIO%d)=%d SCL(GPIO%d)=%d (expect 1/1)",
             label, sda, sda_level, scl, scl_level);

    if (sda_level == 0 && scl_level == 0) {
        ESP_LOGE(TAG, "[%s] both LOW -> short to GND or device holding the bus", label);
    } else if (scl_level == 0) {
        ESP_LOGE(TAG, "[%s] SCL stuck LOW -> check SCL wiring / device power", label);
    } else if (sda_level == 0) {
        ESP_LOGE(TAG, "[%s] SDA stuck LOW -> check SDA wiring / device power", label);
    } else {
        ESP_LOGI(TAG, "[%s] lines idle HIGH (pulled up; note: internal pull-up alone also gives HIGH)",
                 label);
    }
}

int i2c_diag_scan_pair(int sda, int scl, const char *label, uint8_t expect_addr)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_1,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] cannot open bus: %s", label, esp_err_to_name(err));
        return -1;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGW(TAG, "[%s]   device at 0x%02X%s", label, addr,
                     (addr == expect_addr) ? "  <-- expected" : "");
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGE(TAG, "[%s] scan 0x08-0x77: NO device on GPIO%d/GPIO%d", label, sda, scl);
    } else if (expect_addr && i2c_master_probe(bus, expect_addr, PROBE_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] %d device(s) present but NOT at expected 0x%02X -> address mismatch",
                 label, found, expect_addr);
    } else {
        ESP_LOGI(TAG, "[%s] scan 0x08-0x77: %d device(s), expected 0x%02X present", label, found,
                 expect_addr);
    }

    i2c_del_master_bus(bus);
    return found;
}

/* 常见可安全用作 I2C 的 GPIO（ESP32-D0WD-V3 / WROOM-32）：
 * 排除 6-11（内部 Flash）、1/3（控制台 UART）、34-39（仅输入）、
 * 0/2/5/12/15（strapping）、16/17（PSRAM 风险）。 */
static const int CANDIDATE_PINS[] = {
    4, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33
};
#define CANDIDATE_COUNT (int)(sizeof(CANDIDATE_PINS) / sizeof(CANDIDATE_PINS[0]))

int i2c_diag_sweep_pins(void)
{
    int hits = 0;

    ESP_LOGW(TAG, "=== pull-up guided pin sweep for SHT4x (0x44/0x45) ===");

    /* 第一阶段：找出**确实带外部上拉**的引脚。
     * 一条真正接了 I2C 从机的线必然有上拉；没有上拉的引脚不可能是有效 I2C 线，
     * 据此把候选从 13×12 组合缩到极少数，既快又避开大量无效探测的日志噪声。 */
    int pulled[16];
    int pulled_count = 0;
    for (int i = 0; i < CANDIDATE_COUNT && pulled_count < 16; i++) {
        int pin = CANDIDATE_PINS[i];
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        vTaskDelay(pdMS_TO_TICKS(3));
        int level = gpio_get_level((gpio_num_t)pin);
        gpio_reset_pin((gpio_num_t)pin);

        if (level == 1) {   /* 内部 45k 下拉被外部上拉压过 -> 有外部上拉 */
            pulled[pulled_count++] = pin;
            ESP_LOGW(TAG, "  GPIO%d HAS external pull-up", pin);
        }
    }
    ESP_LOGW(TAG, "pins with external pull-up: %d", pulled_count);

    if (pulled_count < 2) {
        ESP_LOGE(TAG, "fewer than 2 pulled-up pins -> no usable I2C bus exists outside the OLED pair");
        ESP_LOGE(TAG, "  => SDA/SCL pull-ups are missing on the sensor wiring");
        return 0;
    }

    /* 第二阶段：只在这些引脚之间两两组合探测 */
    for (int i = 0; i < pulled_count; i++) {
        for (int j = 0; j < pulled_count; j++) {
            if (i == j) continue;
            int sda = pulled[i];
            int scl = pulled[j];
            if (sda == 21 && scl == 22) continue;   /* 跳过 OLED 总线 */

            i2c_master_bus_config_t bus_cfg = {
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .i2c_port = I2C_NUM_1,
                .scl_io_num = scl,
                .sda_io_num = sda,
                .glitch_ignore_cnt = 7,
                .flags.enable_internal_pullup = true,
            };
            i2c_master_bus_handle_t bus = NULL;
            if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) continue;

            for (uint8_t addr = 0x44; addr <= 0x45; addr++) {
                if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
                    ESP_LOGE(TAG, "  SHT4x-like device FOUND: addr=0x%02X SDA=GPIO%d SCL=GPIO%d",
                             addr, sda, scl);
                    hits++;
                }
            }
            i2c_del_master_bus(bus);
        }
    }

    if (hits == 0) {
        ESP_LOGE(TAG, "sweep: no SHT4x at 0x44/0x45 on any pulled-up pin pair");
        ESP_LOGE(TAG, "  => sensor not electrically present (VDD/GND, SDA, SCL) or module faulty");
    } else {
        ESP_LOGE(TAG, "sweep found %d match(es) -> update BOARD_SHT40_I2C_SDA/SCL in board_pins.h", hits);
    }
    return hits;
}
