# miniprogram/ — 微信小程序

## 文件说明

| 文件 | 功能 |
|------|------|
| `app.js` | 小程序入口，globalData（apiBase/apiKey），全局配置 |
| `app.json` | 页面注册 + tabBar（告警/设备） + 导航栏绿色主题 |
| `app.wxss` | 全局样式 |
| `sitemap.json` | 微信索引配置 |
| `utils/api.js` | API 请求封装（wx.request），统一拼接 URL + X-API-Key 鉴权 + 错误处理 |
| `pages/alerts/*` | 告警列表页：加载/分页/轮询/筛选/多选/长按/右划/温湿度解析 |
| `pages/detail/*` | 告警详情页：加载/标记已读/图片预览/右划返回/温湿度解析 |
| `pages/devices/*` | 设备状态页：加载/轮询/筛选/在线判断（last_seen_ts超时） |
| `pages/device-alerts/*` | 设备告警页：同告警列表，但仅显示单设备（navigateTo独立页） |
| `pages/register/*` | 设备注册页：device_id/名称/位置 → POST /api/devices |

## 页面列表

| 页面 | 路径 | Tab |
|------|------|-----|
| 告警列表 | pages/alerts/alerts | Tab1（首页） |
| 告警详情 | pages/detail/detail | 非Tab（navigateTo） |
| 设备状态 | pages/devices/devices | Tab2 |
| 设备注册 | pages/register/register | 非Tab（navigateTo） |
| 设备告警 | pages/device-alerts/device-alerts | 非Tab（navigateTo） |

## 关键交互

1. **告警卡片**：点击→详情，长按→弹出标记已读/删除菜单，绿色左竖边=未读
2. **多选模式**：顶部按钮"多选"→复选框→全选/删除选中
3. **筛选**：统计栏切换（全部/今日/未读），当前筛选高亮
4. **右划返回**：告警列表和详情页均支持，阈值80px，水平为主方向
5. **30秒轮询**：onLoad启动定时器，onUnload清除，保持数据实时
6. **离线检测**：设备页优先用last_seen_ts超时判断（120秒无心跳→离线），而非online字段

## prepareAlerts() 数据映射

| 输入字段 | 输出 | 示例 |
|---------|------|------|
| crop_name + disease_name | displayName | "番茄晚疫病" |
| top1_prob × 100 | probPercent | "48" |
| probPercent | severityClass | ≥85 severe / ≥70 warning / <70 normal |
| temperature_c + humidity_rh | envText | "31.5°C / 75.6%" |
| acknowledged | cardClass/statusText | unread/未读 或 done/已读 |
