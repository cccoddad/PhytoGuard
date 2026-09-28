# cloud/ — 云端 API 服务 (Node.js + Express)

## 文件说明

| 文件 | 功能 |
|------|------|
| `server.js` | Express 主入口，注册路由、中间件、静态文件，监听 :8080 |
| `db.js` | SQLite 数据库封装（better-sqlite3），自动建表+迁移，CRUD 方法 |
| `routes/alerts.js` | 告警 API：POST（接收）/ GET（列表）/ GET :id（详情）/ PATCH ack（已读）/ DELETE（删除）/ GET stats（统计） |
| `routes/devices.js` | 设备 API：GET（列表）/ POST（注册）/ POST status（心跳更新） |
| `routes/labels.js` | 标签 API：GET 返回 labels_zh.json 全量映射 |
| `middleware/auth.js` | X-API-Key 鉴权中间件 |
| `data/labels_zh.json` | 16 类标签中文映射表（class_name → crop + disease） |
| `uploads/` | 告警图片落盘目录（base64 解码后写入，静态路由暴露） |
| `data/alerts.db` | SQLite 数据库文件（运行时生成，已 gitignore） |
| `package.json` | NPM 依赖：express, better-sqlite3, multer, cors |
| `deploy.sh` | 一键部署到 VPS 脚本（rsync + npm install + pm2 restart） |

## 核心 API

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/alerts | 板子上报告警（JSON + base64 图片自动解码保存） |
| GET | /api/alerts?page=&limit=&device_id= | 分页查询告警列表 |
| GET | /api/alerts/stats | 统计（总数/未读/今日新增） |
| GET | /api/alerts/latest?device_id= | 最新告警 |
| GET | /api/alerts/:id | 告警详情 |
| PATCH | /api/alerts/:id/ack | 标记已读 |
| DELETE | /api/alerts/:id | 删除告警 |
| GET | /api/devices | 设备列表（含温湿度、告警数、心跳时间） |
| POST | /api/devices/status | 设备心跳上报 |
| GET | /api/health | 健康检查 |

## 关键设计

1. **中文标签自动映射**：告警入库时用 top1_label 字符串匹配 labels_zh.json，生成 crop_name、disease_name、top1_label_zh 三个衍生字段，优先于 top1_id 数字映射（兼容两阶段格式）
2. **数据库自动迁移**：通过 PRAGMA table_info 检测缺失列，ALTER TABLE 添加，向后兼容
3. **Base64 图片自动解码**：板子发来的 image_base64 字段自动 Buffer.from→写文件→返回相对 URL
4. **设备查询含温湿度**：GET /api/devices 通过子查询关联告警表最新记录的 temperature_c/humidity_rh

## 部署

```bash
cd cloud/
sh deploy.sh root@your-vps-ip
# rsync → npm install → pm2 restart plant-alert
```
