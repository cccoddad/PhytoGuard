#ifndef ALERT_CONFIG_H
#define ALERT_CONFIG_H

#include "alert_dispatch.h"

/* 填充默认配置，保证即使配置文件缺失也能继续运行。 */
int alert_config_load(const char *path, alert_config_t *config);
/* 从 JSON 配置文件读取云端地址、API Key、短信参数等。 */
void alert_config_defaults(alert_config_t *config);

#endif
