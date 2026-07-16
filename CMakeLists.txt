cmake_minimum_required(VERSION 3.24)
project(bmm-translib CXX)

# ---------------------------------------------------------------------------
# Общие настройки
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # для clangd/IDE

option(BMM_ENABLE_TRACY "Собрать с активным Tracy-инструментированием (ZoneScoped и т.п.)" ON)

include(FetchContent)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# mockturtle / kitty — header-only, но НЕ ставятся через `make install` в
# .devcontainer/Dockerfile (см. coder-deploy/workspace-image/Dockerfile —
# собирается в /opt/mockturtle, install не делается), поэтому find_package
# им не подходит: указываем include-пути напрямую. lib/*/include
# захватывает все header-only зависимости, которые mockturtle тянет как
# git submodules (kitty, lorina, parallel_hashmap, rang, fmt, ...) —
# конкретный набор зависит от версии mockturtle, поэтому не перечисляем
# руками.
# ---------------------------------------------------------------------------

set(MOCKTURTLE_ROOT "/opt/mockturtle" CACHE PATH "Путь к собранному дереву mockturtle")
if(NOT EXISTS "${MOCKTURTLE_ROOT}/include")
    message(WARNING "MOCKTURTLE_ROOT=${MOCKTURTLE_ROOT} не похож на дерево mockturtle "
                     "(нет include/) — переопределите -DMOCKTURTLE_ROOT=... , если "
                     "используете другой .devcontainer-образ.")
endif()
file(GLOB MOCKTURTLE_LIB_CANDIDATES "${MOCKTURTLE_ROOT}/lib/*")
foreach(item ${MOCKTURTLE_LIB_CANDIDATES})
    if(IS_DIRECTORY ${item})
        list(APPEND MOCKTURTLE_LIB_INCLUDES ${item})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Sylvan — ставится через `make install && ldconfig` (см. Dockerfile).
# Не полагаемся на find_package(Sylvan CONFIG) и угадывание имени
# экспортируемого таргета (в разных версиях Sylvan это могло называться
# по-разному) — ищем заголовок/библиотеку напрямую, предсказуемо.
# ---------------------------------------------------------------------------

find_path(SYLVAN_INCLUDE_DIR NAMES sylvan_obj.hpp PATHS /usr/local/include /usr/include)
find_library(SYLVAN_LIBRARY NAMES sylvan PATHS /usr/local/lib /usr/lib)
if(NOT SYLVAN_INCLUDE_DIR OR NOT SYLVAN_LIBRARY)
    message(FATAL_ERROR "Sylvan не найден (sylvan_obj.hpp/libsylvan). Проверьте, что "
                         ".devcontainer собрал Sylvan (см. "
                         "coder-deploy/workspace-image/Dockerfile) — без него core/common.hpp "
                         "не скомпилируется (тип Bdd). Если заголовки/библиотека стоят в "
                         "нестандартном месте, переопределите -DSYLVAN_INCLUDE_DIR=... и "
                         "-DSYLVAN_LIBRARY=... вручную.")
endif()

# ---------------------------------------------------------------------------
# BRiAl (ANF) — см. core/CONVENTIONS.md п.5. Со второй итерации задания
# .devcontainer/Dockerfile собирает BRiAl ОТДЕЛЬНО как самостоятельную
# системную C++-библиотеку (autotools, install в /usr/local — см. блок
# "BRiAl" в Dockerfile), а не только как приватный бэкенд SageMath — поэтому
# find_path/find_library ниже (пути /usr/local/include, /usr/local/lib уже
# были в списке и раньше, для forward-совместимости) должны находить её "из
# коробки" в актуальном образе. Ищем и по conda-путям тоже — на случай более
# старого образа/другого devcontainer, где системной сборки ещё нет: тогда
# не ошибка, core/common.hpp сам переключится на AnfFallback через
# __has_include(<polybori.h>) на этапе препроцессинга (см. common.hpp).
# ---------------------------------------------------------------------------

find_path(BRIAL_INCLUDE_DIR NAMES polybori.h
    PATHS /opt/conda/include /opt/conda/include/polybori /usr/local/include /usr/include)
find_library(BRIAL_LIBRARY NAMES brial polybori
    PATHS /opt/conda/lib /usr/local/lib /usr/lib)
if(BRIAL_INCLUDE_DIR)
    message(STATUS "BRiAl: заголовки найдены в ${BRIAL_INCLUDE_DIR} -> Anf будет использовать "
                    "BoolePolynomial (BMM_HAVE_BRIAL=1)")
    if(NOT BRIAL_LIBRARY)
        message(WARNING "polybori.h найден, но libbrial/libpolybori — нет: линковка Anf-таргетов, "
                         "скорее всего, упадёт. См. core/CONVENTIONS.md п.5.")
    endif()
else()
    message(STATUS "BRiAl: заголовки не найдены -> Anf будет использовать AnfFallback "
                    "(BMM_HAVE_BRIAL=0, см. core/CONVENTIONS.md п.5)")
endif()

# ---------------------------------------------------------------------------
# TBB (aig/*, anf/*) — НЕ входит в текущий .devcontainer-образ (см.
# coder-deploy/workspace-image/Dockerfile — libtbb-dev не ставится).
# Пробуем системный find_package, при неудаче подтягиваем oneTBB через
# FetchContent (собирается один раз и кешируется в build/, а не при каждом
# `cmake --build`, см. core/CONVENTIONS.md п.6).
# ---------------------------------------------------------------------------

find_package(TBB QUIET)
if(NOT TBB_FOUND)
    message(STATUS "TBB не найден в системе — подтягиваю oneTBB через FetchContent "
                    "(один раз, см. core/CONVENTIONS.md п.6)")
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        oneTBB
        GIT_REPOSITORY https://github.com/oneapi-src/oneTBB.git
        GIT_TAG v2021.13.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(oneTBB)
endif()

# ---------------------------------------------------------------------------
# Google OR-Tools (ILP-солвер) — нужен bdd/bdd_to_thr.* (извлечение весов
# пороговой функции по prime implicants + ограничениям симметрии из вектора
# Чоу, см. doc-комментарий в файле), собирается в .devcontainer/Dockerfile
# из исходников с install-префиксом /usr/local (блок "Google OR-Tools").
# find_package(ortools CONFIG REQUIRED) и таргет ortools::ortools — текущий
# документированный способ подключения C++ OR-Tools (см. cmake/README.md в
# google/or-tools, ветка stable); ЕСЛИ он поменялся между релизами —
# сверьтесь с актуальной документацией и поправьте здесь (см. предупреждение
# в самом Dockerfile рядом со сборкой OR-Tools).
# ---------------------------------------------------------------------------

find_package(ortools CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# OpenMP (thr/*, *_to_tt/tt_to_*) — часть build-essential/gcc (libgomp),
# должен быть в любом .devcontainer-образе с gcc/clang из Dockerfile.
# ---------------------------------------------------------------------------

find_package(OpenMP REQUIRED)

# ---------------------------------------------------------------------------
# Catch2 (тесты) — FetchContent, header-only порог входа минимален для
# начинающих; см. обоснование выбора в README.md.
# ---------------------------------------------------------------------------

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2  # если тег устарел на момент сборки — обновите на актуальный релиз Catch2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

# ---------------------------------------------------------------------------
# Tracy Profiler — см. profiling/README.md. ВНИМАНИЕ: в исходном
# инфраструктурном промте репозиторий указан как github.com/wolfai/tracy —
# это не существующий адрес; реальный upstream Tracy Profiler находится на
# github.com/wolfpld/tracy (автор Bartosz Taudul, ник wolfpld). Используем
# правильный URL здесь и в profiling/README.md.
# ---------------------------------------------------------------------------

set(TRACY_ENABLE ${BMM_ENABLE_TRACY} CACHE BOOL "" FORCE)
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.11.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tracy)

# ---------------------------------------------------------------------------
# bmm_core — INTERFACE: core/common.hpp header-only, но собирает все
# include-пути и линковки внешних зависимостей в одном месте.
# ---------------------------------------------------------------------------

add_library(bmm_core INTERFACE)
target_include_directories(bmm_core INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MOCKTURTLE_ROOT}/include"
    ${MOCKTURTLE_LIB_INCLUDES}
)
target_compile_features(bmm_core INTERFACE cxx_std_23)

target_include_directories(bmm_core INTERFACE "${SYLVAN_INCLUDE_DIR}")
target_link_libraries(bmm_core INTERFACE "${SYLVAN_LIBRARY}")

if(BRIAL_INCLUDE_DIR)
    target_include_directories(bmm_core INTERFACE "${BRIAL_INCLUDE_DIR}")
    if(BRIAL_LIBRARY)
        target_link_libraries(bmm_core INTERFACE "${BRIAL_LIBRARY}")
    endif()
endif()

target_link_libraries(bmm_core INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# bmm_verify — verify/ полностью реализован, собирается один раз и
# линкуется во все test_<format> таргеты.
# ---------------------------------------------------------------------------

add_library(bmm_verify STATIC
    verify/ground_truth/ground_truth.cpp
    verify/metamorphic/metamorphic.cpp
    verify/sat_encoding/sat_encoding.cpp
    verify/reference_builders.cpp
)
target_link_libraries(bmm_verify PUBLIC bmm_core)

# ---------------------------------------------------------------------------
# bmm_config_dump — крошечная утилита, печатающая, какие условные ветки
# core/common.hpp реально активны в этой сборке (Result<T> backend, ANF
# backend). Единственный источник истины для generate_status.sh — не
# дублируем детектирование на уровне CMake.
# ---------------------------------------------------------------------------

add_executable(bmm_config_dump verify/config_dump.cpp)
target_link_libraries(bmm_config_dump PRIVATE bmm_core)

# ---------------------------------------------------------------------------
# bmm_aig / bmm_bdd / bmm_anf / bmm_thr — по 5 функций трансляции каждая
# (сейчас — стабы, throw/return NotImplemented, см. core/CONVENTIONS.md п.2).
# ---------------------------------------------------------------------------

function(bmm_add_format_library target_name)
    add_library(${target_name} STATIC ${ARGN})
    target_link_libraries(${target_name} PUBLIC bmm_core PRIVATE TracyClient)
endfunction()

bmm_add_format_library(bmm_aig
    aig/tt_to_aig.cpp
    aig/aig_to_bdd.cpp
    aig/aig_to_anf.cpp
    aig/aig_to_thr.cpp
    aig/aig_to_tt.cpp
)
target_link_libraries(bmm_aig PRIVATE TBB::tbb ortools::ortools)  # aig_to_thr — LP/ILP (Muroga), см. aig_to_thr.hpp

bmm_add_format_library(bmm_bdd
    bdd/tt_to_bdd.cpp
    bdd/bdd_to_aig.cpp
    bdd/bdd_to_anf.cpp
    bdd/bdd_to_thr.cpp
    bdd/bdd_to_tt.cpp
)
# bdd/* намеренно не линкует TBB/OpenMP — только Sylvan/Lace, см.
# core/CONVENTIONS.md п.6. ortools::ortools — не нарушение этого правила:
# OR-Tools не трогает BDD-узлы и не соревнуется с Lace за обход графа, это
# отдельный самодостаточный ILP-солвер для bdd_to_thr (шаг 4 его пайплайна,
# см. doc-комментарий в bdd_to_thr.hpp) — вызывается ПОСЛЕ того, как из BDD
# уже извлечены prime implicants обычным Sylvan-обходом.
target_link_libraries(bmm_bdd PRIVATE ortools::ortools)

bmm_add_format_library(bmm_anf
    anf/tt_to_anf.cpp
    anf/anf_to_aig.cpp
    anf/anf_to_bdd.cpp
    anf/anf_to_thr.cpp
    anf/anf_to_tt.cpp
)
target_link_libraries(bmm_anf PRIVATE TBB::tbb OpenMP::OpenMP_CXX ortools::ortools)  # anf_to_thr — LP/ILP; см. заметку-исключение tt_to_anf

bmm_add_format_library(bmm_thr
    thr/tt_to_thr.cpp
    thr/thr_to_aig.cpp
    thr/thr_to_bdd.cpp
    thr/thr_to_anf.cpp
    thr/thr_to_tt.cpp
)
target_link_libraries(bmm_thr PRIVATE OpenMP::OpenMP_CXX TBB::tbb ortools::ortools)  # thr_to_aig/thr_to_anf строят граф -> TBB; tt_to_thr — LP/ILP

# ---------------------------------------------------------------------------
# test_aig / test_bdd / test_anf / test_thr
# ---------------------------------------------------------------------------

function(bmm_add_test_executable target_name format_lib source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${format_lib} bmm_verify bmm_core
                                                  Catch2::Catch2WithMain)
endfunction()

bmm_add_test_executable(test_aig bmm_aig aig/test_aig.cpp)
bmm_add_test_executable(test_bdd bmm_bdd bdd/test_bdd.cpp)
bmm_add_test_executable(test_anf bmm_anf anf/test_anf.cpp)
bmm_add_test_executable(test_thr bmm_thr thr/test_thr.cpp)

# test_aig/test_anf сами используют oneapi::tbb::global_control в секциях
# "*_tbb_scaling" (см. benchmarks/tbb_scaling.hpp, добавлено во второй
# итерации задания) — bmm_aig/bmm_anf линкуют TBB::tbb как PRIVATE, это не
# распространяется транзитивно на test_aig/test_anf, поэтому линкуем явно.
target_link_libraries(test_aig PRIVATE TBB::tbb)
target_link_libraries(test_anf PRIVATE TBB::tbb)

include(CTest)
include(Catch)
catch_discover_tests(test_aig)
catch_discover_tests(test_bdd)
catch_discover_tests(test_anf)
catch_discover_tests(test_thr)

# ---------------------------------------------------------------------------
# status — прогоняет все 4 тестовых бинаря + bmm_config_dump, генерирует
# STATUS.md в корне репозитория. Не считается частью `ctest` (см.
# .github/workflows/ci.yml — CI вызывает его отдельным шагом).
# ---------------------------------------------------------------------------

add_custom_target(status
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/generate_status.sh" "${CMAKE_BINARY_DIR}"
    DEPENDS test_aig test_bdd test_anf test_thr bmm_config_dump
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Генерирую STATUS.md"
    VERBATIM
)
