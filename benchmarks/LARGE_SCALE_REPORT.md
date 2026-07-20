# Отчёт: масштабирование однопоточно vs наш параллельный формат (benchmarks/large_scale_bench.cpp)

Автоматически перезаписывается при каждом запуске `large_scale_bench` — не редактировать руками.

Без ограничения в `kMaxGroundTruthVars=12` — размер входа растёт до собственного статического лимита функции (`TooManyVariables`) или до бюджета времени в 15с на один прогон, смотря что раньше. Корректность — не полный перебор (бессмысленен на таком n), а сверка однопоточного результата с параллельным на 30 случайных точках (`Assignment`), метод независим от полного перебора.

| Функция | Механизм | n | Однопоточно, мс | Параллельно, мс | Ускорение | Корректность |
|---|---|---|---|---|---|---|
| aig_to_anf | TBB | 50 | 0.080551 | 0.081844 | 0.984202x | PASS |
| aig_to_anf | TBB | 100 | 0.265041 | 0.25085 | 1.05657x | PASS |
| aig_to_anf | TBB | 200 | 0.45358 | 0.428932 | 1.05746x | PASS |
| aig_to_anf | TBB | 500 | 1.78789 | 1.72061 | 1.0391x | PASS |
| aig_to_anf | TBB | 1000 | 3.36292 | 3.23341 | 1.04005x | PASS |
| aig_to_anf | TBB | 2000 | 6.88689 | 8.70449 | 0.791189x | PASS |
| aig_to_thr | TBB | 12 | 290.004 | 151.696 | 1.91174x | PASS |
| aig_to_thr | TBB | 16 | SKIP (aig_to_thr: функция не является пороговой) -- дальше не растим | | | |
| tt_to_aig | TBB | 12 | 0.962677 | 1.31544 | 0.731831x | PASS |
| tt_to_aig | TBB | 16 | 116.211 | 25.0667 | 4.63608x | PASS |
| tt_to_aig | TBB | 20 | SKIP (пробный вызов уже 1130491.803153мс > бюджета) -- дальше не растим | | | |
| anf_to_aig | seq | 50 | 0.133344 | 0.098967 | 1.34736x | PASS |
| anf_to_aig | seq | 100 | 0.179216 | 0.169998 | 1.05422x | PASS |
| anf_to_aig | seq | 200 | 0.438188 | 0.46643 | 0.939451x | PASS |
| anf_to_aig | seq | 500 | 1.02768 | 1.06866 | 0.961652x | PASS |
| anf_to_aig | seq | 1000 | 3.34843 | 3.00087 | 1.11582x | PASS |
| anf_to_aig | seq | 2000 | 5.68125 | 9.90143 | 0.573781x | PASS |
| anf_to_thr | seq | 12 | SKIP (anf_to_thr: not a threshold function) -- дальше не растим | | | |
| tt_to_anf | OpenMP | 16 | 126.668 | 87.7579 | 1.44338x | PASS |
| tt_to_anf | OpenMP | 20 | 3211.34 | 3430.18 | 0.936199x | PASS |
| tt_to_anf | OpenMP | 22 | SKIP (пробный вызов уже 41222.374353мс > бюджета) -- дальше не растим | | | |
| aig_to_tt | OpenMP | 12 | 0.005686 | 0.005678 | 1.00141x | PASS |
| aig_to_tt | OpenMP | 16 | 0.104843 | 0.109695 | 0.955768x | PASS |
| aig_to_tt | OpenMP | 20 | 2.64005 | 2.8747 | 0.918376x | PASS |
| aig_to_tt | OpenMP | 24 | 49.2046 | 50.4602 | 0.975117x | PASS |
| anf_to_tt | OpenMP | 12 | 0.063834 | 0.063539 | 1.00464x | PASS |
| anf_to_tt | OpenMP | 16 | 1.18676 | 1.19453 | 0.993495x | PASS |
| anf_to_tt | OpenMP | 20 | 23.2754 | 6.95477 | 3.34668x | PASS |
| anf_to_tt | OpenMP | 24 | 457.415 | 278.517 | 1.64233x | PASS |
| tt_to_thr | OpenMP | 12 | SKIP (The given truth table is not a threshold function) -- дальше не растим | | | |
| thr_to_anf | OpenMP | 50 | SKIP (thr_to_anf: n > 24 exceeds flat array memory limits) -- дальше не растим | | | |
| thr_to_tt | OpenMP | 12 | 0.174974 | 0.17477 | 1.00117x | PASS |
| thr_to_tt | OpenMP | 16 | 2.86238 | 2.78837 | 1.02654x | PASS |
| thr_to_tt | OpenMP | 20 | 50.3748 | 36.4632 | 1.38152x | PASS |
| thr_to_tt | OpenMP | 24 | 1021.43 | 249.091 | 4.10065x | PASS |
| thr_to_aig | TBB | 50 | 0.122597 | 0.108828 | 1.12652x | PASS |
| thr_to_aig | TBB | 100 | 0.215045 | 0.206875 | 1.03949x | PASS |
| thr_to_aig | TBB | 200 | 0.436412 | 0.439471 | 0.993039x | PASS |
| thr_to_aig | TBB | 500 | 1.32311 | 1.35335 | 0.977657x | PASS |
| thr_to_aig | TBB | 1000 | 3.31254 | 3.3994 | 0.97445x | PASS |
| thr_to_aig | TBB | 2000 | 8.52628 | 8.07106 | 1.0564x | PASS |
