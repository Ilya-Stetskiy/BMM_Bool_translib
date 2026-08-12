#!/usr/bin/env bash
# ci/install-deps.sh — сборка нетривиальных C++-зависимостей bmm-translib из
# исходников для GitHub Actions (CUDD, Sylvan, mockturtle-заголовки, m4ri,
# BRiAl, kissat, CaDiCaL, Google OR-Tools).
#
# Источник истины по версиям/тегам/коммитам — .devcontainer/Dockerfile (блоки
# CUDD/Sylvan/mockturtle/kissat/CaDiCaL/m4ri/BRiAl/OR-Tools). Версии здесь
# ПРОДУБЛИРОВАНЫ, не переиспользованы напрямую из Dockerfile (тот собирает
# полный интерактивный workspace-образ с CUDA/conda/Sage/PyTorch/code-server,
# ничего из этого CI не нужно — незачем гонять docker build в GitHub Actions
# только чтобы прочитать оттуда 7 строк с версиями). Если меняете версию в
# Dockerfile — поменяйте и здесь, и наоборот (тот же принцип, что уже
# применяется к common.hpp/CONVENTIONS.md в этом проекте).
#
# Отличия от Dockerfile, все — намеренные CI-only оптимизации, не имеющие
# отношения к самим зависимостям:
#   - ставим в отдельный префикс /opt/bmm-deps, не /usr/local (см.
#     .github/workflows/ci.yml — так безопаснее кэшировать директорию, не
#     трогая то, что раннер уже держит в /usr/local);
#   - без ldconfig (нестандартный префикс и так не в ld.so-путях по
#     умолчанию; LD_LIBRARY_PATH выставляется в workflow);
#   - mockturtle: только `cmake configure` (может генерировать служебные
#     заголовки), без `make` — bmm-translib использует mockturtle исключительно
#     как header-only include-путь (см. комментарий в корневом
#     CMakeLists.txt: "header-only, но НЕ ставятся через make install... "),
#     собственные объектники/примеры mockturtle никуда не линкуются;
#   - OR-Tools собирается с -j2, не -j8/nproc: тот же риск OOM, что уже
#     задокументирован в Dockerfile для сборки protobuf/abseil из исходников
#     (-DBUILD_DEPS=ON), на стандартном GitHub-hosted раннере (4 vCPU/16 ГБ)
#     запас по памяти меньше, чем на выделенной машине из Dockerfile-истории.
#
# Не идемпотентен по своей природе (`git clone` в существующую директорию
# упадёт) — рассчитан на запуск ровно один раз на чистом раннере, при
# cache-miss (см. workflow: шаг пропускается целиком при cache-hit).

set -euo pipefail

PREFIX="${BMM_DEPS_PREFIX:-/opt/bmm-deps}"
JOBS="$(nproc)"

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

echo "::group::Google OR-Tools v9.11 (-j2 — см. предупреждение об OOM в шапке файла)"
git clone --depth 1 --branch v9.11 https://github.com/google/or-tools.git or-tools
cd or-tools
cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -DBUILD_DEPS=ON \
      -DBUILD_EXAMPLES=OFF -DBUILD_SAMPLES=OFF -DBUILD_TESTING=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build build --config Release -j2
cmake --install build
cd "$PREFIX/src"
echo "::endgroup::"

rm -rf "$PREFIX/src"
echo "Все зависимости установлены в $PREFIX"
