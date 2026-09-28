const API_KEY = process.env.API_KEY || 'REPLACE_ME_API_KEY';

module.exports = function authMiddleware(req, res, next) {
    // 板端写入告警和心跳时必须携带 X-API-Key，避免接口被随意写入脏数据。
    const key = req.headers['x-api-key'];
    if (!key || key !== API_KEY) {
        return res.status(401).json({ error: 'unauthorized' });
    }
    next();
};
