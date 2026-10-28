#ifndef RULES_H
#define RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * 设备端本地控制规则
 *
 * 为什么规则要跑在**设备上**而不是 APP 里：
 *   断网、APP 未打开、手机没电时，水泵/加热器仍必须按计划开关。
 *   APP 只是"下发与查看"的界面，不是控制逻辑的宿主——
 *   把定时逻辑放在 APP 里，等于"手机不在就没人浇地"。
 *
 * 三种模式：
 *   manual    手动：规则不干预，继电器只由 relay:* 指令直接控制
 *   schedule  定时：每天 on 时刻开、off 时刻关（off <= on 视为跨午夜）
 *   condition 条件：传感器读数满足 op/val 且持续 hold 秒后开；不再满足则关
 *
 * 配置语法（一条指令写完一条规则，方便 APP 一次下发）：
 *   rule:list                          列出全部规则（JSON）
 *   rule:add [k=v ...]                 新建规则（占用最小空闲槽位），返回其 id
 *   rule:<id> [k=v ...]                只修改给出的字段，其余保持不变
 *   rule:del <id>                      删除规则（槽位释放，**其余 id 不变**）
 *
 *   注意 `rule:del <id>` 用空格而不是冒号：字段值里的时刻是 HH:MM，
 *   本身含冒号，再引入冒号分隔会制造歧义。
 *
 *   键：mode=manual|schedule|condition   relay=0|1|2
 *       enabled=0|1                      on=HH:MM   off=HH:MM
 *       src=air_t|air_rh|water_t         op=>|<    val=30.0   hold=秒
 *       invalid=hold|off|on
 *
 *   例：rule:add mode=schedule relay=0 on=07:30 off=18:00 enabled=1
 *       rule:1 mode=condition relay=2 src=water_t op=< val=5.0 hold=600
 *
 * ⚠️ **固件里没有任何写死的传感器阈值**。判据的每一项（用哪路读数、
 *    大于还是小于、阈值多少、要持续多少秒、读数无效时怎么办）都来自配置，
 *    存在 NVS 里、可随时改。理由是阈值依作物/季节/传感器布置而变，
 *    写进固件意味着每次调整都要重新编译烧录。
 *
 * 为什么用 "k=v 空格分隔" 而不是 JSON：
 *   不引入 JSON 库依赖，且**可以在蓝牙助手里直接打字测试**。
 *   值里不允许出现空格，因此没有分隔歧义（时刻用 HH:MM，不含空格）。
 */

#define RULES_MAX 4

typedef enum {
    RULE_MODE_MANUAL = 0,
    RULE_MODE_SCHEDULE,
    RULE_MODE_CONDITION,
} rule_mode_t;

/* 条件规则的读数来源 */
typedef enum {
    RULE_SRC_AIR_T = 0,   /* 空气温度 */
    RULE_SRC_AIR_RH,      /* 空气湿度 */
    RULE_SRC_WATER_T,     /* 水温 */
} rule_src_t;

/* 条件规则遇到"读数无效"（传感器故障 / 读取失败）时怎么办。
 * 这也做成**配置项**而不写死：不同场合答案不同——
 *   防冻加热器：读数没了应当继续加热（on），否则会冻坏
 *   灌溉水泵：  读数没了应当停泵（off），否则可能淹掉
 * 出厂默认 hold（保持现状）最保守：既不误开也不误关，同时打日志告警。 */
typedef enum {
    RULE_INVALID_HOLD = 0,   /* 保持当前输出不变（默认） */
    RULE_INVALID_OFF,        /* 强制关闭 */
    RULE_INVALID_ON,         /* 强制开启 */
} rule_invalid_policy_t;

typedef struct {
    bool     used;        /* 槽位是否已占用。**删除时只清 used 不搬移**，
                           * 这样规则 id 永远稳定——否则删掉 1 号会让 2/3 号
                           * 变成 1/2 号，APP 里的引用全部错位。 */
    bool     enabled;
    uint8_t  mode;        /* rule_mode_t */
    uint8_t  relay;       /* 0..2 */

    /* schedule */
    uint16_t on_min;      /* 0..1439，自 00:00 起的分钟数 */
    uint16_t off_min;

    /* condition */
    uint8_t  src;         /* rule_src_t */
    uint8_t  cmp;         /* 0: 大于阈值, 1: 小于阈值 */
    int16_t  val10;       /* 阈值，放大 10 倍（与遥测精度一致） */
    uint16_t hold_s;      /* 需持续满足的秒数 */
    uint8_t  invalid_policy;  /* rule_invalid_policy_t */
} rule_t;

/** 规则求解所需的输入（由主循环提供，rules 模块不自己去读传感器）。 */
typedef struct {
    bool     air_ok;
    int16_t  air_t10;
    int16_t  air_rh10;
    bool     water_ok;
    int16_t  water_t10;
    uint32_t uptime_s;    /* 用于条件规则的"持续满足"计时 */
} rules_inputs_t;

/** 初始化：从 NVS 载入规则。可重复调用（幂等）。 */
esp_err_t rules_init(void);

/** 执行一条 rule: 指令（arg 是 "rule:" 之后的部分）。
 *  ack 中写入回执 JSON。纯逻辑，可在自检里安全调用（见 rules_selftest）。 */
typedef enum {
    RULES_OK = 0,
    RULES_OK_PUSH_TELEM,   /* 成功，调用方宜立即补发一条遥测 */
    RULES_ERR,
} rules_status_t;

rules_status_t rules_execute(const char *arg, char *ack, size_t ack_len);

/** 取当前规则数（用于遥测/状态上报）。 */
int rules_count(void);

/** 求解各继电器**应有**的状态（由启用的规则决定）。
 *
 *  只有被启用规则覆盖的继电器会被改写，其余保持当前状态
 *  （这样手动 relay:* 控制的继电器不会被规则引擎无故复位）。
 *
 *  **失效安全**：某路继电器上一轮受规则控制、这一轮不再受控制时
 *  （规则被删除或被禁用），会被**强制关闭**——否则用户删掉规则以为停了水泵，
 *  实际继电器冻在 ON 继续抽水。
 *
 *  @param in        本次求解的输入（传感器读数与运行时长）
 *  @param out_mask  输出掩码（bit0..bit2 对应继电器 1..3）
 *  @return 参与本次求解的启用规则条数（仅供日志/诊断参考；
 *          调用方**不应**用它决定是否应用 out_mask——规则被删除时它是 0，
 *          但那正是必须应用"强制关闭"的时刻）
 *
 *  ⚠️ 时钟未同步时**不做定时判断**：此时 time() 是 1970 年，
 *     拿它算"现在是不是在 on/off 窗口内"会把所有规则误判，
 *     从而在开机瞬间乱动继电器。宁可先不动，等对时完成再接管。 */
int rules_evaluate(const rules_inputs_t *in, uint8_t *out_mask);

/** 时钟是否可用于定时判断（SNTP 之前为 false）。 */
bool rules_clock_ready(void);

/** 目标板上自检**纯解析逻辑**（不写 NVS、不动继电器）。 */
void rules_selftest(void);

#endif /* RULES_H */
