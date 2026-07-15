# bmm-translib

Библиотека на C++ для трансляции между пятью представлениями булевых
функций: **AIG** (mockturtle), **BDD** (Sylvan), **ANF** (полином
Жегалкина, BRiAl), **Thr** (пороговые функции) и **TT** (таблица истинности,
вспомогательный формат, n ≤ 24 — своей папки не имеет).

Инфраструктура (сборка, тесты, верификация, профилирование) уже готова.
Студенты дописывают только тела 20 функций трансляции — сигнатуры,
doc-комментарии с алгоритмом/литературой и требованиями к параллелизму уже
на месте в `aig/`, `bdd/`, `anf/`, `thr/`.

## 1. Открыть в Coder workspace

Workspace собирается из образа, описанного в `coder-deploy/` (уровнем выше
этого репозитория в общем монорепозитории — см. инфраструктурный промт).
Коротко:

1. Откройте Coder ("New workspace"), укажите URL этого git-репозитория в
   параметре `git_repo_url` при создании workspace — он склонируется в
   `~/project` при старте (см. `coder_agent.main.startup_script` в
   `coder-deploy/template/main.tf`).
2. Откроется VS Code в браузере (`code-server`) уже внутри контейнера со
   всем стеком (mockturtle/Sylvan/BRiAl/CaDiCaL/kissat/CUDA — см.
   `.devcontainer/Dockerfile`, это копия из инфраструктурного промта, см.
   `.devcontainer/SOURCE.md`).
3. Если репозиторий уже склонирован вручную — просто откройте папку
   `bmm-translib/` в контейнере.

## 2. Собрать

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

Сразу после генерации репозитория (до того как кто-либо реализовал хотя бы
одну функцию) сборка должна пройти без ошибок, а все тесты — показать
**SKIP** (не FAIL): это NotImplemented-стабы, а не баги. Если у вас FAIL
или ошибка компиляции на чистом дереве — что-то не так с окружением,
смотрите `core/CONVENTIONS.md` (там разобраны все известные пробелы
`.devcontainer`: TBB не предустановлен апт-пакетом, тянется через
FetchContent при первой сборке; CaDiCaL/kissat — только бинарники, без
`libcadical.a`/заголовков. BRiAl и Google OR-Tools собираются в
`.devcontainer/Dockerfile` как отдельные системные C++-зависимости, "из
коробки" — см. п.5а/5 CONVENTIONS.md, если у вас другой/старый образ, где
их ещё нет).

## 3. Запустить тесты

Все тесты одного формата:

```sh
./build/test_aig     # ./build/test_bdd, test_anf, test_thr — аналогично
```

Только одну функцию (быстрая обратная связь после реализации одной из 20):

```sh
./build/test_aig --section "aig_to_bdd"
```

Реализовав функцию, откройте соответствующий `test_<format>.cpp` в
`aig/`/`bdd/`/`anf/`/`thr/`, найдите свою секцию по имени функции и
гоняйте только её, пока не увидите PASS.

## 4. Прочитать STATUS.md

```sh
cmake --build build --target status
```

Прогоняет все 4 тестовых бинаря + `bmm_config_dump`, перезаписывает
[`STATUS.md`](STATUS.md) в корне — таблица 4×5 (папка × 5 функций в ней) со
статусами PASS/✅ / FAIL/❌ / SKIP/⬛, плюс какой бэкенд `Result<T>`
(`std::expected` vs `std::variant`-fallback) и какой бэкенд `Anf`
(BRiAl vs fallback) реально активны в вашей сборке, плюс отдельный раздел
"Параллельность aig/anf" — для функций, использующих TBB (`tt_to_aig`,
`aig_to_anf`, `aig_to_thr`, `tt_to_anf`, `anf_to_aig`, `anf_to_thr`),
результат обязательного бенчмарка 1-поточной vs TBB-версии (см.
`benchmarks/tbb_scaling.hpp`, секции `*_tbb_scaling` в `test_aig.cpp`/
`test_anf.cpp`) — "ускорение xN" или честно зафиксированное "медленнее на
этих размерах" тоже валидный результат. **Не редактируйте `STATUS.md`
руками** — он перезаписывается при каждом запуске.

CI (`.github/workflows/ci.yml`) делает то же самое на каждый push/PR;
SKIP не валит сборку, FAIL — валит. Exhaustive-проверки в CI идут до n=16
включительно (`verify::kMaxGroundTruthVars`) — для большего n используйте
`benchmarks/`, это не задача обязательного прогона на каждый PR.

## 5. Контракт и конвенции

Прежде чем писать тело функции — прочитайте
[`core/CONVENTIONS.md`](core/CONVENTIONS.md): порядок бит (`LSB_FIRST`),
почему `Result<T>` вместо исключений, единый интерфейс представления
(`n_vars()`/`evaluate()`/`to_tt()`), правила выбора параллелизма
(TBB/Sylvan-Lace/OpenMP — и как разрешается конфликт правил для функций
вроде `aig_to_tt`/`tt_to_anf`/`thr_to_aig`), и честные пробелы
(`bdd_to_thr` — литературы не найдено; `verify/sat_encoding` — структурные
энкодеры готовы только для AIG).

Верификация каждой функции идёт в 3 независимых шага (см.
`verify/test_runner.hpp`): `verify/ground_truth` (прямой перебор точек) →
`verify/metamorphic` (Chow-параметры, степень АНФ, кофакторы) →
`verify/sat_encoding` (Tseitin + CaDiCaL/kissat, только для функций с
выходом в AIG). Первое найденное расхождение — это FAIL с конкретным
контрпримером в выводе теста, а не просто "тест не прошёл".

## 6. Профилирование

См. [`profiling/README.md`](profiling/README.md) — Tracy (real-time
таймлайн по потокам/ядрам, зоны `ZoneScoped` уже расставлены во всех 20
функциях) как основной путь, `profiling/perf_report.sh` как fallback без
GUI.

## 7. Бенчмарки

`benchmarks/scripts/` — скрипты скачивания EPFL/HWMCC/PB и генерации
S-box-тестов; `download_iscas.sh`/`download_hwmcc.sh`/`download_pb.sh`
требуют явного `BASE_URL` (у этих архивов нет одного постоянного адреса,
см. предупреждения в самих скриптах) — `download_epfl.sh` работает из
коробки (github.com/lsils/benchmarks).

## 8. Кто за что отвечает

Заполняется вручную командой.

| Функция | Ответственный | Статус |
|---|---|---|
| tt_to_aig | | |
| aig_to_bdd | | |
| aig_to_anf | | |
| aig_to_thr | | |
| aig_to_tt | | |
| tt_to_bdd | | |
| bdd_to_aig | | |
| bdd_to_anf | | |
| bdd_to_thr | | |
| bdd_to_tt | | |
| tt_to_anf | | |
| anf_to_aig | | |
| anf_to_bdd | | |
| anf_to_thr | | |
| anf_to_tt | | |
| tt_to_thr | | |
| thr_to_aig | | |
| thr_to_bdd | | |
| thr_to_anf | | |
| thr_to_tt | | |
