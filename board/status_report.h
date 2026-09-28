#ifndef STATUS_REPORT_H
#define STATUS_REPORT_H

/* 初始化设备状态上报模块。 */
int status_report_init(const char *device_id, const char *api_url, const char *api_key);
/* 释放状态上报模块内部状态。 */
void status_report_deinit(void);
/* 按固定周期发送一次设备心跳；未到周期时直接返回。 */
int status_report_tick(void);

#endif
