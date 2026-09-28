const express = require('express');
const router = express.Router();
const labels = require('../data/labels_zh.json');

// 返回全量标签映射表，主要给前端或调试工具做静态字典查询。
router.get('/', (req, res) => {
    res.json(labels);
});

module.exports = router;
