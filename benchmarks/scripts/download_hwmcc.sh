#!/usr/bin/env bash
# download_hwmcc.sh [dest_dir] [base_url] — скачивает выбор бенчмарков
# Hardware Model Checking Competition (HWMCC, формат AIGER .aig/.aag).
#
# ВНИМАНИЕ, честно: как и в download_iscas.sh, составитель этого скелета
# не может подтвердить прямо сейчас рабочий постоянный URL конкретного
# архива HWMCC (соревнование проводится по годам, архивы годами расходятся
# по разным местам). Наиболее вероятная отправная точка — сайт формата
# AIGER, fmv.jku.at (JKU Linz, где формат и придуман) — начните поиск там
# ("HWMCC benchmarks fmv.jku.at"), а рабочий URL архива конкретного года
# впишите сюда через BASE_URL, не полагаясь на угаданную ссылку.
#
# Ожидаемый формат: скрипт скачивает "${BASE_URL}/<name>.aig" для каждого
# имени, перечисленного в NAMES_FILE (по умолчанию — просто список,
# который вы сами составите после того, как посмотрите оглавление архива;
# HWMCC-наборы слишком велики и разнородны по составу от года к году,
# чтобы жёстко зашивать конкретные имена в скрипт).
set -euo pipefail

DEST="${1:-benchmarks/data/hwmcc}"
BASE_URL="${2:-${HWMCC_BASE_URL:-}}"
NAMES_FILE="${HWMCC_NAMES_FILE:-benchmarks/scripts/hwmcc_names.txt}"

if [[ -z "$BASE_URL" ]]; then
    echo "download_hwmcc.sh: BASE_URL не задан — см. предупреждение в шапке скрипта." >&2
    echo "Использование: $0 [dest_dir] <base_url>  (или переменная окружения HWMCC_BASE_URL)" >&2
    exit 1
fi
if [[ ! -f "$NAMES_FILE" ]]; then
    echo "download_hwmcc.sh: не найден $NAMES_FILE — создайте его (по одному имени схемы" >&2
    echo "на строку, без расширения .aig), сверившись с оглавлением выбранного архива." >&2
    exit 1
fi

mkdir -p "$DEST"

while IFS= read -r name; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    url="${BASE_URL}/${name}.aig"
    out="${DEST}/${name}.aig"
    if curl -fsSL -o "$out" "$url"; then
        echo "  ok: $name"
    else
        echo "  ПРОПУЩЕНО (не скачалось): $name" >&2
        rm -f "$out"
    fi
done < "$NAMES_FILE"

echo "Готово (то, что скачалось) в $DEST/*.aig"
