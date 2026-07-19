# Отчёт: масштабирование однопоточно vs наш параллельный формат (benchmarks/large_scale_bench.cpp)

Автоматически перезаписывается при каждом запуске `large_scale_bench` — не редактировать руками.

Без ограничения в `kMaxGroundTruthVars=12` — размер входа растёт до собственного статического лимита функции (`TooManyVariables`) или до бюджета времени в 15с на один прогон, смотря что раньше. Корректность — не полный перебор (бессмысленен на таком n), а сверка однопоточного результата с параллельным на 30 случайных точках (`Assignment`), метод независим от полного перебора.

| Функция | Механизм | n | Однопоточно, мс | Параллельно, мс | Ускорение | Корректность |
|---|---|---|---|---|---|---|
| aig_to_anf | TBB | 50 | 0.075099 | 0.070061 | 1.07191x | PASS |
| aig_to_anf | TBB | 100 | 0.234358 | 0.220274 | 1.06394x | PASS |
| aig_to_anf | TBB | 200 | 0.460297 | 0.445169 | 1.03398x | PASS |
| aig_to_anf | TBB | 500 | 1.7665 | 1.64772 | 1.07209x | PASS |
| aig_to_anf | TBB | 1000 | 3.43236 | 2.84391 | 1.20692x | PASS |
| aig_to_anf | TBB | 2000 | 6.73296 | 7.11948 | 0.94571x | PASS |
| aig_to_thr | TBB | 12 | 271.728 | 160.394 | 1.69413x | PASS |
| aig_to_thr | TBB | 16 | SKIP (aig_to_thr: функция не является пороговой) -- дальше не растим | | | |
| tt_to_aig | TBB | 12 | 1.01512 | 1.01737 | 0.997793x | PASS |
| tt_to_aig | TBB | 16 | 128.538 | 32.471 | 3.95854x | PASS |
| tt_to_aig | TBB | 20 | SKIP (пробный вызов уже 839826.185123мс > бюджета) -- дальше не растим | | | |
| anf_to_aig | seq | 50 | 0.192023 | 0.093846 | 2.04615x | PASS |
| anf_to_aig | seq | 100 | 0.247384 | 0.179582 | 1.37755x | PASS |
| anf_to_aig | seq | 200 | 1.2375 | 0.607783 | 2.03609x | PASS |
| anf_to_aig | seq | 500 | 1.14514 | 1.04025 | 1.10084x | PASS |
| anf_to_aig | seq | 1000 | 3.74964 | 3.63074 | 1.03275x | PASS |
| anf_to_aig | seq | 2000 | 17.1183 | 7.10815 | 2.40826x | PASS |
| anf_to_thr | seq | 12 | SKIP (anf_to_thr: not a threshold function) -- дальше не растим | | | |
| tt_to_anf | OpenMP | 16 | 118.468 | 121.132 | 0.978004x | PASS |
| tt_to_anf | OpenMP | 20 | 3167.69 | 8348.49 | 0.379432x | PASS |
| tt_to_anf | OpenMP | 22 | SKIP (пробный вызов уже 45259.536545мс > бюджета) -- дальше не растим | | | |
| aig_to_tt | OpenMP | 12 | 0.008983 | 0.009118 | 0.985194x | PASS |
| aig_to_tt | OpenMP | 16 | 0.116863 | 0.118218 | 0.988538x | PASS |
| aig_to_tt | OpenMP | 20 | 4.30954 | 3.1703 | 1.35935x | PASS |
| aig_to_tt | OpenMP | 24 | 61.7577 | 67.9538 | 0.908819x | PASS |
| anf_to_tt | OpenMP | 12 | 0.077015 | 0.0755 | 1.02007x | PASS |
| anf_to_tt | OpenMP | 16 | 1.37975 | 1.32046 | 1.04489x | PASS |
| anf_to_tt | OpenMP | 20 | 19.9893 | 40.2683 | 0.496402x | PASS |
| anf_to_tt | OpenMP | 24 | 525.404 | 600.677 | 0.874686x | PASS |
| tt_to_thr | OpenMP | 12 | SKIP (The given truth table is not a threshold function) -- дальше не растим | | | |
| thr_to_anf | OpenMP | 50 | SKIP (thr_to_anf: n > 24 exceeds flat array memory limits) -- дальше не растим | | | |
| thr_to_tt | OpenMP | 12 | 0.15984 | 0.158439 | 1.00884x | PASS |
| thr_to_tt | OpenMP | 16 | 3.19389 | 3.56026 | 0.897095x | PASS |
| thr_to_tt | OpenMP | 20 | 72.3263 | 51.057 | 1.41658x | PASS |
| thr_to_tt | OpenMP | 24 | 1133.99 | 537.143 | 2.11116x | PASS |
| thr_to_aig | TBB | 50 | 0.144681 | 0.140314 | 1.03112x | PASS |
| thr_to_aig | TBB | 100 | 0.31647 | 1.26644 | 0.24989x | PASS |
| thr_to_aig | TBB | 200 | 0.8022 | 1.61397 | 0.497034x | PASS |
| thr_to_aig | TBB | 500 | 2.63682 | 7.39853 | 0.356398x | PASS |
| thr_to_aig | TBB | 1000 | 13.2155 | 20.1788 | 0.654919x | PASS |
| thr_to_aig | TBB | 2000 | 38.8509 | 23.4509 | 1.65669x | PASS |
