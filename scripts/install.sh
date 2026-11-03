#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
DEVICE="${DEVICE:-ableton@move.local}"
TARGET="/data/UserData/schwung/modules/sound_generators/dubstarmoonicon"
[ -f build/pack/dsp.so ] || { echo "run bash scripts/build.sh first" >&2; exit 1; }
export COPYFILE_DISABLE=1
tar -czf build/dubstarmoonicon-module.tar.gz -C build/pack .
scp build/dubstarmoonicon-module.tar.gz "$DEVICE:/data/UserData/"
ssh "$DEVICE" "mkdir -p '$TARGET' && rm -f '$TARGET'/_* && tar -xzf /data/UserData/dubstarmoonicon-module.tar.gz -C '$TARGET'"
echo "done. Reboot the Move and pick DubStarMooniCon."
