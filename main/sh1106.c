/* SH1106 128x64 OLED 驱动（横屏，I2C）。
 *
 * 来源：LUXAR 驱动库 sh1106_1p3_landscape v1.0.0（器件模式）。
 * 移植：原版使用 ESP-IDF 旧 I2C API（driver/i2c.h，已被 IDF 6.x 移除），
 *       本文件改用 i2c_master 主机驱动（i2c_new_master_bus / i2c_master_transmit）。
 * 修正：原版初始化序列混入 SSD1306 专用命令 0x8D(电荷泵) 与 0x20(寻址模式)，
 *       SH1106 不支持；改为手册的 0xAD,0x8B（内置 DC-DC 开），并删除 0x20。 */

#include "sh1106.h"
#include "font_8x8.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define DRV_TAG "sh1106"

/* 页寻址帧缓冲：fb[page][column]，page 0 为最上 8 行，共 1024 字节。 */
static uint8_t s_fb[SH1106_PAGES][SH1106_WIDTH];

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

/* 单个 I2C 事务缓冲：1 字节控制字 + 最多一页 128 字节数据。 */
#define SH1106_XFER_MAX (1 + SH1106_WIDTH)

/* 字模契约：FONT_8X13 为固定 8x13 字形，每字形 16 字节 = 8 列 x 2 页。
 * 字节 (列 c, 页 p) = FONT[i*16 + c*2 + p]；页内位 0 = 该页顶行。 */
#define GLYPH_W     FONT_8X13_WIDTH   /* 8 */
#define GLYPH_H     FONT_8X13_HEIGHT  /* 13 */
#define GLYPH_PAGES ((FONT_8X13_HEIGHT + 7) / 8)  /* 2 */
#define GLYPH_BYTES FONT_8X13_BYTES_PER_GLYPH     /* 16 */

/* 码点 -> 字形下标；缺字符返回 -1。 */
static int glyph_index(char c)
{
    for (int i = 0; i < FONT_8X13_GLYPH_COUNT; i++) {
        if (FONT_8X13_CODEPOINTS[i] == (uint16_t)(uint8_t)c) {
            return i;
        }
    }
    return -1;
}

/* 一次 I2C 写：is_cmd=1 控制字 0x00（后续字节均为命令），0 则 0x40（均为显存数据）。 */
static esp_err_t sh1106_send(const uint8_t *payload, size_t len, int is_cmd)
{
    if (len == 0 || len > SH1106_WIDTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t xfer[SH1106_XFER_MAX];
    xfer[0] = is_cmd ? 0x00 : 0x40;
    memcpy(&xfer[1], payload, len);
    return i2c_master_transmit(s_dev, xfer, len + 1, 50);
}

static esp_err_t wr_cmd(const uint8_t *cmd, size_t n) { return sh1106_send(cmd, n, 1); }
static esp_err_t wr_data(const uint8_t *data, size_t n) { return sh1106_send(data, n, 0); }

esp_err_t sh1106_init(void)
{
    /* ---- I2C 主机总线 ---- */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = SH1106_I2C_GPIO_SCL,
        .sda_io_num = SH1106_I2C_GPIO_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), DRV_TAG, "new master bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SH1106_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), DRV_TAG,
                        "add device 0x%02X failed", SH1106_I2C_ADDR);

    /* ---- SH1106 初始化序列（逐条注明手册含义）---- */
    static const uint8_t init[] = {
        0xAE,        /* display off */
        0xD5, 0x80,  /* display clock divide / oscillator: 默认分频 */
        0xA8, 0x3F,  /* multiplex ratio = 64 (0x3F) */
        0xD3, 0x00,  /* display offset = 0 */
        0x40,        /* display start line = 0 */
        0xAD, 0x8B,  /* 内置 DC-DC 电荷泵开（SH1106 手册值；原版误用 SSD1306 的 0x8D）*/
        0xA1,        /* segment remap: 列地址反向，横屏接线方向 */
        0xC8,        /* COM scan direction: 反向扫描 */
        0xDA, 0x12,  /* COM pins 硬件配置（128x64 交替）*/
        0x81, 0xCF,  /* contrast = 0xCF */
        0xD9, 0xF1,  /* pre-charge period */
        0xDB, 0x40,  /* VCOM deselect level */
        0xA4,        /* 显示跟随 RAM 内容（非全亮）*/
        0xA6,        /* 正常显示（非反色）*/
        0xAF,        /* display on */
    };
    esp_err_t err = wr_cmd(init, sizeof(init));
    if (err != ESP_OK) {
        ESP_LOGE(DRV_TAG, "init sequence failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(DRV_TAG, "sh1106 up @0x%02X sda=%d scl=%d", SH1106_I2C_ADDR,
             SH1106_I2C_GPIO_SDA, SH1106_I2C_GPIO_SCL);
    return ESP_OK;
}

void sh1106_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

esp_err_t sh1106_flush(void)
{
    for (int page = 0; page < SH1106_PAGES; page++) {
        /* 列地址 = 段 2 起：低 4 位 0x02 + 高 4 位 0x10。 */
        const uint8_t col[] = { 0x02, 0x10 };
        esp_err_t err = wr_cmd(col, sizeof(col));
        if (err != ESP_OK) return err;
        uint8_t pg = 0xB0 | (uint8_t)page;
        err = wr_cmd(&pg, 1);
        if (err != ESP_OK) return err;
        err = wr_data(s_fb[page], SH1106_WIDTH);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

const uint8_t *sh1106_framebuffer(void) { return (const uint8_t *)s_fb; }

/* 把字形 OR 合入帧缓冲；字形 13 高，跨 top_y 与 top_y+8 两页。 */
static void blit_glyph(int x, int top_y, int glyph)
{
    const uint8_t *base = &FONT_8X13[glyph * GLYPH_BYTES];
    int page0 = top_y >> 3;
    for (int c = 0; c < GLYPH_W; c++) {
        int fx = x + c;
        if (fx < 0 || fx >= SH1106_WIDTH) continue;
        s_fb[page0][fx] |= base[c * GLYPH_PAGES + 0];
        if (page0 + 1 < SH1106_PAGES) {
            s_fb[page0 + 1][fx] |= base[c * GLYPH_PAGES + 1];
        }
    }
}

int sh1106_draw_char(int x, int y, char ch)
{
    if (x < 0 || x >= SH1106_WIDTH) return 0;
    if (y < 0 || y + GLYPH_H > SH1106_HEIGHT) return 0;
    if (y & 7) return 0;   /* 只接受页对齐起点 */
    int g = glyph_index(ch);
    if (g < 0) return 0;
    blit_glyph(x, y, g);
    return GLYPH_W;
}

int sh1106_draw_row(int frame_x, int frame_pixel_y, const char *str)
{
    if (!str) return 0;
    int cx = frame_x;
    for (const char *p = str; *p; p++) {
        if (cx >= SH1106_WIDTH) break;   /* 越界截断，不回绕 */
        int w = sh1106_draw_char(cx, frame_pixel_y, *p);
        cx += (w > 0) ? w : 1;           /* 缺字留一格占位，保持排版 */
    }
    return cx - frame_x;
}

int sh1106_row_centered(int frame_pixel_y, const char *str, int *out_x)
{
    size_t n = str ? strlen(str) : 0;
    int w = GLYPH_W * (int)n;
    if (w > SH1106_WIDTH) w = SH1106_WIDTH;
    int x = (SH1106_WIDTH - w) / 2;
    if (out_x) *out_x = x;
    sh1106_draw_row(x, frame_pixel_y, str);
    return w;
}
