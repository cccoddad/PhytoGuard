# PlantGuard — 基于边缘AI的植物病害智能监测系统

## 项目简介

本系统以海思SS928V100嵌入式AI开发板为核心，搭载8MP高清摄像头和昇腾NPU，创新性地采用"植物识别→病害分类"双阶段级联推理架构，通过6个轻量化YOLOv8n-cls模型完成端侧全流程推理。同时集成SHT3X温湿度传感器和L610 4G通信模块，实现环境感知与远程告警推送。云端采用Node.js+Express+SQLite，微信小程序提供移动端告警展示。

## 目录结构

```
plantguard/
├── README.md              # 本文件
├── doc/                   # 方案与设计文档
│   ├── 00-master-plan.md      # 四阶段总览与进度
│   ├── 01-phase1-plan.md      # 阶段一止血清单
│   ├── design-decisions.md    # 关键设计决策记录
│   └── measurement-notes.md   # 性能指标测量口径
├── board/                 # 板子端C代码（SS928V100）
│   ├── README.md
│   ├── heshi_v2_dual_infer.c   # 核心推理程序（2235行）
│   ├── sht3x.c / sht3x.h       # SHT3X温湿度传感器驱动
│   ├── alert_dispatch.c/h      # HTTP告警发送 + SMS后备
│   ├── alert_config.c/h        # 告警配置管理
│   ├── status_report.c/h       # 设备心跳上报
│   ├── Makefile                # ARM64交叉编译
│   ├── run_attach_two_stage.sh # 两阶段推理一键启动
│   ├── l610_connect.sh         # L610 4G拨号（幂等）
│   ├── l610_watchdog.sh        # 4G链路守护进程
│   ├── S98l610                 # L610 开机自启脚本
│   └── S99heshi                # 推理开机自启脚本
├── cloud/                 # 云端API（Node.js + Express）
│   ├── README.md
│   ├── server.js               # Express主入口
│   ├── db.js                   # SQLite数据库封装
│   ├── package.json            # NPM依赖
│   ├── routes/                 # API路由（alerts/devices/labels）
│   ├── middleware/auth.js       # API鉴权中间件
│   ├── data/labels_zh.json     # 16类中文标签映射
│   ├── uploads/                # 告警图片落盘目录
│   └── deploy.sh               # 部署到VPS脚本
├── miniprogram/           # 微信小程序
│   ├── README.md
│   ├── app.js/json/wxss        # 小程序入口
│   ├── utils/api.js            # API请求封装
│   └── pages/                  # alerts/detail/devices/register/device-alerts
└── training/              # 训练脚本
    ├── README.md
    ├── prepare_two_stage.py     # 数据集重组（16类→双阶段）
    ├── train_plant_classifier.py # 训练植物分类器
    ├── train_disease_models.py   # 训练病害分类器×5
    ├── convert_two_stage.sh      # ONNX导出+ATC转OM
    └── test_sht3x.py             # SHT3X传感器测试
```

## 技术栈

| 层级 | 技术 |
|------|------|
| 板子端 | C语言, ARM Linux 4.19, 昇腾ACL NPU API, I2C, USB CDC-ECM |
| 云端 | Node.js 20, Express, better-sqlite3, PM2 |
| 小程序 | 微信原生框架 (JS + WXML + WXSS) |
| AI模型 | YOLOv8n-cls × 6, PyTorch → ONNX (opset 11) → ATC (OPTG) |
| 通信 | HTTP/1.0 (raw socket, 自研), L610 4G ECM (AT command) |

## 关键性能

- 植物分类 Top-1: 98.4%（6类）
- 病害分类 Top-1: 97.0% ~ 100%（5个模型）
- NPU推理延迟: ~8ms（双阶段合计）
- 6个模型总大小: 20.4MB
- 整机功耗: < 8.5W
- 开机到首条告警: < 60秒

## 文档

详细的设计决策与测量口径见 [doc/](doc/)：

- [00-master-plan.md](doc/00-master-plan.md) — 四阶段总览、排期与进度
- [01-phase1-plan.md](doc/01-phase1-plan.md) — 阶段一止血清单与验证命令
- [design-decisions.md](doc/design-decisions.md) — 两阶段级联 / 告警防抖 / raw socket / 双网口降级
- [measurement-notes.md](doc/measurement-notes.md) — 性能指标的测量方法

## 快速开始

```bash
# 云端
cd cloud && npm install && npm start        # :8080

# 板端（需交叉编译环境）
cd board && make all
sh run_attach_two_stage.sh

# 训练
cd training && python3 prepare_two_stage.py && python3 train_plant_classifier.py
```
