/* BLE 指令解析与执行。 */

#include "cmd.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "net_config.h"
#include "relay.h"
#include "rules.h"
#include "wifi_mgr.h"

#define TAG "cmd"

/* ---------------- 纯解析 ---------------- */

/* 跳过首尾空白，返回内部指针（不复制） */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static size_t trimmed_len(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                     s[n - 1] == '\n')) {
        n--;
    }
    return n;
}

static bool token_eq(const char *s, size_t n, const char *lit)
{
    size_t m = strlen(lit);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)lit[i])) return false;
    }
    return true;
}

/* 解析 "100" 这样的 3 位继电器字符串 -> 掩码；任何非法字符都判失败 */
static bool parse_mask3(const char *s, size_t n, uint8_t *mask_out)
{
    if (n != 3) return false;
    uint8_t mask = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '0' && s[i] != '1') return false;
        if (s[i] == '1') mask |= (uint8_t)(1u << i);
    }
    *mask_out = mask;
    return true;
}

bool cmd_parse(const char *text, cmd_parsed_t *out)
{
    if (text == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    const char *p = skip_ws(text);
    size_t n = trimmed_len(p);
    if (n == 0) return false;

    /* relay:... 形式 */
    const char *colon = memchr(p, ':', n);
    if (colon != NULL) {
        size_t head = (size_t)(colon - p);
        const char *arg = colon + 1;
        size_t argn = n - head - 1;

        if (!token_eq(p, head, "relay")) {
            /* ---- 本地控制规则 ----
             * 把 "rule:" 之后的**全部内容**交给 rules.c 解析（它有独立的子语法）。
             * 这一层不能按冒号再切：规则字段里的时刻是 HH:MM，本身含冒号。 */
            if (token_eq(p, head, "rule")) {
                out->kind = CMD_KIND_RULE;
                out->value = arg;
                out->value_len = argn;
                return true;
            }

            /* ---- 配网指令 ---- */
            if (token_eq(p, head, "wifi")) {
                if (token_eq(arg, argn, "connect")) { out->kind = CMD_KIND_WIFI_CONNECT; return true; }
                if (token_eq(arg, argn, "status"))  { out->kind = CMD_KIND_WIFI_STATUS;  return true; }
                if (token_eq(arg, argn, "clear"))   { out->kind = CMD_KIND_WIFI_CLEAR;   return true; }

                /* wifi:ssid:<值> / wifi:pass:<值>
                 * 只按**前两个**冒号切分，故值里可以含冒号——
                 * 这正是"密码/SSID 里含冒号"这个经典歧义的解法。 */
                const char *c2 = memchr(arg, ':', argn);
                if (c2 != NULL) {
                    size_t keyn = (size_t)(c2 - arg);
                    const char *val = c2 + 1;
                    size_t valn = argn - keyn - 1;
                    if (token_eq(arg, keyn, "ssid")) {
                        out->kind = CMD_KIND_WIFI_SSID;
                        out->value = val;
                        out->value_len = valn;
                        return true;
                    }
                    if (token_eq(arg, keyn, "pass")) {
                        out->kind = CMD_KIND_WIFI_PASS;
                        out->value = val;
                        out->value_len = valn;
                        return true;
                    }
                }
            }
            return false;
        }

        /* relay:100 —— 3 位组合 */
        uint8_t mask = 0;
        if (parse_mask3(arg, argn, &mask)) {
            out->kind = CMD_KIND_RELAY_ALL;
            out->mask = mask;
            return true;
        }

        /* relay:<idx>:<0|1> —— 单路 */
        const char *c2 = memchr(arg, ':', argn);
        if (c2 != NULL) {
            size_t idxn = (size_t)(c2 - arg);
            const char *val = c2 + 1;
            size_t valn = argn - idxn - 1;

            if (idxn != 1 || valn != 1) return false;
            if (arg[0] < '0' || arg[0] > '2') return false;
            if (val[0] != '0' && val[0] != '1') return false;

            out->kind = CMD_KIND_RELAY_ONE;
            out->index = arg[0] - '0';
            out->mask = (val[0] == '1') ? (uint8_t)(1u << out->index) : 0;
            return true;
        }
        return false;
    }

    /* 无参数指令 */
    if (token_eq(p, n, "get"))    { out->kind = CMD_KIND_GET;    return true; }
    if (token_eq(p, n, "status")) { out->kind = CMD_KIND_STATUS; return true; }
    if (token_eq(p, n, "help"))   { out->kind = CMD_KIND_HELP;   return true; }

    return false;
}

/* ---------------- 执行 ---------------- */

static void write_help(char *ack, size_t n)
{
    snprintf(ack, n,
             "{\"ack\":\"help\",\"cmds\":[\"get\",\"status\",\"relay:100\",\"relay:0:1\","
             "\"wifi:ssid:<SSID>\",\"wifi:pass:<PASS>\",\"wifi:connect\","
             "\"wifi:status\",\"wifi:clear\"],"
             "\"note\":\"wifi:* only works over Bluetooth (BLE); remote channels"
             " accept get/status/relay only\"}");
}

/* 把一条指令文本渲染成可安全写日志的形式（理由见 cmd.h）。 */
static char lower_ascii(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

void cmd_mask_for_log(const char *text, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    out[0] = '\0';
    if (text == NULL) return;

    static const char MARK[] = "wifi:pass:";
    const size_t mark_len = sizeof(MARK) - 1;

    /* 用**文本片段**判定，而不是用 cmd_parse 的结果：
     * 解析失败时也必须遮蔽（否则畸形指令走 "unknown command" 分支被原文打印）。 */
    for (const char *p = text; *p != '\0'; p++) {
        size_t k = 0;
        /* p[k] 一旦是 '\0' 就与 MARK[k]（都是字母）不等，循环必然停下，不会越界读 */
        while (k < mark_len && lower_ascii(p[k]) == MARK[k]) k++;
        if (k == mark_len) {
            /* 保留前缀（通常是空）+ 长度；**绝不回显内容** */
            snprintf(out, out_len, "%.*swifi:pass:<%u bytes hidden>",
                     (int)(p - text), text, (unsigned)strlen(p + mark_len));
            return;
        }
    }

    /* 其余指令不含凭据，原样返回以便排查 */
    snprintf(out, out_len, "%s", text);
}

cmd_status_t cmd_execute(const char *text, char *ack, size_t ack_len,
                         bool allow_provisioning)
{
    if (ack == NULL || ack_len == 0) return CMD_ERR;
    ack[0] = '\0';

    cmd_parsed_t c;
    if (!cmd_parse(text, &c)) {
        /* 这里也要遮蔽：无法解析的文本同样可能夹带密码
         * （例如 "xwifi:pass:<密码>"）。 */
        char shown[96];
        cmd_mask_for_log(text, shown, sizeof(shown));
        ESP_LOGW(TAG, "unknown command: \"%s\"", shown);
        write_help(ack, ack_len);
        return CMD_ERR;
    }

    /* 远程通道用**白名单**：只放行明确列出的指令，其余一律拒绝。
     *
     * 为什么不是黑名单（列出禁止的 wifi 指令）：**黑名单是"失败开放"**——
     * 以后新增任何 wifi 指令，它默认就是允许的。白名单则是"失败关闭"。
     *
     * 为什么配网必须只限本地：改 WiFi 一旦失败，你正在用的这条 MQTT 通道
     * 本身也就断了——它依赖 WiFi。所以远程改 WiFi 是**自毁操作**，
     * 不只是"没价值"。配网天生是"站在设备旁边做"的事。 */
    if (!allow_provisioning) {
        bool remote_ok =
            (c.kind == CMD_KIND_GET || c.kind == CMD_KIND_STATUS ||
             c.kind == CMD_KIND_HELP || c.kind == CMD_KIND_RELAY_ALL ||
             c.kind == CMD_KIND_RELAY_ONE ||
             /* wifi:status 是**只读**的，不修改任何设置，远程放行用于诊断
              * （远程排查"设备连在哪个 AP、信号多强、凭据来自哪"很有用） */
             c.kind == CMD_KIND_WIFI_STATUS ||
             /* 规则是 APP 的主要配置面，远程可读写。
              * 这不引入新的风险类别：拿到 MQTT 凭据的人本来就能直接发 relay:*
              * 控制继电器，改规则并不比直接控制更有威力。 */
             c.kind == CMD_KIND_RULE);

        if (!remote_ok) {
            ESP_LOGW(TAG, "command rejected on remote channel (not whitelisted)");
            snprintf(ack, ack_len,
                     "{\"ack\":\"denied\",\"err\":\"not allowed on remote channel\","
                     "\"hint\":\"WiFi settings can only be changed over Bluetooth;"
                     " remote accepts get/status/relay only\"}");
            return CMD_ERR;
        }
    }

    switch (c.kind) {
    case CMD_KIND_GET:
        snprintf(ack, ack_len, "{\"ack\":\"get\"}");
        return CMD_OK_PUSH_TELEM;

    case CMD_KIND_STATUS:
        snprintf(ack, ack_len, "{\"ack\":\"status\",\"relay\":%u,\"fw\":\"%s\"}",
                 (unsigned)relay_get_all(), SMARTFARM_FW_VERSION);
        return CMD_OK;

    case CMD_KIND_HELP:
        write_help(ack, ack_len);
        return CMD_OK;

    case CMD_KIND_RELAY_ALL:
    case CMD_KIND_RELAY_ONE: {
        uint8_t before = relay_get_all();
        uint8_t target;

        if (c.kind == CMD_KIND_RELAY_ALL) {
            target = c.mask;
        } else {
            /* 只改指定那一位，其余保持 */
            target = before;
            if (c.mask & (uint8_t)(1u << c.index)) {
                target |= (uint8_t)(1u << c.index);
            } else {
                target &= (uint8_t)~(1u << c.index);
            }
        }

        relay_set_all(target);
        uint8_t after = relay_get_all();
        ESP_LOGI(TAG, "relay %u -> %u (bits: %d%d%d)", (unsigned)before, (unsigned)after,
                 (after >> 0) & 1, (after >> 1) & 1, (after >> 2) & 1);
        snprintf(ack, ack_len, "{\"ack\":\"relay\",\"relay\":%u,\"bits\":\"%d%d%d\"}",
                 (unsigned)after, (after >> 0) & 1, (after >> 1) & 1, (after >> 2) & 1);
        return CMD_OK;
    }

    case CMD_KIND_WIFI_SSID: {
        char tmp[64];
        size_t n = c.value_len < sizeof(tmp) - 1 ? c.value_len : sizeof(tmp) - 1;
        memcpy(tmp, c.value, n);
        tmp[n] = '\0';
        esp_err_t err = wifi_mgr_set_ssid(tmp);
        snprintf(ack, ack_len, "{\"ack\":\"wifi:ssid\",\"ok\":%d,\"len\":%u}",
                 err == ESP_OK ? 1 : 0, (unsigned)n);
        return (err == ESP_OK) ? CMD_OK : CMD_ERR;
    }

    case CMD_KIND_WIFI_PASS: {
        char tmp[80];
        size_t n = c.value_len < sizeof(tmp) - 1 ? c.value_len : sizeof(tmp) - 1;
        memcpy(tmp, c.value, n);
        tmp[n] = '\0';
        esp_err_t err = wifi_mgr_set_pass(tmp);
        /* 回执里只有长度，**绝不含密码** */
        snprintf(ack, ack_len, "{\"ack\":\"wifi:pass\",\"ok\":%d,\"len\":%u}",
                 err == ESP_OK ? 1 : 0, (unsigned)n);
        return (err == ESP_OK) ? CMD_OK : CMD_ERR;
    }

    case CMD_KIND_WIFI_CONNECT: {
        esp_err_t err = wifi_mgr_connect_with_pending();
        if (err == ESP_OK) {
            snprintf(ack, ack_len, "{\"ack\":\"wifi:connect\",\"ok\":1}");
        } else if (err == ESP_ERR_INVALID_STATE) {
            /* 只有"确实没设 SSID"才是这个错，提示才对得上 */
            snprintf(ack, ack_len,
                     "{\"ack\":\"wifi:connect\",\"ok\":0,\"err\":\"%s\","
                     "\"hint\":\"set wifi:ssid:<SSID> and wifi:pass:<PASS> first\"}",
                     esp_err_to_name(err));
        } else {
            /* 其它错误原样报出，不要套用不相干的提示——
             * 之前所有错误都带那句 hint，结果 ESP_ERR_WIFI_STATE 时提示是错的，误导排查。 */
            snprintf(ack, ack_len, "{\"ack\":\"wifi:connect\",\"ok\":0,\"err\":\"%s\"}",
                     esp_err_to_name(err));
        }
        return (err == ESP_OK) ? CMD_OK : CMD_ERR;
    }

    case CMD_KIND_WIFI_STATUS:
        /* 状态本身就是 JSON，直接用 */
        wifi_mgr_state_json(ack, ack_len);
        return CMD_OK;

    case CMD_KIND_WIFI_CLEAR:
        wifi_mgr_clear_credentials();
        snprintf(ack, ack_len, "{\"ack\":\"wifi:clear\"}");
        return CMD_OK;

    case CMD_KIND_RULE: {
        /* rules.c 需要 NUL 结尾的字符串。**超长必须显式拒绝**，
         * 不能静默截断——截断后的规则可能被解析成另一种含义。 */
        char arg[192];
        if (c.value_len >= sizeof(arg)) {
            snprintf(ack, ack_len,
                     "{\"ack\":\"rule\",\"ok\":0,\"err\":\"command too long\",\"max\":%u}",
                     (unsigned)(sizeof(arg) - 1));
            return CMD_ERR;
        }
        memcpy(arg, c.value, c.value_len);
        arg[c.value_len] = '\0';
        rules_status_t rs = rules_execute(arg, ack, ack_len);
        return (rs == RULES_ERR) ? CMD_ERR : CMD_OK;
    }

    default:
        write_help(ack, ack_len);
        return CMD_ERR;
    }
}

/* ---------------- 自检 ---------------- */

/* ⚠️ 只测纯解析，**绝不调用 relay_set_all**：
 * 若在启动时误动继电器，接真实负载（水泵/加热器）就可能造成事故。 */
void cmd_selftest(void)
{
    cmd_parsed_t c;
    int fails = 0;

    struct {
        const char *text;
        int expect_kind;
        int expect_index;
        int expect_mask;
        const char *expect_value;   /* 非 NULL 时还要校验 value 内容 */
    } cases[] = {
        { "get",           CMD_KIND_GET,       -1, 0, NULL },
        { "GET",           CMD_KIND_GET,       -1, 0, NULL },
        { "  status  ",    CMD_KIND_STATUS,    -1, 0, NULL },
        { "help",          CMD_KIND_HELP,      -1, 0, NULL },
        { "relay:100",     CMD_KIND_RELAY_ALL, -1, 0x01, NULL },
        { "relay:010",     CMD_KIND_RELAY_ALL, -1, 0x02, NULL },
        { "relay:001",     CMD_KIND_RELAY_ALL, -1, 0x04, NULL },
        { "relay:111",     CMD_KIND_RELAY_ALL, -1, 0x07, NULL },
        { "relay:000",     CMD_KIND_RELAY_ALL, -1, 0x00, NULL },
        { "RELAY:1:1",     CMD_KIND_RELAY_ONE,  1, 0x02, NULL },
        { "relay:0:0",     CMD_KIND_RELAY_ONE,  0, 0x00, NULL },
        { "relay:2:1",     CMD_KIND_RELAY_ONE,  2, 0x04, NULL },
        /* 配网指令 */
        { "wifi:connect",  CMD_KIND_WIFI_CONNECT, -1, 0, NULL },
        { "wifi:status",   CMD_KIND_WIFI_STATUS,  -1, 0, NULL },
        { "wifi:clear",    CMD_KIND_WIFI_CLEAR,   -1, 0, NULL },
        { "wifi:ssid:MyAP", CMD_KIND_WIFI_SSID,   -1, 0, "MyAP" },
        /* ★ 关键用例：值里含冒号。这正是"密码含冒号"这个经典歧义的回归测试 */
        { "wifi:pass:a:b:c", CMD_KIND_WIFI_PASS,  -1, 0, "a:b:c" },
        { "WIFI:PASS:x",   CMD_KIND_WIFI_PASS,    -1, 0, "x" },
        { "wifi:ssid:",    CMD_KIND_WIFI_SSID,    -1, 0, "" },
        /* 规则指令：value 是 "rule:" 之后的**原文**。
         * 第 2 条专门盯"时刻里的冒号"——若这一层误按冒号再切，value 就会变短。 */
        { "rule:list",     CMD_KIND_RULE,      -1, 0, "list" },
        { "rule:add mode=schedule relay=0 on=07:30 off=18:00",
                           CMD_KIND_RULE,      -1, 0,
                           "add mode=schedule relay=0 on=07:30 off=18:00" },
        { "RULE:DEL 1",    CMD_KIND_RULE,      -1, 0, "DEL 1" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool ok = cmd_parse(cases[i].text, &c) &&
                  (int)c.kind == cases[i].expect_kind &&
                  (cases[i].expect_index < 0 || c.index == cases[i].expect_index) &&
                  c.mask == (uint8_t)cases[i].expect_mask;

        if (ok && cases[i].expect_value != NULL) {
            size_t want = strlen(cases[i].expect_value);
            ok = (c.value != NULL) && (c.value_len == want) &&
                 (memcmp(c.value, cases[i].expect_value, want) == 0);
        }

        if (!ok) {
            ESP_LOGE(TAG, "selftest FAIL: \"%s\" -> kind=%d idx=%d mask=%u val_len=%u"
                          " (expected kind=%d idx=%d mask=%u val=\"%s\")",
                     cases[i].text, (int)c.kind, c.index, (unsigned)c.mask,
                     (unsigned)c.value_len,
                     cases[i].expect_kind, cases[i].expect_index, cases[i].expect_mask,
                     cases[i].expect_value ? cases[i].expect_value : "-");
            fails++;
        }
    }

    /* 必须被拒绝的输入 */
    const char *bad[] = { "", "gett", "relay", "relay:12", "relay:1x0",
                          "relay:3:1", "relay:0:2", "relay:1:1:1", "hello",
                          "wifi", "wifi:", "wifi:ssid", "wifi:foo:bar", "rule" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (cmd_parse(bad[i], &c)) {
            ESP_LOGE(TAG, "selftest FAIL: \"%s\" should be rejected", bad[i]);
            fails++;
        }
    }

    if (fails == 0) {
        ESP_LOGI(TAG, "selftest OK (%u accept + %u reject cases, relays untouched)",
                 (unsigned)(sizeof(cases) / sizeof(cases[0])),
                 (unsigned)(sizeof(bad) / sizeof(bad[0])));
    } else {
        ESP_LOGE(TAG, "selftest FAILED: %d case(s)", fails);
    }
}

/* 远程通道白名单自检：安全不变量，每次启动都验。
 * ⚠️ 只测拒绝路径；放行路径会真的操作继电器，不能在这里测。 */
void cmd_remote_whitelist_selftest(void)
{
    static const char *must_deny[] = {
        "wifi:ssid:TestAP", "wifi:pass:secret", "wifi:connect", "wifi:clear",
    };
    char ack[192];
    int fails = 0;

    for (size_t i = 0; i < sizeof(must_deny) / sizeof(must_deny[0]); i++) {
        cmd_status_t st = cmd_execute(must_deny[i], ack, sizeof(ack), false);
        if (st != CMD_ERR || strstr(ack, "\"denied\"") == NULL) {
            ESP_LOGE(TAG, "whitelist selftest FAIL: \"%s\" was NOT denied (ack=%s)",
                     must_deny[i], ack);
            fails++;
        }
    }

    /* wifi:status 只读，远程必须放行（否则远程就没法诊断 WiFi 状态了） */
    cmd_status_t st = cmd_execute("wifi:status", ack, sizeof(ack), false);
    if (st != CMD_OK || strstr(ack, "\"wifi\":\"status\"") == NULL) {
        ESP_LOGE(TAG, "whitelist selftest FAIL: wifi:status should be allowed (ack=%s)", ack);
        fails++;
    }

    /* 规则是 APP 的主要配置面，远程必须放行——白名单最容易"顺手把该开的也关了"，
     * 所以这里也要有正向用例盯着。（rule:list 只读，无副作用，可安全自检。） */
    st = cmd_execute("rule:list", ack, sizeof(ack), false);
    if (st != CMD_OK || strstr(ack, "\"rule:list\"") == NULL) {
        ESP_LOGE(TAG, "whitelist selftest FAIL: rule:list should be allowed (ack=%s)", ack);
        fails++;
    }

    if (fails == 0) {
        ESP_LOGI(TAG, "remote whitelist selftest OK (%u denied + wifi:status allowed)",
                 (unsigned)(sizeof(must_deny) / sizeof(must_deny[0])));
    } else {
        ESP_LOGE(TAG, "remote whitelist selftest FAILED: %d case(s)", fails);
    }
}

/* ⚠️ 只测纯函数 cmd_mask_for_log，**不执行任何指令、不动 WiFi/继电器**。
 *
 * 攻击场景：拿到设备串口日志的人（截图、粘贴进 issue、固件包里的日志）
 * 不应能读出 WiFi 密码。 */
void cmd_log_mask_selftest(void)
{
    int fails = 0;

    /* {原始指令, 遮蔽输出中必须保留的片段} —— "SuperSecret" 是哨兵密码，
     * 它出现在任何一条输出里都算泄漏。 */
    static const struct { const char *in; const char *needle; } cases[] = {
        { "wifi:pass:SuperSecret1",     "wifi:pass:<"     },  /* 标准形态 */
        { "WIFI:PASS:SuperSecret2",     "wifi:pass:<"     },  /* 大小写不敏感 */
        { "xwifi:pass:SuperSecret3",    "wifi:pass:<"     },  /* 解析失败也必须遮蔽 */
        { "  wifi:pass:SuperSecret4 ",  "wifi:pass:<"     },  /* 前后空白 */
        { "wifi:ssid:MyNetwork",        "wifi:ssid:MyNetwork" }, /* SSID 非机密：保持可读 */
        { "relay:100",                  "relay:100"       },
        { "rule:add mode=condition relay=2 src=water_t op=< val=5.0 hold=600 enabled=1",
          "hold=600" },                                       /* 普通指令不得被误伤 */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char shown[160];
        cmd_mask_for_log(cases[i].in, shown, sizeof(shown));

        /* 断言一：该保留的片段必须还在（防"遮蔽过度"把日志变得没用） */
        if (strstr(shown, cases[i].needle) == NULL) {
            ESP_LOGE(TAG, "log-mask selftest FAIL: case %u lost \"%s\" (got \"%s\")",
                     (unsigned)i, cases[i].needle, shown);
            fails++;
        }
        /* 断言二（安全不变量）：哨兵密码绝不允许出现 */
        if (strstr(shown, "SuperSecret") != NULL) {
            ESP_LOGE(TAG, "log-mask selftest FAIL: case %u LEAKED the password", (unsigned)i);
            fails++;
        }
    }

    /* 小缓冲也必须 NUL 结尾且不越界（snprintf 保证，但用探针钉住） */
    {
        char tiny[8];
        memset(tiny, 0x7f, sizeof(tiny));
        cmd_mask_for_log("wifi:pass:SuperSecret5", tiny, sizeof(tiny));
        if (tiny[sizeof(tiny) - 1] != '\0' || strstr(tiny, "Super") != NULL) {
            ESP_LOGE(TAG, "log-mask selftest FAIL: small buffer unsafe (\"%s\")", tiny);
            fails++;
        }
    }

    /* NULL 输入不得崩溃（日志路径上必须防御） */
    {
        char shown[16];
        cmd_mask_for_log(NULL, shown, sizeof(shown));
        if (shown[0] != '\0') {
            ESP_LOGE(TAG, "log-mask selftest FAIL: NULL input not handled");
            fails++;
        }
    }

    if (fails == 0) {
        ESP_LOGI(TAG, "log-mask selftest OK (password never echoed in any form; "
                      "other commands stay readable)");
    } else {
        ESP_LOGE(TAG, "log-mask selftest FAILED: %d case(s)", fails);
    }
}
