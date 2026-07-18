#!/usr/bin/env bash
# download_satlib.sh [dest_dir] — скачивает весь SATLIB Benchmark Problems
# набор (cs.ubc.ca/~hoos/SATLIB — RND3SAT, CBS, GCP/flat, SW-GCP,
# PLANNING, DIMACS-зеркало, AIS, BMC, QG и др., ~100 архивов .tar.gz,
# суммарно порядка 50-100 МБ — каждый архив в отдельности небольшой,
# проверено HEAD-запросами перед написанием скрипта) в формате CNF.
#
# ВАЖНО: как и download_dimacs.sh — bmm-translib пока не имеет
# CNF-импортёра, это сырьё для будущей работы (cnf_to_aig или аналог),
# не готовый "из коробки" тестовый корпус для существующих 20 функций.
set -euo pipefail

DEST="${1:-benchmarks/data/satlib}"
BASE_URL="https://www.cs.ubc.ca/~hoos/SATLIB/Benchmarks/SAT"
INDEX_URL="https://www.cs.ubc.ca/~hoos/SATLIB/benchm.html"

mkdir -p "$DEST"

echo "download_satlib.sh: получаю список архивов из $INDEX_URL"
page="$(curl -fsSL "$INDEX_URL")"

rel_paths="$(printf '%s' "$page" | grep -oiE 'href=[^ >]*\.tar\.gz' | sed -E 's/href=//i' | sort -u)"

count=0
while IFS= read -r rel_path; do
    [[ -z "$rel_path" ]] && continue
    # rel_path вида "RND3SAT/uf20-91.tar.gz" (в архиве уже есть "SAT/" в начале части)
    rel_path="${rel_path#Benchmarks/SAT/}"
    subdir="$(dirname "$rel_path")"
    fname="$(basename "$rel_path")"
    out_dir="${DEST}/${subdir}"
    out="${out_dir}/${fname}"
    if [[ -f "$out" ]]; then
        continue
    fi
    mkdir -p "$out_dir"
    url="${BASE_URL}/${rel_path}"
    if curl -fsSL -o "$out" "$url"; then
        count=$((count + 1))
        tar -xzf "$out" -C "$out_dir" 2>/dev/null || true
    else
        echo "  ПРОПУЩЕНО (не скачалось): $rel_path" >&2
        rm -f "$out"
    fi
done <<< "$rel_paths"

echo "Скачано и распаковано архивов: $count"
echo "Готово. CNF-файлы лежат в $DEST/<категория>/*.cnf"
