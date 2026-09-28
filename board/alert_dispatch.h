#ifndef ALERT_DISPATCH_H
#define ALERT_DISPATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 告警分发配置。
 * 这里把“往哪里发、是否启用、多久允许重复发送一次”集中收口，
 * 板端主流程只需要在检测到病害后调用 alert_dispatch_send()，
 * 不需要关心 HTTP / 短信各自的发送细节。
 */
typedef struct {
    const char *cloud_api_url;
    const char *cloud_api_key;
    const char *sms_tty;
    const char *sms_phone;
    const char *device_id;
    int sms_cooldown_sec;
    int http_enabled;
    int sms_enabled;
} alert_config_t;

/* 初始化告警发送模块，内部会复制配置并按需打开短信串口。 */
int alert_dispatch_init(const alert_config_t *config);
/* 释放告警发送模块占用的资源。 */
void alert_dispatch_deinit(void);

/*
 * 发送一次告警。
 * ppm_path 会被压缩为缩略 JPEG 再转 base64，随 JSON 一并发送到云端；
 * 同时在满足节流条件时，可额外通过 4G 模块发送短信。
 */
int alert_dispatch_send(const char *ppm_path, int camera_id,
    int top1_id, const char *top1_label, float top1_prob,
    int consecutive_hits, long long ts_ms,
    float temperature_c, float humidity_rh);

#ifdef __cplusplus
}
#endif
#endif
