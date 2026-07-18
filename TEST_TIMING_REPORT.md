# Отчёт: время прогонов тестов (сессия улучшения тестовой инфраструктуры)

Снимок одного полного прогона `ctest` (39 тестов, включая новый `chain_tests`)
в контейнере `genetica-boolean-lib:latest`, чистая пересборка. Числа — время
одного прогона, не медиана (в отличие от бенчмарков внутри самих тестов,
которые уже используют медиану — см. `benchmarks/scaling.hpp`); ориентир для
порядка величины, не точная метрика.

**Итог: 39/39 тестов пройдено, 0 провалов. Общее время — 697.56 с (~11.6 мин).**

## Почему это число снизилось на порядки в этой сессии

До фикса `core/anf_repr.hpp::Anf::to_tt()` (см. ниже) секция `tt_to_anf`
одна не укладывалась в 120 секунд (убита по таймауту) — на дефолтном
`kMaxGroundTruthVars=12` `Anf::evaluate()` стоит O(размера полинома) за
вызов, а `verify/test_runner.hpp` вызывал его до 4 раз на каждую из 2^n точек
на каждую тестовую функцию → O(4^n)-подобный рост на плотных функциях
(`or_all`/`random`). После фикса (настоящее преобразование Мёбиуса в
`to_tt()` + материализация один раз в `test_runner.hpp` вместо четырёх
проходов) те же секции — доли секунды.

## Корректность: 26 секций отдельных функций + инфраструктурные проверки — 34.6 с

| # | Тест | Время | Категория |
|---|---|---|---|
| 1 | tt_to_aig | 1.75 с | функция |
| 2 | aig_to_bdd | 0.93 с | функция |
| 3 | aig_to_anf | 0.69 с | функция |
| 4 | aig_to_thr | 3.51 с | функция |
| 5 | aig_to_tt | 0.68 с | функция |
| 9 | tt_to_bdd | 0.90 с | функция |
| 10 | bdd_to_aig | 1.95 с | функция |
| 11 | bdd_to_anf | 0.99 с | функция (портирована с egor в этой сессии) |
| 12 | bdd_to_thr | 0.36 с | функция (портирована с egor, K-гейтинг исправлен) |
| 13 | bdd_to_tt | 0.91 с | функция |
| 14 | tt_to_anf | 0.67 с | функция (было >120 с до фикса test-инфраструктуры) |
| 15 | anf_to_aig | 5.36 с | функция |
| 16 | anf_to_bdd | 1.64 с | функция |
| 17 | anf_to_thr | 3.79 с | функция (variant-API/OOM/verify-before-return исправлены) |
| 18 | anf_to_tt | 0.62 с | функция |
| 24 | tt_to_thr | 4.83 с | функция (solver-фолбэк добавлен) |
| 25 | thr_to_aig | 0.61 с | функция (фикс полярности в full_adder) |
| 26 | thr_to_bdd | 0.57 с | функция |
| 27 | thr_to_anf | 0.66 с | функция (портирована с origin/Thr) |
| 28 | thr_to_tt | 0.50 с | функция (OMP-порог добавлен) |
| 33 | Aig::to_tt | 0.28 с | инфраструктура |
| 34 | Bdd::to_tt | 0.31 с | инфраструктура |
| 35 | Anf::to_tt | 0.31 с | инфраструктура (новый Мёбиус-based to_tt) |
| 36 | Thr::to_tt | 0.28 с | инфраструктура |
| 37 | check_aig_equivalence_via_sat (identical) | 0.51 с | SAT-верификация |
| 38 | check_aig_equivalence_via_sat (different) | 0.25 с | SAT-верификация |

## Тесты композиции (новое в этой сессии): chain_tests — 50.24 с

23 проверки (round-trip по 20 парам представлений + 3 более длинных цикла) +
10 сравнений "прямой путь vs обход через третье представление" — 0 провалов.
Полная разбивка и тайминги внутри — `verify/CHAIN_TESTS_REPORT.md`
(перегенерируется при каждом запуске `test_chains`).

Главный результат: перевод через третье представление БЫСТРЕЕ прямого в 5 из
10 проверенных случаев, разрыв растёт с n (Anf->Bdd напрямую в 12.6 раза
медленнее, чем Anf->Tt->Bdd, при n=17).

## Бенчмарки (`[benchmark]`-секции, TBB/OpenMP scaling) — 632.7 с (91% времени)

Не про корректность — про измерение параллелизма (медиана 5 повторов на
каждую сторону, см. `benchmarks/scaling.hpp`). Ожидаемо доминируют общее
время прогона; не блокируют CI по своей природе (PASS означает "функция
измерена", не "ускорение подтверждено" — см. `*/README.md` за интерпретацией
конкретных чисел).

| # | Тест | Время |
|---|---|---|
| 6 | tt_to_aig_tbb_scaling | 0.43 с |
| 7 | aig_to_anf_tbb_scaling | 0.46 с |
| 8 | aig_to_thr_tbb_scaling | 24.87 с |
| 19 | tt_to_anf_tbb_scaling | 1.12 с |
| 20 | anf_to_aig_tbb_scaling | 1.48 с |
| 21 | anf_to_thr_tbb_scaling | 71.06 с |
| 22 | tt_to_anf_openmp_scaling | 189.21 с |
| 23 | anf_to_tt_openmp_scaling | 251.43 с |
| 29 | tt_to_thr_openmp_scaling | 71.28 с |
| 30 | thr_to_aig_openmp_scaling | 0.49 с |
| 31 | thr_to_anf_openmp_scaling | 0.91 с |
| 32 | thr_to_tt_openmp_scaling | 0.53 с |

## Как повторить

```sh
MSYS_NO_PATHCONV=1 docker run --rm -v "D:/Proga/web/bmm-translib:/workspace" -w /workspace genetica-boolean-lib:latest bash -c "cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j\$(nproc) && cd build && ctest --output-on-failure"
```

Только тесты композиции (быстрее, без TBB/OpenMP-бенчмарков остальных
модулей):
```sh
MSYS_NO_PATHCONV=1 docker run --rm -v "D:/Proga/web/bmm-translib:/workspace" -w /workspace genetica-boolean-lib:latest bash -c "cd /workspace && ./build/test_chains"
```
