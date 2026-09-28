#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "alert_dispatch.h"

#define THUMB_MAX_W 640
#define JPEG_QUALITY 75

/* 保存当前生效的告警发送配置。 */
static alert_config_t g_config;
/* 短信通道的节流状态：记录上一次发送的类别和时间。 */
static int g_last_alert_class = -1;
static time_t g_last_sms_time = 0;
/* HTTP 通道也单独做节流，避免同一种病害在短时间内刷屏。 */
static time_t g_last_http_time = 0;
static int g_last_http_class = -1;

static int sms_fd = -1;

/* ---------- JPEG 写回调 ---------- */

struct jpeg_write_ctx {
    unsigned char *buf;
    int len;
};

/* stb_image_write 会在编码过程中多次回调这里。
 * 我们把每一段 JPEG 字节不断追加到内存缓冲区，最终得到完整图片。
 */
static void jpeg_write_cb(void *ctx_ptr, void *data, int size)
{
    struct jpeg_write_ctx *c = (struct jpeg_write_ctx *)ctx_ptr;
    unsigned char *newbuf = (unsigned char *)realloc(c->buf, c->len + size);
    if (newbuf) {
        memcpy(newbuf + c->len, data, size);
        c->buf = newbuf;
        c->len += size;
    }
}

/* ---------- 读取 PPM -> 缩放 -> 编码 JPEG ---------- */

static unsigned char *read_ppm_resize_jpeg(const char *ppm_path, int *jpeg_len)
{
    FILE *f;
    int w, h, maxval;
    unsigned char *src = NULL;
    unsigned char *thumb = NULL;
    unsigned char *jpeg = NULL;
    int tw, th, x, y, c;

    *jpeg_len = 0;
    f = fopen(ppm_path, "rb");
    if (!f) return NULL;

    if (fscanf(f, "P6 %d %d %d", &w, &h, &maxval) != 3 || maxval != 255) {
        fclose(f);
        return NULL;
    }
    fgetc(f); /* skip single whitespace after header */

    src = (unsigned char *)malloc((size_t)w * h * 3);
    if (!src) { fclose(f); return NULL; }

    if (fread(src, 1, (size_t)w * h * 3, f) != (size_t)w * h * 3) {
        free(src);
        fclose(f);
        return NULL;
    }
    fclose(f);

    /* 告警图片不直接上传原始 PPM。
     * 这里先压成较小缩略图，显著减少 HTTP 负载和云端存储压力。
     */
    tw = w > THUMB_MAX_W ? THUMB_MAX_W : w;
    th = (int)((float)h * tw / w);
    if (th < 1) th = 1;

    thumb = (unsigned char *)malloc((size_t)tw * th * 3);
    if (!thumb) { free(src); return NULL; }

    for (y = 0; y < th; y++) {
        for (x = 0; x < tw; x++) {
            int sx = (int)((float)x * w / tw);
            int sy = (int)((float)y * h / th);
            if (sx >= w) sx = w - 1;
            if (sy >= h) sy = h - 1;
            for (c = 0; c < 3; c++) {
                thumb[(y * tw + x) * 3 + c] = src[(sy * w + sx) * 3 + c];
            }
        }
    }
    free(src);

    /* 通过回调把 JPEG 编码结果直接写进内存，不走中间临时文件。 */
    {
        struct jpeg_write_ctx ctx = { NULL, 0 };
        if (stbi_write_jpg_to_func(jpeg_write_cb, &ctx, tw, th, 3, thumb, JPEG_QUALITY) == 0) {
            free(ctx.buf);
            ctx.buf = NULL;
            ctx.len = 0;
        }
        *jpeg_len = ctx.len;
        jpeg = ctx.buf;
    }
    free(thumb);
    return jpeg;
}

/* ---------- Base64 编码 ---------- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, int len)
{
    /* 云端接口走 JSON，所以图片需要先转成 base64 文本。 */
    int out_len = ((len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(out_len);
    int i, j;

    if (!out) return NULL;

    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64_table[v & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

/* ---------- 极简 HTTP 客户端 ---------- */

static int http_parse_url(const char *url, char *host, int host_len,
    int *port, char *path, int path_len)
{
    const char *p;
    const char *host_start;
    const char *path_start;

    *port = 80;
    host[0] = '\0';
    path[0] = '/';
    path[1] = '\0';

    if (strncmp(url, "http://", 7) != 0) return -1;
    p = url + 7;

    host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    {
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
        path_start = p;
        {
            int len = (int)strlen(path_start);
            if (len >= path_len) len = path_len - 1;
            memcpy(path, path_start, len);
            path[len] = '\0';
        }
    }
    return 0;
}

/* 基于阻塞 socket 建立 HTTP 连接。
 * 这里故意不用 libcurl，主要是为了减少板端依赖。
 */
static int http_connect(const char *server_url)
{
    char host[256];
    char path[512];
    int port;
    int sock;
    struct sockaddr_in addr;
    struct hostent *he;

    if (http_parse_url(server_url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        fprintf(stderr, "alert: bad url %s\n", server_url);
        return -1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    {
        /* 给收发都加超时，避免 4G 链路异常时永久卡死在系统调用里。 */
        struct timeval tv = {5, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    he = gethostbyname(host);
    if (!he) {
        close(sock);
        return -1;
    }

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
    char header[1024];
    char *req = NULL;
    int body_len, total_len, n;
    ssize_t w;
    char rsp[256];

    sock = http_connect(server_url);
    if (sock < 0) return -1;

    body_len = (int)strlen(json_body);

    /* 从完整 URL 里拆出 Host 和 Path，手工拼装 HTTP/1.0 请求。 */
    {
        const char *p = server_url + 7; /* skip http:// */
        const char *host_start = p;
        const char *host_end = p;
        const char *path_start = "/";

        while (*host_end && *host_end != ':' && *host_end != '/') host_end++;

        while (*p && *p != '/') p++;
        if (*p) path_start = p;

        n = snprintf(header, sizeof(header),
            "POST %s HTTP/1.0\r\n"
            "Host: %.*s\r\n"
            "Content-Type: application/json\r\n"
            "X-API-Key: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            path_start,
            (int)(host_end - host_start), host_start,
            api_key, body_len);

        /* 请求头和请求体一次性拼成连续缓冲区，便于 write 发送。 */
        total_len = n + body_len;
        req = (char *)malloc(total_len);
        if (!req) { close(sock); return -1; }
        memcpy(req, header, n);
        memcpy(req + n, json_body, body_len);
    }

    w = write(sock, req, total_len);
    free(req);
    if (w < 0) { close(sock); return -1; }

    {
        /* 只有响应里明确包含 200 才算成功;
         * 401/500 等响应同样有字节返回, 不能当成发送成功,
         * 否则告警会"假成功”, 云端根本收不到。 */
        ssize_t r = read(sock, rsp, sizeof(rsp) - 1);
        close(sock);
        if (r > 0) {
            rsp[r] = '\0';
            if (strstr(rsp, "200")) return 0;
            fprintf(stderr, "alert: http response: %.200s\n", rsp);
        }
        return -1;
    }
}

/* ---------- 通过 L610 模块发送短信 ---------- */

static int sms_open(const char *tty_path)
{
    struct termios tio;
    int fd;

    fd = open(tty_path, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "alert: open %s failed, errno=%d\n", tty_path, errno);
        return -1;
    }

    fcntl(fd, F_SETFL, 0);

    /* 串口采用最简单的 115200 8N1 原始模式。 */
    memset(&tio, 0, sizeof(tio));
    tio.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
    tio.c_iflag = IGNPAR;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cc[VTIME] = 5;
    tio.c_cc[VMIN] = 0;
    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, &tio);

    return fd;
}

static int sms_wait_ok(int fd, int timeout_ms)
{
    char buf[256];
    int elapsed = 0;
    int total_read = 0;

    /* AT 命令响应长度不大，这里用一个小缓冲区轮询拼接即可。 */
    while (elapsed < timeout_ms) {
        int r = read(fd, buf + total_read, sizeof(buf) - 1 - total_read);
        if (r > 0) {
            total_read += r;
            buf[total_read] = '\0';
            if (strstr(buf, "OK") || strstr(buf, "> ")) return 0;
            if (strstr(buf, "ERROR")) return -1;
        }
        usleep(100000);
        elapsed += 100;
    }
    return -1;
}

static int sms_send(const char *phone, const char *message)
{
    char cmd[256];
    int n;

    if (sms_fd < 0) return -1;

    /* 每次发短信前重新做一轮 AT 同步，降低模块状态不一致导致的失败概率。 */
    write(sms_fd, "AT\r\n", 4);
    if (sms_wait_ok(sms_fd, 2000) != 0) return -1;
    usleep(200000);

    /* 切换短信文本模式。 */
    write(sms_fd, "AT+CMGF=1\r\n", 11);
    if (sms_wait_ok(sms_fd, 2000) != 0) return -1;
    usleep(200000);

    /* 指定收件人号码。 */
    n = snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", phone);
    write(sms_fd, cmd, n);
    if (sms_wait_ok(sms_fd, 3000) != 0) return -1;
    usleep(200000);

    /* 文本内容发送完后，用 Ctrl-Z 提交。 */
    write(sms_fd, message, strlen(message));
    write(sms_fd, "\x1A", 1);

    /* 等待模块返回 +CMGS / OK，确认短信已经被基带接受。 */
    {
        char buf[256];
        int total = 0;
        int i;
        for (i = 0; i < 40; i++) {
            int r = read(sms_fd, buf + total, sizeof(buf) - 1 - total);
            if (r > 0) {
                total += r;
                buf[total] = '\0';
                if (strstr(buf, "+CMGS:") || strstr(buf, "OK")) {
                    printf("alert: sms sent ok\n");
                    return 0;
                }
                if (strstr(buf, "ERROR")) break;
            }
            usleep(250000);
        }
    }
    fprintf(stderr, "alert: sms send timeout\n");
    return -1;
}

/* ---------- 对外接口 ---------- */

int alert_dispatch_init(const alert_config_t *config)
{
    /* 复制配置，避免调用方传入的临时结构体在外部失效。 */
    g_config = *config;
    g_last_alert_class = -1;
    g_last_sms_time = 0;
    g_last_http_class = -1;
    g_last_http_time = 0;

    /* 短信初始化失败时只降级关闭短信，不影响主推理流程继续工作。 */
    if (g_config.sms_enabled && g_config.sms_tty) {
        sms_fd = sms_open(g_config.sms_tty);
        if (sms_fd < 0) {
            fprintf(stderr, "alert: sms init failed, continuing without sms\n");
            g_config.sms_enabled = 0;
        }
    }

    printf("alert: dispatch init, http=%d sms=%d sms_cooldown=%ds\n",
        g_config.http_enabled, g_config.sms_enabled, g_config.sms_cooldown_sec);
    return 0;
}

void alert_dispatch_deinit(void)
{
    if (sms_fd >= 0) {
        close(sms_fd);
        sms_fd = -1;
    }
}

int alert_dispatch_send(const char *ppm_path, int camera_id,
    int top1_id, const char *top1_label, float top1_prob,
    int consecutive_hits, long long ts_ms,
    float temperature_c, float humidity_rh)
{
    char *json = NULL;
    char sms_msg[512];
    unsigned char *jpeg = NULL;
    char *b64 = NULL;
    int jpeg_len = 0;
    int sent = 0;
    int json_len;
    time_t now = time(NULL);

    /* 1. 读取当前抓拍图，压缩成缩略 JPEG，再编码成 base64。 */
    jpeg = read_ppm_resize_jpeg(ppm_path, &jpeg_len);
    if (jpeg && jpeg_len > 0) {
        b64 = base64_encode(jpeg, jpeg_len);
    }

    /* 2. 构造发往云端的 JSON。
     * 如果图片压缩失败，也允许继续上报纯文本告警，尽量保证“告警不断流”。
     */
    {
        const char *fmt_with_img =
            "{"
            "\"device_id\":\"%s\","
            "\"ts_ms\":%lld,"
            "\"camera_id\":%d,"
            "\"top1_id\":%d,"
            "\"top1_label\":\"%s\","
            "\"top1_prob\":%.4f,"
            "\"consecutive_hits\":%d,"
            "\"temperature_c\":%.2f,"
            "\"humidity_rh\":%.2f,"
            "\"image_base64\":\"%s\""
            "}";
        const char *fmt_no_img =
            "{"
            "\"device_id\":\"%s\","
            "\"ts_ms\":%lld,"
            "\"camera_id\":%d,"
            "\"top1_id\":%d,"
            "\"top1_label\":\"%s\","
            "\"top1_prob\":%.4f,"
            "\"consecutive_hits\":%d,"
            "\"temperature_c\":%.2f,"
            "\"humidity_rh\":%.2f"
            "}";

        if (b64) {
            json_len = snprintf(NULL, 0, fmt_with_img,
                g_config.device_id, ts_ms, camera_id,
                top1_id, top1_label, top1_prob, consecutive_hits,
                (double)temperature_c, (double)humidity_rh, b64);
            json = (char *)malloc(json_len + 1);
            if (json) {
                snprintf(json, json_len + 1, fmt_with_img,
                    g_config.device_id, ts_ms, camera_id,
                    top1_id, top1_label, top1_prob, consecutive_hits,
                    (double)temperature_c, (double)humidity_rh, b64);
            }
        } else {
            json_len = snprintf(NULL, 0, fmt_no_img,
                g_config.device_id, ts_ms, camera_id,
                top1_id, top1_label, top1_prob, consecutive_hits,
                (double)temperature_c, (double)humidity_rh);
            json = (char *)malloc(json_len + 1);
            if (json) {
                snprintf(json, json_len + 1, fmt_no_img,
                    g_config.device_id, ts_ms, camera_id,
                    top1_id, top1_label, top1_prob, consecutive_hits,
                    (double)temperature_c, (double)humidity_rh);
            }
        }
    }

    if (!json) {
        free(jpeg);
        free(b64);
        return 0;
    }

    /* 3. 发送 HTTP 告警。
     * 节流规则是“类别变化立即发；类别相同则需要等待 cooldown 到期”。
     */
    if (g_config.http_enabled && g_config.cloud_api_url) {
        int http_cooldown = g_config.sms_cooldown_sec > 0 ? g_config.sms_cooldown_sec : 300;
        if (g_last_http_class != top1_id || (now - g_last_http_time) >= http_cooldown) {
            printf("alert: sending http to %s\n", g_config.cloud_api_url);
            if (http_post_json(g_config.cloud_api_url, g_config.cloud_api_key, json) == 0) {
                printf("alert: http sent ok (%s, prob=%.3f, img=%d bytes)\n",
                    top1_label, top1_prob, b64 ? (int)strlen(b64) : 0);
                g_last_http_class = top1_id;
                g_last_http_time = now;
                sent = 1;
            } else {
                fprintf(stderr, "alert: http send failed\n");
            }
        } else {
            printf("alert: http suppressed (cooldown %ds)\n",
                (int)(now - g_last_http_time));
        }
    }

    /* 4. 发送短信告警。
     * 短信成本更高，因此和 HTTP 一样也做按类别节流。
     */
    if (g_config.sms_enabled && g_config.sms_phone && sms_fd >= 0) {
        int cooldown = g_config.sms_cooldown_sec > 0 ? g_config.sms_cooldown_sec : 300;
        if (g_last_alert_class != top1_id ||
            (now - g_last_sms_time) >= cooldown) {
            g_last_alert_class = top1_id;
            g_last_sms_time = now;
            snprintf(sms_msg, sizeof(sms_msg),
                "[PlantGuard]\r\n"
                "Device: %s\r\n"
                "Alert: %s\r\n"
                "Confidence: %.1f%%\r\n"
                "Hits: %d\r\n"
                "Time: %lld",
                g_config.device_id, top1_label,
                top1_prob * 100.0f, consecutive_hits, ts_ms);
            if (sms_send(g_config.sms_phone, sms_msg) == 0) {
                sent = 1;
            }
        } else {
            printf("alert: sms suppressed (cooldown, last=%ds ago)\n",
                (int)(now - g_last_sms_time));
        }
    }

    /* 5. 清理临时资源。 */
    free(json);
    free(jpeg);
    free(b64);
    return sent;
}
