# training/ — 模型训练与转换脚本

## 文件说明

| 文件 | 功能 |
|------|------|
| `prepare_two_stage.py` | 数据集重组：16类单数据集 → 1个植物集（6类）+ 5个病害集（2-4类） |
| `train_plant_classifier.py` | 训练 Stage1 植物分类器：YOLOv8n-cls, 6类, 20 epochs |
| `train_disease_models.py` | 训练 Stage2 病害分类器×5：YOLOv8n-cls, 2-4类/模型, 20 epochs |
| `convert_two_stage.sh` | 6个模型 ONNX 导出 + ATC 转 OM（SOC=OPTG, NCHW FP32） |
| `test_sht3x.py` | SHT3X 传感器测试脚本：扫描 I2C-0 0x44/0x45 → 连续读温湿度 |

## 训练流程

```bash
# 1. 准备数据集
python3 prepare_two_stage.py
# 输出: heshi_two_stage/plant/ + disease_{apple,corn,grape,potato,tomato}/

# 2. 训练植物分类器（~60分钟 CPU）
python3 train_plant_classifier.py
# 输出: runs/two_stage/plant_classifier/weights/best.pt

# 3. 训练病害分类器×5（~150分钟 CPU，序列执行）
python3 train_disease_models.py
# 输出: runs/two_stage/disease_*/weights/best.pt

# 4. 导出 ONNX + 转 OM
bash convert_two_stage.sh
# 输出: heshi_two_stage_deploy/*.om (6 files, 各~3.4MB)
```

## 模型性能

| 模型 | 类别数 | 训练集 | Top-1 | GFLOPs | 参数 |
|------|--------|--------|-------|--------|------|
| plant | 6 | 15,631 | 98.4% | 3.3 | 1.44M |
| disease_apple | 2 | 1,955 | 100% | 3.3 | 1.44M |
| disease_corn | 4 | 3,476 | 97.0% | 3.3 | 1.44M |
| disease_grape | 2 | 1,536 | 100% | 3.3 | 1.44M |
| disease_potato | 3 | 2,286 | 98.0% | 3.3 | 1.44M |
| disease_tomato | 4 | 4,863 | 97.7% | 3.3 | 1.44M |

## 环境要求

| 依赖 | 版本 |
|------|------|
| Python | 3.10 |
| PyTorch | 2.12+ |
| ultralytics | 8.4+ |
| onnx | 1.21 |
| onnxslim | 0.1.94 |
| Ascend Toolkit (ATC) | 5.20.t6.2.b060 |
| ATC Python | 3.9 (conda env `atc`) |

## 关键参数

- 输入尺寸：224×224 NCHW float32
- 归一化：仅 /255.0（无 ImageNet mean/std）
- ONNX opset：11
- ATC soc_version：OPTG
- 优化器：AdamW, lr0=0.001, cosine 衰减

## 预处理说明

板子端预处理与训练时保持一致：
1. Square center crop（取短边做正方形裁剪）
2. Bilinear resize 到 224×224
3. /255.0 归一化（无 mean/std 减除）
4. HWC→NCHW 排列
