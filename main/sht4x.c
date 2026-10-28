/* SHT4x 温湿度传感器驱动（ESP-IDF i2c_master 版）。
 *
 * 复用来源：
 *   - CRC-8 与定点换算写法移植自厂商 STM32 例程 sht4x.c / iic.c：
 *       sens_crc8()                -> sht4x_crc8()
 *       t_val = -45000 + 175*t_raw*1000/65535  -> 同式（放大 1000 倍）
 *   - 命令值、CRC 参数、换算公式、时序来自 SHT4x 数据手册 v2（已入知识库）。
 *   - 驱动库中无 SHT4x 驱动，故非库复用。
 *
 * 与原例程的关键差异（按手册修正）：
 *   - 原例程用软件模拟 I²C + delay_us 轮询；此处改用 i2c_master 硬件主机驱动。
 *   - 原例程高精度等待写 10000 µs；手册 tMEAS,h 最大 8.2 ms，此处用 10 ms 含余量。
 *   - 手册明确 SHT4x 不支持时钟拉伸且数据只能读一次，故重试时重发测量命令。
 */

#include "sht4x.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "sht4x"

/* 测量返回固定 6 字节 */
#define SHT4X_RESP_LEN 6
#define SHT4X_XFER_TIMEOUT_MS 100

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* 编译期防漂移：board_pins.h 是引脚权威表，本驱动必须与之一致。 */
_Static_assert(BOARD_SHT40_I2C_ADDR == SHT4X_I2C_ADDR, "board_pins.h 与 sht4x.h 的地址不一致");

uint8_t sht4x_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;   /* 手册 Table 6：Initialization 0xFF */
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            /* 多项式 0x31 (x^8 + x^5 + x^4 + 1)，不反射、无最终异或 */
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sht4x_init(void)
{
    if (s_ready) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_SHT40_I2C_PORT,
        .scl_io_num = BOARD_SHT40_I2C_SCL,
        .sda_io_num = BOARD_SHT40_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 手册要求 SDA/SCL 上拉到 VDD */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "new master bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT4X_I2C_ADDR,
        .scl_speed_hz = 400000,   /* 手册支持标准/快速/快速+ */
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG,
                        "add device 0x%02X failed", SHT4X_I2C_ADDR);

    /* 上电后先软复位并等 tSR，让传感器进入确定的空闲态。
     * 实测：不做这一步时，启动后最初 1–2 次读会收到 NACK（传感器忙），
     * 第 3 次才成功。手册 4.7 给出软复位为正式的复位手段，故在此使用。
     * 失败不算致命（例如传感器本就有问题），只记录警告继续。 */
    const uint8_t reset = SHT4X_CMD_SOFT_RESET;
    esp_err_t rst_err = i2c_master_transmit(s_dev, &reset, 1, SHT4X_XFER_TIMEOUT_MS);
    if (rst_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHT4X_WAIT_RESET_MS + 2));   /* tSR 最大 1 ms，留余量 */
        ESP_LOGI(TAG, "soft reset sent, sensor settled");
    } else {
        ESP_LOGW(TAG, "soft reset failed (%s), continuing", esp_err_to_name(rst_err));
    }

    s_ready = true;
    ESP_LOGI(TAG, "sht4x bus up: port=%d sda=%d scl=%d addr=0x%02X",
             BOARD_SHT40_I2C_PORT, BOARD_SHT40_I2C_SDA, BOARD_SHT40_I2C_SCL, SHT4X_I2C_ADDR);
    return ESP_OK;
}

esp_err_t sht4x_probe(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(s_bus, SHT4X_I2C_ADDR, SHT4X_XFER_TIMEOUT_MS);
}

void sht4x_deinit(void)
{
    if (!s_ready) return;
    /* 先摘设备再删总线，顺序不能颠倒 */
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_ready = false;
    ESP_LOGI(TAG, "sht4x bus released");
}

esp_err_t sht4x_soft_reset(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    const uint8_t cmd = SHT4X_CMD_SOFT_RESET;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, &cmd, 1, SHT4X_XFER_TIMEOUT_MS), TAG,
                        "soft reset transmit failed");
    vTaskDelay(pdMS_TO_TICKS(SHT4X_WAIT_RESET_MS));
    return ESP_OK;
}

/* 把 16 位原始值按手册公式换算，放大 1000 倍后用整数四舍五入到放大 10 倍。
 * RH 按手册建议裁剪到 0–100 %RH。 */
static void convert(uint16_t t_raw, uint16_t rh_raw, sht4x_sample_t *out)
{
    /* T = -45 + 175 * S_T / 65535；×1000 避免浮点 */
    int32_t t_milli = -45000 + (int32_t)((175000LL * t_raw) / 65535);
    /* RH = -6 + 125 * S_RH / 65535；×1000 */
    int32_t rh_milli = -6000 + (int32_t)((125000LL * rh_raw) / 65535);

    out->t_raw = t_raw;
    out->rh_raw = rh_raw;
    out->temperature_c = -45.0f + 175.0f * ((float)t_raw / 65535.0f);

    /* 裁剪非物理湿度值（手册 4.5 建议） */
    if (rh_milli < 0) rh_milli = 0;
    if (rh_milli > 100000) rh_milli = 100000;
    out->humidity_rh = (float)rh_milli / 1000.0f;

    /* milli -> 十分位，四舍五入（对负数向远离零方向取整） */
    out->temperature_c10 = (int16_t)((t_milli >= 0 ? (t_milli + 50) : (t_milli - 50)) / 100);
    out->humidity_rh10 = (uint16_t)((rh_milli + 50) / 100);
}

/* 单次测量尝试：发命令 → 延时 → 读 6 字节 → 两次 CRC 校验 → 换算。
 * 注意数据只能被读一次，因此调用者重试时必须重新调用本函数。 */
static esp_err_t measure_once(uint8_t cmd, uint32_t wait_ms, sht4x_sample_t *out)
{
    uint8_t buf[SHT4X_RESP_LEN];
    const uint8_t command = cmd;

    /* 手册 4.1：传感器不支持时钟拉伸，主机发完命令后必须自行等待 */
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, &command, 1, SHT4X_XFER_TIMEOUT_MS), TAG,
                        "measure cmd 0x%02X transmit failed", cmd);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));

    /* 若传感器仍忙，它会对读头返回 NACK，这里表现为接收失败 */
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, buf, sizeof(buf), SHT4X_XFER_TIMEOUT_MS), TAG,
                        "measure receive failed (sensor busy? NACK)");

    if (sht4x_crc8(&buf[0], 2) != buf[2]) {
        ESP_LOGW(TAG, "temperature CRC mismatch: calc=0x%02X recv=0x%02X",
                 sht4x_crc8(&buf[0], 2), buf[2]);
        return ESP_ERR_INVALID_CRC;
    }
    if (sht4x_crc8(&buf[3], 2) != buf[5]) {
        ESP_LOGW(TAG, "humidity CRC mismatch: calc=0x%02X recv=0x%02X",
                 sht4x_crc8(&buf[3], 2), buf[5]);
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t t_raw = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t rh_raw = ((uint16_t)buf[3] << 8) | buf[4];
    convert(t_raw, rh_raw, out);
    return ESP_OK;
}

esp_err_t sht4x_read(sht4x_sample_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_FAIL;
    /* 手册：数据只能读一次，所以每次重试都重发测量命令。
     * 重试之间留出间隔：首次失败通常是传感器尚未就绪（对读头 NACK），
     * 立即重试仍会撞上同一个状态，故退避后再试。 */
    for (int attempt = 0; attempt < 3; attempt++) {
        err = measure_once(SHT4X_CMD_MEAS_HIGH, SHT4X_WAIT_HIGH_MS, out);
        if (err == ESP_OK) return ESP_OK;
        if (attempt < 2) {
            ESP_LOGW(TAG, "read attempt %d failed (%s), retrying", attempt + 1, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5));   /* 退避，给传感器进入空闲态的时间 */
        }
    }
    return err;
}

esp_err_t sht4x_read_serial(uint32_t *serial)
{
    if (!serial) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t buf[SHT4X_RESP_LEN];
    const uint8_t cmd = SHT4X_CMD_READ_SERIAL;
    esp_err_t err = ESP_FAIL;

    /* 序列号读取（0x89）用递增等待重试，以区分两种可能：
     *   - 等待不足：某个较长的等待会成功
     *   - 本模块不实现该命令：三种等待都稳定 NACK
     * 手册未给出该命令的等待时间，故以实验方式判定。 */
    static const uint32_t waits[] = { 2, 5, 10 };
    for (size_t attempt = 0; attempt < sizeof(waits) / sizeof(waits[0]); attempt++) {
        err = i2c_master_transmit(s_dev, &cmd, 1, SHT4X_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(waits[attempt]));
        err = i2c_master_receive(s_dev, buf, sizeof(buf), SHT4X_XFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "serial read ok at wait=%lu ms", (unsigned long)waits[attempt]);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (err != ESP_OK) {
        /* 仅信息性：序列号不参与测量，失败不影响温湿度读数与 CRC 校验 */
        ESP_LOGI(TAG, "read serial (0x89) not answered (tried 2/5/10 ms); informational only");
        return err;
    }

    if (sht4x_crc8(&buf[0], 2) != buf[2]) return ESP_ERR_INVALID_CRC;
    if (sht4x_crc8(&buf[3], 2) != buf[5]) return ESP_ERR_INVALID_CRC;

    uint16_t word0 = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t word1 = ((uint16_t)buf[3] << 8) | buf[4];
    *serial = ((uint32_t)word0 << 16) | word1;
    return ESP_OK;
}

/* ---------------- 算法自检（纯计算，不访问 I²C） ----------------
 * 开发机没有可作为宿主目标的 C 编译器（esp-clang 仅含 xtensa/riscv 后端，
 * 本机亦无 MSVC/mingw），无法在 PC 上运行 C 单元测试。
 * 因此把自检编译进固件，由目标硬件在启动时对同一份代码做真实执行验证。 */

static bool check_i32(const char *what, int32_t got, int32_t want)
{
    if (got == want) return true;
    ESP_LOGE(TAG, "selftest FAIL %s: got %ld want %ld", what, (long)got, (long)want);
    return false;
}

static bool check_u8(const char *what, uint8_t got, uint8_t want)
{
    if (got == want) return true;
    ESP_LOGE(TAG, "selftest FAIL %s: got 0x%02X want 0x%02X", what, got, want);
    return false;
}

esp_err_t sht4x_selftest(void)
{
    bool ok = true;

    /* CRC-8：手册 Table 6 的测试向量，另附两个由独立实现交叉核对过的向量 */
    const uint8_t v_beef[2] = { 0xBE, 0xEF };
    const uint8_t v_zero[2] = { 0x00, 0x00 };
    const uint8_t v_ff[2]   = { 0xFF, 0xFF };
    ok &= check_u8("CRC(0xBEEF)", sht4x_crc8(v_beef, 2), 0x92);
    ok &= check_u8("CRC(0x0000)", sht4x_crc8(v_zero, 2), 0x81);
    ok &= check_u8("CRC(0xFFFF)", sht4x_crc8(v_ff, 2), 0xAC);

    /* 换算公式边界值（手册 4.5），同时验证 RH 裁剪 */
    sht4x_sample_t s;

    convert(0, 0, &s);
    ok &= check_i32("T(S_T=0)", s.temperature_c10, -450);      /* -45.0 °C */
    ok &= check_i32("RH(S_RH=0) cropped", (int32_t)s.humidity_rh10, 0);  /* -6 % 裁剪为 0 */

    convert(65535, 65535, &s);
    ok &= check_i32("T(S_T=65535)", s.temperature_c10, 1300);  /* 130.0 °C */
    ok &= check_i32("RH(S_RH=65535) cropped", (int32_t)s.humidity_rh10, 1000);  /* 119 % 裁剪为 100 */

    convert(32768, 32768, &s);
    ok &= check_i32("T(S_T=32768)", s.temperature_c10, 425);   /* 42.5 °C */
    ok &= check_i32("RH(S_RH=32768)", (int32_t)s.humidity_rh10, 565);  /* 56.5 % */

    if (!ok) {
        ESP_LOGE(TAG, "sht4x selftest FAILED");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "selftest OK (CRC vectors + conversion boundaries)");
    return ESP_OK;
}
