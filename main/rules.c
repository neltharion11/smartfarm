/* 设备端本地控制规则：模型、NVS 持久化、配置指令解析。 */

#include "rules.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "relay.h"

#define TAG "rules"

#define NVS_NS      "smartfarm"
#define NVS_K_RULES "rules"

/* 版本化 blob。
 * ⚠️ 结构体只能被**同版本固件**正确解释：固件升级后字段含义可能变，
 * 把旧字节按新含义读会把定时规则变成随机行为。所以带 magic+version，
 * 不匹配就整体忽略（回落到"无规则"），并在日志里明确说明。 */
#define RULES_MAGIC   0x5346524C   /* "SFRL" */
#define RULES_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;        /* 仅记录曾经用过的槽位数，便于诊断 */
    rule_t   rules[RULES_MAX];
} rules_blob_t;

static rules_blob_t s_blob;

/* 条件规则的运行时状态（不持久化）。
 *
 * `s_cond_acc`：条件已**持续满足**的累计秒数。
 * 用"累计"而不是"起始时刻"，是为了让读数无效时能自然**暂停计时**：
 * 若用起始时刻，一次传感器抖动就得从头再等 hold 秒——
 * 对防冻这种保护性用途，晚 10 分钟加热可能已经冻坏了。
 * 分段累计则只跳过"不知道"的那一段。 */
static uint32_t s_cond_acc[RULES_MAX];
static bool     s_invalid_warned[RULES_MAX];
static uint32_t s_eval_last;

/* ---------------- 基础工具 ---------------- */

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool tok_eq(const char *s, size_t n, const char *lit)
{
    size_t m = strlen(lit);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)lit[i])) return false;
    }
    return true;
}

/* 解析十进制无符号整数（整段必须是数字） */
static bool parse_u32(const char *s, size_t n, uint32_t *out)
{
    if (n == 0 || n > 9) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

/* 解析 "HH:MM" -> 自 00:00 起的分钟数 */
static bool parse_hhmm(const char *s, size_t n, uint16_t *out)
{
    if (n != 5 || s[2] != ':') return false;
    uint32_t hh = 0, mm = 0;
    if (!parse_u32(s, 2, &hh) || !parse_u32(s + 3, 2, &mm)) return false;
    if (hh > 23 || mm > 59) return false;
    *out = (uint16_t)(hh * 60 + mm);
    return true;
}

/* 解析放大 10 倍的定点数，如 "30" "30.0" "-5.5" */
static bool parse_val10(const char *s, size_t n, int16_t *out)
{
    if (n == 0 || n > 8) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; i = 1; }
    if (i >= n) return false;

    int32_t whole = 0;
    size_t digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        whole = whole * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0) return false;

    int32_t frac = 0;
    if (i < n) {
        if (s[i] != '.') return false;
        i++;
        if (i >= n) return false;
        if (s[i] < '0' || s[i] > '9') return false;
        frac = s[i] - '0';       /* 只取一位小数，与遥测精度一致 */
        i++;
        if (i != n) return false;
    }

    int32_t v = whole * 10 + frac;
    if (neg) v = -v;
    if (v < -32768 || v > 32767) return false;
    *out = (int16_t)v;
    return true;
}

/* ---------------- 纯解析（可自检） ---------------- */

typedef struct {
    enum { RC_NONE = 0, RC_LIST, RC_ADD, RC_UPDATE, RC_DELETE } op;
    int id;                 /* UPDATE / DELETE */
    uint32_t set_mask;      /* 哪些字段被显式给出 */
    rule_t   f;             /* 给出的字段值 */
} rule_cmd_t;

#define F_MODE    (1u << 0)
#define F_RELAY   (1u << 1)
#define F_ENABLED (1u << 2)
#define F_ON      (1u << 3)
#define F_OFF     (1u << 4)
#define F_SRC     (1u << 5)
#define F_CMP     (1u << 6)
#define F_VAL     (1u << 7)
#define F_HOLD    (1u << 8)
#define F_INVALID (1u << 9)

static bool parse_kv(const char *tok, size_t n, rule_cmd_t *out)
{
    const char *eq = memchr(tok, '=', n);
    if (eq == NULL) return false;
    const char *key = tok;
    size_t kl = (size_t)(eq - tok);
    const char *val = eq + 1;
    size_t vl = n - kl - 1;
    if (vl == 0) return false;

    uint32_t u = 0;
    int16_t v10 = 0;

    if (tok_eq(key, kl, "mode")) {
        if (tok_eq(val, vl, "manual"))         out->f.mode = RULE_MODE_MANUAL;
        else if (tok_eq(val, vl, "schedule"))  out->f.mode = RULE_MODE_SCHEDULE;
        else if (tok_eq(val, vl, "condition")) out->f.mode = RULE_MODE_CONDITION;
        else return false;
        out->set_mask |= F_MODE;
        return true;
    }
    if (tok_eq(key, kl, "relay")) {
        if (!parse_u32(val, vl, &u) || u > 2) return false;
        out->f.relay = (uint8_t)u;
        out->set_mask |= F_RELAY;
        return true;
    }
    if (tok_eq(key, kl, "enabled")) {
        if (!parse_u32(val, vl, &u) || u > 1) return false;
        out->f.enabled = (u == 1);
        out->set_mask |= F_ENABLED;
        return true;
    }
    if (tok_eq(key, kl, "on")) {
        if (!parse_hhmm(val, vl, &out->f.on_min)) return false;
        out->set_mask |= F_ON;
        return true;
    }
    if (tok_eq(key, kl, "off")) {
        if (!parse_hhmm(val, vl, &out->f.off_min)) return false;
        out->set_mask |= F_OFF;
        return true;
    }
    if (tok_eq(key, kl, "src")) {
        if (tok_eq(val, vl, "air_t"))        out->f.src = RULE_SRC_AIR_T;
        else if (tok_eq(val, vl, "air_rh"))  out->f.src = RULE_SRC_AIR_RH;
        else if (tok_eq(val, vl, "water_t")) out->f.src = RULE_SRC_WATER_T;
        else return false;
        out->set_mask |= F_SRC;
        return true;
    }
    if (tok_eq(key, kl, "op")) {
        if (vl != 1) return false;
        if (val[0] == '>')      out->f.cmp = 0;
        else if (val[0] == '<') out->f.cmp = 1;
        else return false;
        out->set_mask |= F_CMP;
        return true;
    }
    if (tok_eq(key, kl, "val")) {
        if (!parse_val10(val, vl, &v10)) return false;
        out->f.val10 = v10;
        out->set_mask |= F_VAL;
        return true;
    }
    if (tok_eq(key, kl, "hold")) {
        if (!parse_u32(val, vl, &u) || u > 86400) return false;
        out->f.hold_s = (uint16_t)u;
        out->set_mask |= F_HOLD;
        return true;
    }
    if (tok_eq(key, kl, "invalid")) {
        if (tok_eq(val, vl, "hold"))      out->f.invalid_policy = RULE_INVALID_HOLD;
        else if (tok_eq(val, vl, "off"))  out->f.invalid_policy = RULE_INVALID_OFF;
        else if (tok_eq(val, vl, "on"))   out->f.invalid_policy = RULE_INVALID_ON;
        else return false;
        out->set_mask |= F_INVALID;
        return true;
    }
    return false;   /* 未知键 -> 明确失败，不静默忽略 */
}

/* 纯解析：把 "rule:" 之后的部分解析成命令。**无任何副作用**，可安全自检。 */
static bool rule_parse(const char *arg, rule_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    if (arg == NULL) return false;

    const char *p = skip_ws(arg);
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n == 0) return false;

    /* 第一个空白之前是子命令 / id */
    size_t head = 0;
    while (head < n && p[head] != ' ') head++;
    const char *rest = (head < n) ? skip_ws(p + head) : (p + n);

    if (tok_eq(p, head, "list")) { out->op = RC_LIST; return *rest == '\0'; }
    if (tok_eq(p, head, "add"))  { out->op = RC_ADD; }
    if (tok_eq(p, head, "del")) {
        out->op = RC_DELETE;
        size_t rn = strlen(rest);
        uint32_t id = 0;
        if (!parse_u32(rest, rn, &id) || id < 1 || id > RULES_MAX) return false;
        out->id = (int)id;
        return true;
    }
    if (out->op == RC_NONE) {
        uint32_t id = 0;
        if (!parse_u32(p, head, &id) || id < 1 || id > RULES_MAX) return false;
        out->op = RC_UPDATE;
        out->id = (int)id;
    }

    /* 逐 token 解析 k=v */
    while (*rest != '\0') {
        rest = skip_ws(rest);
        if (*rest == '\0') break;
        size_t tl = 0;
        while (rest[tl] != '\0' && rest[tl] != ' ' && rest[tl] != '\t') tl++;
        if (!parse_kv(rest, tl, out)) return false;
        rest += tl;
    }
    return true;
}

/* ---------------- NVS ---------------- */

static void rules_save(void)
{
    s_blob.magic = RULES_MAGIC;
    s_blob.version = RULES_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, NVS_K_RULES, &s_blob, sizeof(s_blob));
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save rules failed: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "rules saved (%d slot(s) used)", rules_count());
    nvs_close(h);
}

esp_err_t rules_init(void)
{
    memset(&s_blob, 0, sizeof(s_blob));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no rules stored yet");
        return ESP_OK;
    }

    rules_blob_t tmp;
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(h, NVS_K_RULES, &tmp, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no rules stored yet");
        return ESP_OK;
    }
    if (len != sizeof(tmp) || tmp.magic != RULES_MAGIC || tmp.version != RULES_VERSION) {
        /* 明确拒绝而不是"尽力解释"：按错误含义执行的定时规则可能直接烧坏水泵 */
        ESP_LOGW(TAG, "stored rules ignored: format mismatch (len=%u magic=0x%08lx ver=%u)",
                 (unsigned)len, (unsigned long)tmp.magic, (unsigned)tmp.version);
        return ESP_OK;
    }

    s_blob = tmp;
    ESP_LOGI(TAG, "rules loaded from NVS: %d slot(s) used", rules_count());
    return ESP_OK;
}

/* ---------------- 执行 ---------------- */

int rules_count(void)
{
    int n = 0;
    for (int i = 0; i < RULES_MAX; i++) {
        if (s_blob.rules[i].used) n++;
    }
    return n;
}

static const char *mode_str(uint8_t m)
{
    switch (m) {
    case RULE_MODE_SCHEDULE:  return "schedule";
    case RULE_MODE_CONDITION: return "condition";
    default:                  return "manual";
    }
}

static const char *src_str(uint8_t s)
{
    switch (s) {
    case RULE_SRC_AIR_RH:  return "air_rh";
    case RULE_SRC_WATER_T: return "water_t";
    default:               return "air_t";
    }
}

static void append_rule_json(const rule_t *r, int id, char *out, size_t n)
{
    char on[8]  = "";
    char off[8] = "";
    if (r->mode == RULE_MODE_SCHEDULE) {
        snprintf(on, sizeof(on), "%02u:%02u", r->on_min / 60, r->on_min % 60);
        snprintf(off, sizeof(off), "%02u:%02u", r->off_min / 60, r->off_min % 60);
    }

    char head[64];
    snprintf(head, sizeof(head), "{\"id\":%d,\"en\":%d,\"mode\":\"%s\",\"relay\":%u",
             id, r->enabled ? 1 : 0, mode_str(r->mode), (unsigned)r->relay);

    char tail[128];
    if (r->mode == RULE_MODE_SCHEDULE) {
        snprintf(tail, sizeof(tail), ",\"on\":\"%s\",\"off\":\"%s\"}", on, off);
    } else if (r->mode == RULE_MODE_CONDITION) {
        const char *inv = (r->invalid_policy == RULE_INVALID_OFF) ? "off" :
                          (r->invalid_policy == RULE_INVALID_ON)  ? "on"  : "hold";
        snprintf(tail, sizeof(tail),
                 ",\"src\":\"%s\",\"op\":\"%s\",\"val\":%d.%d,\"hold\":%u,\"invalid\":\"%s\"}",
                 src_str(r->src), r->cmp == 0 ? ">" : "<",
                 r->val10 / 10, abs(r->val10 % 10), (unsigned)r->hold_s, inv);
    } else {
        snprintf(tail, sizeof(tail), "}");
    }

    strlcat(out, head, n);
    strlcat(out, tail, n);
}

rules_status_t rules_execute(const char *arg, char *ack, size_t ack_len)
{
    if (ack == NULL || ack_len == 0) return RULES_ERR;
    ack[0] = '\0';

    rule_cmd_t c;
    if (!rule_parse(arg, &c)) {
        ESP_LOGW(TAG, "bad rule command: \"%s\"", arg ? arg : "(null)");
        snprintf(ack, ack_len,
                 "{\"ack\":\"rule\",\"ok\":0,\"err\":\"syntax\","
                 "\"hint\":\"rule:list | rule:add k=v.. | rule:<id> k=v.. | rule:del <id>\"}");
        return RULES_ERR;
    }

    switch (c.op) {
    case RC_LIST: {
        strlcpy(ack, "{\"ack\":\"rule:list\",\"n\":", ack_len);
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d,\"rules\":[", rules_count());
        strlcat(ack, tmp, ack_len);
        bool first = true;
        for (int i = 0; i < RULES_MAX; i++) {
            if (!s_blob.rules[i].used) continue;
            if (!first) strlcat(ack, ",", ack_len);
            first = false;
            append_rule_json(&s_blob.rules[i], i + 1, ack, ack_len);
        }
        strlcat(ack, "]}", ack_len);
        return RULES_OK;
    }

    case RC_ADD: {
        int slot = -1;
        for (int i = 0; i < RULES_MAX; i++) {
            if (!s_blob.rules[i].used) { slot = i; break; }
        }
        if (slot < 0) {
            snprintf(ack, ack_len,
                     "{\"ack\":\"rule:add\",\"ok\":0,\"err\":\"no free slot\",\"max\":%d}",
                     RULES_MAX);
            return RULES_ERR;
        }
        rule_t *r = &s_blob.rules[slot];
        memset(r, 0, sizeof(*r));
        s_cond_acc[slot] = 0;          /* 槽位可能被复用：清掉上一轮的计时 */
        s_invalid_warned[slot] = false;
        r->used = true;
        r->enabled = false;          /* 新建默认**不启用**：避免配置一半就被执行 */
        r->mode = RULE_MODE_MANUAL;
        if (c.set_mask & F_MODE)    r->mode = c.f.mode;
        if (c.set_mask & F_RELAY)   r->relay = c.f.relay;
        if (c.set_mask & F_ENABLED) r->enabled = c.f.enabled;
        if (c.set_mask & F_ON)      r->on_min = c.f.on_min;
        if (c.set_mask & F_OFF)     r->off_min = c.f.off_min;
        if (c.set_mask & F_SRC)     r->src = c.f.src;
        if (c.set_mask & F_CMP)     r->cmp = c.f.cmp;
        if (c.set_mask & F_VAL)     r->val10 = c.f.val10;
        if (c.set_mask & F_HOLD)    r->hold_s = c.f.hold_s;
        if (c.set_mask & F_INVALID) r->invalid_policy = c.f.invalid_policy;
        s_blob.count = (uint16_t)rules_count();
        rules_save();

        snprintf(ack, ack_len, "{\"ack\":\"rule:add\",\"ok\":1,\"id\":%d,\"en\":%d}",
                 slot + 1, r->enabled ? 1 : 0);
        ESP_LOGI(TAG, "rule %d added (mode=%s relay=%u)", slot + 1, mode_str(r->mode),
                 (unsigned)r->relay);
        return RULES_OK;
    }

    case RC_UPDATE: {
        rule_t *r = &s_blob.rules[c.id - 1];
        if (!r->used) {
            snprintf(ack, ack_len, "{\"ack\":\"rule\",\"ok\":0,\"err\":\"no such rule\",\"id\":%d}",
                     c.id);
            return RULES_ERR;
        }
        if (c.set_mask == 0) {
            snprintf(ack, ack_len, "{\"ack\":\"rule\",\"ok\":0,\"err\":\"no fields given\",\"id\":%d}",
                     c.id);
            return RULES_ERR;
        }
        if (c.set_mask & F_MODE)    r->mode = c.f.mode;
        if (c.set_mask & F_RELAY)   r->relay = c.f.relay;
        if (c.set_mask & F_ENABLED) r->enabled = c.f.enabled;
        if (c.set_mask & F_ON)      r->on_min = c.f.on_min;
        if (c.set_mask & F_OFF)     r->off_min = c.f.off_min;
        if (c.set_mask & F_SRC)     r->src = c.f.src;
        if (c.set_mask & F_CMP)     r->cmp = c.f.cmp;
        if (c.set_mask & F_VAL)     r->val10 = c.f.val10;
        if (c.set_mask & F_HOLD)    r->hold_s = c.f.hold_s;
        if (c.set_mask & F_INVALID) r->invalid_policy = c.f.invalid_policy;
        rules_save();

        strlcpy(ack, "{\"ack\":\"rule:set\",\"ok\":1,\"rule\":", ack_len);
        append_rule_json(r, c.id, ack, ack_len);
        strlcat(ack, "}", ack_len);
        ESP_LOGI(TAG, "rule %d updated (mask=0x%lx)", c.id, (unsigned long)c.set_mask);
        return RULES_OK;
    }

    case RC_DELETE: {
        rule_t *r = &s_blob.rules[c.id - 1];
        if (!r->used) {
            snprintf(ack, ack_len, "{\"ack\":\"rule:del\",\"ok\":0,\"err\":\"no such rule\",\"id\":%d}",
                     c.id);
            return RULES_ERR;
        }
        memset(r, 0, sizeof(*r));    /* 只清该槽位，其余 id 保持不变 */
        s_cond_acc[c.id - 1] = 0;
        s_invalid_warned[c.id - 1] = false;
        s_blob.count = (uint16_t)rules_count();
        rules_save();
        snprintf(ack, ack_len, "{\"ack\":\"rule:del\",\"ok\":1,\"id\":%d}", c.id);
        ESP_LOGI(TAG, "rule %d deleted", c.id);
        return RULES_OK;
    }

    default:
        snprintf(ack, ack_len, "{\"ack\":\"rule\",\"ok\":0,\"err\":\"unreachable\"}");
        return RULES_ERR;
    }
}

/* ---------------- 定时窗口判定（纯函数，可自检） ---------------- */

/* 判断 now_min 是否落在 [on_min, off_min) 窗口内。
 *
 * 两个容易写错的点，都用自检盯住了：
 *   1. **跨午夜**：off <= on 时窗口绕一圈（如 22:00 -> 06:00）。
 *      此时"在窗口内"是 now >= on **或** now < off。
 *   2. **off == on**：这是零长度窗口。若套用上面的公式会得到"恒为真"，
 *      导致继电器**整天开着**（水泵会一直抽）。这里明确判为不在窗口内。 */
static bool in_window(uint16_t now_min, uint16_t on_min, uint16_t off_min)
{
    if (off_min == on_min) return false;                 /* 零长度：不动作 */
    if (off_min > on_min)  return (now_min >= on_min && now_min < off_min);
    return (now_min >= on_min || now_min < off_min);     /* 跨午夜 */
}

bool rules_clock_ready(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_year + 1900) >= 2020;   /* 1970 = 未对时 */
}

/* 条件比较（纯函数，可自检）。**严格比较**：等于阈值不算满足。 */
static bool cond_satisfied(uint8_t cmp, int16_t val10, int16_t reading10)
{
    return (cmp == 0) ? (reading10 > val10) : (reading10 < val10);
}

/* 按配置取对应的读数。返回 false 表示该读数当前无效。 */
static bool src_value(const rules_inputs_t *in, uint8_t src, int16_t *out10)
{
    if (in == NULL) return false;
    switch (src) {
    case RULE_SRC_AIR_RH:  if (!in->air_ok)   return false; *out10 = in->air_rh10; return true;
    case RULE_SRC_WATER_T: if (!in->water_ok) return false; *out10 = in->water_t10; return true;
    default:               if (!in->air_ok)   return false; *out10 = in->air_t10;  return true;
    }
}

/* 只提示一次，避免每秒刷屏 */
static bool s_clock_warned;

/* 上一轮被规则覆盖的继电器掩码。
 * 用途：**失效安全**——某路继电器上一轮还受规则控制、这一轮不再受控制时
 * （规则被删除或被禁用），必须把它归到"关闭"。
 * 否则用户删掉规则以为停了水泵，实际继电器冻在 ON 继续抽水，非常危险。 */
static uint8_t s_last_claimed;

int rules_evaluate(const rules_inputs_t *in, uint8_t *out_mask)
{
    if (out_mask == NULL) return 0;

    /* 以当前输出为基础：没被规则覆盖的继电器保持原状态（手动控制不受影响） */
    *out_mask = relay_get_all();

    bool claimed[RELAY_COUNT] = { false };
    uint8_t claimed_mask = 0;
    int applied = 0;
    bool clock_ok = rules_clock_ready();

    /* 用实际间隔累计，而不是假设"每秒调用一次"；
     * 并夹住上限——长时间没求解（如刚开机）不应被当成"已满足很久"。 */
    uint32_t now_s = (in != NULL) ? in->uptime_s : 0;
    uint32_t dt = (now_s >= s_eval_last) ? (now_s - s_eval_last) : 0;
    if (dt > 5) dt = 5;
    s_eval_last = now_s;

    for (int i = 0; i < RULES_MAX; i++) {
        const rule_t *r = &s_blob.rules[i];
        if (!r->used || !r->enabled) {
            s_cond_acc[i] = 0;   /* 未启用：计时清零；重新启用后从头算起 */
            continue;
        }
        if (r->relay >= RELAY_COUNT) continue;

        /* ⚠️ manual 规则**不参与求解，也不占用该路继电器**。
         *
         * 这一条必须放在"占用"之前，否则会踩到一个很隐蔽的坑：
         *   一条启用的 manual 规则先把它那一路标记为「已占用」，于是
         *   **同路后续的 schedule/condition 规则会被静默忽略**（只留一条 WARN），
         *   而且 claimed_mask 会把该路算作「受规则控制」，使失效安全也不再对它生效
         *   ——表现就是"规则明明还在，继电器却不动 / 冻在 ON"。
         *
         * 为什么特别容易踩：`rule:add` 的默认 mode 就是 manual
         * （见上面新建规则处），用户写 `rule:add relay=0 enabled=1`
         * 忘了带 mode= 就会中招。 */
        if (r->mode == RULE_MODE_MANUAL) {
            s_cond_acc[i] = 0;   /* 与"未启用"一致：不保留计时 */
            continue;
        }

        /* 同一路继电器被多条规则控制时，**按规则 id 从小到大优先**，
         * 其余忽略并告警。策略必须确定且可解释——
         * 若改成"最后一条生效"，行为会随规则增删而漂移，极难排查。
         * 比较范围是「真正会决定输出的规则」（schedule/condition），
         * manual 已在上面排除。 */
        if (claimed[r->relay]) {
            ESP_LOGW(TAG, "relay %u also targeted by rule %d -> ignored"
                          " (lowest rule id wins)", (unsigned)r->relay, i + 1);
            continue;
        }
        claimed[r->relay] = true;
        claimed_mask |= (uint8_t)(1u << r->relay);
        applied++;

        bool want_on = false;
        bool decided = false;

        if (r->mode == RULE_MODE_SCHEDULE) {
            if (!clock_ok) {
                /* 时钟不可信：**不判断、也不改**。宁可先不动，
                 * 等 SNTP 对时完成后再接管，也不要在开机瞬间乱动继电器。 */
                if (!s_clock_warned) {
                    s_clock_warned = true;
                    ESP_LOGW(TAG, "schedule rules suspended: clock not valid yet"
                                  " (waiting for SNTP)");
                }
                continue;
            }
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);
            uint16_t now_min = (uint16_t)(t.tm_hour * 60 + t.tm_min);
            want_on = in_window(now_min, r->on_min, r->off_min);
            decided = true;
        } else if (r->mode == RULE_MODE_CONDITION) {
            int16_t reading10 = 0;
            bool valid = src_value(in, r->src, &reading10);

            if (!valid) {
                /* 读数无效（传感器故障/读取失败）按**配置**决定，不写死：
                 *   防冻加热器 -> 应当继续加热（invalid=on）
                 *   灌溉水泵   -> 应当停泵      （invalid=off）
                 * 默认 hold：保持现状，既不误开也不误关，并告警一次。
                 * 关键是**不能把"读数无效"当成"条件不满足"** ——
                 * 那会在传感器故障时把防冻加热器关掉。 */
                switch (r->invalid_policy) {
                case RULE_INVALID_ON:  want_on = true;  decided = true; break;
                case RULE_INVALID_OFF: want_on = false; decided = true; break;
                default:
                    decided = false;                 /* hold：不改动 */
                    if (!s_invalid_warned[i]) {
                        s_invalid_warned[i] = true;
                        ESP_LOGW(TAG, "rule %d: reading unavailable -> holding output"
                                      " (invalid=hold)", i + 1);
                    }
                    break;
                }
            } else {
                s_invalid_warned[i] = false;         /* 读数恢复，解除告警抑制 */
                if (!cond_satisfied(r->cmp, r->val10, reading10)) {
                    s_cond_acc[i] = 0;               /* 不满足 -> 计时清零 */
                    want_on = false;
                    decided = true;
                } else {
                    s_cond_acc[i] += dt;             /* 满足 -> 累计（dt 已夹上限） */
                    if (s_cond_acc[i] >= r->hold_s) {
                        want_on = true;
                        decided = true;
                    } else {
                        decided = false;             /* 还没到持续时间：先不动 */
                    }
                }
            }
        } else {
            continue;   /* 未知 mode：不干预（manual 已在上面提前跳过） */
        }

        if (decided) {
            if (want_on) *out_mask |= (uint8_t)(1u << r->relay);
            else         *out_mask &= (uint8_t)~(1u << r->relay);
        }
    }

    if (clock_ok) s_clock_warned = false;

    /* 失效安全：上一轮受规则控制、这一轮不再受控制的继电器 -> 强制关闭。
     * 覆盖"删除规则""禁用规则"这两条最容易出事、也最容易漏掉的路径。 */
    uint8_t dropped = s_last_claimed & (uint8_t)~claimed_mask;
    if (dropped != 0) {
        *out_mask &= (uint8_t)~dropped;
        ESP_LOGW(TAG, "relay(s) %d%d%d no longer controlled by any rule -> forced OFF",
                 (dropped >> 0) & 1, (dropped >> 1) & 1, (dropped >> 2) & 1);
    }
    s_last_claimed = claimed_mask;

    return applied;
}

/* ---------------- 自检 ---------------- */

/* ⚠️ 只测**纯解析**：不写 NVS、不动继电器。
 * 若在启动时误写规则或误动继电器，接真实负载就可能造成事故。 */
void rules_selftest(void)
{
    struct {
        const char *arg;
        int  op;
        int  id;
        bool want_ok;
        uint32_t want_mask;
    } cases[] = {
        { "list",                              1, 0, true,  0 },
        { "LIST",                              1, 0, true,  0 },
        { "add",                               2, 0, true,  0 },
        { "add mode=schedule relay=0 on=07:30 off=18:00 enabled=1",
                                               2, 0, true,  F_MODE | F_RELAY | F_ON | F_OFF | F_ENABLED },
        { "add mode=condition relay=2 src=water_t op=< val=5.0 hold=600",
                                               2, 0, true,  F_MODE | F_RELAY | F_SRC | F_CMP | F_VAL | F_HOLD },
        { "add mode=condition src=air_rh op=> val=80.0 hold=0 invalid=off",
                                               2, 0, true,  F_MODE | F_SRC | F_CMP | F_VAL | F_HOLD | F_INVALID },
        { "1 invalid=on",                      3, 1, true,  F_INVALID },
        { "1 enabled=0",                       3, 1, true,  F_ENABLED },
        { "2 mode=manual",                     3, 2, true,  F_MODE },
        { "4 relay=2",                         3, 4, true,  F_RELAY },
        { "del 1",                             4, 1, true,  0 },
        /* 必须被拒绝 */
        { "",                                  0, 0, false, 0 },
        { "5 mode=manual",                     0, 0, false, 0 },   /* id 越界 */
        { "del 0",                             0, 0, false, 0 },
        { "del",                               0, 0, false, 0 },
        { "1 bogus=1",                         0, 0, false, 0 },   /* 未知键必须失败 */
        { "add mode=banana",                   0, 0, false, 0 },
        { "add on=25:00",                      0, 0, false, 0 },
        { "add on=07:70",                      0, 0, false, 0 },
        { "add relay=3",                       0, 0, false, 0 },
        { "add op=>=",                         0, 0, false, 0 },
        { "add val=abc",                       0, 0, false, 0 },
        { "add enabled=2",                     0, 0, false, 0 },
        { "add invalid=maybe",                 0, 0, false, 0 },
        { "add val=",                          0, 0, false, 0 },
    };

    rule_cmd_t c;
    int fails = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool got = rule_parse(cases[i].arg, &c);
        bool ok = (got == cases[i].want_ok);
        if (ok && got) {
            ok = ((int)c.op == cases[i].op) &&
                 (cases[i].id == 0 || c.id == cases[i].id) &&
                 c.set_mask == cases[i].want_mask;
        }
        if (!ok) {
            ESP_LOGE(TAG, "rules selftest FAIL: \"%s\" -> ok=%d op=%d id=%d mask=0x%lx"
                          " (want ok=%d op=%d id=%d mask=0x%lx)",
                     cases[i].arg, got, (int)c.op, c.id, (unsigned long)c.set_mask,
                     cases[i].want_ok, cases[i].op, cases[i].id,
                     (unsigned long)cases[i].want_mask);
            fails++;
        }
    }

    /* ---- 定时窗口判定（B2 里风险最高的纯逻辑，必须逐边界盯住）---- */
    struct {
        uint16_t now, on, off;
        bool want;
        const char *desc;
    } win[] = {
        /* 普通窗口 07:30 - 18:00 */
        { 7 * 60 + 29, 7 * 60 + 30, 18 * 60, false, "just before on" },
        { 7 * 60 + 30, 7 * 60 + 30, 18 * 60, true,  "exactly at on" },
        { 12 * 60,     7 * 60 + 30, 18 * 60, true,  "mid window" },
        { 17 * 60 + 59, 7 * 60 + 30, 18 * 60, true, "just before off" },
        { 18 * 60,     7 * 60 + 30, 18 * 60, false, "exactly at off (exclusive)" },
        { 23 * 60,     7 * 60 + 30, 18 * 60, false, "late night" },
        /* 跨午夜 22:00 - 06:00 */
        { 21 * 60 + 59, 22 * 60, 6 * 60, false, "before wrap window" },
        { 22 * 60,     22 * 60, 6 * 60, true,  "exactly at wrap on" },
        { 23 * 60 + 59, 22 * 60, 6 * 60, true, "before midnight" },
        { 0,           22 * 60, 6 * 60, true,  "midnight inside window" },
        { 5 * 60 + 59, 22 * 60, 6 * 60, true,  "just before wrap off" },
        { 6 * 60,      22 * 60, 6 * 60, false, "exactly at wrap off" },
        { 12 * 60,     22 * 60, 6 * 60, false, "noon outside window" },
        /* 零长度窗口：若不特殊处理会恒为真 -> 继电器整天开着（水泵会一直抽） */
        { 12 * 60, 12 * 60, 12 * 60, false, "zero-length window" },
        { 0,       0,       0,       false, "zero-length at midnight" },
    };

    for (size_t i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
        bool got = in_window(win[i].now, win[i].on, win[i].off);
        if (got != win[i].want) {
            ESP_LOGE(TAG, "window selftest FAIL: now=%u on=%u off=%u -> %d (want %d) [%s]",
                     win[i].now, win[i].on, win[i].off, got, win[i].want, win[i].desc);
            fails++;
        }
    }

    /* ---- 条件比较（**严格比较**：等于阈值不算满足）---- */
    struct {
        uint8_t cmp;
        int16_t val10, reading10;
        bool want;
        const char *desc;
    } cond[] = {
        { 0, 300, 301, true,  "> above threshold" },
        { 0, 300, 300, false, "> exactly at threshold is NOT satisfied" },
        { 0, 300, 299, false, "> below threshold" },
        { 1, 300, 299, true,  "< below threshold" },
        { 1, 300, 300, false, "< exactly at threshold is NOT satisfied" },
        { 1, 300, 301, false, "< above threshold" },
        { 1, -50, -60, true,  "< negative threshold" },
        { 0, -50, -40, true,  "> negative threshold" },
    };

    for (size_t i = 0; i < sizeof(cond) / sizeof(cond[0]); i++) {
        bool got = cond_satisfied(cond[i].cmp, cond[i].val10, cond[i].reading10);
        if (got != cond[i].want) {
            ESP_LOGE(TAG, "cond selftest FAIL: cmp=%u val=%d reading=%d -> %d (want %d) [%s]",
                     cond[i].cmp, cond[i].val10, cond[i].reading10, got, cond[i].want,
                     cond[i].desc);
            fails++;
        }
    }

    if (fails == 0) {
        ESP_LOGI(TAG, "selftest OK (%u parse + %u window + %u cond cases,"
                      " no NVS write, no relay action)",
                 (unsigned)(sizeof(cases) / sizeof(cases[0])),
                 (unsigned)(sizeof(win) / sizeof(win[0])),
                 (unsigned)(sizeof(cond) / sizeof(cond[0])));
    } else {
        ESP_LOGE(TAG, "selftest FAILED: %d case(s)", fails);
    }
}
