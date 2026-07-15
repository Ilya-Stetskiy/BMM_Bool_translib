#!/usr/bin/env bash
# download_epfl.sh [dest_dir] — скачивает EPFL Combinational Benchmark Suite
# (github.com/lsils/benchmarks — тот же lsils, что и mockturtle) в формате
# .aig/.blif. Используется как источник крупных AIG для benchmarks/*
# (в отличие от verify/ground_truth, где n<=20 и функции синтетические).
set -euo pipefail

DEST="${1:-benchmarks/data/epfl}"
mkdir -p "$DEST"

REPO_URL="https://github.com/lsils/benchmarks.git"

if [[ -d "$DEST/.git" ]]; then
    echo "download_epfl.sh: $DEST уже существует, обновляю (git pull)"
    git -C "$DEST" pull --ff-only
else
    echo "download_epfl.sh: клонирую $REPO_URL в $DEST"
    git clone --depth 1 "$REPO_URL" "$DEST"
fi

echo "Готово. Схемы лежат в $DEST/{arithmetic,random_control}/*.{aig,blif}"
