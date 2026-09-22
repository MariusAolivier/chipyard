#!/usr/bin/env bash
#SBATCH --job-name=ne16-dory-network
#SBATCH --time=02:00:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --output=/cluster/work/mariusao/slurm-ne16-dory-network-%j.out

set -euo pipefail

chipyard_root=${CHIPYARD_ROOT:-/cluster/work/mariusao/chipyard-dory-agent}
dory_root=${DORY_ROOT:-/cluster/work/mariusao/dory}

cd "$chipyard_root"
export PAGER=cat
export GIT_PAGER=cat

if [[ -f /cluster/work/mariusao/dory/.venv/bin/activate ]]; then
  source /cluster/work/mariusao/dory/.venv/bin/activate
fi
python3 generators/ne16/scripts/generate-dory-ne16-generated-network.py \
  --dory-root "$dory_root" \
  --config generators/ne16/config/config_three_layer_ne16_chipyard.json \
  --output-dir tests/ne16-dory-generated-network
python3 generators/ne16/scripts/test-import-dory-ne16-generated-network.py

if [[ -f /cluster/work/mariusao/chipyard/.conda-env/bin/activate ]]; then
  export RISCV="${RISCV:-}"
  export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
  source /cluster/work/mariusao/chipyard/.conda-env/bin/activate
  export PATH=/cluster/work/mariusao/chipyard/.conda-env/riscv-tools/bin:$PATH
fi
source env.sh
export XDG_RUNTIME_DIR=/tmp/${USER}-runtime-${SLURM_JOB_ID}
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

generators/ne16/scripts/run-conv-test.sh
