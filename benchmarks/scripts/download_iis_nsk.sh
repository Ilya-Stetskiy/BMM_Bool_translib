#!/usr/bin/env bash
# download_iis_nsk.sh [dest_dir] — скачивает два датасета с
# persons.iis.nsk.su/files/persons/epg/:
#
#   bdd-bench.zip   — 895 готовых BDD (формат DDDMP-2.0, CUDD dddmp) для
#                      классических аппаратных бенчмарков (ISCAS, LGSynth91,
#                      MCNC, IWLS, ITC99, EPFL, Espresso, QUIP, ...),
#                      171 МБ распакованные. Полезно как реальный вход для
#                      bdd_to_aig/bdd_to_anf/bdd_to_tt/bdd_to_thr — если для
#                      той же схемы есть .aig в benchmarks/data/epfl/, можно
#                      сверить aig_to_bdd(эта схема) против готового .bdd.
#
#   100-10k-rnd.zip — 165 случайных ANF (n=100 переменных, 100/1000/10000
#                      мономов на файл, DIMACS ANF текстовый формат: "p anf
#                      N M" + по одной строке на моном, индексы через
#                      пробел, "0" в конце строки) + для каждого — готовые
#                      BDD/FDD/ZDD (тот же DDDMP), 1.4 ГБ распакованные.
#                      Прямое расширение реального ANF-корпуса (см.
#                      thr/bench_real_corpus.cpp) на бОльшие n со
#                      встроенным эталоном.
#
# ФОРМАТ .anf ЧИТАЕТСЯ НАПРЯМУЮ (простой текст, см. выше) — парсер в проект
# пока не написан. ФОРМАТ .bdd/.fdd/.zdd (DDDMP, узлы в бинарной кодировке,
# ".mode B") ТРЕБУЕТ CUDD dddmp (уже собран в .devcontainer/Dockerfile,
# --enable-dddmp, как транзитивная зависимость BRiAl) — либо распарсить
# через libdddmp в CUDD-представление и вручную перестроить в Sylvan
# (bmm::Bdd), либо (проще, но не сделано) написать отдельный текстовый
# парсер DDDMP-заголовка + бинарного узлового блока напрямую в Sylvan.
#
# ВНИМАНИЕ: сжатие в архивах — LZMA (zip method 14), СТАРЫЙ `unzip`
# (Info-ZIP 6.00) его не поддерживает ("need PK compat. v6.3") и молча
# ничего не распаковывает — используем python3 (zipfile, модуль lzma
# входит в стандартную поставку начиная с Python 3.3).
#
# ВАЖНО про место на диске: сжатие LZMA даёт огромный коэффициент на этих
# файлах (текст с длинными повторяющимися последовательностями индексов) —
# распакованный объём в разы больше архива (100-10k-rnd.zip: 77 МБ архив →
# 1.4 ГБ распакованный). Держите это в уме, если решите добавить сюда же
# соседние 100-100k-rnd.zip (550 МБ архив → ~10.2 ГБ распакованный) или
# 100-1000k-anf.zip (2.05 ГБ архив → ~98 ГБ распакованный, только *.anf,
# без bdd/fdd/zdd) — эти два сознательно НЕ включены в этот скрипт.
set -euo pipefail

DEST="${1:-benchmarks/data/iis-nsk}"
BASE_URL="https://persons.iis.nsk.su/files/persons/epg"

mkdir -p "$DEST"

for name in bdd-bench 100-10k-rnd; do
    zip_path="${DEST}/${name}.zip"
    out_dir="${DEST}/${name}"
    if [[ -d "$out_dir" && -n "$(ls -A "$out_dir" 2>/dev/null)" ]]; then
        echo "download_iis_nsk.sh: $out_dir уже существует и не пуст, пропускаю"
        continue
    fi
    echo "download_iis_nsk.sh: скачиваю ${name}.zip"
    curl -fSL -o "$zip_path" "${BASE_URL}/${name}.zip"
    mkdir -p "$out_dir"
    python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$zip_path" "$DEST"
    rm -f "$zip_path"
done

echo "Готово. $DEST/bdd-bench/*.bdd, $DEST/100-10k-rnd/*.{anf,bdd.bdd,fdd.fdd,zdd.zdd}"
