#!/usr/bin/env bash
#SBATCH --job-name=ne16-dory-network
#SBATCH --time=02:00:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --output=/cluster/work/mariusao/slurm-ne16-dory-network-%j.out

set -euo pipefail

chipyard_root=${CHIPYARD_ROOT:-/cluster/work/mariusao/chipyard-dory-agent}
dory_root=${DORY_ROOT:-/cluster/work/mariusao/dory}
dory_python=${DORY_PYTHON:-$dory_root/.venv/bin/python3}

cd "$chipyard_root"
export PAGER=cat
export GIT_PAGER=cat

if [[ ! -x "$dory_python" ]]; then
  echo "DORY Python interpreter not found: $dory_python" >&2
  exit 1
fi
rm -rf tests/ne16-dory-generated-network
"$dory_python" generators/ne16/scripts/generate-dory-ne16-generated-network.py \
  --dory-root "$dory_root" \
  --config generators/ne16/config/config_three_layer_ne16_chipyard.json \
  --output-dir tests/ne16-dory-generated-network
"$dory_python" generators/ne16/scripts/test-import-dory-ne16-generated-network.py

module load Miniconda3/24.7.1-0
set +u
source /cluster/work/mariusao/chipyard/env.sh
set -u
export XDG_RUNTIME_DIR=/tmp/${USER}-runtime-${SLURM_JOB_ID}
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

if [[ "${BUILD_SIMULATOR:-1}" == 1 ]]; then
  export MAKEFLAGS="-j${JOBS:-${SLURM_CPUS_PER_TASK:-2}}"
  generators/ne16/scripts/build-chipyard-sim.sh
fi

generators/ne16/scripts/run-conv-test.sh
