const Database = require('better-sqlite3');
const path = require('path');

const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'data', 'alerts.db');

let db;

function getDb() {
    if (!db) {
        // 采用单例连接，配合 WAL 模式提高读写并发下的稳定性。
        db = new Database(DB_PATH);
        db.pragma('journal_mode = WAL');
        initSchema();
    }
    return db;
}

function initSchema() {
    // 设备表保存在线状态、心跳时间和一些展示用元数据。
    // 告警表保存每一条病害识别结果，以及附带的环境信息和图片地址。
    db.exec(`
        CREATE TABLE IF NOT EXISTS devices (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT UNIQUE NOT NULL,
            name TEXT DEFAULT '',
            location TEXT DEFAULT '',
            online INTEGER DEFAULT 0,
            powered_on INTEGER DEFAULT 0,
            memory_percent INTEGER,
            last_seen_ts INTEGER,
            created_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS alerts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            ts_ms INTEGER NOT NULL,
            camera_id INTEGER DEFAULT 0,
            top1_id INTEGER NOT NULL,
            top1_label TEXT NOT NULL,
            top1_label_zh TEXT,
            crop_name TEXT,
            disease_name TEXT,
            top1_prob REAL NOT NULL,
            consecutive_hits INTEGER DEFAULT 1,
            image_url TEXT,
            acknowledged INTEGER DEFAULT 0,
            created_at TEXT DEFAULT (datetime('now'))
        );

        CREATE INDEX IF NOT EXISTS idx_alerts_device_time ON alerts(device_id, ts_ms DESC);
        CREATE INDEX IF NOT EXISTS idx_alerts_ack ON alerts(acknowledged, ts_ms DESC);
    `);

    // 对旧数据库做最小迁移，避免升级代码后必须手动删库重建。
    const cols = db.pragma('table_info(devices)').map(c => c.name);
    if (!cols.includes('online')) db.exec('ALTER TABLE devices ADD COLUMN online INTEGER DEFAULT 0');
    if (!cols.includes('powered_on')) db.exec('ALTER TABLE devices ADD COLUMN powered_on INTEGER DEFAULT 0');
    if (!cols.includes('memory_percent')) db.exec('ALTER TABLE devices ADD COLUMN memory_percent INTEGER');
    if (!cols.includes('last_seen_ts')) db.exec('ALTER TABLE devices ADD COLUMN last_seen_ts INTEGER');

    const alertCols = db.pragma('table_info(alerts)').map(c => c.name);
    if (!alertCols.includes('temperature_c')) db.exec('ALTER TABLE alerts ADD COLUMN temperature_c REAL');
    if (!alertCols.includes('humidity_rh')) db.exec('ALTER TABLE alerts ADD COLUMN humidity_rh REAL');
}

function insertAlert(alert) {
    const db = getDb();
    // 数据入库前的中文标签映射由 route 层完成，这里只负责写库。
    const stmt = db.prepare(`
        INSERT INTO alerts (device_id, ts_ms, camera_id, top1_id, top1_label,
            top1_label_zh, crop_name, disease_name, top1_prob, consecutive_hits, image_url,
            temperature_c, humidity_rh)
        VALUES (@device_id, @ts_ms, @camera_id, @top1_id, @top1_label,
            @top1_label_zh, @crop_name, @disease_name, @top1_prob, @consecutive_hits, @image_url,
            @temperature_c, @humidity_rh)
    `);
    return stmt.run(alert);
}

function getAlerts(page = 1, limit = 20, deviceId = '') {
    const db = getDb();
    const offset = (page - 1) * limit;
    let rows;
    let total;

    // 支持按设备过滤，方便小程序从设备页跳转到单设备告警页。
    if (deviceId) {
        rows = db.prepare(`
            SELECT * FROM alerts
            WHERE device_id = ?
            ORDER BY ts_ms DESC LIMIT ? OFFSET ?
        `).all(deviceId, limit, offset);
        total = db.prepare(
            'SELECT COUNT(*) as count FROM alerts WHERE device_id = ?'
        ).get(deviceId);
    } else {
        rows = db.prepare(`
            SELECT * FROM alerts ORDER BY ts_ms DESC LIMIT ? OFFSET ?
        `).all(limit, offset);
        total = db.prepare('SELECT COUNT(*) as count FROM alerts').get();
    }

    return { rows, total: total.count, page, limit };
}

function getAlertStats(deviceId = '') {
    const db = getDb();
    const today = new Date();
    today.setHours(0, 0, 0, 0);
    const todayStart = today.getTime();

    // 统计“总数 / 今日新增 / 未读数”，给列表页顶部摘要栏使用。
    const sql = `
        SELECT
            COUNT(*) as totalCount,
            SUM(CASE WHEN ts_ms >= ? THEN 1 ELSE 0 END) as todayCount,
            SUM(CASE WHEN acknowledged = 0 THEN 1 ELSE 0 END) as unreadCount
        FROM alerts
        ${deviceId ? 'WHERE device_id = ?' : ''}
    `;
    const params = deviceId ? [todayStart, deviceId] : [todayStart];
    const row = db.prepare(sql).get(...params);

    return {
        totalCount: row.totalCount || 0,
        todayCount: row.todayCount || 0,
        unreadCount: row.unreadCount || 0
    };
}

function getAlertById(id) {
    const db = getDb();
    return db.prepare('SELECT * FROM alerts WHERE id = ?').get(id);
}

function getLatestAlert(deviceId) {
    const db = getDb();
    return db.prepare(
        'SELECT * FROM alerts WHERE device_id = ? ORDER BY ts_ms DESC LIMIT 1'
    ).get(deviceId);
}

function acknowledgeAlert(id) {
    const db = getDb();
    return db.prepare(
        'UPDATE alerts SET acknowledged = 1 WHERE id = ?'
    ).run(id);
}

function deleteAlert(id) {
    const db = getDb();
    return db.prepare('DELETE FROM alerts WHERE id = ?').run(id);
}

function upsertDevice(deviceId) {
    const db = getDb();
    // 告警入库时如果设备还没显式注册，也先自动补一行设备记录。
    return db.prepare(`
        INSERT INTO devices (device_id) VALUES (?)
        ON CONFLICT(device_id) DO UPDATE SET device_id = device_id
    `).run(deviceId);
}

function getDevices() {
    const db = getDb();
    // 这里把设备表和告警表做轻量聚合，直接返回前端展示所需字段。
    return db.prepare(`
        SELECT d.*,
            (SELECT COUNT(*) FROM alerts WHERE device_id = d.device_id) as alert_count,
            (SELECT ts_ms FROM alerts WHERE device_id = d.device_id ORDER BY ts_ms DESC LIMIT 1) as last_alert_ts,
            (SELECT temperature_c FROM alerts WHERE device_id = d.device_id AND temperature_c IS NOT NULL ORDER BY ts_ms DESC LIMIT 1) as temperature_c,
            (SELECT humidity_rh FROM alerts WHERE device_id = d.device_id AND humidity_rh IS NOT NULL ORDER BY ts_ms DESC LIMIT 1) as humidity_rh
        FROM devices d ORDER BY d.device_id
    `).all();
}

function registerDevice(deviceId, name, location) {
    const db = getDb();
    // 空字符串不会覆盖已有名称/位置，避免误把人工维护信息清空。
    return db.prepare(`
        INSERT INTO devices (device_id, name, location)
        VALUES (?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET
            name = COALESCE(NULLIF(excluded.name, ''), devices.name),
            location = COALESCE(NULLIF(excluded.location, ''), devices.location)
    `).run(deviceId, name || '', location || '');
}

function getDeviceById(deviceId) {
    const db = getDb();
    const device = db.prepare('SELECT * FROM devices WHERE device_id = ?').get(deviceId);
    if (!device) return null;
    const stats = db.prepare(`
        SELECT COUNT(*) as alert_count,
               MAX(ts_ms) as last_alert_ts
        FROM alerts WHERE device_id = ?
    `).get(deviceId);
    return { ...device, alert_count: stats.alert_count || 0, last_alert_ts: stats.last_alert_ts };
}

function updateDeviceStatus(deviceId, status) {
    const db = getDb();
    // 心跳上报可能先于人工注册到达，所以先确保设备行存在，再更新状态。
    db.prepare(`
        INSERT INTO devices (device_id) VALUES (?)
        ON CONFLICT(device_id) DO NOTHING
    `).run(deviceId);

    const sets = [];
    const params = [];
    if (status.online !== undefined) { sets.push('online = ?'); params.push(status.online ? 1 : 0); }
    if (status.powered_on !== undefined) { sets.push('powered_on = ?'); params.push(status.powered_on ? 1 : 0); }
    if (status.memory_percent !== undefined) { sets.push('memory_percent = ?'); params.push(status.memory_percent); }

    // last_seen_ts 每次都会刷新，前端优先根据它判断设备是否掉线。
    sets.push('last_seen_ts = ?');
    params.push(status.last_seen_ts || Date.now());

    params.push(deviceId);
    db.prepare(`UPDATE devices SET ${sets.join(', ')} WHERE device_id = ?`).run(...params);

    return getDeviceById(deviceId);
}

module.exports = { getDb, insertAlert, getAlerts, getAlertStats, getAlertById, getLatestAlert, acknowledgeAlert, deleteAlert, upsertDevice, registerDevice, getDeviceById, getDevices, updateDeviceStatus };
