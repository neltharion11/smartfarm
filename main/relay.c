/* 继电器输出 + 测试输入实现。
 *
 * 极性说明：多数廉价继电器模块为低电平触发（输入低 = 吸合）。
 * 本文件用 RELAY_ACTIVE_LOW 统一换算，业务层只看"吸合/断开"，不看电平。
 */

#include "relay.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG "relay"

/* 继电器模块触发极性：0 = 高电平触发（输出高 = 吸合），1 = 低电平触发。
 *
 * 本项目按需求采用**高电平触发**：测试输入检测到高电平后，
 * 对应继电器输出高电平打开（input HIGH -> output HIGH -> 吸合）。 */
#define RELAY_ACTIVE_LOW 0

static const int s_out[RELAY_COUNT] = {
    BOARD_RELAY1_OUT, BOARD_RELAY2_OUT, BOARD_RELAY3_OUT
};
static const int s_in[RELAY_TEST_INPUT_COUNT] = {
    BOARD_RELAY_TEST1_IN, BOARD_RELAY_TEST2_IN, BOARD_RELAY_TEST3_IN
};

static uint8_t s_state;   /* bit0..bit2 = 继电器 1..3 的逻辑状态（1 = 吸合） */
static bool s_ready;

static inline int off_level(void) { return RELAY_ACTIVE_LOW ? 1 : 0; }
static inline int on_level(void)  { return RELAY_ACTIVE_LOW ? 0 : 1; }

esp_err_t relay_init(void)
{
    if (s_ready) return ESP_OK;

    /* 输出脚：先把引脚配成**高阻输入**，把输出电平寄存器预置为"断开"，
     * 再切成输出模式。这样对低电平触发的模块也不会在上电瞬间误吸合
     * （若直接配成输出，输出寄存器初值为 0 = 吸合，会有一瞬间误动作）。 */
    for (int i = 0; i < RELAY_COUNT; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << s_out[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&c), TAG, "out%d cfg failed", i + 1);
        gpio_set_level((gpio_num_t)s_out[i], off_level());
        ESP_RETURN_ON_ERROR(gpio_set_direction((gpio_num_t)s_out[i], GPIO_MODE_OUTPUT),
                            TAG, "out%d dir failed", i + 1);
    }

    /* 测试输入：内部下拉 + 高电平有效。
     * 未接线时读低（未触发），给 3.3 V 即读高（触发）——便于用跳线手动测试。 */
    for (int i = 0; i < RELAY_TEST_INPUT_COUNT; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << s_in[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&c), TAG, "in%d cfg failed", i + 1);
    }

    s_state = 0;
    s_ready = true;

    /* 回读输出脚的实际电平，确认"断开"状态确实落在正确电平上
     * （不是只看我们写的变量——要确认引脚真的到位）。 */
    int lv0 = gpio_get_level((gpio_num_t)s_out[0]);
    int lv1 = gpio_get_level((gpio_num_t)s_out[1]);
    int lv2 = gpio_get_level((gpio_num_t)s_out[2]);

    ESP_LOGI(TAG, "relay OUT : GPIO%d / GPIO%d / GPIO%d  (%s)",
             s_out[0], s_out[1], s_out[2], relay_polarity_str());
    /* 注意：运行时日志一律用 ASCII。ESP-IDF 串口控制台不按 UTF-8 渲染，
     * 日志里出现中文会变成一串 '?'（本项目实测）。中文只写在注释里。 */
    ESP_LOGI(TAG, "  OFF readback = %d/%d/%d (expect %d/%d/%d = OFF)",
             lv0, lv1, lv2, off_level(), off_level(), off_level());
    ESP_LOGI(TAG, "test  IN  : GPIO%d / GPIO%d / GPIO%d  (internal PULL-DOWN, active-HIGH)",
             s_in[0], s_in[1], s_in[2]);
    ESP_LOGI(TAG, "  mapping   : input HIGH -> output HIGH -> relay ON (1:1 by index)");
    ESP_LOGW(TAG, "never drive a bare relay coil from a GPIO (needs ~70-90mA); use a driver module");
    return ESP_OK;
}

void relay_set(int index, bool on)
{
    if (!s_ready || index < 0 || index >= RELAY_COUNT) return;

    gpio_set_level((gpio_num_t)s_out[index], on ? on_level() : off_level());

    if (on) {
        s_state |= (uint8_t)(1u << index);
    } else {
        s_state &= (uint8_t)~(1u << index);
    }
}

bool relay_get(int index)
{
    if (index < 0 || index >= RELAY_COUNT) return false;
    return (s_state >> index) & 0x01;
}

void relay_set_all(uint8_t mask)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        relay_set(i, (mask >> i) & 0x01);
    }
}

uint8_t relay_get_all(void)
{
    return (uint8_t)(s_state & 0x07);
}

bool relay_test_input(int index)
{
    if (index < 0 || index >= RELAY_TEST_INPUT_COUNT) return false;
    return gpio_get_level((gpio_num_t)s_in[index]) == 1;   /* 高电平有效 */
}

uint8_t relay_test_inputs_all(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < RELAY_TEST_INPUT_COUNT; i++) {
        if (relay_test_input(i)) mask |= (uint8_t)(1u << i);
    }
    return mask;
}

uint8_t relay_apply_inputs(void)
{
    /* 测试映射：输入 i 为高 → 继电器 i 吸合（输出高）。
     * 这使"给某路测试输入加 3.3 V"成为一个可直接观察的继电器控制测试。 */
    static uint8_t last_in = 0xFF;   /* 0xFF 仅为触发首次打印 */
    uint8_t in = relay_test_inputs_all();

    relay_set_all(in);

    /* 输入发生变化的瞬间打印一次，并回读引脚实际电平——
     * 便于手动测试时确认"输入变高 → 输出脚真的变高"，而不是只看变量。 */
    if (in != last_in) {
        int l0 = gpio_get_level((gpio_num_t)s_out[0]);
        int l1 = gpio_get_level((gpio_num_t)s_out[1]);
        int l2 = gpio_get_level((gpio_num_t)s_out[2]);
        ESP_LOGW(TAG, "input change: I=%d%d%d -> O=%d%d%d  (pin levels %d/%d/%d)",
                 (in >> 0) & 1, (in >> 1) & 1, (in >> 2) & 1,
                 (s_state >> 0) & 1, (s_state >> 1) & 1, (s_state >> 2) & 1,
                 l0, l1, l2);
        last_in = in;
    }

    return in;
}

const char *relay_polarity_str(void)
{
    return RELAY_ACTIVE_LOW ? "active-LOW  (pin LOW  = ON)"
                            : "active-HIGH (pin HIGH = ON)";
}
