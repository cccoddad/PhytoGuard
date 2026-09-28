#!/bin/bash
# Export 6 models to ONNX → ATC convert to OM for two-stage inference.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE="${SCRIPT_DIR}/runs/two_stage"
DEPLOY="${SCRIPT_DIR}/heshi_two_stage_deploy"
SOC="OPTG"
OPSET="11"
IMGSZ="224"

# Use conda Python 3.9 for ATC compatibility
export PATH="${HOME}/anaconda3/envs/atc/bin:${PATH}"

echo "=========================================="
echo "Two-Stage Model Export & Convert Pipeline"
echo "Output: ${DEPLOY}/"
echo "=========================================="

rm -rf "${DEPLOY}"
mkdir -p "${DEPLOY}"

# ---- Stage 1: Plant classifier ----
echo ""
echo "--- Stage1: Plant classifier ---"
python3 -c "
from ultralytics import YOLO
model = YOLO('${BASE}/plant_classifier/weights/best.pt')
model.export(format='onnx', imgsz=${IMGSZ}, opset=${OPSET}, simplify=True)
import shutil
shutil.copy('${BASE}/plant_classifier/weights/best.onnx', '${DEPLOY}/plant.onnx')
print('Exported: plant.onnx')
"

# Copy labels
cp "${BASE}/plant_classifier/weights/best.onnx" "${DEPLOY}/plant.onnx" 2>/dev/null || true
python3 -c "
from pathlib import Path
import shutil
# Labels are in the dataset dir
for cls in sorted((Path('${SCRIPT_DIR}/heshi_two_stage/plant/train')).iterdir()):
    if cls.is_dir():
        with open('${DEPLOY}/plant_labels.txt', 'w') as f:
            for c in sorted((Path('${SCRIPT_DIR}/heshi_two_stage/plant/train')).iterdir()):
                if c.is_dir():
                    f.write(c.name + '\n')
        break
print('Plant labels written')
"

# ---- Stage 2: Disease classifiers ----
for name in disease_apple disease_corn disease_grape disease_potato disease_tomato; do
    echo ""
    echo "--- Stage2: ${name} ---"

    model_pt="${BASE}/${name}/weights/best.pt"
    if [ ! -f "${model_pt}" ]; then
        echo "SKIP: ${model_pt} not found"
        continue
    fi

    # Export ONNX
    python3 -c "
from ultralytics import YOLO
model = YOLO('${model_pt}')
model.export(format='onnx', imgsz=${IMGSZ}, opset=${OPSET}, simplify=True)
import shutil
shutil.copy('${model_pt}'.replace('.pt', '.onnx'), '${DEPLOY}/${name}.onnx')
print('Exported: ${name}.onnx')
"

    # Copy labels from dataset
    dataset_dir="${SCRIPT_DIR}/heshi_two_stage/${name}/train"
    if [ -d "${dataset_dir}" ]; then
        python3 -c "
from pathlib import Path
classes = sorted([d.name for d in Path('${dataset_dir}').iterdir() if d.is_dir()])
with open('${DEPLOY}/${name}_labels.txt', 'w') as f:
    f.write('\n'.join(classes))
print(f'{len(classes)} labels written for ${name}')
"
    fi
done

# ---- ATC convert all ONNX to OM ----
echo ""
echo "=== ATC Conversion ==="

ATC_BIN="${HOME}/Ascend/ascend-toolkit/5.20.t6.2.b060/x86_64-linux/compiler/bin/atc"
export PATH="$(dirname "${ATC_BIN}"):${PATH}"

for onnx_file in "${DEPLOY}"/*.onnx; do
    base=$(basename "${onnx_file}" .onnx)
    om_file="${DEPLOY}/${base}.om"

    echo ""
    echo "Converting: ${base}.onnx → ${base}.om"

    atc --model="${onnx_file}" \
        --framework=5 \
        --output="${DEPLOY}/${base}" \
        --soc_version="${SOC}" \
        --input_shape="images:1,3,224,224" \
        --input_format=NCHW \
        --output_type=FP32

    if [ -f "${om_file}" ]; then
        echo "OK: ${om_file} ($(du -h "${om_file}" | cut -f1))"
    else
        echo "ERROR: ${base}.om not created"
    fi
done

echo ""
echo "=========================================="
echo "All models exported to: ${DEPLOY}/"
ls -lh "${DEPLOY}"/*.om 2>/dev/null || echo "(OM files listed above)"
echo "=========================================="
