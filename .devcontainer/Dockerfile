# Образ workspace для учебного проекта: трансляция между представлениями булевых функций
# (AIG/mockturtle, BDD/CUDD+Sylvan, ANF/BRiAl (системная C++ сборка + отдельно
# внутри SageMath для Python), Thr/Google OR-Tools, SAT/kissat+CaDiCaL)
FROM nvidia/cuda:12.6.3-devel-ubuntu24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ clang clang-format cmake ninja-build git \
        autoconf automake libtool m4 pkg-config \
        libboost-all-dev libgmp-dev libm4ri-dev libm4rie-dev zlib1g-dev libssl-dev \
        python3 python3-pip python3-venv python3-dev \
        curl wget ca-certificates unzip sudo openssh-client vim less htop tmux locales \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash coder && \
    echo "coder ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/coder && \
    chmod 0440 /etc/sudoers.d/coder

# ---------------------------------------------------------------------------
# CUDD 3.0.0 (BDD) — configure script is pre-generated in the repo; touching
# files in dependency order avoids make trying (and failing) to re-run
# aclocal/automake, which aren't installed.
#
# Pinned to tag cudd-3.0.0 (commit f54f533303640afd5dbe47a05ebeabb3066f2a25):
# previously `git clone --depth 1` tracked the upstream default branch tip,
# meaning a rebuild months apart could silently pull a different CUDD
# without a single line of this Dockerfile changing (see bmm-translib
# aig/README.md §6/CONVENTIONS.md for the reproducibility finding this
# fixes). If CUDD needs upgrading later, bump this tag deliberately and
# re-verify bmm-translib's ctest suite, don't drop back to tracking HEAD.
# ---------------------------------------------------------------------------
RUN git clone --depth 1 --branch cudd-3.0.0 https://github.com/ivmai/cudd.git /opt/src/cudd && \
    cd /opt/src/cudd && \
    touch aclocal.m4 && sleep 1 && touch configure config.h.in && sleep 1 && touch Makefile.in && \
    ./configure --enable-shared --enable-dddmp --enable-obj CFLAGS="-fPIC -O3" CXXFLAGS="-fPIC -O3" && \
    touch Makefile && \
    make -j"$(nproc)" && make install && ldconfig && \
    rm -rf /opt/src/cudd

# ---------------------------------------------------------------------------
# Sylvan (parallel BDD/ZDD, multicore via Lace)
#
# Pinned to tag v1.10.0 (commit a38979ce922b34df76fdc82a6ca2ef09116a9a16) —
# same reproducibility reasoning as CUDD above: unpinned `--depth 1` tracked
# whatever the default branch pointed to at build time. A newer v1.11.0 tag
# exists upstream; deliberately NOT jumping to it here without re-verifying
# bmm-translib's ctest suite against it first (Bdd's var_to_level/Lace
# integration is exactly the kind of thing a minor version bump could
# silently change behavior for).
# ---------------------------------------------------------------------------
RUN git clone --depth 1 --branch v1.10.0 https://github.com/trolando/sylvan.git /opt/src/sylvan && \
    cd /opt/src/sylvan && mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j"$(nproc)" && make install && ldconfig && \
    rm -rf /opt/src/sylvan

# ---------------------------------------------------------------------------
# mockturtle (AIG) — pulls kitty, lorina, percy etc. as git submodules
#
# Pinned to commit 25beb0e294e4613bb9fe62319b91d9f2ab764e88 (2026-06-30) —
# mockturtle doesn't tag releases, so unlike CUDD/Sylvan/BRiAl above there's
# no tag to pin to; pin the exact commit instead (same reproducibility
# reasoning). `git clone --branch <ref>` only accepts branch/tag names for a
# shallow clone, not an arbitrary commit SHA — init+fetch-by-SHA below is
# the standard shallow-clone-at-a-commit idiom (works against GitHub, which
# allows fetching any reachable commit, not just refs).
# ---------------------------------------------------------------------------
RUN mkdir -p /opt/mockturtle && cd /opt/mockturtle && \
    git init -q && \
    git remote add origin https://github.com/lsils/mockturtle.git && \
    git fetch --depth 1 origin 25beb0e294e4613bb9fe62319b91d9f2ab764e88 && \
    git checkout -q FETCH_HEAD && \
    git submodule update --init --recursive --depth 1 && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DMOCKTURTLE_BUILD_EXAMPLES=ON .. && \
    make -j"$(nproc)"

# ---------------------------------------------------------------------------
# kissat / CaDiCaL (SAT solvers, for verification)
# ---------------------------------------------------------------------------
RUN git clone --depth 1 https://github.com/arminbiere/kissat.git /opt/src/kissat && \
    cd /opt/src/kissat && ./configure && make -j"$(nproc)" && \
    cp build/kissat /usr/local/bin/ && rm -rf /opt/src/kissat

RUN git clone --depth 1 https://github.com/arminbiere/cadical.git /opt/src/cadical && \
    cd /opt/src/cadical && ./configure && make -j"$(nproc)" && \
    cp build/cadical build/mobical /usr/local/bin/ && rm -rf /opt/src/cadical

# ---------------------------------------------------------------------------
# m4ri (плотная линейная алгебра над GF(2)) из исходников — apt-пакет
# libm4ri-dev в Ubuntu 24.04 даёт версию 20200125, а BRiAl ниже требует
# m4ri >= 20250128 (проверено configure BRiAl: "Package 'm4ri' has version
# '20200125', required version is '>= 20250128'"). Собираем из исходников
# вместо системного, ставим в /usr/local (pkg-config предпочитает его
# apt-версии).
#
# Зафиксировано на коммите ce0fa7a694c44f0bdb90aaa6d92c00fe0df1a2b2
# (AC_INIT([m4ri],[20251207]) — проверено напрямую в configure.ac этого
# коммита, версия проходит требование BRiAl >= 20250128). У m4ri нет
# тега новее release-20240729 (который САМ старше требуемой версии — вот
# почему тег не годится и нужен именно коммит с default-ветки). Раньше
# здесь стоял непинченный `git clone --depth 1` на HEAD ("собираем его
# вместо системного" без фиксации, само по себе воспроизводимость не
# чинило — просто переносило проблему с "какая версия m4ri" на "какой
# именно HEAD был на момент сборки образа"). git init+fetch-by-SHA — тот
# же приём, что и у mockturtle выше, по той же причине (нет тега на нужный
# коммит, --branch для shallow-clone тегов/веток не принимает SHA).
# ---------------------------------------------------------------------------
RUN mkdir -p /opt/src/m4ri && cd /opt/src/m4ri && \
    git init -q && \
    git remote add origin https://github.com/malb/m4ri.git && \
    git fetch --depth 1 origin ce0fa7a694c44f0bdb90aaa6d92c00fe0df1a2b2 && \
    git checkout -q FETCH_HEAD && \
    autoreconf --install && \
    ./configure --enable-shared CFLAGS="-fPIC -O3" && \
    make -j"$(nproc)" && make install && ldconfig && \
    rm -rf /opt/src/m4ri

# ---------------------------------------------------------------------------
# BRiAl (ANF/полином Жегалкина) — самостоятельная C++-библиотека (собственное
# кольцо полиномов над GF(2) через ZDD), собирается и ставится ЗДЕСЬ как
# системная зависимость (заголовки в /usr/local/include, libbrial в
# /usr/local/lib), а не только как приватный бэкенд SageMath ниже. Проекту
# bmm-translib (core/common.hpp) нужны настоящие polybori.h + libbrial,
# видимые find_path/find_library из чистого C++/CMake — Sage ниже тянет
# BRiAl тоже, но как внутреннюю зависимость своего conda-окружения без
# гарантии публичных C++-заголовков, этого недостаточно (см.
# core/CONVENTIONS.md в bmm-translib для истории вопроса). Зависимости
# (boost, gmp, m4ri) уже стоят в первом apt-get выше.
#
# Пинчено на тег 1.2.15 (коммит c89251ea555e4853befff9a11db99fd257f91682).
# Именно требование ЭТОГО релиза (не только более старого HEAD) к m4ri
# (>= 20250128, см. configure.ac тега) — причина, по которой пришлось
# собирать m4ri из исходников блоком выше вместо системного libm4ri-dev.
RUN git clone --depth 1 --branch 1.2.15 https://github.com/BRiAl/BRiAl.git /opt/src/brial && \
    cd /opt/src/brial && \
    ( [ -x ./bootstrap.sh ] && ./bootstrap.sh || true ) && \
    ./configure --enable-shared CFLAGS="-fPIC -O3" CXXFLAGS="-fPIC -O3" && \
    make -j"$(nproc)" && make install && ldconfig && \
    rm -rf /opt/src/brial

# ---------------------------------------------------------------------------
# Google OR-Tools (ILP-солвер, нужен bdd_to_thr в bmm-translib — извлечение
# весов пороговой функции по prime implicants + ограничениям симметрии из
# вектора Чоу). Собираем из исходников через собственный CMake OR-Tools с
# -DBUILD_DEPS=ON (тянет свои версии protobuf/abseil/etc. сам, не полагаемся
# на системные) — это официально документированный кросс-платформенный путь
# сборки (см. cmake/README.md в репозитории google/or-tools, ветка stable),
# менее хрупкий, чем угадывать имя/наличие готового бинарного архива под
# конкретный тег Ubuntu на момент сборки образа.
# ВНИМАНИЕ: способ подключения C++ OR-Tools периодically меняется между
# релизами — если этот блок перестал собираться, сверьтесь с актуальным
# README в google/or-tools (ветка stable) и поправьте GIT_TAG/шаги ниже, а
# не просто добавляйте --allow-unauthenticated/аналоги поверх.
RUN git clone --depth 1 --branch v9.11 https://github.com/google/or-tools.git /opt/src/or-tools && \
    cd /opt/src/or-tools && \
    cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -DBUILD_DEPS=ON \
          -DBUILD_EXAMPLES=OFF -DBUILD_SAMPLES=OFF -DBUILD_TESTING=OFF \
          -DCMAKE_INSTALL_PREFIX=/usr/local && \
    cmake --build build --config Release -j8 && \
    cmake --install build && ldconfig && \
    rm -rf /opt/src/or-tools
# -j8, not -j"$(nproc)": this step builds protobuf/abseil from source
# (-DBUILD_DEPS=ON) and was found (bmm-translib aig/README.md §6) to crash
# the Docker daemon connection under full-core parallelism on a
# memory-constrained VM (looked like OOM) — -j4 was the empirically
# verified fix there; -j8 here is a deliberately conservative middle
# ground on this shared multi-tenant host (also running live student
# workspaces and Genetica staging), not a re-measurement on this exact box.

# ---------------------------------------------------------------------------
# Miniforge (conda/mamba) + SageMath (ships BRiAl/PolyBoRi as sage.rings.polynomial.pbori,
# backed by libbrial 1.2.15 — no separate "PolyBoRi" package exists for noble/conda-forge
# main channel main package), SymPy, NumPy, pytest.
# NOTE: do not pin a bare `brial` package alongside `sage` — conda-forge's standalone
# `brial` build lags behind the `libbrial` version sagelib needs and the solver conflicts.
# BooleanPolynomialRing() from within Sage is the supported way to use BRiAl/ZDD from
# Python here. This is independent of the SYSTEM libbrial built from source above
# (/usr/local, for C++ bmm-translib) — two separate installs under separate prefixes
# (/opt/conda vs /usr/local), no conflict, do not try to unify them.
# ---------------------------------------------------------------------------
RUN curl -fsSL -o /tmp/miniforge.sh \
        https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh && \
    bash /tmp/miniforge.sh -b -p /opt/conda && rm /tmp/miniforge.sh
# Appended, not prepended: conda's own gcc/g++ (pulled in as a sage build
# dependency) must not shadow the system compiler on PATH — see the note by
# the final ENV PATH line below for why.
ENV PATH="${PATH}:/opt/conda/bin"
RUN mamba install -y -c conda-forge sage sympy pytest && mamba clean -afy

# ---------------------------------------------------------------------------
# PyTorch with CUDA support (for an eventual GNN module) — system python3, kept
# separate from the sage/conda environment to avoid solver conflicts.
# ---------------------------------------------------------------------------
RUN python3 -m pip install --break-system-packages --no-cache-dir numpy pytest && \
    python3 -m pip install --break-system-packages --no-cache-dir torch --index-url https://download.pytorch.org/whl/cu126

# ---------------------------------------------------------------------------
# code-server (VS Code in the browser) — Coder's agent starts it at runtime
# ---------------------------------------------------------------------------
RUN curl -fsSL https://code-server.dev/install.sh | sh

# VBF (vectorial Boolean functions): no single canonical standalone package identified;
# treated as SageMath-based research tooling to be added under the student project
# repository once it exists (this path is on the persistent per-workspace home volume).
RUN chown -R coder:coder /home/coder

USER coder
WORKDIR /home/coder

# git has no credential helper configured by default, so every `git push`
# prompts for a token again -- even within the same workspace, in a fresh
# terminal session, with nothing recreated. `store` caches into
# ~/.git-credentials (plain file under /home/coder, on the persistent
# per-workspace volume, survives stop/start) so students enter their PAT
# once per workspace instead of on every push.
RUN git config --global credential.helper store
# Appended, not prepended: conda ships its own gcc/g++ (gxx_linux-64, pulled
# in as a sage build dependency) which shadows the system compiler if placed
# first on PATH. Conda's gcc uses an isolated sysroot and can't see system
# headers like /usr/include/boost, breaking any real C++ project build done
# from an interactive shell. sage/mamba still resolve fine appended (nothing
# in /usr/bin shadows them).
ENV PATH="${PATH}:/opt/conda/bin"
