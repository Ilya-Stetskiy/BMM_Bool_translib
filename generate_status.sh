#!/usr/bin/env bash
# generate_status.sh [build_dir] — прогоняет test_aig/test_bdd/test_anf/
# test_thr + bmm_config_dump и генерирует STATUS.md в корне репозитория.
# Вызывается через `cmake --build <build_dir> --target status`, не
# предполагает ручной запуск (хотя это тоже работает: ./generate_status.sh build).
#
# Не падает на ненулевом exit-коде тестовых бинарей (FAIL даёт ненулевой
# код через Catch2 REQUIRE) — нам нужны все BMM_STATUS-строки из вывода
# независимо от общего результата прогона.
set -uo pipefail

cd "$(dirname "$0")"
BUILD_DIR="${1:-build}"

declare -A STATUS
declare -A DETAIL
declare -A BENCH_SUMMARY
ORDER=()

collect_from_binary() {
    local binary="$1"
    if [[ ! -x "$BUILD_DIR/$binary" ]]; then
        echo "generate_status.sh: предупреждение — $BUILD_DIR/$binary не найден, пропускаю" >&2
        return
    fi
    local output
    # Прогоняет и обычные тесты, и секции "*_tbb_scaling" (см.
    # benchmarks/tbb_scaling.hpp) — они не отдельный бинарь, а TEST_CASE в
    # том же test_aig/test_anf с тегом [benchmark], Catch2 гоняет их по
    # умолчанию вместе со всеми остальными.
    output="$("$BUILD_DIR/$binary" 2>&1 || true)"
    while IFS= read -r line; do
        if [[ "$line" == BMM_STATUS* ]]; then
            local name status detail
            name=$(awk '{print $2}' <<<"$line")
            status=$(awk '{print $3}' <<<"$line")
            detail=$(cut -d' ' -f4- <<<"$line")
            STATUS["$name"]="$status"
            DETAIL["$name"]="$detail"
            ORDER+=("$name")
        elif [[ "$line" == BMM_BENCH_SUMMARY* ]]; then
            local bname bsummary
            bname=$(awk '{print $2}' <<<"$line")
            bsummary=$(cut -d' ' -f3- <<<"$line")
            BENCH_SUMMARY["$bname"]="$bsummary"
        fi
    done <<<"$output"
}

collect_from_binary test_aig
collect_from_binary test_bdd
collect_from_binary test_anf
collect_from_binary test_thr

RESULT_BACKEND="неизвестно (bmm_config_dump не собран/не найден)"
ANF_BACKEND="неизвестно (bmm_config_dump не собран/не найден)"
if [[ -x "$BUILD_DIR/bmm_config_dump" ]]; then
    while IFS= read -r line; do
        [[ "$line" == BMM_INFO* ]] || continue
        key=$(awk '{print $2}' <<<"$line")
        value=$(awk '{print $3}' <<<"$line")
        case "$key" in
            result_backend) RESULT_BACKEND="$value" ;;
            anf_backend) ANF_BACKEND="$value" ;;
        esac
    done < <("$BUILD_DIR/bmm_config_dump")
fi

status_cell() {
    local func="$1"
    local st="${STATUS[$func]:-?}"
    local icon="⬜"
    case "$st" in
        PASS) icon="✅" ;;
        FAIL) icon="❌" ;;
        SKIP) icon="⬛" ;;
        *) st="нет данных" ;;
    esac
    echo "$icon \`$func\`: $st"
}

# Функции aig/*, anf/*, для которых обязателен TBB-бенчмарк (см.
# core/CONVENTIONS.md п.6 — только те, что ДЕЙСТВИТЕЛЬНО используют TBB по
# правилам приоритета, не все 10 функций в этих двух папках).
TBB_BENCH_FUNCS=(tt_to_aig aig_to_anf aig_to_thr tt_to_anf anf_to_aig anf_to_thr)

# Фиксированный порядок функций внутри каждой папки — см.
# aig/bdd/anf/thr/*.hpp и core/CONVENTIONS.md.
AIG_FUNCS=(tt_to_aig aig_to_bdd aig_to_anf aig_to_thr aig_to_tt)
BDD_FUNCS=(tt_to_bdd bdd_to_aig bdd_to_anf bdd_to_thr bdd_to_tt)
ANF_FUNCS=(tt_to_anf anf_to_aig anf_to_bdd anf_to_thr anf_to_tt)
THR_FUNCS=(tt_to_thr thr_to_aig thr_to_bdd thr_to_anf thr_to_tt)

TOTAL=0
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
for name in "${AIG_FUNCS[@]}" "${BDD_FUNCS[@]}" "${ANF_FUNCS[@]}" "${THR_FUNCS[@]}"; do
    TOTAL=$((TOTAL + 1))
    case "${STATUS[$name]:-}" in
        PASS) PASS_COUNT=$((PASS_COUNT + 1)) ;;
        FAIL) FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
        SKIP) SKIP_COUNT=$((SKIP_COUNT + 1)) ;;
    esac
done

{
    echo "# STATUS.md"
    echo
    echo "Автоматически сгенерировано \`generate_status.sh\` через \`cmake --build build"
    echo "--target status\` — **не редактируйте руками**, правки будут перезаписаны."
    echo
    echo "Итого: $PASS_COUNT PASS / $FAIL_COUNT FAIL / $SKIP_COUNT SKIP из $TOTAL функций."
    echo
    echo "- Result<T> backend: **$RESULT_BACKEND** (см. core/CONVENTIONS.md п.2)"
    echo "- ANF backend: **$ANF_BACKEND** (см. core/CONVENTIONS.md п.5)"
    echo "- sat_encoding: реализован структурный Tseitin-энкодер для AIG и запуск"
    echo "  CaDiCaL/kissat как внешнего процесса (см. verify/sat_encoding/sat_encoding.hpp)."
    echo "  **TODO, не реализовано:** структурные энкодеры для Bdd/Anf/Thr — SAT-путь"
    echo "  верификации сейчас покрывает только функции с To=Aig (4 из 20:"
    echo "  tt_to_aig, bdd_to_aig, anf_to_aig, thr_to_aig)."
    echo "- bdd_to_thr: для этого направления не найдено литературы/алгоритма"
    echo "  (\"тёмная ночь\", требует отдельного исследования — см. bdd/bdd_to_thr.hpp)."
    echo
    echo "| Папка | tt_to_X | X→формат2 | X→формат3 | X→формат4 | X_to_tt |"
    echo "|---|---|---|---|---|---|"
    print_row() {
        local label="$1"; shift
        local cells=()
        for f in "$@"; do cells+=("$(status_cell "$f")"); done
        printf '| %s | %s | %s | %s | %s | %s |\n' "$label" "${cells[@]}"
    }
    print_row "**AIG**" "${AIG_FUNCS[@]}"
    print_row "**BDD**" "${BDD_FUNCS[@]}"
    print_row "**ANF**" "${ANF_FUNCS[@]}"
    print_row "**Thr**" "${THR_FUNCS[@]}"
    echo
    echo "## Параллельность aig/anf (обязательный TBB-бенчмарк)"
    echo
    echo "Для функций, реально использующих TBB (core/CONVENTIONS.md п.6,"
    echo "приоритет правил) — сравнение одного и того же вызова под 1-поточным и"
    echo "полным \`tbb::global_control\` на 3 размерах входа, см."
    echo "\`benchmarks/tbb_scaling.hpp\` и секции \`*_tbb_scaling\` в"
    echo "\`test_aig.cpp\`/\`test_anf.cpp\`."
    echo
    echo "| Функция | Результат |"
    echo "|---|---|"
    for name in "${TBB_BENCH_FUNCS[@]}"; do
        summary="${BENCH_SUMMARY[$name]:-нет данных (бенчмарк не запускался или функция не реализована)}"
        echo "| \`$name\` | $summary |"
    done
    echo
    echo "Столбцы 2–5 в каждой строке — это 5 функций конкретной папки в"
    echo "фиксированном порядке (см. массивы AIG_FUNCS/BDD_FUNCS/ANF_FUNCS/THR_FUNCS в"
    echo "generate_status.sh), а не единая сетка \"откуда→куда\" — центральные 3 у"
    echo "каждой строки ведут в разные форматы (пример: у AIG это BDD/ANF/Thr, у BDD —"
    echo "AIG/ANF/Thr), общий заголовок пришлось бы делать нечестным."
    echo
    if ((${#ORDER[@]} > 0)); then
        echo "## Детали (последний прогон)"
        echo
        for name in "${AIG_FUNCS[@]}" "${BDD_FUNCS[@]}" "${ANF_FUNCS[@]}" "${THR_FUNCS[@]}"; do
            [[ -n "${STATUS[$name]:-}" ]] || continue
            echo "- \`$name\` — ${STATUS[$name]}: ${DETAIL[$name]}"
        done
    fi
} > STATUS.md

echo "generate_status.sh: STATUS.md обновлён ($PASS_COUNT PASS / $FAIL_COUNT FAIL / $SKIP_COUNT SKIP)"

# CI использует exit code этого скрипта, чтобы решить, красным или зелёным
# считать шаг "status" (см. .github/workflows/ci.yml) — FAIL здесь означает
# ошибку в уже РЕАЛИЗОВАННОЙ функции, что и должно валить сборку;
# отсутствие данных (тестовый бинарь не собрался) — тоже ошибка. SKIP —
# не ошибка.
if ((FAIL_COUNT > 0)); then
    exit 1
fi
exit 0
