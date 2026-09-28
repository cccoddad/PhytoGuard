const express = require('express');
const path = require('path');
const cors = require('cors');
const auth = require('./middleware/auth');
const db = require('./db');
const alertsRouter = require('./routes/alerts');
const devicesRouter = require('./routes/devices');
const labelsRouter = require('./routes/labels');

const app = express();
const PORT = process.env.PORT || 8080;

// 允许小程序/浏览器跨域访问 API。
app.use(cors());
// 告警 JSON 内可能包含 base64 图片，所以请求体上限适当调大。
app.use(express.json({ limit: '2mb' }));

// 只有写接口要求 API Key，读接口默认开放，便于小程序直接查询。
app.use('/api/alerts', (req, res, next) => {
    if (req.method === 'POST') return auth(req, res, next);
    next();
});
app.use('/api/devices', (req, res, next) => {
    if (req.method === 'POST') return auth(req, res, next);
    next();
});

// 告警图片保存到 uploads 目录，通过静态路由直接暴露给前端查看。
app.use('/api/images', express.static(path.join(__dirname, 'uploads')));

// 注册各业务路由。
app.use('/api/alerts', alertsRouter);
app.use('/api/devices', devicesRouter);
app.use('/api/labels', labelsRouter);

// 给小程序首页统计栏和 tab 角标使用的聚合接口。
app.get('/api/stats', (req, res) => {
    const deviceId = req.query.device_id || req.query.device || '';
    res.json(db.getAlertStats(deviceId));
});

// 简单健康检查，便于部署后探活。
app.get('/api/health', (req, res) => {
    res.json({ status: 'ok', time: new Date().toISOString() });
});

app.listen(PORT, () => {
    console.log(`PhytoGuard Alert API running on http://0.0.0.0:${PORT}`);
});
