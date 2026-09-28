# 阶段一：止血执行清单

出口条件：`cloud` 能起服务并跑通全链路 curl；仓库内无真实密钥；4 个 shell 脚本通过语法检查；README 数字与代码一致。

执行顺序：**G → A → B → C → D → E → F**

---

## G. 新建 `doc/` 目录

```
PhytoGuard/
└── doc/
    ├── README.md              # 目录索引 + 各文件用途
    ├── 00-master-plan.md      # 四阶段总览、排期、进度打勾表
    ├── 01-phase1-plan.md      # 本文件
    ├── design-decisions.md    # 阶段三产出（待写）
    ├── measurement-notes.md   # 阶段三产出（待写）
    └── images/                # 阶段三产出：真机图、拓扑、时序图
```

根 `README.md` 末尾补一节 `## 文档` 链到 `doc/`。

---

## A. cloud 目录归位（6 次移动，0 行代码改动）

| 原位置 | 新位置 | 说明 |
|--------|--------|------|
| `cloud/alerts.route.js` | `cloud/routes/alerts.js` | 内部 `../db`、`../data/labels_zh.json`、`__dirname/../uploads` 归位后自动正确 |
| `cloud/devices.route.js` | `cloud/routes/devices.js` | 同上 |
| `cloud/labels.route.js` | `cloud/routes/labels.js` | 同上 |
| `cloud/auth.middleware.js` | `cloud/middleware/auth.js` | 匹配 `server.js:4` 的 `./middleware/auth` |
| `cloud/labels_zh.json` | `cloud/data/labels_zh.json` | 匹配路由 `../data/...` 与 `db.js` 的 `data/alerts.db` |
| — | `cloud/uploads/.gitkeep` | multer dest 与 base64 落盘都需要该目录 |

`server.js`、`db.js` 不改动。

---

## B. 脱敏（4 处，占位符必须三处一致）

| 位置 | 现状 | 改为 |
|------|------|------|
| `board/alert_config.json` `cloud_api_key` | 明文 API 密钥（本文不再复述） | `REPLACE_ME_API_KEY` |
| `board/alert_config.json` `sms_phone` | 真实手机号 | `+86XXXXXXXXXXX` |
| `cloud/middleware/auth.js:1` | 同一密钥做 fallback | `process.env.API_KEY \|\| 'REPLACE_ME_API_KEY'` |
| `miniprogram/app.js:13` | 同一密钥 | `REPLACE_ME_API_KEY` |

附注：`alert_config.c:104` 默认 tty 为 `/dev/ttyUSB5`，配置文件写 `/dev/ttyUSB0` —— 在 `board/README.md` 注明「以配置文件为准」。

---

## C. 新增 `.gitignore`

```
node_modules/
__pycache__/
*.pyc
cloud/data/*.db
cloud/data/*.db-*
cloud/uploads/*
!cloud/uploads/.gitkeep
board/*.o
board/heshi_v2_dual_infer
```

---

## D. 重写 4 个 shell 脚本

**前置约束**：仓库与本机均未找到 L610 AT 资料，因此按骨架实现：
文件头标注「未经实机验证」；`APN` / `AT 口` / `enx 接口名` / `ECM 激活指令` 全部提为脚本顶部变量。

### D1. `board/l610_connect.sh`（约 40 行）— 拨号，幂等

- 幂等：已存在 `enx*` 接口且已分配 IP → 直接 `exit 0`
- AT 口：`${L610_AT_TTY:-/dev/ttyUSB2}`，`stty -F ... 115200 raw -echo`
- 流程：`AT` 探活 → `AT+CGDCONT=1,"IP","<apn>"` → `AT+CFUN=1` → ECM 激活（占位指令）→ 轮询 `+CGACT: 1,1` → `udhcpc -i <ifname>`
- 日志：`/tmp/l610_connect.log`

### D2. `board/l610_watchdog.sh`（约 55 行）— 守护

- 30 秒循环，四段检测：接口存在 → AT 口可读 → 有 IP → `ping -c3 223.5.5.5`
- 任一失败：调 `l610_connect.sh` 重连；连续失败退避到 60 秒
- `trap` 处理 `TERM INT EXIT`，风格与 `run_attach_two_stage.sh` 的 cleanup 一致

### D3. `board/S98l610`（约 70 行）— init 脚本

- LSB 头（`### BEGIN INIT INFO`）+ `start|stop|restart|status` case 分支（兼容 busybox init）
- `start`：后台拉起 `l610_watchdog.sh`，pid 写 `/var/run/l610_watchdog.pid`

### D4. `board/S99heshi`（约 40 行）— init 脚本

- **设计决策**：`run_attach_two_stage.sh` 内部已包含「起 sample_vio → 轮询 `init success` → 起两阶段推理 → trap 清理」，S99 再写一遍会产生两份重复逻辑
- 因此 S99 = `kill` 残留 `heshi_v2_dual_infer` / `sample_vio` → `exec run_attach_two_stage.sh`
- **连带改动**：`board/README.md` 中 S99 那行措辞同步改写

---

## E. README 对齐

### E1. 根 `README.md`

| 位置 | 现状 | 改为 |
|------|------|------|
| L14 | 核心推理程序（~2000行） | （2235行） |
| 末尾 | — | 新增 `## 文档` 链到 `doc/` |

### E2. `board/README.md` 表格

| 行 | 现状 | 改为 |
|----|------|------|
| heshi_v2_dual_infer.c | ~2000 | 2235 |
| sht3x.c/h | ~140 | 164（150+14） |
| alert_dispatch.c/h | ~550 | 605（573+32） |
| alert_config.c/h | ~100 | 167（156+11） |
| status_report.c/h | ~200 | 258（249+9） |
| run_attach_two_stage.sh | ~50 | 63 |
| S99heshi | 自起 sample_vio | 清理残留 + 调用 run_attach_two_stage.sh |
| 开机自启链 | — | 补一句「脚本未经实机验证」 |

### E3. `training/README.md`

| 位置 | 现状 | 改为 |
|------|------|------|
| 文件表 `train_disease_models.py` | 30 epochs | 20 epochs（代码 `EPOCHS = 20`，plant 分支同为 20） |

---

## F. 验证命令（阶段一出口条件）

```powershell
# 1. cloud 能起
cd cloud; npm install; npm start        # -> PhytoGuard Alert API running on :8080

# 2. 全链路 curl（带假图）
curl POST /api/alerts   {device_id, top1_label:"Tomato___Late_blight", image_base64:<小图>}
curl GET  /api/alerts   # 能查到、crop_name/disease_name 已 enrich
curl GET  /api/devices  # 在线状态
curl -H "X-API-Key: REPLACE_ME_API_KEY" POST /api/devices/status

# 3. 无密钥写入应返回 401

# 4. shell 语法检查
sh -n board/l610_connect.sh
sh -n board/l610_watchdog.sh
sh -n board/S98l610
sh -n board/S99heshi

# 5. 全仓密钥扫描，应零命中
#    扫描对象：原 API 密钥、原手机号（真实值不在本文复述，
#    可用脱敏前的备份或密码管理器里的值做匹配串）
```

---

## 本阶段明确不做

- 不改 `heshi_v2_dual_infer.c` 任何逻辑
- 不 `git init`（属阶段二 2.1）
- 不改 miniprogram，除 `app.js:13` 密钥外
- 不写阶段三的设计文档正文（仅建占位文件）
