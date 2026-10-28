/**
 * 智慧农场 ESP32 主程序 —— 入口
 *
 * 传感器分工：
 *   - SHT40 （空气温湿度，I²C，SDA=GPIO25 / SCL=GPIO26，地址 0x44）
 *   - DS18B20（水温，1-Wire，DQ=GPIO4，防水探头）
 *   - SH1106 OLED 128x64（显示，I²C，SDA=GPIO21 / SCL=GPIO22，地址 0x3C）
 * 引脚权威表见 board_pins.h。
 *
 * 显示布局（8x13 字库，行距 16 px；字库仅 ASCII，故标签用英文）：
 *   第 1 行（页 0）  AIR T: 29.8C    空气温度（SHT40）
 *   第 2 行（页 2）  AIR RH: 60.5%   空气湿度（SHT40）
 *   第 3 行（页 4）  WATER: 24.3C    水温（DS18B20）
 *   第 4 行（页 6）  AIR:OK WTR:OK   传感器状态
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_svc.h"
#include "board_pins.h"
#include "cmd.h"
#include "ds18b20.h"
#include "i2c_diag.h"
#include "mqtt_app.h"
#include "net_config.h"
#include "relay.h"
#include "rules.h"
#include "sht4x.h"
#include "sh1106.h"
#include "wifi_mgr.h"

#include "nvs_flash.h"

/* 编译期防漂移：board_pins.h 是引脚权威表，各驱动头文件自带的定义必须与之一致。 */
_Static_assert(BOARD_OLED_I2C_SDA == SH1106_I2C_GPIO_SDA, "board_pins.h 与 sh1106.h 的 OLED SDA 不一致");
_Static_assert(BOARD_OLED_I2C_SCL == SH1106_I2C_GPIO_SCL, "board_pins.h 与 sh1106.h 的 OLED SCL 不一致");
_Static_assert(BOARD_OLED_I2C_ADDR == SH1106_I2C_ADDR, "board_pins.h 与 sh1106.h 的 OLED 地址不一致");
_Static_assert(BOARD_SHT40_I2C_ADDR == SHT4X_I2C_ADDR, "board_pins.h 与 sht4x.h 的 SHT40 地址不一致");

static const char *TAG = "smartfarm";

/* 把放大 10 倍的整数格式化为 "前缀 + [-]整数.小数 + 后缀"。
 * 正值留一个符号位（空格）以免画面宽度跳动。 */
static void fmt_tenths(char *buf, size_t n, const char *prefix, int value10, const char *suffix)
{
    char sign = ' ';
    int32_t v = value10;
    if (v < 0) {
        sign = '-';
        v = -v;
    }
    snprintf(buf, n, "%s%c%ld.%ld%s", prefix, sign, (long)(v / 10), (long)(v % 10), suffix);
}

/* 把放大 10 倍的整数格式化成 JSON 数字（无空格前缀，负数带 '-'） */
static void fmt_num(char *buf, size_t n, int value10)
{
    int32_t v = value10;
    const char *sign = "";
    if (v < 0) {
        sign = "-";
        v = -v;
    }
    snprintf(buf, n, "%s%ld.%ld", sign, (long)(v / 10), (long)(v % 10));
}

/* 组一条遥测 JSON —— 蓝牙与 MQTT **共用同一份**，保证两条通道数据一致。
 *
 * 读数无效时发 JSON null，而不是把哨兵值当数据发出去：
 * 实测踩过 DS18B20_TEMP_ERROR(32767) 被格式化成 "3276.7"，
 * 消费端若忽略 _ok 标志就会显示 3276.7 ℃ 这种荒谬温度。 */
static void build_telemetry_json(char *out, size_t n,
                                 bool air_ok, const sht4x_sample_t *air,
                                 bool water_ok, int16_t water_c10,
                                 uint8_t rout, uint8_t rin, uint32_t uptime_s)
{
    char t_air[12] = "null", rh_air[12] = "null", t_water[12] = "null";
    if (air_ok) {
        fmt_num(t_air, sizeof(t_air), air->temperature_c10);
        fmt_num(rh_air, sizeof(rh_air), (int)air->humidity_rh10);
    }
    if (water_ok) {
        fmt_num(t_water, sizeof(t_water), water_c10);
    }

    snprintf(out, n,
             "{\"air_ok\":%d,\"air_t\":%s,\"air_rh\":%s,"
             "\"water_ok\":%d,\"water_t\":%s,"
             "\"relay\":%u,\"ain\":%u,\"rssi\":%d,\"up_s\":%lu}",
             air_ok ? 1 : 0, t_air, rh_air,
             water_ok ? 1 : 0, t_water,
             (unsigned)rout, (unsigned)rin, wifi_mgr_rssi(),
             (unsigned long)uptime_s);
}

static void draw_frame(bool air_ok, const sht4x_sample_t *air, bool water_ok, int16_t water_c10,
                       uint8_t relay_out, uint8_t relay_in)
{
    char line[24];

    sh1106_clear();

    /* 第 1 行：空气温度（SHT40） */
    if (air_ok) {
        fmt_tenths(line, sizeof(line), "AIR T:", air->temperature_c10, "C");
    } else {
        snprintf(line, sizeof(line), "AIR T: --");
    }
    sh1106_draw_row(0, 0, line);

    /* 第 2 行：空气湿度（SHT40） */
    if (air_ok) {
        fmt_tenths(line, sizeof(line), "AIR RH:", (int)air->humidity_rh10, "%");
    } else {
        snprintf(line, sizeof(line), "AIR RH: --");
    }
    sh1106_draw_row(0, 16, line);

    /* 第 3 行：水温（DS18B20） */
    if (water_ok) {
        fmt_tenths(line, sizeof(line), "WATER:", water_c10, "C");
    } else {
        snprintf(line, sizeof(line), "WATER: --");
    }
    sh1106_draw_row(0, 32, line);

    /* 第 4 行：继电器输出状态与测试输入状态（O=输出，I=输入，1 表示吸合/高电平） */
    snprintf(line, sizeof(line), "O:%d%d%d I:%d%d%d",
             (relay_out >> 0) & 1, (relay_out >> 1) & 1, (relay_out >> 2) & 1,
             (relay_in >> 0) & 1, (relay_in >> 1) & 1, (relay_in >> 2) & 1);
    sh1106_draw_row(0, 48, line);

    sh1106_flush();
}

void app_main(void)
{
    printf("\n[smartfarm] boot ok\n");

    /* 两个驱动都先跑算法自检：CRC 与换算必须与数据手册一致，否则读数不可信。
     * 开发机无宿主 C 编译器，故由目标硬件在启动时验证同一份代码。 */
    if (sht4x_selftest() != ESP_OK) {
        ESP_LOGE(TAG, "sht4x selftest failed, aborting");
        abort();
    }
    if (ds18b20_selftest() != ESP_OK) {
        ESP_LOGE(TAG, "ds18b20 selftest failed, aborting");
        abort();
    }
    /* 指令解析器自检：只验证纯解析逻辑，**绝不动继电器** */
    cmd_selftest();
    /* 远程通道白名单自检：安全不变量（配网不得从远程修改），同样不动继电器 */
    cmd_remote_whitelist_selftest();
    /* 日志凭据遮蔽自检：安全不变量（WiFi 密码不得进日志），不执行任何指令 */
    cmd_log_mask_selftest();
    /* 规则解析器自检：只验证纯解析，不写 NVS、不动继电器 */
    rules_selftest();
    /* MQTT 入站指令重组自检：只验证纯逻辑，**不联网、不发布任何消息**
     * （超长/分片不一致的远端载荷必须被拒绝，而不是截断后执行） */
    mqtt_app_selftest();

    /* ---- 启动总线诊断（必须在 I2C 驱动安装前，诊断需独占引脚）----
     * 带阳性对照：先扫已知良好的 OLED 总线（GPIO21/22）。
     * 若 OLED 在 0x3C 应答，则证明扫描器本身正常，
     * 此时 SHT40 总线扫不到设备即可确定为接线/供电问题，而不是工具问题。 */
    ESP_LOGW(TAG, "bus diagnosis with positive control (OLED @21/22)");
    i2c_diag_lines(BOARD_OLED_I2C_SDA, BOARD_OLED_I2C_SCL, "OLED-ctrl");
    int oled_found = i2c_diag_scan_pair(BOARD_OLED_I2C_SDA, BOARD_OLED_I2C_SCL,
                                        "OLED-ctrl", BOARD_OLED_I2C_ADDR);

    i2c_diag_lines(BOARD_SHT40_I2C_SDA, BOARD_SHT40_I2C_SCL, "SHT40");
    int sht_found = i2c_diag_scan_pair(BOARD_SHT40_I2C_SDA, BOARD_SHT40_I2C_SCL,
                                       "SHT40", BOARD_SHT40_I2C_ADDR);

    if (oled_found > 0) {
        ESP_LOGI(TAG, "positive control PASSED: scanner works (OLED seen, %d dev)", oled_found);
    } else {
        ESP_LOGE(TAG, "positive control FAILED: OLED not seen either -> suspect scanner/pin setup, "
                      "diag conclusions about SHT40 are unreliable");
    }
    if (sht_found == 0) {
        ESP_LOGE(TAG, "SHT40 bus: nothing responds on GPIO%d/GPIO%d",
                 BOARD_SHT40_I2C_SDA, BOARD_SHT40_I2C_SCL);
        /* 最后手段：在常见 I2C 引脚对中搜索 0x44/0x45 */
        i2c_diag_sweep_pins();
    }

    esp_err_t err = sh1106_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sh1106 init failed: %s (%d)", esp_err_to_name(err), (int)err);
        return;
    }

    /* ---- SHT40：空气温湿度 ---- */
    err = sht4x_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sht4x init failed: %s (%d)", esp_err_to_name(err), (int)err);
    } else if (sht4x_probe() == ESP_OK) {
        ESP_LOGI(TAG, "sht4x detected @0x%02X", SHT4X_I2C_ADDR);
    } else {
        ESP_LOGW(TAG, "sht4x NOT responding on GPIO%d/GPIO%d",
                 BOARD_SHT40_I2C_SDA, BOARD_SHT40_I2C_SCL);
    }

    /* ---- DS18B20：水温 ---- */
    err = ds18b20_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ds18b20 init failed: %s", esp_err_to_name(err));
    } else if (ds18b20_probe() == ESP_OK) {
        uint8_t rom[DS18B20_ROM_SIZE] = { 0 };
        if (ds18b20_read_rom(rom) == ESP_OK) {
            ESP_LOGI(TAG, "ds18b20 detected on GPIO%d, ROM=%02X%02X%02X%02X%02X%02X%02X%02X",
                     BOARD_DS18B20_DQ, rom[0], rom[1], rom[2], rom[3],
                     rom[4], rom[5], rom[6], rom[7]);
        } else {
            ESP_LOGI(TAG, "ds18b20 detected on GPIO%d (ROM read failed)", BOARD_DS18B20_DQ);
        }
    } else {
        ESP_LOGW(TAG, "ds18b20 NOT responding on GPIO%d (check 4.7k pull-up to 3V3, VDD, GND)",
                 BOARD_DS18B20_DQ);
    }

    /* ---- 继电器输出 + 测试输入 ---- */
    err = relay_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "relay init failed: %s", esp_err_to_name(err));
    }

    /* 启动首次温度转换；等它完成后再进主循环，避免读到 +85 ℃ 上电值 */
    if (ds18b20_start_conversion() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(800));
    }

    /* ---- 网络：NVS + WiFi（全程非阻塞）----
     * 设计原则：联网是"增强"而非"前提"。WiFi 起不来或连不上时，
     * 显示、采集、继电器（含本地条件逻辑）都必须照常工作。 */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(nvs_err));
    } else {
        /* WiFi 驱动需要 NVS 保存 PHY 校准数据；NimBLE 也需要 NVS 存配对密钥 */
        /* 本地控制规则也存 NVS —— 必须在 nvs_flash_init 之后再读 */
        rules_init();
        wifi_mgr_start();   /* 非阻塞；失败只记日志，不影响后续 */
        ble_svc_start();    /* 非阻塞；同样只是"增强" */
    }

    bool sntp_started = false;
    bool mqtt_started = false;
    uint32_t last_pub_ms = 0;

    while (1) {
        /* 循环开头取一次时间戳：规则引擎（条件计时）与遥测（up_s）都要用，
         * 放在这里可保证同一轮里两者用的是同一个时刻。 */
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* 空气温湿度（SHT40） */
        sht4x_sample_t air = { 0 };
        bool air_ok = (sht4x_read(&air) == ESP_OK);

        /* 水温（DS18B20）：返回上一次转换结果，并在内部启动下一次转换 */
        int16_t water_c10 = DS18B20_TEMP_ERROR;
        bool water_ok = (ds18b20_read_temperature(&water_c10) == ESP_OK);
        static bool ds_diag_done = false;
        static int ds_fail_streak = 0;

        /* 传感器日志降到 **10 秒一次**：1 Hz 刷屏会把真正要看的信息淹掉
         * （实测每次读串口都上万字符，排查时全是无效信息）。
         * 实时性不受影响：蓝牙仍每秒推送、MQTT 仍每 30 秒上报，都是同一份 JSON。
         * 失败日志（WARN/ERROR）不受此限，永远打印。 */
        static uint32_t log_div;
        bool log_now = ((log_div++ % 10) == 0);

        if (air_ok) {
            if (log_now) {
                ESP_LOGI(TAG, "AIR  T=%d.%d C  RH=%u.%u %%",
                         air.temperature_c10 / 10, abs(air.temperature_c10 % 10),
                         (unsigned)air.humidity_rh10 / 10, (unsigned)air.humidity_rh10 % 10);
            }
        } else {
            ESP_LOGW(TAG, "SHT40 (air) read failed");
        }

        if (water_ok) {
            ds_fail_streak = 0;
            if (log_now) {
                ESP_LOGI(TAG, "WATER T=%d.%d C", water_c10 / 10, abs(water_c10 % 10));
            }
        } else {
            ds_fail_streak++;
            ESP_LOGW(TAG, "DS18B20 (water) read failed (streak %d)", ds_fail_streak);
            /* 只有**连续**失败才判定为真故障并做诊断。
             * 本工程实测：WiFi 活动期间的一次 CRC 偶发错就会误触发整轮重型诊断，
             * 而诊断本身长时间持临界区，反过来又会干扰 WiFi（两者都在 core 0）。
             * 因此要求连续 3 次失败，避免"诊断自己制造故障"。 */
            if (ds_fail_streak >= 3 && !ds_diag_done) {
                ds_diag_done = true;
                ds18b20_diag();
            }
        }

        /* 只**读取**测试输入用于上报，绝不用它驱动输出。
         *
         * 踩过的坑（真实案例）：这里原来是 relay_apply_inputs()，会把输入直接镜像到输出。
         * 于是 BLE/MQTT 刚把继电器打开，1 秒后主循环又按输入(000)把它写回关闭——
         * **两个"主控"抢同一个输出**，表现为"继电器开一两秒就自己关了"。
         * 输入镜像只是当初手动验证继电器用的测试手段；现在有了正式控制通道，
         * 本地开关若要控制继电器，应作为阶段 B 的**显式规则**，而不是无条件覆盖。 */
        /* ---- 规则引擎：由启用的规则决定各继电器"应有"的状态 ----
         * 只有被规则覆盖的继电器会被改写，手动 relay:* 控制的继电器不受影响。
         * 时钟未同步时定时规则不做判断（宁可先不动，见 rules_evaluate 说明）。
         * 传感器读数由这里传入，rules 模块自己不读传感器（保持可自检）。
         *
         * 注意**不要**用"有规则参与"作为是否应用的条件：规则被禁用/删除时
         * applied 可能是 0，但恰恰此时需要把继电器归到关闭（失效安全）。 */
        rules_inputs_t rin_rule = {
            .air_ok = air_ok,
            .air_t10 = air.temperature_c10,
            .air_rh10 = (int16_t)air.humidity_rh10,
            .water_ok = water_ok,
            .water_t10 = water_c10,
            .uptime_s = now_ms / 1000,
        };
        uint8_t rule_mask = 0;
        rules_evaluate(&rin_rule, &rule_mask);
        {
            uint8_t before = relay_get_all();
            if (rule_mask != before) {
                relay_set_all(rule_mask);
                ESP_LOGI(TAG, "rule engine: out %d%d%d -> %d%d%d",
                         (before >> 0) & 1, (before >> 1) & 1, (before >> 2) & 1,
                         (rule_mask >> 0) & 1, (rule_mask >> 1) & 1, (rule_mask >> 2) & 1);
            }
        }

        uint8_t rin = relay_test_inputs_all();
        uint8_t rout = relay_get_all();
        /* 只在**状态变化**时打印：继电器持续吸合时每秒打印会把日志淹掉 */
        static uint8_t prev_in, prev_out;
        if (rin != prev_in || rout != prev_out) {
            ESP_LOGI(TAG, "RELAY out=%d%d%d in=%d%d%d",
                     (rout >> 0) & 1, (rout >> 1) & 1, (rout >> 2) & 1,
                     (rin >> 0) & 1, (rin >> 1) & 1, (rin >> 2) & 1);
            prev_in = rin;
            prev_out = rout;
        }

        /* ---- 网络状态机（非阻塞推进，绝不阻塞采集与显示）----
         * 顺序不能颠倒：WiFi → SNTP → MQTT。
         * SNTP 必须先成功，因为 TLS 要校验证书有效期，时钟停在 1970 会导致
         * 证书被判为"尚未生效"而握手失败。 */
        if (!sntp_started && wifi_mgr_is_connected()) {
            if (wifi_mgr_sntp_start() == ESP_OK) sntp_started = true;
        }
        if (!mqtt_started && sntp_started && wifi_mgr_time_synced()) {
            if (mqtt_app_start() == ESP_OK) mqtt_started = true;
        }

        /* WiFi 反复连不上时做一次扫描诊断（只做一次）。
         * 目的：区分"密码错误"与"安全模式不匹配(WPA3/PMF)"——
         * 两者症状都是 4 次握手超时，但处置办法完全不同。 */
        static bool wifi_scan_done = false;
        if (!wifi_scan_done && !wifi_mgr_is_connected() && wifi_mgr_disconnect_count() >= 3) {
            wifi_scan_done = true;
            wifi_mgr_diag_scan();
        }

        /* 定时上报（30 秒一次）。算一下用量：约 140 B/条 × 2880 条/月 ≈ 0.4 MB，
         * 只占 HiveMQ 免费额度 10 GB 的 0.004%。所以不必为省流量牺牲体验——
         * APP 一打开就能看到近期数据；更细的内容仍可走 cmd 主题按需拉取。 */
        /* 遥测：蓝牙**每秒**推送（近距离实时），MQTT **每 30 秒**发一次，
         * 两条通道内容完全相同（同一个 build_telemetry_json 结果）。
         *
         * 用量对比：蓝牙是本地射频，不消耗服务器流量；MQTT 30 秒一次约 0.4 MB/月，
         * 只占 HiveMQ 免费额度 10 GB 的 0.004%。 */
        char json[224];
        build_telemetry_json(json, sizeof(json), air_ok, &air, water_ok, water_c10,
                             rout, rin, now_ms / 1000);

        /* 未连接或对端未开启 Notify 时内部直接返回，属正常情况，不刷日志 */
        ble_svc_notify_telemetry(json);

        /* MQTT 上报：常规每 30 秒一次；收到控制指令时立刻补发一条，
         * 否则 APP 要等下一个周期才能看到控制结果。 */
        bool immediate = mqtt_app_take_publish_request();
        if (mqtt_app_is_connected() &&
            (immediate || last_pub_ms == 0 || now_ms - last_pub_ms >= 30000)) {
            if (mqtt_app_publish_telemetry(json) == ESP_OK) {
                ESP_LOGI(TAG, "published telemetry%s: %s", immediate ? " (on request)" : "", json);
                last_pub_ms = now_ms;
            }
        }

        draw_frame(air_ok, &air, water_ok, water_c10, rout, rin);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
