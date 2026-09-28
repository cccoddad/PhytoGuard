#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "status_report.h"

/* 设备状态上报模块只维护一个全局上下文；
 * 当前程序是单实例运行，因此不需要再抽象成对象化接口。
 */
static char g_device_id[128];
static char g_api_url[512];
static char g_api_key[128];
static time_t g_last_tick;

/* ---------- 极简 HTTP POST（实现方式与告警发送保持一致） ---------- */

static int http_parse_url(const char *url, char *host, int host_len,
    int *port, char *path, int path_len)
{
    const char *p;

    *port = 80;
    host[0] = '\0';
    path[0] = '/';
    path[1] = '\0';

    if (strncmp(url, "http://", 7) != 0) return -1;
    p = url + 7;

    {
        const char *host_start = p;
        while (*p && *p != ':' && *p != '/') p++;
        int len = (int)(p - host_start);
        if (len >= host_len) len = host_len - 1;
        memcpy(host, host_start, len);
        host[len] = '\0';
    }

    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    }

    if (*p) {
        int len = (int)strlen(p);
        if (len >= path_len) len = path_len - 1;
        memcpy(path, p, len);
        path[len] = '\0';
    }
    return 0;
}

/* 建立 TCP 连接，发送心跳时直接复用这套最小实现。 */
static int http_connect(const char *server_url)
{
    char host[256];
    char path[512];
    int port;
    int sock;
    struct sockaddr_in addr;
    struct hostent *he;

    if (http_parse_url(server_url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        fprintf(stderr, "status: bad url %s\n", server_url);
        return -1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    {
        /* 心跳不是强实时业务，但也不能无限阻塞。 */
        struct timeval tv = {5, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    he = gethostbyname(host);
    if (!he) { close(sock); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static int http_post_json(const char *server_url, const char *api_key,
    const char *json_body)
{
    int sock;
    char buf[4096];
    int n;

    sock = http_connect(server_url);
    if (sock < 0) return -1;

    /* 手工拼装一个完整 HTTP 请求，避免引入重量级网络库。 */
    {
        const char *p = server_url;
        const char *path_start;
        if (strncmp(p, "http://", 7) == 0) p += 7;
        while (*p && *p != '/' && *p != ':') p++;
        if (*p == ':') { p++; while (*p >= '0' && *p <= '9') p++; }
        path_start = (*p) ? p : "/";

        n = snprintf(buf, sizeof(buf), "POST %s HTTP/1.0\r\nHost: ", path_start);
        {
            const char *h = server_url + 7;
            const char *he = h;
            while (*he && *he != ':' && *he != '/') he++;
            n += snprintf(buf + n, sizeof(buf) - n, "%.*s", (int)(he - h), h);
        }
        n += snprintf(buf + n, sizeof(buf) - n,
            "\r\n"
            "Content-Type: application/json\r\n"
            "X-API-Key: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            api_key, (int)strlen(json_body), json_body);
    }

    {
        ssize_t w = write(sock, buf, n);
        if (w < 0) { close(sock); return -1; }
    }

    {
        ssize_t r = read(sock, buf, sizeof(buf) - 1);
        close(sock);
        if (r > 0) {
            buf[r] = '\0';
            if (strstr(buf, "200")) return 0;
        }
        return -1;
    }
}

/* ---------- 读取内存占用 ---------- */

static int get_memory_percent(void)
{
    FILE *f;
    char line[256];
    long total = 0, available = 0;

    f = fopen("/proc/meminfo", "r");
    if (!f) return -1;

    /* 直接从 /proc/meminfo 读取，总开销很小。 */
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0)
            total = atol(line + 9);
        else if (strncmp(line, "MemAvailable:", 13) == 0)
            available = atol(line + 13);
        if (total && available) break;
    }
    fclose(f);

    if (total <= 0) return -1;
    return (int)((total - available) * 100 / total);
}

/* ---------- 对外接口 ---------- */

int status_report_init(const char *device_id, const char *api_url, const char *api_key)
{
    if (!device_id || !api_url || !api_url[0]) return -1;

    snprintf(g_device_id, sizeof(g_device_id), "%s", device_id);
    snprintf(g_api_url, sizeof(g_api_url), "%s", api_url);
    snprintf(g_api_key, sizeof(g_api_key), "%s", api_key ? api_key : "");

    /* 置 0 表示允许程序启动后第一次 tick 立刻发送。 */
    g_last_tick = 0;
    printf("status: init ok, device=%s url=%s\n", g_device_id, g_api_url);
    return 0;
}

void status_report_deinit(void)
{
    g_device_id[0] = '\0';
}

int status_report_tick(void)
{
    time_t now = time(NULL);
    char json[512];
    char url[600];
    int mem;

    if (!g_device_id[0] || !g_api_url[0]) return 0;

    /* 心跳默认每 60 秒发送一次，既能反映在线状态，也不会太频繁。 */
    if (now - g_last_tick < 60) return 0;
    g_last_tick = now;

    mem = get_memory_percent();

    snprintf(json, sizeof(json),
        "{"
        "\"device_id\":\"%s\","
        "\"online\":true,"
        "\"powered_on\":true,"
        "\"memory_percent\":%d,"
        "\"last_seen_ts\":%ld"
        "}",
        g_device_id, mem, (long)(now * 1000));

    /* 配置文件里存的是 alerts 接口地址，这里派生出 devices/status 心跳地址。 */
    {
        /* 优先替换已知路径，避免重复拼接 /api 前缀。 */
        const char *base = g_api_url;
        const char *alerts_path = strstr(base, "/api/alerts");
        if (alerts_path) {
            int prefix_len = (int)(alerts_path - base);
            memcpy(url, base, prefix_len);
            snprintf(url + prefix_len, sizeof(url) - prefix_len, "/api/devices/status");
        } else {
            /* 如果配置不是完整 alerts 路径，则退化为 host:port + 固定路径。 */
            snprintf(url, sizeof(url), "%s/api/devices/status", base);
        }
    }

    printf("status: tick, mem=%d%% -> %s\n", mem, url);

    if (http_post_json(url, g_api_key, json) == 0) {
        printf("status: sent ok (mem=%d%%)\n", mem);
        return 1;
    } else {
        fprintf(stderr, "status: send failed\n");
        return -1;
    }
}
