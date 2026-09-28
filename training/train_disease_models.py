#!/usr/bin/env python3
"""Train Stage 2 disease classifiers (5 models, YOLOv8n-cls, CPU)."""
"""
这个脚本顺序训练 5 个病害分类模型。
每个模型只负责一种作物，因此类别更少、决策边界也更聚焦。
"""
from ultralytics import YOLO
from pathlib import Path

BASE = Path("/home/hero/Workspace/heshi_two_stage")
PROJECT = Path("/home/hero/Workspace/runs/two_stage")
IMGSZ = 224
BATCH = 256
EPOCHS = 20

CONFIGS = [
    {"name": "disease_apple",  "classes": 2},
    {"name": "disease_corn",   "classes": 4},
    {"name": "disease_grape",  "classes": 2},
    {"name": "disease_potato", "classes": 3},
    {"name": "disease_tomato", "classes": 4},
]

for cfg in CONFIGS:
    name = cfg["name"]
    dataset = BASE / name
    if not dataset.exists():
        print(f"SKIP {name}: dataset not found")
        continue

    print(f"\n{'='*60}")
    print(f"Training: {name} ({cfg['classes']} classes, {EPOCHS} epochs)")
    print(f"{'='*60}")

    model = YOLO("yolov8n-cls.pt")
    results = model.train(
        data=str(dataset),
        epochs=EPOCHS,
        imgsz=IMGSZ,
        batch=BATCH,
        device="cpu",
        optimizer="AdamW",
        lr0=0.001,
        lrf=0.01,
        cos_lr=True,
        augment=True,
        project=str(PROJECT),
        name=name,
        exist_ok=True,
    )
    print(f"Done: {name}")

print("\n=== All disease models trained! ===")
