const express = require('express');
const router = express.Router();
const multer = require('multer');
const path = require('path');
const fs = require('fs');
const crypto = require('crypto');
const db = require('../db');
const labels = require('../data/labels_zh.json');

const upload = multer({
    dest: path.join(__dirname, '..', 'uploads'),
    limits: { fileSize: 30 * 1024 * 1024 }  // 限制上传体积，避免异常大图占满磁盘。
});

function saveBase64Image(b64) {
    if (!b64 || typeof b64 !== 'string' || b64.length < 10) return null;
    try {
        // 板端通常上传的是压缩后的 JPEG base64，解码后直接落盘即可。
        const buf = Buffer.from(b64, 'base64');
        if (buf.length < 100) return null;  // too small to be valid
        const ext = '.jpg';
        const name = crypto.randomBytes(16).toString('hex') + ext;
        const filepath = path.join(__dirname, '..', 'uploads', name);
        fs.writeFileSync(filepath, buf);
        return '/api/images/' + name;
    } catch (e) {
        console.error('base64 decode error:', e.message);
        return null;
    }
}

// 先构建一个“英文类名 -> 中文元数据”的查找表，后续 enrich 时直接 O(1) 查。
const LABELS_BY_NAME = {};
for (const key of Object.keys(labels)) {
    const entry = labels[key];
    if (entry && entry.class_name) {
        LABELS_BY_NAME[entry.class_name] = entry;
    }
}

function enrichAlert(alert) {
    if (!alert) return alert;
    // 优先使用 top1_label 文本做匹配，因为两阶段模式下这个字段更稳定。
    let meta = null;
    if (alert.top1_label) {
        meta = LABELS_BY_NAME[alert.top1_label];
        if (!meta) {
            // 如果组合标签没命中，再退回旧 16 类 ID 映射。
            meta = labels[String(alert.top1_id)];
        }
    } else {
        meta = labels[String(alert.top1_id)];
    }
    return {
        ...alert,
        top1_label_zh: meta ? meta.disease : null,
        crop_name: meta ? meta.crop : null,
        disease_name: meta ? meta.disease : null
    };
}

// POST 入库前也先补中文字段，这样数据库里的记录本身就带有展示友好信息。
function enrichAndInsert(alert) {
    const label = alert.top1_label || '';
    const parts = label.split('___');
    let meta = LABELS_BY_NAME[label] || labels[String(alert.top1_id)];
    if (!meta && parts.length >= 2) {
        // 两阶段模式本身就是 Plant___Disease，所以这里更多是保留兼容思路。
        meta = LABELS_BY_NAME[label];
    }
    if (meta) {
        alert.top1_label_zh = meta.disease;
        alert.crop_name = meta.crop;
        alert.disease_name = meta.disease;
    }
    return alert;
}

// 板端上报告警入口。
router.post('/', upload.single('image'), (req, res) => {
    const { device_id, ts_ms, camera_id, top1_id, top1_label,
            top1_prob, consecutive_hits, temperature_c, humidity_rh } = req.body;

    if (!device_id || !top1_label) {
        return res.status(400).json({ error: 'missing required fields' });
    }

    // 既兼容 multipart 上传文件，也兼容 JSON 内直接放 image_base64。
    let image_url = req.file ? '/api/images/' + req.file.filename : null;
    if (!image_url && req.body.image_base64) {
        image_url = saveBase64Image(req.body.image_base64);
    }

    const alert = {
        device_id,
        ts_ms: parseInt(ts_ms) || Date.now(),
        camera_id: parseInt(camera_id) || 0,
        top1_id: parseInt(top1_id) || 0,
        top1_label,
        top1_prob: parseFloat(top1_prob) || 0,
        consecutive_hits: parseInt(consecutive_hits) || 1,
        image_url,
        temperature_c: parseFloat(temperature_c) || null,
        humidity_rh: parseFloat(humidity_rh) || null
    };

    const result = db.insertAlert(enrichAndInsert(alert));

    // 设备首次发来告警时自动补注册。
    db.upsertDevice(device_id);

    res.json({ id: result.lastInsertRowid, ...alert });
});

// 分页获取告警列表，可按设备过滤。
router.get('/', (req, res) => {
    const page = parseInt(req.query.page) || 1;
    const limit = Math.min(parseInt(req.query.limit) || 20, 100);
    const deviceId = req.query.device_id || req.query.device || '';
    const data = db.getAlerts(page, limit, deviceId);

    res.json({
        ...data,
        rows: data.rows.map(enrichAlert)
    });
});

// 返回告警总数、今日新增和未读数。
router.get('/stats', (req, res) => {
    const deviceId = req.query.device_id || req.query.device || '';
    res.json(db.getAlertStats(deviceId));
});

// 获取指定设备最近一条告警。
router.get('/latest', (req, res) => {
    const { device_id } = req.query;
    if (!device_id) {
        return res.status(400).json({ error: 'device_id required' });
    }
    const alert = db.getLatestAlert(device_id);
    res.json(enrichAlert(alert) || null);
});

// 获取单条告警详情。
router.get('/:id', (req, res) => {
    const alert = db.getAlertById(req.params.id);
    if (!alert) {
        return res.status(404).json({ error: 'not found' });
    }
    res.json(enrichAlert(alert));
});

// 把告警标记为已读。
router.patch('/:id/ack', (req, res) => {
    const result = db.acknowledgeAlert(req.params.id);
    if (result.changes === 0) {
        return res.status(404).json({ error: 'not found' });
    }
    res.json({ acknowledged: true });
});

// 删除一条告警记录。
router.delete('/:id', (req, res) => {
    const result = db.deleteAlert(req.params.id);
    if (result.changes === 0) {
        return res.status(404).json({ error: 'not found' });
    }
    res.json({ deleted: true });
});

module.exports = router;
