#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
echo "compiling..."
${CC:-cc} -O2 -Wall -Wextra -DDSM_HOST_SIM -I src -o build/host_sim \
    src/host_sim.c src/suborg.c src/schwung_dubstarmoonicon.c -lm
echo "compiled clean. running:"; echo
./build/host_sim
