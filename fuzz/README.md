# fuzz/ — libFuzzer-харнессы для собственных парсеров untrusted-форматов

Два харнесса, по одному на каждый парсер DIMACS-подобного формата, который
этот проект пишет и поддерживает сам (не проксирует внешнюю библиотеку):

- [`fuzz_cnf_dimacs_loader.cpp`](fuzz_cnf_dimacs_loader.cpp) —
  `benchmarks/cnf_dimacs_loader.hpp::load_cnf_dimacs`.
- [`fuzz_anf_dimacs_loader.cpp`](fuzz_anf_dimacs_loader.cpp) —
  `benchmarks/anf_dimacs_loader.hpp::load_anf_dimacs`.

**`benchmarks/dddmp_loader.hpp` намеренно не фаззится** — это в основном
тонкая обёртка над `Dddmp_cuddBddArrayLoad` (парсер самого CUDD, не код
этого проекта); фаззить его означало бы фаззить CUDD, у которого своя
история как отдельного проекта, а не находить баги здесь.

## Находка, ради которой это заведено

При аудите (см. `CHANGELOG.md`) нашлась неограниченная аллокация: оба
загрузчика читают `n_vars`/`n_clauses`/`n_monomials` из **заголовка** файла
и сразу используют их в `reserve()` — до какого-либо перебора реальных
строк. Специально сконструированный файл (`p cnf 4000000000 4000000000`)
вызывает DoS/OOM одной строкой. Исправлено (`kMaxReasonableCnfVars`/
`kMaxReasonableAnfVars` в самих загрузчиках) — харнессы здесь существуют,
чтобы такой класс находок ловился автоматически, а не только руками при
следующем ревью.

## Сборка

Требует **Clang** — `-fsanitize=fuzzer` не поддерживается GCC. Обычная
сборка проекта (GCC, `cmake -S . -B build`) эти таргеты не создаёт вообще
(`BMM_BUILD_FUZZERS` по умолчанию `OFF`).

```sh
cmake -S . -B build-fuzz \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DBMM_BUILD_FUZZERS=ON
cmake --build build-fuzz --target fuzz_cnf_dimacs_loader fuzz_anf_dimacs_loader
```

## Запуск

```sh
./build-fuzz/fuzz_cnf_dimacs_loader fuzz/corpus/cnf
./build-fuzz/fuzz_anf_dimacs_loader fuzz/corpus/anf
```

Без ограничения по времени эти команды фаззят бесконечно (стандартное
поведение libFuzzer) — останавливать вручную (`Ctrl+C`) или ограничить:

```sh
./build-fuzz/fuzz_cnf_dimacs_loader fuzz/corpus/cnf -max_total_time=300
```

Найденный крэш сохраняется в `crash-<hash>` в текущей директории — не
теряется при следующем запуске (libFuzzer сам это делает), не удалять
руками до разбора.

## Что это НЕ делает

- Не гоняется в лёгком CI дольше короткого smoke-теста (`-max_total_time`
  порядка минуты, см. `.github/workflows/ci.yml`, job `fuzz-smoke`) — цель
  smoke-прогона — убедиться, что харнесс собирается и не падает на
  исходном корпусе сразу, не полноценная фаззинг-кампания. Для реального
  поиска багов запускать вручную (см. выше), часами/сутками, желательно на
  выделенной машине.
- Не фаззит `cnf_to_aig`/сборку `Aig` из `CnfFormula` — только сам парсинг
  (`load_cnf_dimacs`/`load_anf_dimacs`). Это осознанное сужение скоупа: то,
  что реально принимает недоверенный внешний файл (не программно
  построенный `CnfFormula`), — именно парсер.
