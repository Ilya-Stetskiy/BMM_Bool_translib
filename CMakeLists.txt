cmake_minimum_required(VERSION 3.28)  # EXCLUDE_FROM_ALL в FetchContent_Declare (см. ниже) требует 3.28+
# Версия — только для генерации bmm-translibConfigVersion.cmake (см. install()
# в конце файла); полноценное версионирование (тег/CHANGELOG) — отдельная,
# ещё не сделанная задача.
project(bmm-translib VERSION 0.1.0 LANGUAGES CXX)

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

# Параллелизм Sylvan (Lace) — отдельная статическая библиотека, которую
# libsylvan не включает в себя, линковать нужно явно вместе с ней.
find_library(LACE_LIBRARY NAMES lace PATHS /usr/local/lib /usr/lib)


if(NOT SYLVAN_INCLUDE_DIR OR NOT SYLVAN_LIBRARY OR NOT LACE_LIBRARY)
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

# CUDD (+ встроенный dddmp) — отдельно от BRiAl: BRiAl использует CUDD внутри
# себя, но не экспортирует Dddmp_*-символы наружу через свою библиотеку,
# поэтому для моста DDDMP->Sylvan (verify/diag_dddmp_probe.cpp и далее) нужна
# прямая линковка на libcudd.
find_path(CUDD_INCLUDE_DIR NAMES cudd.h
    PATHS /usr/local/include /usr/include)
find_library(CUDD_LIBRARY NAMES cudd
    PATHS /usr/local/lib /usr/lib)

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
        # БЕЗ EXCLUDE_FROM_ALL — та же причина, что и у tracy ниже: bmm_aig/
        # bmm_anf/bmm_thr линкуют TBB::tbb как PRIVATE, но для статических
        # библиотек потребитель всё равно обязан предоставить TBB при
        # финальной линковке. В ЭТОМ конкретном окружении (единый Docker-
        # образ на весь курс) системного TBB нет вовсе (см. find_package(TBB
        # QUIET) выше) — значит и потребителю неоткуда его взять, кроме как
        # через установленную вместе с bmm-translib копию. Ставим её.
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
    # В отличие от TBB/Tracy ниже — EXCLUDE_FROM_ALL оставлен: Catch2 нужен
    # только для сборки/тестов ЭТОГО репозитория, ни один экспортируемый
    # bmm_<format> не линкует его даже PRIVATE, внешнему потребителю библиотеки
    # он не нужен вообще ни в каком виде — без EXCLUDE_FROM_ALL install()
    # ниже заодно потащил бы его в install-дерево bmm-translib без причины.
    EXCLUDE_FROM_ALL
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
    # БЕЗ EXCLUDE_FROM_ALL, в отличие от Catch2 ниже: bmm_aig/bmm_bdd/
    # bmm_anf/bmm_thr линкуют TracyClient как PRIVATE, но для СТАТИЧЕСКИХ
    # библиотек это всё равно попадает в экспортируемый link-интерфейс
    # (потребитель обязан предоставить Tracy::TracyClient при финальной
    # линковке своего приложения, PRIVATE тут не спасает — работает только
    # для include-путей/compile-definitions, не для самого факта линковки
    # статического архива). У Tracy есть собственный релокейтабл
    # TracyConfig.cmake — устанавливаем её вместе с bmm-translib (см.
    # cmake/bmm-translibConfig.cmake.in: find_dependency(Tracy)),
    # а не заставляем потребителя доставать её самостоятельно.
)
FetchContent_MakeAvailable(tracy)

# ---------------------------------------------------------------------------
# Публичный include-неймспейс bmm/ — core/common.hpp и все 20 функций
# трансляции подключаются везде в дереве (и внутри самого репозитория, и
# из внешнего проекта после `find_package`) как
# `#include <bmm/core/common.hpp>`, `#include <bmm/aig/aig_to_bdd.hpp>` и
# т.д. — совпадает с C++ `namespace bmm`, в котором и так лежит весь код.
#
# Физически core/, aig/, bdd/, anf/, thr/ остаются на прежних местах в
# корне репозитория (не переносим — на них ссылаются CONVENTIONS.md,
# READMEs каждой папки и весь студенческий workflow "открой aig/
# aig_to_bdd.cpp"), поэтому для сборки ВНУТРИ дерева генерируем symlink-
# прослойку в build/ (${BMM_INCLUDE_ROOT}/bmm/<module> -> <module>/) и
# добавляем её в include-пути bmm_core. Для УСТАНОВЛЕННОГО дерева ниже есть
# отдельный `install(DIRECTORY ...)` на реальные .hpp (см. секцию install()
# в конце файла) — та же логика, но не symlink, а физическая копия
# заголовков под <prefix>/include/bmm/.
# ---------------------------------------------------------------------------

set(BMM_INCLUDE_ROOT "${CMAKE_BINARY_DIR}/_bmm_include")
file(MAKE_DIRECTORY "${BMM_INCLUDE_ROOT}/bmm")
foreach(_bmm_module core aig bdd anf thr)
    if(NOT EXISTS "${BMM_INCLUDE_ROOT}/bmm/${_bmm_module}")
        file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/${_bmm_module}"
                          "${BMM_INCLUDE_ROOT}/bmm/${_bmm_module}"
                          SYMBOLIC)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# bmm_core — INTERFACE: core/common.hpp header-only, но собирает все
# include-пути и линковки внешних зависимостей в одном месте.
# ---------------------------------------------------------------------------

add_library(bmm_core INTERFACE)
# CMAKE_CURRENT_SOURCE_DIR/BMM_INCLUDE_ROOT обёрнуты в $<BUILD_INTERFACE:...>
# — оба пути лежат внутри дерева этого проекта (source/build), и
# install(EXPORT ...) ниже ОБЯЗАН это требование соблюсти (иначе ошибка
# конфигурации "contains path ... which is prefixed in the build/source
# directory"): для установленного потребителя эти пути не существуют, вместо
# них подставляется $<INSTALL_INTERFACE:...> из INCLUDES DESTINATION у
# install(TARGETS ...). MOCKTURTLE_ROOT (/opt/mockturtle) в эту проверку не
# попадает — это абсолютный путь ВНЕ дерева проекта (тот же компромисс,
# что описан в cmake/bmm-translibConfig.cmake.in: работает на машине с тем
# же layout зависимостей, не полностью релокейтабл).
target_include_directories(bmm_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${BMM_INCLUDE_ROOT}>
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
target_include_directories(bmm_core INTERFACE "${SYLVAN_INCLUDE_DIR}")
target_link_libraries(bmm_core INTERFACE "${SYLVAN_LIBRARY}" "${LACE_LIBRARY}")
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
    verify/test_main.cpp
)
target_link_libraries(bmm_verify PUBLIC bmm_core Catch2::Catch2)

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
                                                  Catch2::Catch2)
endfunction()

bmm_add_test_executable(test_aig bmm_aig aig/test_aig.cpp)
bmm_add_test_executable(test_bdd bmm_bdd bdd/test_bdd.cpp)
bmm_add_test_executable(test_anf bmm_anf anf/test_anf.cpp)
bmm_add_test_executable(test_thr bmm_thr thr/test_thr.cpp)
add_executable(test_core core/test_core.cpp verify/sat_encoding/test_sat_encoding.cpp)
target_link_libraries(test_core PRIVATE bmm_core bmm_verify Catch2::Catch2)
# test_aig/test_anf сами используют oneapi::tbb::global_control в секциях
# "*_tbb_scaling" (см. benchmarks/tbb_scaling.hpp, добавлено во второй
# итерации задания) — bmm_aig/bmm_anf линкуют TBB::tbb как PRIVATE, это не
# распространяется транзитивно на test_aig/test_anf, поэтому линкуем явно.
target_link_libraries(test_aig PRIVATE TBB::tbb)
target_link_libraries(test_anf PRIVATE TBB::tbb OpenMP::OpenMP_CXX)
target_link_libraries(test_thr PRIVATE OpenMP::OpenMP_CXX)

# ВРЕМЕННЫЙ таргет для anf/bench_real_corpus.cpp (см. комментарий в файле) —
# удаляется вместе с самим .cpp после того, как числа записаны в anf/README.md.
add_executable(bench_real_corpus anf/bench_real_corpus.cpp)
target_link_libraries(bench_real_corpus PRIVATE bmm_anf bmm_core)

# ПОСТОЯННЫЙ таргет для thr/bench_real_corpus.cpp (см. комментарий в файле —
# тот же принцип "не удаляется", что и у anf/bench_real_corpus.cpp).
add_executable(bench_real_corpus_thr thr/bench_real_corpus.cpp)
target_link_libraries(bench_real_corpus_thr PRIVATE bmm_thr bmm_core)

# ПОСТОЯННЫЙ таргет — сравнительная таблица однопоточно/параллельно для
# всех 12 TBB/OpenMP-функций без ограничения kMaxGroundTruthVars=12 (см.
# benchmarks/large_scale_bench.cpp). Намеренно НЕ зарегистрирован в ctest —
# по решению с пользователем: отдельный опциональный запуск, не удлиняет
# обычный прогон ctest (сейчас ~12-13 минут и так, см. TEST_TIMING_REPORT.md).
add_executable(large_scale_bench benchmarks/large_scale_bench.cpp)
target_link_libraries(large_scale_bench PRIVATE bmm_aig bmm_anf bmm_thr bmm_core
                                                 TBB::tbb OpenMP::OpenMP_CXX ortools::ortools)

include(CTest)
include(Catch)
catch_discover_tests(test_aig)
catch_discover_tests(test_bdd)
catch_discover_tests(test_anf)
catch_discover_tests(test_thr)
catch_discover_tests(test_core)

# ---------------------------------------------------------------------------
# examples/ — минимальные автономные примеры вызова библиотеки БЕЗ Catch2/
# bmm_verify (см. examples/README.md). В отличие от test_<format>.cpp они не
# используют verify::run_translation_tests<X,Y> — только сама функция
# трансляции и её прямой Result<T>. Не зарегистрированы в ctest — это не
# тесты корректности (та же функция уже проверена в test_bdd/test_aig), а
# демонстрация вызова для внешнего потребителя библиотеки.
# ---------------------------------------------------------------------------

add_executable(example_quickstart_bdd examples/quickstart_bdd.cpp)
target_link_libraries(example_quickstart_bdd PRIVATE bmm_bdd bmm_core)

add_executable(example_quickstart_non_bdd examples/quickstart_non_bdd.cpp)
target_link_libraries(example_quickstart_non_bdd PRIVATE bmm_aig bmm_core)

# ---------------------------------------------------------------------------
# test_chains — round-trip/циклы/прямой-vs-обходной путь между всеми пятью
# представлениями (см. verify/chain_tests.cpp). ПОСТОЯННЫЙ таргет (не
# удаляется), в отличие от bench_*/diag_*: дополняет тесты отдельных функций
# проверкой их КОМПОЗИЦИИ. Не через catch_discover_tests — свой формат вывода
# (markdown-отчёт verify/CHAIN_TESTS_REPORT.md), не Catch2 TEST_CASE.
# ---------------------------------------------------------------------------

add_executable(test_chains verify/chain_tests.cpp)
target_link_libraries(test_chains PRIVATE bmm_aig bmm_bdd bmm_anf bmm_thr bmm_verify bmm_core
                                           TBB::tbb OpenMP::OpenMP_CXX ortools::ortools)
add_test(NAME chain_tests COMMAND test_chains
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# test_real_datasets — те же проверки (отдельные функции + round-trip), но
# на РЕАЛЬНЫХ наборах данных (EPFL AIG-схемы, AES S-box/χ/бент-функция,
# Коллегия выборщиков США), а не на синтетических growing_test_functions()
# (см. verify/real_datasets_tests.cpp). Требует предварительно скачанного
# benchmarks/data/epfl (benchmarks/scripts/download_epfl.sh) — если его нет,
# соответствующие EPFL-случаи просто SKIP, не FAIL всего теста.
# ---------------------------------------------------------------------------

add_executable(test_real_datasets verify/real_datasets_tests.cpp)
target_link_libraries(test_real_datasets PRIVATE bmm_aig bmm_bdd bmm_anf bmm_thr bmm_verify
                                                  bmm_core TBB::tbb OpenMP::OpenMP_CXX ortools::ortools)
# lorina (для чтения AIGER) тянет fmt v6, который в этом дереве mockturtle не
# собран отдельной статической библиотекой (никакой другой таргет проекта не
# вызывал lorina::read_aiger по-настоящему, поэтому проблема не всплывала
# раньше) — FMT_HEADER_ONLY заставляет fmt быть полностью inline, без
# недостающих внешних символов при линковке. Только для этого таргета —
# не переиспользуется в одном бинарнике с другими TU, ODR-риска нет.
target_compile_definitions(test_real_datasets PRIVATE FMT_HEADER_ONLY)
add_test(NAME real_datasets_tests COMMAND test_real_datasets
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ВРЕМЕННЫЙ таргет этой сессии — проверка парсера benchmarks/
# anf_dimacs_loader.hpp на реальном ANF-корпусе (persons.iis.nsk.su,
# n=100/M=10000). Не таргет ctest, не коммитится.
add_executable(diag_anf_dataset verify/diag_anf_dataset.cpp)
target_link_libraries(diag_anf_dataset PRIVATE bmm_aig bmm_anf bmm_verify bmm_core
                                                TBB::tbb OpenMP::OpenMP_CXX ortools::ortools)

add_executable(diag_anf_to_bdd_dataset verify/diag_anf_to_bdd_dataset.cpp)
target_link_libraries(diag_anf_to_bdd_dataset PRIVATE bmm_anf bmm_bdd bmm_core TBB::tbb)

# Офлайн-генератор bdd/chow_database_generated.inc (см. bdd/chow_detail.hpp,
# bdd/README.md §5.4а) — перебор весов + исчерпывающий перебор всех
# монотонных функций K<=6 переменных со сверкой каждого расхождения против
# независимого ILP-оракула tt_to_thr. Не таргет ctest (выполняется вручную,
# результат коммитится как сгенерированный .inc-файл, не пересобирается на
# каждом прогоне).
add_executable(diag_chow_database_gen verify/diag_chow_database_gen.cpp)
target_link_libraries(diag_chow_database_gen PRIVATE bmm_thr bmm_core
                                                      OpenMP::OpenMP_CXX TBB::tbb ortools::ortools)

# ВРЕМЕННЫЙ таргет этой сессии — проверка cnf_to_aig (benchmarks/
# cnf_dimacs_loader.hpp) на реальном корпусе DIMACS CNF, независимая
# проверка через внешний SAT-солвер (verify/sat_encoding). Не таргет ctest,
# не коммитится.
add_executable(diag_cnf_dataset verify/diag_cnf_dataset.cpp)
target_link_libraries(diag_cnf_dataset PRIVATE bmm_aig bmm_verify bmm_core TBB::tbb)

add_executable(diag_cnf_chain_dataset verify/diag_cnf_chain_dataset.cpp)
target_link_libraries(diag_cnf_chain_dataset PRIVATE bmm_aig bmm_bdd bmm_anf bmm_thr bmm_verify
                                                      bmm_core TBB::tbb OpenMP::OpenMP_CXX
                                                      ortools::ortools)

if(CUDD_INCLUDE_DIR AND CUDD_LIBRARY)
    add_executable(diag_dddmp_probe verify/diag_dddmp_probe.cpp)
    # benchmarks/cudd_stubs ПЕРЕД системным путём — см. benchmarks/
    # cudd_stubs/util.h за объяснением (dddmp.h требует util.h, которого нет
    # в этом образе, но фактически не использует ничего из него).
    target_include_directories(diag_dddmp_probe PRIVATE benchmarks/cudd_stubs "${CUDD_INCLUDE_DIR}")
    target_link_libraries(diag_dddmp_probe PRIVATE "${CUDD_LIBRARY}")

    add_executable(diag_dddmp_dataset verify/diag_dddmp_dataset.cpp)
    target_include_directories(diag_dddmp_dataset PRIVATE benchmarks/cudd_stubs "${CUDD_INCLUDE_DIR}")
    target_link_libraries(diag_dddmp_dataset PRIVATE bmm_aig bmm_bdd bmm_verify bmm_core
                                                      "${CUDD_LIBRARY}" TBB::tbb)
    target_compile_definitions(diag_dddmp_dataset PRIVATE FMT_HEADER_ONLY)

    add_executable(diag_dddmp_sweep verify/diag_dddmp_sweep.cpp)
    target_include_directories(diag_dddmp_sweep PRIVATE benchmarks/cudd_stubs "${CUDD_INCLUDE_DIR}")
    target_link_libraries(diag_dddmp_sweep PRIVATE bmm_bdd bmm_core "${CUDD_LIBRARY}" TBB::tbb)

    add_executable(diag_anf_sample_dump verify/diag_anf_sample_dump.cpp)
    target_link_libraries(diag_anf_sample_dump PRIVATE bmm_anf bmm_core TBB::tbb
                                                        OpenMP::OpenMP_CXX ortools::ortools)
else()
    message(STATUS "CUDD (libcudd) не найден — diag_dddmp_probe/мост DDDMP->Sylvan недоступны")
endif()

# ---------------------------------------------------------------------------
# install()/export() — find_package(bmm-translib) со стороны внешнего
# проекта. Раньше подключить библиотеку можно было, только скопировав
# исходники внутрь своего дерева и вручную повторив список файлов/линковок
# из этого CMakeLists.txt — теперь это `find_package(bmm-translib REQUIRED)`
# + `target_link_libraries(... bmm::bmm_aig ...)`, как для любой обычной
# CMake-библиотеки.
#
# ЧЕСТНАЯ ОГОВОРКА (см. cmake/bmm-translibConfig.cmake.in за подробностями):
# Sylvan/BRiAl/mockturtle не поставляют собственный CMake-пакет в этом
# окружении — bmm_core подключает их через find_path/find_library с
# абсолютными путями (см. начало файла), и эти абсолютные пути "запекаются"
# в экспортируемые таргеты как есть. Это значит: экспорт воспроизводимо
# работает на машине С ТЕМ ЖЕ layout зависимостей (тот же Docker-образ
# genetica-boolean-lib или его копия), но не является полностью
# релокейтабл-пакетом в общем смысле CMake — осознанный компромисс для
# проекта с одним общим учебным Docker-образом на весь курс, не недосмотр.
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS bmm_core bmm_aig bmm_bdd bmm_anf bmm_thr
    EXPORT bmm-translibTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# Только .hpp (не .cpp/test_*.cpp/README.md/*.inc) — под
# <prefix>/include/bmm/<module>, тот же путь, что и symlink-прослойка
# bmm/<module> для сборки внутри дерева (см. выше, до add_library(bmm_core)).
foreach(_bmm_module core aig bdd anf thr)
    install(DIRECTORY "${_bmm_module}/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/bmm/${_bmm_module}"
        FILES_MATCHING PATTERN "*.hpp"
    )
endforeach()

install(EXPORT bmm-translibTargets
    FILE bmm-translibTargets.cmake
    NAMESPACE bmm::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bmm-translib"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bmm-translibConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/bmm-translibConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bmm-translib"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bmm-translibConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/bmm-translibConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/bmm-translibConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/bmm-translib"
)

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
