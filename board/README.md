# board/ — SS928V100 板子端 C 代码

## 文件说明

| 文件 | 行数 | 功能 |
|------|------|------|
| `heshi_v2_dual_infer.c` | 2235 | 核心推理程序：VPSS帧捕获→预处理→双阶段NPU推理→JSON输出→HTTP告警→SHT3X读取 |
| `sht3x.c` / `sht3x.h` | 150 + 16 | SHT3X-DIS温湿度传感器驱动（I2C_RDWR ioctl，地址0x44） |
| `alert_dispatch.c` / `alert_dispatch.h` | 576 + 43 | HTTP告警发送（raw socket HTTP/1.0 POST，Base64 JPEG编码）+ SMS后备 |
| `alert_config.c` / `alert_config.h` | 156 + 11 | 读取/mnt/heshi_v2/alert_config.json配置 |
| `status_report.c` / `status_report.h` | 249 + 11 | 设备心跳上报（每60秒POST到云端） |
| `Makefile` | ~85 | ARM aarch64交叉编译（aarch64-mix210-linux-gcc） |
| `run_attach_two_stage.sh` | 63 | 两阶段推理一键启动：sample_vio → 等待sensor → --two-stage --attach-only |
| `l610_connect.sh` | 82 | L610 4G拨号（幂等）：AT探活 → APN设置 → ECM激活 → DHCP |
| `l610_watchdog.sh` | 72 | 4G链路守护进程：每30秒检测enx接口/AT端口/IP/Ping→异常自动重连，连续失败退避 |
| `S98l610` | 58 | L610 开机自启：拉起 l610_connect.sh + l610_watchdog.sh（LSB头，兼容 busybox init） |
| `S99heshi` | 50 | 推理开机自启：kill 残留进程 → exec run_attach_two_stage.sh |
| `alert_config.json` | ~10 | 告警配置文件（云端URL、API Key、SMS开关等） |

> **注意**：`l610_connect.sh` / `l610_watchdog.sh` / `S98l610` / `S99heshi` 四个脚本依据 L610 常见流程编写，**未经实机验证**；AT 指令、APN、接口名等参数均在脚本头部变量中，拿到模组手册后需核对。
>
> **注意**：`alert_config.c` 内建默认串口与 `alert_config.json` 配置的 `sms_tty` 可能不一致，**以配置文件为准**。

## 编译方法

```bash
cd board/
make all
# 产物: heshi_v2_dual_infer (ARM64 ELF, ~2.4MB)
```

## 板子端开机自启链

```
系统上电
  → S02initfs (文件系统)
  → S89nettools (网络)
  → S90autorun (媒体模块: ISP/VI/VPSS/NPU)
  → S91pinmux (GPIO/I2C/SPI复用)
  → S92wifi (可选)
  → S98l610 (L610 4G拨号 + watchdog)
  → S99heshi (推理启动)
60秒内完成全部初始化
```

## 核心数据流

```
摄像头 → ISP → VPSS → RGB888
                       ↓
                预处理(CPU)
            square crop 2160→224
            /255.0 → NCHW float32
                       ↓
              Stage1 NPU推理
            YOLOv8n 植物分类(6类)
                 top1=Tomato
                       ↓
              按需加载 Stage2 OM
            YOLOv8n 病害分类(4类)
                top1=Late_blight
                       ↓
          组合: "Tomato___Late_blight"
           + SHT3X温湿度(每2秒)
                       ↓
          last_result.json (每3秒)
          HTTP POST → 云端 API
```

## 关键设计

1. **预处理只做一次**：两个Stage共享同一块NCHW tensor，避免重复crop+resize
2. **病害模型懒加载**：Stage1结果出来后才aclmdlLoadFromFile，用毕立即aclmdlUnload
3. **双网络通道**：eth0有线 + L610 4G enx，告警不依赖特定网络
4. **watchdog自愈**：独立守护进程，推理或4G异常→自动重启/重连
5. **setlinebuf(stdout)**：避免管道重定向输出缓冲导致的"假死"
