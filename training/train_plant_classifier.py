#!/usr/bin/env python3
"""Train Stage 1 plant classifier. 6 classes, YOLOv8n-cls."""
"""
这个脚本只训练第一阶段“作物分类器”。
输出模型负责回答“当前叶片属于哪一种作物”，为第二阶段病害模型选择提供入口。
"""
from ultralytics import YOLO
from pathlib import Path

DATASET = Path("/home/hero/Workspace/heshi_two_stage/plant")
PROJECT = Path("/home/hero/Workspace/runs/two_stage")
RUN_NAME = "plant_classifier"
EPOCHS = 20
IMGSZ = 224
BATCH = 128

print(f"=== Stage1: Plant Classifier ({EPOCHS} epochs) ===")
print(f"  model    : yolov8n-cls.pt")
print(f"  dataset  : {DATASET}")
print(f"  imgsz    : {IMGSZ}")
print(f"  batch    : {BATCH}")
print()

model = YOLO("yolov8n-cls.pt")

results = model.train(
    data=str(DATASET),
    epochs=EPOCHS,
    imgsz=IMGSZ,
    batch=BATCH,
    device="cpu",
    optimizer="AdamW",
    lr0=0.001,
    lrf=0.01,
    cos_lr=True,
    augment=False,  # plant shapes are obvious, no augmentation needed
    project=str(PROJECT),
    name=RUN_NAME,
    exist_ok=True,
)
