#ifndef SH1106_H
#define SH1106_H

#include <stdint.h>
#include "esp_err.h"

/* 中景园 1.3 寸 128x64 单色 OLED（SH1106 / SSD1106G），横屏。
 * I2C 从机地址 0x3C，SDA=GPIO21 / SCL=GPIO22。
 * 显存按 8 页 x 128 列页寻址；字模为列主序（每字形 8 列 x 2 页，8x13）。 */

#define SH1106_WIDTH  128
#define SH1106_HEIGHT 64
#define SH1106_PAGES  8   /* 64/8 */

#define SH1106_I2C_GPIO_SDA 21
#define SH1106_I2C_GPIO_SCL 22
#define SH1106_I2C_ADDR     0x3C

/* 建 I2C 主机总线 + 挂设备 + 按 SH1106 手册开显（display on、内置 DC-DC 电荷泵）。
 * 成功返回 ESP_OK；失败返回底层 I2C 错误码。 */
esp_err_t sh1106_init(void);

/* 清空整帧（不立即上传）。 */
void sh1106_clear(void);

/* 把整帧上传到屏：逐页 8 次 I2C 写，每页从 GDDRAM 段 2 起写 128 字节，
 * 以避开 SH1106 132 列 RAM 与本屏 128 列可视窗之间的列偏移。 */
esp_err_t sh1106_flush(void);

/* 在位面 (frame_x, frame_pixel_y) 处绘制 8x13 字形，返回占用列宽（8，缺字返回 0）。
 * frame_pixel_y 必须是 8 的倍数；字形高 13 会跨越 2 页。 */
int sh1106_draw_char(int frame_x, int frame_pixel_y, char ch);

/* 在同一行从 frame_x 起绘制字符串，越过右边界截断不回绕。返回实际占用列宽。 */
int sh1106_draw_row(int frame_x, int frame_pixel_y, const char *str);

/* 居中绘制一行；out_x 回传实际起始列。返回占用列宽。 */
int sh1106_row_centered(int frame_pixel_y, const char *str, int *out_x);

/* 帧缓冲页主序线性视图（页大端，每页 128 列），供自检 CRC。 */
const uint8_t *sh1106_framebuffer(void);

#endif /* SH1106_H */
