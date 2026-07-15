#!/usr/bin/env bash
# download_pb.sh [dest_dir] [base_url] — скачивает выбор псевдобулевых
# (Pseudo-Boolean, .opb формат) бенчмарков — полезны как тестовые входы
# для thr/* (пороговые/псевдобулевы ограничения — прямое соответствие
# .opb-констрейнтам вида "sum w_i*x_i >= theta").
#
# ВНИМАНИЕ, честно: как и в download_iscas.sh/download_hwmcc.sh, составитель
# этого скелета не может подтвердить прямо сейчас конкретный рабочий URL
# архива Pseudo-Boolean Competition (соревнование проводится по годам,
# организаторы и хостинг менялись). Начните поиск с "pseudo-boolean
# competition benchmarks" — впишите найденный рабочий URL через BASE_URL.
#
# Ожидаемый формат: скрипт скачивает файлы по списку из NAMES_FILE (по
# одному относительному пути на строку, например "DEC-SMALLINT/foo.opb"),
# т.к. структура каталогов реальных архивов PB-конкурса обычно
# многоуровневая и меняется от года к году — жёстко зашивать её в скрипт
# было бы обманчиво стабильным.
set -euo pipefail

DEST="${1:-benchmarks/data/pb}"
BASE_URL="${2:-${PB_BASE_URL:-}}"
NAMES_FILE="${PB_NAMES_FILE:-benchmarks/scripts/pb_names.txt}"

if [[ -z "$BASE_URL" ]]; then
    echo "download_pb.sh: BASE_URL не задан — см. предупреждение в шапке скрипта." >&2
    echo "Использование: $0 [dest_dir] <base_url>  (или переменная окружения PB_BASE_URL)" >&2
    exit 1
fi
if [[ ! -f "$NAMES_FILE" ]]; then
    echo "download_pb.sh: не найден $NAMES_FILE — создайте его (по одному относительному" >&2
    echo "пути на строку), сверившись со структурой выбранного архива." >&2
    exit 1
fi

mkdir -p "$DEST"

while IFS= read -r rel_path; do
    [[ -z "$rel_path" || "$rel_path" == \#* ]] && continue
    url="${BASE_URL}/${rel_path}"
    out="${DEST}/$(basename "$rel_path")"
    if curl -fsSL -o "$out" "$url"; then
        echo "  ok: $rel_path"
    else
        echo "  ПРОПУЩЕНО (не скачалось): $rel_path" >&2
        rm -f "$out"
    fi
done < "$NAMES_FILE"

echo "Готово (то, что скачалось) в $DEST/*.opb"
