# 测量口径说明

> 目的：让 README 里的每个性能数字都能回答「怎么测的」。
> **状态：测量方法已定义，标注 ⏳ 的数值尚未在当前仓库状态下复测，面试前需真机复核。**

## 1. NPU 推理延迟 ~8ms（双阶段合计）⏳

**口径**：单轮「Stage1 推理 + Stage2 推理」的 NPU 耗时之和，不含抓帧、预处理、模型加载、JSON/HTTP。

**测量方法**：
```c
// 在 heshi_run_model() 内包裹 Execute 调用：
struct timeval t0, t1;
gettimeofday(&t0, NULL);
Execute(...);            // ACL 推理执行
gettimeofday(&t1, NULL);
printf("exec_us=%ld\n",
    (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec));
```

**记录**：Stage1 单次、Stage2 单次、两者之和，各跑 200 轮取平均与 P95。

**注意必须区分的三个数字**（面试常被追问）：
| 指标 | 含什么 | 预期量级 |
|------|--------|----------|
| NPU exec | 仅 Execute() | ~ms 级 |
| 单轮推理 | 预处理 + exec + softmax/topk | 明显大于 exec |
| 单轮端到端 | 抓帧 + 预处理 + 两阶段 + JSON 落盘 | 更大 |

README 里写的 ~8ms 指第一行。**若实测不一致，改 README 而不是改口径。**

## 2. 模型懒加载开销 ⏳

**口径**：`aclmdlLoadFromFile` 到 `aclmdlGetDesc` 完成的耗时，以及该模型常驻时的内存增量。

**测量方法**：在 `heshi_infer_two_stage` 的加载块（`heshi_v2_dual_infer.c:1671-1684`）前后打时间戳；RSS 用 `cat /proc/<pid>/status | grep VmRSS` 在加载前后各采一次。

**意义**：验证「懒加载换内存」这个决策的实际代价——如果加载耗时接近甚至超过推理本身，需要考虑缓存上一轮命中的模型而不是每轮卸载。

## 3. 内存占用（RSS）⏳

**口径**：推理主进程稳定运行 10 分钟后的 VmRSS。

**测量方法**：
```sh
# 板上执行
pidof heshi_v2_dual_infer
cat /proc/<pid>/status | grep -E 'VmRSS|VmHWM'
```
- `VmRSS`：当前常驻内存（报告用这个）
- `VmHWM`：历史峰值（论证「懒加载避免了多大峰值」用这个，对照组为 6 模型全常驻）

## 4. 开机到首条告警 < 60s ⏳

**口径**：从上电（或 `S99heshi` 启动时刻）到云端 `/api/alerts` 收到第一条告警的时间。

**测量方法**：
1. 串口/日志记录 `run_attach_two_stage.sh` 首行输出时间戳 T0；
2. 云端记录第一条 POST 到达时间戳 T1（或 `sqlite3 alerts.db "SELECT created_at FROM alerts ORDER BY id LIMIT 1"`）；
3. T1 - T0。

**依赖项**：sample_vio 起 sensor（脚本轮询最多 30s）+ L610 拨号 + 首轮推理。4G 拨号是最大不确定项，应取 3 次冷启动的最差值。

## 5. 模型精度（Top-1）⏳

**口径**：验证集上 Top-1 准确率，非训练集。

**来源**：`training/train_*.py` 训练日志的 val 结果。**注意**：需确认 ultralytics 输出的是 val 集而非 train 集指标；健康类/背景类的 precision 尤其容易被质疑，建议同时保存 confusion matrix。

## 6. 整机功耗 < 8.5W ⏳

**口径**：12V（或实测供电电压）输入端功率，系统满载（推理循环运行中）稳态读数。

**测量方法**：功耗仪串联在电源输入端，等数值稳定 30 秒后读数；分别记录待机（无推理）与满载两个值。

## 复测清单（面试前完成）

- [ ] NPU exec 延迟（Stage1 / Stage2 / 合计，mean + P95）
- [ ] 懒加载耗时 vs 推理耗时
- [ ] VmRSS / VmHWM
- [ ] 开机→首条告警 ×3 次冷启动
- [ ] Top-1 指标确认为 val 集
- [ ] 满载功耗
- [ ] 按实测结果回填 README「关键性能」一节
