#ifndef CMD_H
#define CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * BLE 指令解析与执行
 *
 * 为什么用紧凑文本而不是 JSON：
 *   1. 不引入 JSON 库依赖（IDF 6.0.2 已把 cJSON 移出核心，需额外下载）
 *   2. **可以在蓝牙助手里直接打字测试**（写 `relay:100`），无需手敲 JSON
 *   3. 语法极小，可以做到无歧义，并且能写纯解析函数在目标板上自检
 *
 * 语法（大小写不敏感，首尾空白忽略）：
 *   get                   立即回一条遥测
 *   status                回状态
 *   help                  回命令列表
 *   relay:100             三路继电器设为 开/关/关（1 位一路，3 个字符）
 *   relay:0:1             只把第 0 路设为 1（索引从 0 开始）
 *
 * 配网指令（建议通过 WiFi 特征下发，用后即清）：
 *   wifi:ssid:<SSID>      暂存 SSID
 *   wifi:pass:<密码>      暂存密码（**绝不会出现在日志或回执里**）
 *   wifi:connect          用暂存的凭据连接（先落盘 NVS）
 *   wifi:status           回当前连接状态
 *   wifi:clear            清除已保存的凭据
 *
 * 本地控制规则（语法细节见 rules.h）：
 *   rule:list                          列出全部规则
 *   rule:add [k=v ...]                 新建规则
 *   rule:<id> [k=v ...]                修改规则的指定字段
 *   rule:del <id>                      删除规则
 *
 * 分两条指令设置 SSID 与密码（而不是 wifi:ssid:pass 一行），是为了避开
 * "密码里含冒号"和"SSID 里含冒号"的切分歧义——这个问题没有可靠的转义方案。
 *
 * ⚠️ BLE 侧的身份与加密由 ble_svc 负责（A8：配对绑定 + WRITE_ENC），
 *    MQTT 侧的权限边界见 cmd_execute 的 allow_provisioning 参数。
 */

typedef enum {
    CMD_OK = 0,          /* 成功，ACK 已生成 */
    CMD_OK_PUSH_TELEM,   /* 成功，且调用方应立即再推一条遥测 */
    CMD_ERR,             /* 失败，ACK 中是错误说明 */
} cmd_status_t;

/** 纯解析结果（不含副作用，供自检使用）。 */
typedef struct {
    enum {
        CMD_KIND_NONE = 0,
        CMD_KIND_GET,
        CMD_KIND_STATUS,
        CMD_KIND_HELP,
        CMD_KIND_RELAY_ALL,   /* relay:100   */
        CMD_KIND_RELAY_ONE,   /* relay:0:1   */
        CMD_KIND_WIFI_SSID,   /* wifi:ssid:<值> */
        CMD_KIND_WIFI_PASS,   /* wifi:pass:<值> */
        CMD_KIND_WIFI_CONNECT,
        CMD_KIND_WIFI_STATUS,
        CMD_KIND_WIFI_CLEAR,
        CMD_KIND_RULE,        /* rule:<子命令>，语法见 rules.h */
    } kind;
    int index;             /* CMD_KIND_RELAY_ONE：0..2 */
    uint8_t mask;          /* CMD_KIND_RELAY_ALL：bit0=第1路 */
    const char *value;     /* WIFI_SSID/WIFI_PASS/RULE：指向原文本内部，不复制 */
    size_t value_len;
} cmd_parsed_t;

/** 解析指令文本。纯函数，**不产生任何副作用**。
 *  @return true 表示语法可识别。 */
bool cmd_parse(const char *text, cmd_parsed_t *out);

/** 解析并执行（会操作继电器）。ack 中写入回执 JSON。
 *
 *  @param allow_provisioning
 *     是否允许配网类指令（wifi:*）。**BLE 侧传 true，MQTT 侧传 false**：
 *     远程重配 WiFi 没有实际意义——WiFi 一旦变了，设备本来就已经离线、
 *     远程也够不着；而放开它等于让一个 MQTT 账号就能把设备"踢下线"。
 *     配网天生是"站在设备旁边做"的事。
 *  @return cmd_status_t */
cmd_status_t cmd_execute(const char *text, char *ack, size_t ack_len,
                         bool allow_provisioning);

/** 目标板上自检解析器（**绝不动继电器**，只验证纯解析逻辑）。 */
void cmd_selftest(void);

/** 目标板上自检"远程通道白名单"（安全不变量）。
 *
 *  只验证**该被拒绝的确实被拒绝**，以及只读的 wifi:status 被放行；
 *  **不测放行路径**——那会真的操作继电器。
 *  之所以放进启动自检：安全不变量应该像 CRC 一样被持续验证，
 *  而不是只在开发时手工点一次。 */
void cmd_remote_whitelist_selftest(void);

/** 把一条指令文本渲染成**可安全写进日志**的形式。
 *
 *  唯一的机密型指令是 `wifi:pass:<密码>`：它会被渲染成
 *  `wifi:pass:<N bytes hidden>`，**绝不回显内容**；其余指令原样返回
 *  （它们不含凭据，保持可读以便排查）。
 *
 *  为什么要有这个函数：指令文本会出现在**多个通道**的日志里（BLE 指令特征、
 *  MQTT、以及解析失败分支），任何一处漏掉原文打印都会让 WiFi 密码进入串口日志。
 *  日志常被截图、粘贴进 issue、随固件一起打包上传——凭据一旦进去就等同泄漏。
 *  所以遮蔽必须是一个**共用的**动作，而不是各处各自记得"别打印"。
 *
 *  ⚠️ 判定用**文本片段**而不是解析结果：解析失败也必须遮蔽，
 *  否则一条畸形指令（如 `xwifi:pass:<密码>`）会走 "unknown command" 分支被原文打印。 */
void cmd_mask_for_log(const char *text, char *out, size_t out_len);

/** 目标板上自检"日志凭据遮蔽"（安全不变量）。
 *
 *  攻击场景：拿到设备日志（截图 / 粘贴 / 固件包）的人不应能读出 WiFi 密码。
 *  探针验证 `cmd_mask_for_log` 对各类含密码输入都不回显内容，
 *  且不会误伤普通指令的可读性。 */
void cmd_log_mask_selftest(void);

#endif /* CMD_H */
