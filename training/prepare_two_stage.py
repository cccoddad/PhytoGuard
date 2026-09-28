#!/usr/bin/env python3
"""Reorganize 16-class dataset into two-stage structure:
  Stage1: 1 plant dataset (6 classes)
  Stage2: 5 disease datasets (2-4 classes each)
"""
"""这个脚本的作用是把原始 16 类单阶段数据集重组成两阶段训练所需的目录结构。

思路：
1. Stage1 只关心“是什么作物”，所以把同一作物下的不同病害样本合并到同一个类别。
2. Stage2 按作物拆成多个小数据集，每个数据集只保留该作物相关的病害类别。
"""
import shutil
import hashlib
from pathlib import Path

SRC = Path("/home/hero/Workspace/heshi_16cls_v3_dataset")
OUT = Path("/home/hero/Workspace/heshi_two_stage")
PLANT_MAP = {
    "Apple___Apple_scab": "Apple",
    "Apple___healthy": "Apple",
    "Corn___Cercospora_leaf_spot Gray_leaf_spot": "Corn",
    "Corn___Common_rust": "Corn",
    "Corn___Northern_Leaf_Blight": "Corn",
    "Corn___healthy": "Corn",
    "Grape___Black_rot": "Grape",
    "Grape___healthy": "Grape",
    "Potato___Early_blight": "Potato",
    "Potato___Late_blight": "Potato",
    "Potato___healthy": "Potato",
    "Tomato___Early_blight": "Tomato",
    "Tomato___Late_blight": "Tomato",
    "Tomato___Leaf_Mold": "Tomato",
    "Tomato___healthy": "Tomato",
    "Unknown_or_Background": "Unknown_or_Background",
}

DISEASE_CONFIG = {
    "apple":    {"plants": ["Apple"],    "keep": ["Apple_scab", "healthy"]},
    "corn":     {"plants": ["Corn"],     "keep": ["Cercospora_leaf_spot Gray_leaf_spot", "Common_rust", "Northern_Leaf_Blight", "healthy"]},
    "grape":    {"plants": ["Grape"],    "keep": ["Black_rot", "healthy"]},
    "potato":   {"plants": ["Potato"],   "keep": ["Early_blight", "Late_blight", "healthy"]},
    "tomato":   {"plants": ["Tomato"],   "keep": ["Early_blight", "Late_blight", "Leaf_Mold", "healthy"]},
}


def make_plant_dataset():
    """Stage 1: merge disease classes into plant type."""
    src = SRC
    dst = OUT / "plant"
    print(f"\n=== Stage1: Plant dataset -> {dst} ===")

    for split in ["train", "val", "test"]:
        src_split = src / split
        if not src_split.exists():
            continue
        for cls_dir in sorted(src_split.iterdir()):
            if not cls_dir.is_dir():
                continue
            plant = PLANT_MAP[cls_dir.name]
            dst_dir = dst / split / plant
            dst_dir.mkdir(parents=True, exist_ok=True)
            for f in cls_dir.iterdir():
                if f.suffix.lower() in (".jpg", ".jpeg", ".png", ".ppm"):
                    uniq = f"{cls_dir.name}_{f.stem}"[:120] + f.suffix.lower()
                    shutil.copy2(f, dst_dir / uniq)
        # count
        total = sum(1 for _ in (dst / split).rglob("*.*"))
        print(f"  {split}: {total} images")


def make_disease_dataset(name, config):
    """Stage 2: one dataset per plant type with disease labels."""
    dst = OUT / f"disease_{name}"
    plants = config["plants"]
    keep = config["keep"]
    print(f"\n=== Stage2: {name} disease dataset -> {dst} ===")

    for split in ["train", "val", "test"]:
        src_split = SRC / split
        if not src_split.exists():
            continue
        for cls_dir in sorted(src_split.iterdir()):
            if not cls_dir.is_dir():
                continue
            parts = cls_dir.name.split("___")
            if len(parts) < 2:
                continue
            plant, disease = parts[0], parts[1]
            if plant not in plants:
                continue
            if disease not in keep:
                continue
            dst_dir = dst / split / disease
            dst_dir.mkdir(parents=True, exist_ok=True)
            for f in cls_dir.iterdir():
                if f.suffix.lower() in (".jpg", ".jpeg", ".png", ".ppm"):
                    uniq = f"{cls_dir.name}_{f.stem}"[:120] + f.suffix.lower()
                    shutil.copy2(f, dst_dir / uniq)
        total = sum(1 for _ in (dst / split).rglob("*.*"))
        print(f"  {split}: {total} images")


if __name__ == "__main__":
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    # Stage 1
    make_plant_dataset()

    # Stage 2
    for name, config in DISEASE_CONFIG.items():
        make_disease_dataset(name, config)

    print(f"\nDone. Output: {OUT}")
    for d in sorted(OUT.iterdir()):
        print(f"  {d.name}/")
