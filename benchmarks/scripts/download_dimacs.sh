#!/usr/bin/env bash
# download_dimacs.sh [dest_dir] — скачивает весь DIMACS Challenge SAT
# benchmark набор (archive.dimacs.rutgers.edu, классика: aim/jnh/ii/par/
# dubois/ssa/pret/hole/hanoi/etc, все n небольшие, ~4 МБ на 249 файлов) в
# формате CNF (DIMACS cnf, .cnf.Z — сжато старым Unix compress).
#
# ВАЖНО: bmm-translib пока не имеет CNF-импортёра (только обратное
# направление — Tseitin AIG→CNF в verify/sat_encoding, для верификации, не
# для чтения внешних .cnf). Эти данные — сырьё для будущего cnf_to_aig
# (например через готовый Tseitin-энкодер в обратную сторону, или прямое
# чтение клозов как AND/OR-дерева), не готовый к употреблению тестовый
# набор "из коробки".
set -euo pipefail

DEST="${1:-benchmarks/data/dimacs-cnf}"
BASE_URL="http://archive.dimacs.rutgers.edu/pub/challenge/sat/benchmarks/cnf"

mkdir -p "$DEST"

echo "download_dimacs.sh: получаю список файлов из $BASE_URL"
listing="$(curl -fsSL "$BASE_URL/")"

names="$(printf '%s' "$listing" | grep -oE 'href="[^"?][^"]*"' | sed -E 's/href="//;s/"$//' | grep -v '^/' )"

count=0
while IFS= read -r name; do
    [[ -z "$name" ]] && continue
    url="${BASE_URL}/${name}"
    out="${DEST}/${name}"
    if [[ -f "$out" || -f "${out%.Z}" ]]; then
        continue
    fi
    if curl -fsSL -o "$out" "$url"; then
        count=$((count + 1))
    else
        echo "  ПРОПУЩЕНО (не скачалось): $name" >&2
        rm -f "$out"
    fi
done <<< "$names"

echo "Скачано файлов: $count"

echo "Распаковываю .cnf.Z -> .cnf"
for z in "$DEST"/*.cnf.Z; do
    [[ -e "$z" ]] || continue
    gzip -dk "$z" 2>/dev/null || uncompress -c "$z" > "${z%.Z}"
done

echo "Готово. CNF-файлы лежат в $DEST/*.cnf"
