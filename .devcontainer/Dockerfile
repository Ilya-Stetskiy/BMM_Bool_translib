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
# ---------------------------------------------------------------------------
RUN git clone --depth 1 https://github.com/ivmai/cudd.git /opt/src/cudd && \
    cd /opt/src/cudd && \
    touch aclocal.m4 && sleep 1 && touch configure config.h.in && sleep 1 && touch Makefile.in && \
    ./configure --enable-shared --enable-dddmp --enable-obj CFLAGS="-fPIC -O3" CXXFLAGS="-fPIC -O3" && \
    touch Makefile && \
    make -j"$(nproc)" && make install && ldconfig && \
    rm -rf /opt/src/cudd

# ---------------------------------------------------------------------------
# Sylvan (parallel BDD/ZDD, multicore via Lace)
# ---------------------------------------------------------------------------
RUN git clone --depth 1 https://github.com/trolando/sylvan.git /opt/src/sylvan && \
    cd /opt/src/sylvan && mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j"$(nproc)" && make install && ldconfig && \
    rm -rf /opt/src/sylvan

# ---------------------------------------------------------------------------
# mockturtle (AIG) — pulls kitty, lorina, percy etc. as git submodules
# ---------------------------------------------------------------------------
RUN git clone --recurse-submodules --depth 1 --shallow-submodules \
        https://github.com/lsils/mockturtle.git /opt/mockturtle && \
    cd /opt/mockturtle && mkdir build && cd build && \
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
RUN git clone --depth 1 https://github.com/BRiAl/BRiAl.git /opt/src/brial && \
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
    cmake --build build --config Release -j"$(nproc)" && \
    cmake --install build && ldconfig && \
    rm -rf /opt/src/or-tools

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
ENV PATH="/opt/conda/bin:${PATH}"
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
ENV PATH="/opt/conda/bin:${PATH}"
