#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "alert_config.h"
#include "alert_dispatch.h"

/* 这些静态缓冲区承担配置字符串的真实存储，
 * alert_config_t 里只保存指针，避免在多个模块间重复拷贝。
 */
static char g_url[256];
static char g_key[128];
static char g_tty[64];
static char g_phone[32];
static char g_devid[64];

/* 极简 JSON 字符串提取器。
 * 这里故意不引入第三方 JSON 库，方便板端独立编译和部署。
 * 代价是它只适合读取这种简单的扁平配置文件，不支持复杂 JSON 结构。
 */
static int json_get_str(const char *json, const char *key, char *out, int out_len)
{
    char search[128];
    const char *p, *start, *end;
    int n = snprintf(search, sizeof(search), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(search)) return -1;

    p = strstr(json, search);
    if (!p) return -1;
    p += n;

    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != '"') return -1;
    p++;
    start = p;
    end = start;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p++;
        p++;
    }
    end = p;
    {
        int len = (int)(end - start);
        if (len >= out_len) len = out_len - 1;
        memcpy(out, start, len);
        out[len] = '\0';
    }
    return 0;
}

static int json_get_int(const char *json, const char *key, int *val)
{
    char search[128];
    const char *p;
    int n = snprintf(search, sizeof(search), "\"%s\"", key);
    if (n < 0 || n >= (int)sizeof(search)) return -1;

    p = strstr(json, search);
    if (!p) return -1;
    p += n;

    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    *val = atoi(p);
    return 0;
}

/* 一次性把整个配置文件读入内存，便于后续用 strstr 做简单解析。 */
static char *slurp(const char *path)
{
    FILE *fp;
    long size;
    char *buf;

    fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

void alert_config_defaults(alert_config_t *config)
{
    /* 先清零，再填一个“可运行但尽量保守”的默认配置。 */
    memset(config, 0, sizeof(*config));

    g_url[0] = '\0';
    g_key[0] = '\0';
    snprintf(g_tty, sizeof(g_tty), "/dev/ttyUSB5");
    g_phone[0] = '\0';
    snprintf(g_devid, sizeof(g_devid), "ss928-001");

    config->cloud_api_url = g_url;
    config->cloud_api_key = g_key;
    config->sms_tty = g_tty;
    config->sms_phone = g_phone;
    config->device_id = g_devid;
    config->sms_cooldown_sec = 300;
    config->http_enabled = 0;
    config->sms_enabled = 0;
}

int alert_config_load(const char *path, alert_config_t *config)
{
    char *json;
    int tmp;

    /* 无论配置文件是否存在，先铺默认值，保证调用方总能拿到有效结构体。 */
    alert_config_defaults(config);

    if (!path || !path[0]) return 0;

    json = slurp(path);
    if (!json) {
        fprintf(stderr, "alert_config: cannot read %s, using defaults\n", path);
        return -1;
    }

    /* 逐字段覆盖默认值。读取失败的字段会继续沿用默认配置。 */
    if (json_get_str(json, "cloud_api_url", g_url, sizeof(g_url)) == 0)
        config->cloud_api_url = g_url;
    if (json_get_str(json, "cloud_api_key", g_key, sizeof(g_key)) == 0)
        config->cloud_api_key = g_key;
    if (json_get_str(json, "sms_tty", g_tty, sizeof(g_tty)) == 0)
        config->sms_tty = g_tty;
    if (json_get_str(json, "sms_phone", g_phone, sizeof(g_phone)) == 0)
        config->sms_phone = g_phone;
    if (json_get_str(json, "device_id", g_devid, sizeof(g_devid)) == 0)
        config->device_id = g_devid;
    if (json_get_int(json, "sms_cooldown_sec", &tmp) == 0)
        config->sms_cooldown_sec = tmp;
    if (json_get_int(json, "http_enabled", &tmp) == 0)
        config->http_enabled = tmp;
    if (json_get_int(json, "sms_enabled", &tmp) == 0)
        config->sms_enabled = tmp;

    free(json);
    printf("alert_config: loaded from %s (http=%d sms=%d)\n",
        path, config->http_enabled, config->sms_enabled);
    return 0;
}
