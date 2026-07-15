#!/usr/bin/env bash
# profiling/perf_report.sh <бинарь> [аргументы...] — fallback-профилирование
# без Tracy GUI (см. profiling/README.md): perf stat + опционально
# perf record/flamegraph, если доступны в контейнере.
#
# Пример: profiling/perf_report.sh build/test_aig --section aig_to_bdd
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Использование: $0 <бинарь> [аргументы...]" >&2
    exit 1
fi

BINARY="$1"
shift

if ! command -v perf >/dev/null 2>&1; then
    echo "perf не найден в PATH. В .devcontainer (см. coder-deploy/workspace-image/Dockerfile)" >&2
    echo "linux-tools/perf не ставится по умолчанию — добавьте пакет linux-tools-generic" >&2
    echo "(или соответствующий версии ядра хоста пакет) в Dockerfile, если нужен этот путь." >&2
    exit 1
fi

OUT_DIR="profiling/out"
mkdir -p "$OUT_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
BASENAME="$(basename "$BINARY")_${STAMP}"

echo "== perf stat =="
perf stat -e task-clock,cycles,cache-misses,cache-references,context-switches \
    -- "$BINARY" "$@" 2>&1 | tee "$OUT_DIR/${BASENAME}_stat.txt"

if command -v flamegraph.pl >/dev/null 2>&1 || [[ -x "${FLAMEGRAPH_DIR:-}/flamegraph.pl" ]]; then
    echo
    echo "== perf record + flamegraph =="
    perf record -F 999 -g -o "$OUT_DIR/${BASENAME}.perf.data" -- "$BINARY" "$@"
    perf script -i "$OUT_DIR/${BASENAME}.perf.data" > "$OUT_DIR/${BASENAME}.perf.script"

    FLAMEGRAPH_PL="flamegraph.pl"
    STACKCOLLAPSE_PL="stackcollapse-perf.pl"
    if [[ -n "${FLAMEGRAPH_DIR:-}" ]]; then
        FLAMEGRAPH_PL="${FLAMEGRAPH_DIR}/flamegraph.pl"
        STACKCOLLAPSE_PL="${FLAMEGRAPH_DIR}/stackcollapse-perf.pl"
    fi

    if command -v "$STACKCOLLAPSE_PL" >/dev/null 2>&1 || [[ -x "$STACKCOLLAPSE_PL" ]]; then
        "$STACKCOLLAPSE_PL" "$OUT_DIR/${BASENAME}.perf.script" > "$OUT_DIR/${BASENAME}.folded"
        "$FLAMEGRAPH_PL" "$OUT_DIR/${BASENAME}.folded" > "$OUT_DIR/${BASENAME}.svg"
        echo "flame graph: $OUT_DIR/${BASENAME}.svg"
    else
        echo "stackcollapse-perf.pl не найден — оставляю $OUT_DIR/${BASENAME}.perf.script как есть." >&2
        echo "Возьмите FlameGraph (github.com/brendangregg/FlameGraph) и укажите FLAMEGRAPH_DIR." >&2
    fi
else
    echo
    echo "flamegraph.pl не найден — пропускаю perf record/flame graph, только perf stat." >&2
    echo "Возьмите FlameGraph (github.com/brendangregg/FlameGraph) и укажите FLAMEGRAPH_DIR," >&2
    echo "если нужен flame graph." >&2
fi

echo
echo "Готово: результаты в $OUT_DIR/${BASENAME}_*"
