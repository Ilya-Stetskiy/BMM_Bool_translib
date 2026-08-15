# Changelog

Формат по мотивам [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/).
Версия — `project(bmm-translib VERSION ...)` в `CMakeLists.txt`; git-тег на
конкретный коммит ставится отдельно, после мержа в `master` (см.
`core/CONVENTIONS.md`, если там появится раздел про релизы — сверяйтесь с
ним, а не только с этим файлом).

## [Unreleased]

### Added
- `examples/external_consumer/` — отдельный CMake-проект (не таргет в этом
  же `build/` дереве, как `examples/quickstart_*.cpp`), доказывающий, что
  `find_package(bmm-translib REQUIRED)` реально работает на установленной
  (`cmake --install`) копии библиотеки. `install()`/`export()` существуют
  с прошлой сессии, но живьём через `find_package` из другого CMake-
  проекта не проверялись НИ РАЗУ до этого — теперь гоняется в CI
  (`build-and-test`, шаги после `STATUS.md`).
- `fuzz/` — libFuzzer-харнессы для собственных парсеров untrusted-форматов
  (`fuzz_cnf_dimacs_loader`, `fuzz_anf_dimacs_loader`; `benchmarks/
  dddmp_loader.hpp` намеренно не фаззится — в основном проксирует парсер
  CUDD, не код проекта). `BMM_BUILD_FUZZERS` (по умолчанию `OFF`, требует
  Clang — `-fsanitize=fuzzer` не GCC-флаг). CI (job `fuzz-smoke`) гоняет
  только короткий smoke-прогон (`-max_total_time=60`), не полноценную
  кампанию — см. `fuzz/README.md`.

### Changed
- `aig/aig_to_tt.cpp`: `kBlockVars` 6 → 10 — закрывает TODO из
  `TRANSLATION_MATRIX.md` про allocation-free подход + широкий блок,
  раньше не измерявшиеся вместе. Реальный замер (`verify/
  diag_aig_to_tt_k10.cpp`, временный, удалён после записи чисел) показал
  K=10 быстрее K=6 в 1.5-2.5x на n=16/20/24 (только на n=12, <0.01мс,
  доминирует шум CI-раннера); cross-check K6==K10 — PASS на всех размерах,
  подтверждено полным `ctest` + ASan/UBSan.
- `TRANSLATION_MATRIX.md`/`aig/README.md`/`anf/README.md`/`bdd/README.md`/
  `thr/README.md`/`core/CONVENTIONS.md`: закрыт `kMaxTruthTableVars=24`
  staleness — все писались ДО подъёма лимита до 32 (см. запись ниже про
  `bd39682`). Статус верификации n=13…32 явно зафиксирован для
  `tt_to_bdd`/`thr_to_tt`/`tt_to_thr`/`bdd_to_tt` (только метаморфные
  проверки, не exhaustive) и n=20…32 для `anf_to_tt`/`tt_to_anf`
  (`verify/large_n_tests.cpp`). Заодно найден и исправлен мелкий хвост:
  `thr/tt_to_thr.cpp` имел свой хардкод `n>=32`, раньше "с запасом" над
  старым лимитом 24, теперь ровно совпадает с новым 32.
- `ci/install-deps.sh` → `scripts/install-deps.sh` — переименован и
  переописан как общий bootstrap-скрипт зависимостей (CUDD/Sylvan/
  mockturtle/m4ri/BRiAl/kissat/CaDiCaL/OR-Tools), не только для CI: строит
  тот же layout из исходников на любой Ubuntu 24.04-совместимой машине
  через `BMM_DEPS_PREFIX`/`BMM_DEPS_JOBS_OR_TOOLS` (последняя — новая,
  раньше `-j2` для OR-Tools был захардкожен). Это практический ответ на
  пробел "нерелокейтабл-пакет" (`cmake/bmm-translibConfig.cmake.in`,
  README.md §4б) — не полный vcpkg/Conan-порт (для этого нет портов ни
  у одной из трёх библиотек, отдельная кратно более дорогая задача), а
  воспроизводимая сборка с нуля где угодно. README.md/CMakeLists.txt/
  `cmake/bmm-translibConfig.cmake.in`/`core/CONVENTIONS.md` обновлены.

### Fixed
- `benchmarks/cnf_dimacs_loader.hpp`/`anf_dimacs_loader.hpp`:
  `n_vars`/`n_clauses`/`n_monomials` из заголовка файла использовались в
  `reserve()` без верхней границы — специально сконструированный файл
  (`p cnf 4000000000 4000000000`) вызывал DoS/OOM одной строкой заголовка,
  до какого-либо перебора реальных строк. Найдено при аудите (см. ниже,
  `[0.1.0]` "Топ-5"), формально подтверждено фаззингом (`fuzz/`, см. Added
  выше). Фикс — `kMaxReasonableCnfVars`/`kMaxReasonableAnfVars` (100 млн,
  с большим запасом выше реальных датасетов проекта).
- `verify/real_datasets_tests.cpp`: отсутствие `benchmarks/data/epfl`
  (недокачанный датасет) роняло весь `test_real_datasets` через
  `rep.bullet(false, ...)` (учитывается в `failed_checks`) вместо
  `rep.line("- SKIP ...")` (как везде в файле для легитимных пропусков) —
  расхождение с собственным комментарием в `CMakeLists.txt` ("если данных
  нет — EPFL-случаи SKIP, не FAIL всего теста"), найденное живым прогоном
  CI (см. запись `[0.1.0]` ниже, "Verified" п.1). Однострочный фикс: теперь
  реально SKIP.

### Fixed
- `benchmarks/cnf_dimacs_loader.hpp`/`anf_dimacs_loader.hpp`: даже с
  `kMaxReasonableCnfVars`/`kMaxReasonableAnfVars` (запись выше) `reserve()`
  оставался уязвим — `n_clauses_declared`/`n_monoms_declared` у самой
  границы лимита (99999999, ещё проходит проверку) даёт `reserve()` на
  ~2.4 ГБ из 20-байтного заголовка файла (`sizeof(CnfClause)==24` байта,
  `sizeof(std::vector<uint32_t>)==24` байта для ANF). Найдено многочасовым
  ручным фаззинг-прогоном (не 60с CI-smoke — см. Verified ниже), не
  теоретически. Фикс: `reserve()` дополнительно ограничен фактическим
  размером файла (`std::filesystem::file_size`) — атакующий не может
  форсировать аллокацию больше, чем байт сам передал (PR #7, коммит
  `55958c3`).
- `benchmarks/anf_dimacs_loader.hpp`: независимый второй источник OOM,
  найденный той же кампанией — конструктор `polybori::BoolePolyRing(n)`
  (BRiAl) сам стоит ~4.2КБ RAM на переменную ВНЕ ЗАВИСИМОСТИ от числа
  мономов в файле (измерено эмпирически: n=100000 → ~470МБ, n=1000000 →
  ~4.2ГБ за один malloc, n=3000000 сегфолтится внутри `libbrial`). Входной
  файл `"p anf 2999999 8\n1 2 1 2"` (23 байта, 8 мономов) обрушивал
  процесс — `kMaxReasonableAnfVars` для ЭТОЙ операции не защищал, т.к.
  аллокация пропорциональна n_vars уже при конструировании кольца, до
  использования мономов. `AnfFallback` (`BMM_HAVE_BRIAL=0`) не затронут
  (`std::set`, растёт только с реальными данными). Фикс: отдельный,
  сильно более строгий `kMaxReasonableAnfRingVars=50000` (~210МБ худший
  случай), проверяется только на BRiAl-пути (PR #7, коммит `4991f3d`).

### Verified

- **Пункты выше подтверждены живым прогоном** на GitHub Actions (2026-08-13,
  PR #6, run 31674727813) — все три job'а зелёные: `build-and-test`
  (11m23s, включая `find_package`/`external_consumer` — `AND3 корректно
  построен и оценён`), `asan-ubsan` (34m13s), `fuzz-smoke` (3m17s, оба
  харнесса реально исследовали покрытие — `fuzz_anf_dimacs_loader`: 74731
  запуск за 61с, cov: 167, 0 крэшей на стартовом корпусе). По дороге одна
  находка: `find_package(OpenMP REQUIRED)` (безусловный, нужен `thr/*`)
  падал под Clang в `fuzz-smoke` — GCC получает OpenMP через `libgomp`
  (часть пакета `gcc`), Clang `-fopenmp` требует отдельный `libomp-dev`,
  не входивший в apt-список этого job'а. Исправлено.
- **Многочасовая ручная фаззинг-кампания** (2026-08-14/15, сервер
  192.168.1.61, Clang 18 + ASan/UBSan, НЕ CI-smoke — по прямому запросу
  "фаззить дольше вручную — часами, поищи реальные баги"): три прогона —
  первый упал за 15 секунд (первая находка Fixed выше), второй нашёл
  вторую независимую находку через ~1 час, третий (уже на обоих фиксах)
  отработал полностью честные 4 часа на каждый харнесс без единого нового
  крэша — `fuzz_cnf_dimacs_loader`: 403 391 956 итераций (28011 exec/s,
  cov 133), `fuzz_anf_dimacs_loader`: 11 104 825 итераций (771 exec/s, cov
  179). Оба сохранённых crash-артефакта первой кампании подтверждённо не
  воспроизводятся на исправленных бинарниках. См. `fuzz/README.md` за
  полной методологией и известными граблями сборки под Clang в
  одноразовых Docker-контейнерах (apt-кэш не персистентен между `docker
  run --rm`, `-G Ninja` + C++23 требует `clang-scan-deps`, нужен отдельный
  `libclang-rt-18-dev`).
- **PR #7 (`fix/dimacs-reserve-oom`) смёржен в `master`** (2026-08-14,
  коммит `8ccde3b`).
- **Портируемость (`scripts/install-deps.sh`) подтверждена живым прогоном
  "с нуля" на "сыром" устройстве** (2026-08-14, сервер 192.168.1.61):
  чистый контейнер `ubuntu:24.04` — не `.devcontainer`-образ, без
  CI-кэша зависимостей — apt-пакеты из `.github/workflows/ci.yml` →
  `scripts/install-deps.sh` (сборка CUDD/Sylvan/mockturtle/m4ri/BRiAl/
  kissat/CaDiCaL/OR-Tools из исходников) → `cmake`-сборка → полный
  `ctest`. Результат: 30/30 тестов пройдено, включая `real_datasets_tests`
  (корректно SKIP без EPFL-датасета — тот же фикс, что выше в `Fixed`,
  подтверждён и вне CI). Первая проверка портируемости не на GitHub-hosted
  раннере и не на предсобранном Docker-образе, а на действительно пустой
  машине.

## [0.1.0] - 2026-08-11

Правки по итогам ревью репозитория как библиотеки общего назначения (не
только как студенческого задания) — см. полный текст ревью и роадмап для
контекста, каждый пункт ниже без даты соответствует одному коммиту на
ветке `fix/library-hygiene-p2`.

**Оговорка про сам тег:** `v0.1.0` поставлен на HEAD ветки
`fix/library-hygiene-p2` (PR #5), а не на `master` — сознательное отступление
от политики выше (обычно тег ставится после мержа). Причина: `master` на
момент тега вообще не имеет `VERSION` в `project(bmm-translib ...)` — вся
работа, которая делает версию/тег осмысленными (`VERSION 0.1.0`,
`install()`/`export()`, этот файл), сама живёт в PR #5 и ещё не слита.
Решение осознанное (обсуждено с автором), не забытая политика — если PR #5
смержится с изменением SHA коммитов (squash/rebase), этот тег останется
валидным указателем на исходный коммит, просто не будет лежать на первом
родителе `master` напрямую.

### Added
- `out_of_memory<T>(fn_name)` в `core/common.hpp` — единая точка построения
  `Result<T>` с `ErrorCode::OutOfMemory`, вместо независимых копий в каждой
  из 20 функций трансляции.
- `examples/quickstart_bdd.cpp` и `examples/quickstart_non_bdd.cpp` —
  минимальные автономные примеры вызова библиотеки вне тестовой
  инфраструктуры (Catch2/`verify`), с объяснением обязательной
  инициализации Sylvan/Lace для `Bdd` в `examples/README.md`.
- `install()`/`export()`: `find_package(bmm-translib REQUIRED)` +
  `target_link_libraries(... bmm::bmm_aig ...)` теперь работает как для
  обычной CMake-библиотеки — раньше единственным способом подключения было
  скопировать исходники внутрь своего дерева. Публичный include-неймспейс
  `bmm/` (`#include <bmm/core/common.hpp>` и т.п.) совпадает с C++
  `namespace bmm`, используется одинаково внутри репозитория и снаружи.
- `verify/full_matrix_tests.cpp` (`test_full_matrix`) — полная матрица
  корректности для ВСЕХ упорядоченных пар представлений и ВСЕХ
  промежуточных шагов (в отличие от курируемого набора в
  `verify/chain_tests.cpp`), сравнивает результат с независимым эталоном,
  не только факт, что перевод выполнился.
- `verify/large_n_tests.cpp` (`test_large_n`) — независимая (эталон —
  формула чётности, не код проекта) проверка `anf_to_tt`/`tt_to_anf` на
  n=20..32: единственный тест, реально закрывающий диапазон, открытый
  подъёмом `kMaxTruthTableVars`, — ни один из существующих таргетов не
  проверяет корректность выше `kMaxGroundTruthVars=12`.
- `CHANGELOG.md` (этот файл).
- `.github/workflows/ci.yml` + `ci/install-deps.sh` — лёгкий CI: собирает
  CUDD/Sylvan/mockturtle/m4ri/BRiAl/kissat/CaDiCaL/OR-Tools из исходников на
  GitHub-hosted раннере (версии продублированы из `.devcontainer/Dockerfile`,
  результат кэшируется), не через публикацию Docker-образа в реестр (та
  часть отложенного решения из `core/CONVENTIONS.md` п.8 всё ещё не сделана).
  Гоняет весь `ctest`, кроме секций `*_tbb_scaling`/`*_openmp_scaling` (91%
  времени полного прогона, см. `TEST_TIMING_REPORT.md`, — про измерение
  параллелизма, не про корректность).
- `CMakeLists.txt`/`BMM_SANITIZE` — опция `-DBMM_SANITIZE=address,undefined`
  (ASan/UBSan), продолжение методологии, уже нашедшей реальную гонку в BRiAl
  через ThreadSanitizer (`aig/README.md` §1.3). Пусто по умолчанию. Гоняется
  в CI отдельным job'ом (`asan-ubsan` в `.github/workflows/ci.yml`), см.
  README.md §3а за деталями и оговоркой про неинструментированные
  зависимости (CUDD/Sylvan/mockturtle/m4ri/BRiAl/OR-Tools).

### Changed
- Единая политика перехвата исключений во всех функциях трансляции: только
  `catch (const std::bad_alloc&)`, никакого `catch (const std::exception&)`.
  Раньше 7 функций дополнительно ловили любое исключение и заворачивали его
  в `ErrorCode::InvalidArgument`, смешивая внутренние баги (нарушенный
  инвариант) с невалидным входом вызывающей стороны.
- Добавлена защита от `bad_alloc` в `aig_to_tt`/`anf_to_aig` — те же
  критерии структурного риска (п.2а `core/CONVENTIONS.md`), что и у их уже
  защищённых "сиблингов", просто раньше не были применены к этим двум.
- `.devcontainer/Dockerfile`: зафиксированы версии CUDD (`cudd-3.0.0`),
  Sylvan (`v1.10.0`), BRiAl (`1.2.15`), mockturtle и m4ri (конкретные
  коммиты — у обоих нет подходящего тега) — раньше все пять клонировались
  на upstream HEAD без фиксации, что уже сейчас сделало пересборку образа
  "с нуля" невозможной (конфликт версий BRiAl/m4ri). Решено пересборкой
  m4ri из исходников на зафиксированной версии.
- CMakeLists.txt: `EXCLUDE_FROM_ALL` для Catch2 (не нужен потребителю
  библиотеки ни в каком виде); TBB и Tracy, наоборот, устанавливаются
  вместе с bmm-translib — для статических библиотек `PRIVATE`-линковка не
  освобождает потребителя от необходимости предоставить их при финальной
  линковке своего приложения.
- `core/common.hpp`: `kMaxTruthTableVars` поднят с 24 до 32 — проверено по
  исходнику `kitty::dynamic_truth_table`, что 24 было решением самого
  bmm-translib, не потолком kitty/mockturtle снизу. Перед подъёмом
  проведён аудит на 32-битные сдвиги (`bdd_to_anf.cpp` использовал
  `1u << n`, при n=32 undefined behavior — исправлено на `uint64_t`/`1ULL`,
  единственный такой случай в репозитории) и добавлена `bad_alloc`-защита
  в `anf_to_tt`/`tt_to_anf` (их Мёбиус-буфер — 1 байт на строку, 16 МБ на
  n=24, но 4 ГБ на n=32 — на старом лимите такая аллокация не могла
  реалистично провалиться, на новом может).

### Fixed
- `aig/aig_to_bdd.hpp`: doc-комментарий описывал `aig_to_bdd(aig)` как
  использующий натуральный (MinIndex) порядок переменных без защиты от
  взрыва BDD — фактическая реализация уже использует FORCE по умолчанию
  (правка более раннего коммита, не отражённая в комментарии вовремя).
- `bdd_to_thr` игнорировал `Bdd::var_at_level()` для входов с нестандартным
  порядком уровней (таких, как выход `aig_to_bdd`/`anf_to_bdd` по
  умолчанию, FORCE-эвристика) — молча писал веса не в ту позицию
  глобального вектора. Найдено `verify/full_matrix_tests.cpp`
  (`Aig->Bdd->Thr`, `Anf->Bdd->Thr`, n=4/5/6); исправлено однострочным
  фиксом (`global_weights[unique_vars[i]]` →
  `global_weights[bdd.var_at_level(unique_vars[i])]`) — вся внутренняя
  логика (Chow-параметры, унитарность, ILP-lookup) работала только с
  позициями, не с сырыми уровнями, там ничего менять не потребовалось.
  `test_full_matrix` теперь полностью зелёный (2191/2191).

### Verified

- **CI (`.github/workflows/ci.yml`) подтверждён живым прогоном** на GitHub
  Actions (2026-08-11/12, PR #5) — оба job'а, `build-and-test` и
  `asan-ubsan`, зелёные (финальный run 31624272862, 27-58 мин). Потребовалось
  четыре итерации, каждая нашла и починила реальную проблему, не
  гипотетическую:
  1. `test_real_datasets` реально проваливался без EPFL-датасета (4 FAIL:
     `ctrl`/`int2float`/`cavlc`/`router.aig` не загружены) — комментарий в
     `CMakeLists.txt` ("если данных нет — SKIP, не FAIL") оказался НЕ
     соответствующим фактическому поведению `verify/real_datasets_tests.cpp`.
     Исправлено в CI (качаем EPFL через `download_epfl.sh`) — сам разрыв
     документация/код теста остаётся отдельным, не исправленным здесь багом
     (кто угодно, гоняющий `ctest` локально без EPFL-данных, увидит тот же
     спонтанный FAIL).
  2. `benchmarks/scripts/download_epfl.sh` не executable в git (`100644`) —
     `./script.sh` падал с exit 126; исправлено на `bash script.sh`.
  3. Связанный `actions/cache@v4` сохраняет кэш только при успехе ВСЕГО
     job'а — падение на (1)/(2) стирало уже собранные (~30-40 мин)
     зависимости при каждой итерации. Разделено на `actions/cache/restore`
     + `actions/cache/save` (последний с `if: always()`).
  4. Первый прогон с РЕАЛЬНО восстановленным кэшем зависимостей (не
     собранным заново) неожиданно провалил ВСЕ тестовые бинари сразу —
     `Illegal instruction` (SIGILL) при простом запуске для перечисления
     тестов (`CatchAddTests`), ещё до первого реального теста. Причина:
     Lace (github.com/trolando/lace v1.6.0, тянется Sylvan-ом через
     `FetchContent`) собирается с `-march=native` по умолчанию
     (`LACE_NATIVE_OPT=ON`) — GitHub-hosted раннеры физически разные VM в
     пуле, кэш, собранный на одной, восстанавливается на другой с
     потенциально другим набором инструкций CPU. `bmm_core` линкует
     Sylvan+Lace безусловно, поэтому падало абсолютно всё, не только
     BDD-функции. Исправлено (`ci/install-deps.sh`:
     `-DLACE_NATIVE_OPT=OFF`). Sylvan сам по себе такого флага не имеет;
     mockturtle/OR-Tools проверены — чисты.
- **`asan-ubsan` job подтверждён тем же прогоном** — `continue-on-error:
  true` снят (был временным, до первого живого подтверждения). Ни одного
  срабатывания ASan/UBSan на всём прогоне, несмотря на то, что CUDD/Sylvan/
  mockturtle/m4ri/BRiAl/OR-Tools сами собраны без санитайзеров.

### Known issues
- Публикация `genetica-boolean-lib` в реестр (GHCR) — по-прежнему отдельная,
  не сделанная задача; если/когда она случится, можно рассмотреть переход
  на неё как на более быстрый путь (без пересборки OR-Tools на каждый
  cache miss в CI).
- ~~`verify/real_datasets_tests.cpp` не SKIP-ает недостающий EPFL-датасет~~
  — **исправлено**, см. `[Unreleased]` выше.
