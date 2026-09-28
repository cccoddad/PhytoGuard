const express = require('express');
const router = express.Router();
const db = require('../db');

// 获取设备列表，结果中已经包含聚合统计字段。
router.get('/', (req, res) => {
    const devices = db.getDevices();
    res.json(devices);
});

// 手动注册或更新设备名称、位置等元信息。
router.post('/', (req, res) => {
    const { device_id, name, location } = req.body;
    if (!device_id) {
        return res.status(400).json({ error: 'device_id is required' });
    }
    db.registerDevice(device_id, name || '', location || '');
    const device = db.getDeviceById(device_id);
    res.json(device);
});

// 板端周期性心跳接口，用于刷新在线状态和内存占用。
router.post('/status', (req, res) => {
    const { device_id, online, powered_on, memory_percent, last_seen_ts } = req.body;
    if (!device_id) {
        return res.status(400).json({ error: 'device_id is required' });
    }
    const device = db.updateDeviceStatus(device_id, {
        online, powered_on, memory_percent, last_seen_ts
    });
    res.json(device);
});

module.exports = router;
