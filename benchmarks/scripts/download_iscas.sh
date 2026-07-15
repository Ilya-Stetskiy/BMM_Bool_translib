#!/usr/bin/env bash
# download_iscas.sh [dest_dir] [base_url] — скачивает ISCAS'85 benchmark
# suite (классические комбинационные схемы, .bench формат).
#
# ВНИМАНИЕ, честно: у ISCAS'85/89 нет единого официального репозитория с
# постоянным URL — файлы разошлись по нескольким университетским зеркалам
# за десятилетия, и составитель этого скелета не может подтвердить, какое
# из них живо прямо сейчас (в отличие от EPFL/HWMCC ниже, у которых есть
# явный текущий канонический GitHub-репозиторий — см. download_epfl.sh,
# download_hwmcc.sh). Вместо того чтобы вписать сюда правдоподобный, но
# непроверенный URL, скрипт требует BASE_URL явным аргументом или
# переменной окружения — заполните его сами после того, как найдёте
# рабочее зеркало (поиск "iscas85 bench files download" обычно находит
# актуальное на момент вашей работы), и, если хотите, пришлите PR,
# обновляющий дефолт в этом файле.
#
# Ожидаемый формат BASE_URL: скрипт скачивает "${BASE_URL}/<circuit>.bench"
# для каждой схемы из списка ниже.
set -euo pipefail

DEST="${1:-benchmarks/data/iscas}"
BASE_URL="${2:-${ISCAS_BASE_URL:-}}"

if [[ -z "$BASE_URL" ]]; then
    echo "download_iscas.sh: BASE_URL не задан — см. предупреждение в шапке скрипта." >&2
    echo "Использование: $0 [dest_dir] <base_url>  (или переменная окружения ISCAS_BASE_URL)" >&2
    exit 1
fi

mkdir -p "$DEST"

ISCAS85_CIRCUITS=(c17 c432 c499 c880 c1355 c1908 c2670 c3540 c5315 c6288 c7552)

echo "download_iscas.sh: скачиваю ISCAS'85 (.bench) из $BASE_URL"
for circuit in "${ISCAS85_CIRCUITS[@]}"; do
    url="${BASE_URL}/${circuit}.bench"
    out="${DEST}/${circuit}.bench"
    if curl -fsSL -o "$out" "$url"; then
        echo "  ok: $circuit"
    else
        echo "  ПРОПУЩЕНО (не скачалось): $circuit" >&2
        rm -f "$out"
    fi
done

echo "Готово (то, что скачалось) в $DEST/*.bench. ISCAS'89 (секвенциальные, с DFF) в"
echo "этот список не включены — при необходимости добавьте отдельно."
