#!/usr/bin/env bash
# scripts/install-deps.sh — сборка нетривиальных C++-зависимостей
# bmm-translib из исходников (CUDD, Sylvan, mockturtle-заголовки, m4ri,
# BRiAl, kissat, CaDiCaL, Google OR-Tools) в самодостаточный префикс.
#
# ДВА РЕЖИМА ИСПОЛЬЗОВАНИЯ — файл один и тот же, менять ничего не нужно:
#   1. CI (.github/workflows/ci.yml) — вызывается на чистом GitHub-hosted
#      раннере, результат кэшируется между прогонами.
#   2. Любая другая машина БЕЗ Docker-образа genetica-boolean-lib — это и
#      есть практический ответ на "портируемость" (см. честную оговорку в
#      README.md §4б/cmake/bmm-translibConfig.cmake.in про то, что
#      экспортируемый CMake-пакет не релокейтабл: полного vcpkg/Conan-порта
#      для Sylvan/BRiAl/mockturtle нет и не планируется — но ЭТОТ скрипт
#      воспроизводимо строит тот же layout зависимостей на любой Ubuntu
#      24.04-совместимой машине, из исходников, без Docker вообще).
#      Запуск:
#        BMM_DEPS_PREFIX=$HOME/bmm-deps ./scripts/install-deps.sh
#        cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/bmm-deps \
#              -DMOCKTURTLE_ROOT=$HOME/bmm-deps/mockturtle
#      Префикс по умолчанию — /opt/bmm-deps (см. PREFIX ниже), это удобно
#      для CI/root-контейнеров, но требует sudo на большинстве обычных
#      Linux-машин (root владеет /opt) — для локального запуска ВНЕ
#      CI/devcontainer переопределите BMM_DEPS_PREFIX на что-то в своём
#      $HOME, как в примере выше, чтобы не нужен был root вообще.
#
# Источник истины по версиям/тегам/коммитам — .devcontainer/Dockerfile (блоки
# CUDD/Sylvan/mockturtle/kissat/CaDiCaL/m4ri/BRiAl/OR-Tools). Версии здесь
# ПРОДУБЛИРОВАНЫ, не переиспользованы напрямую из Dockerfile (тот собирает
# полный интерактивный workspace-образ с CUDA/conda/Sage/PyTorch/code-server,
# ничего из этого CI/локальной сборке не нужно — незачем гонять docker build
# только чтобы прочитать оттуда 7 строк с версиями). Если меняете версию в
# Dockerfile — поменяйте и здесь, и наоборот (тот же принцип, что уже
# применяется к common.hpp/CONVENTIONS.md в этом проекте).
#
# Отличия от Dockerfile, все — намеренные оптимизации для сборки "с нуля" вне
# интерактивного workspace, не имеющие отношения к самим зависимостям:
#   - ставим в отдельный (настраиваемый) префикс, не /usr/local (проще
#     кэшировать в CI, не трогая то, что раннер уже держит в /usr/local; вне
#     CI — не требует root при выборе префикса в $HOME, см. выше);
#   - без ldconfig (нестандартный префикс и так не в ld.so-путях по
#     умолчанию; LD_LIBRARY_PATH нужно выставить самостоятельно — см. пример
#     выше и .github/workflows/ci.yml за тем, как это делает CI);
#   - mockturtle: только `cmake configure` (может генерировать служебные
#     заголовки), без `make` — bmm-translib использует mockturtle исключительно
#     как header-only include-путь (см. комментарий в корневом
#     CMakeLists.txt: "header-only, но НЕ ставятся через make install... "),
#     собственные объектники/примеры mockturtle никуда не линкуются;
#   - OR-Tools собирается с -j2, не -j8/nproc: тот же риск OOM, что уже
#     задокументирован в Dockerfile для сборки protobuf/abseil из исходников
#     (-DBUILD_DEPS=ON) — на менее мощной машине (в т.ч. типичный CI-раннер,
#     4 vCPU/16 ГБ) запас по памяти меньше, чем на выделенной машине из
#     Dockerfile-истории; если у вас мощная локальная машина — можно смело
#     поднять JOBS_OR_TOOLS ниже вручную.
#
# Не идемпотентен по своей природе (`git clone` в существующую директорию
# упадёт) — рассчитан на запуск на чистом/пустом префиксе (в CI это
# гарантируется cache-miss veтвлением в workflow; локально — просто не
# запускайте дважды на одну и ту же директорию, или удалите её перед
# повторным запуском).

set -euo pipefail

PREFIX="${BMM_DEPS_PREFIX:-/opt/bmm-deps}"
JOBS="$(nproc)"
# Отдельная переменная для OR-Tools (не JOBS) — см. предупреждение выше про
# OOM при -DBUILD_DEPS=ON на слабой машине; переопределяйте явно
# (BMM_DEPS_JOBS_OR_TOOLS=8 ./scripts/install-deps.sh), если знаете, что
# памяти достаточно, вместо того чтобы менять число в файле.
JOBS_OR_TOOLS="${BMM_DEPS_JOBS_OR_TOOLS:-2}"

mkdir -p "$PREFIX"/{include,lib,bin,src}
cd "$PREFIX/src"

echo "::group::CUDD 3.0.0"
git clone --depth 1 --branch cudd-3.0.0 https://github.com/ivmai/cudd.git cudd
cd cudd
touch aclocal.m4 && sleep 1 && touch configure config.h.in && sleep 1 && touch Makefile.in
./configure --prefix="$PREFIX" --enable-shared --enable-dddmp --enable-obj \
    CFLAGS="-fPIC -O3" CXXFLAGS="-fPIC -O3"
touch Makefile
make -j"$JOBS"
make install
cd "$PREFIX/src"
echo "::endgroup::"

echo "::group::Sylvan v1.10.0"
git clone --depth 1 --branch v1.10.0 https://github.com/trolando/sylvan.git sylvan
cd sylvan
mkdir build && cd build
# -DLACE_NATIVE_OPT=OFF — КРИТИЧНО для CI, не для .devcontainer (там сборка
# и запуск на одной и той же машине, риска нет). Sylvan тянет Lace через
# FetchContent (github.com/trolando/lace v1.6.0); у Lace
# LACE_NATIVE_OPT=ON по умолчанию (-march=native). На GitHub-hosted раннере
# кэш $PREFIX собирается на одной физической VM, а восстанавливается позже
# на ДРУГОЙ (разный набор физических хостов в пуле) — бинарник, слинкованный
# с -march=native первой машины, падает с SIGILL ("Illegal instruction") на
# второй, если её CPU не поддерживает те же инструкции. Найдено живым
# прогоном CI (2026-08-12): все тестовые бинари падали в CatchAddTests
# (discover_tests) сразу при старте — bmm_core линкует Sylvan+Lace
# безусловно, поэтому падало абсолютно всё.
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DLACE_NATIVE_OPT=OFF ..
make -j"$JOBS"
make install
cd "$PREFIX/src"
echo "::endgroup::"

echo "::group::mockturtle @ 25beb0e (headers only, см. комментарий выше)"
mkdir -p "$PREFIX/mockturtle" && cd "$PREFIX/mockturtle"
git init -q
git remote add origin https://github.com/lsils/mockturtle.git
git fetch --depth 1 origin 25beb0e294e4613bb9fe62319b91d9f2ab764e88
git checkout -q FETCH_HEAD
git submodule update --init --recursive --depth 1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTURTLE_BUILD_EXAMPLES=OFF
cd "$PREFIX/src"
echo "::endgroup::"

echo "::group::kissat + CaDiCaL (бинарники для verify/sat_encoding)"
git clone --depth 1 https://github.com/arminbiere/kissat.git kissat
( cd kissat && ./configure && make -j"$JOBS" && cp build/kissat "$PREFIX/bin/" )

git clone --depth 1 https://github.com/arminbiere/cadical.git cadical
( cd cadical && ./configure && make -j"$JOBS" && cp build/cadical build/mobical "$PREFIX/bin/" )
echo "::endgroup::"

echo "::group::m4ri @ ce0fa7a (BRiAl требует >= 20250128, apt-пакет Ubuntu 24.04 старее)"
mkdir -p m4ri && cd m4ri
git init -q
git remote add origin https://github.com/malb/m4ri.git
git fetch --depth 1 origin ce0fa7a694c44f0bdb90aaa6d92c00fe0df1a2b2
git checkout -q FETCH_HEAD
autoreconf --install
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    ./configure --prefix="$PREFIX" --enable-shared CFLAGS="-fPIC -O3"
make -j"$JOBS"
make install
cd "$PREFIX/src"
echo "::endgroup::"

echo "::group::BRiAl 1.2.15"
git clone --depth 1 --branch 1.2.15 https://github.com/BRiAl/BRiAl.git brial
cd brial
[ -x ./bootstrap.sh ] && ./bootstrap.sh || true
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    ./configure --prefix="$PREFIX" --enable-shared CFLAGS="-fPIC -O3" CXXFLAGS="-fPIC -O3"
make -j"$JOBS"
make install
cd "$PREFIX/src"
echo "::endgroup::"

echo "::group::Google OR-Tools v9.11 (JOBS_OR_TOOLS=$JOBS_OR_TOOLS — см. предупреждение об OOM в шапке файла)"
git clone --depth 1 --branch v9.11 https://github.com/google/or-tools.git or-tools
cd or-tools
cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -DBUILD_DEPS=ON \
      -DBUILD_EXAMPLES=OFF -DBUILD_SAMPLES=OFF -DBUILD_TESTING=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build build --config Release -j"$JOBS_OR_TOOLS"
cmake --install build
cd "$PREFIX/src"
echo "::endgroup::"

rm -rf "$PREFIX/src"
echo "Все зависимости установлены в $PREFIX"
