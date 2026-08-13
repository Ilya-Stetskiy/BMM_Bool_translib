# STATUS.md

Автоматически сгенерировано `generate_status.sh` через `cmake --build build
--target status` — **не редактируйте руками**, правки будут перезаписаны.

Итого: 20 PASS / 0 FAIL / 0 SKIP из 20 функций.

- Result<T> backend: **std::variant(fallback)** (см. core/CONVENTIONS.md п.2)
- ANF backend: **BRiAl(BoolePolynomial)** (см. core/CONVENTIONS.md п.5)
- sat_encoding: реализован структурный Tseitin-энкодер для AIG и запуск
  CaDiCaL/kissat как внешнего процесса (см. verify/sat_encoding/sat_encoding.hpp).
  **TODO, не реализовано:** структурные энкодеры для Bdd/Anf/Thr — SAT-путь
  верификации сейчас покрывает только функции с To=Aig (4 из 20:
  tt_to_aig, bdd_to_aig, anf_to_aig, thr_to_aig).
- bdd_to_thr: для этого направления не найдено литературы/алгоритма
  ("тёмная ночь", требует отдельного исследования — см. bdd/bdd_to_thr.hpp).

| Папка | tt_to_X | X→формат2 | X→формат3 | X→формат4 | X_to_tt |
|---|---|---|---|---|---|
| **AIG** | ✅ `tt_to_aig`: PASS | ✅ `aig_to_bdd`: PASS | ✅ `aig_to_anf`: PASS | ✅ `aig_to_thr`: PASS | ✅ `aig_to_tt`: PASS |
| **BDD** | ✅ `tt_to_bdd`: PASS | ✅ `bdd_to_aig`: PASS | ✅ `bdd_to_anf`: PASS | ✅ `bdd_to_thr`: PASS | ✅ `bdd_to_tt`: PASS |
| **ANF** | ✅ `tt_to_anf`: PASS | ✅ `anf_to_aig`: PASS | ✅ `anf_to_bdd`: PASS | ✅ `anf_to_thr`: PASS | ✅ `anf_to_tt`: PASS |
| **Thr** | ✅ `tt_to_thr`: PASS | ✅ `thr_to_aig`: PASS | ✅ `thr_to_bdd`: PASS | ✅ `thr_to_anf`: PASS | ✅ `thr_to_tt`: PASS |

## Параллельность aig/anf (обязательный TBB-бенчмарк)

Для функций, реально использующих TBB (core/CONVENTIONS.md п.6,
приоритет правил) — сравнение одного и того же вызова под 1-поточным и
полным `tbb::global_control` на 3 размерах входа, см.
`benchmarks/tbb_scaling.hpp` и секции `*_tbb_scaling` в
`test_aig.cpp`/`test_anf.cpp`.

| Функция | Результат |
|---|---|
| `tt_to_aig` | заметного эффекта от параллелизма не обнаружено (медиана x1.04573) |
| `aig_to_anf` | заметного эффекта от параллелизма не обнаружено (медиана x1.01725) |
| `aig_to_thr` | параллельная версия в среднем быстрее (медиана x1.38939) |
| `tt_to_anf` | заметного эффекта от параллелизма не обнаружено (медиана x0.997583) |
| `anf_to_aig` | заметного эффекта от параллелизма не обнаружено (медиана x0.995699) |
| `anf_to_thr` | заметного эффекта от параллелизма не обнаружено (медиана x1.00062) |

Столбцы 2–5 в каждой строке — это 5 функций конкретной папки в
фиксированном порядке (см. массивы AIG_FUNCS/BDD_FUNCS/ANF_FUNCS/THR_FUNCS в
generate_status.sh), а не единая сетка "откуда→куда" — центральные 3 у
каждой строки ведут в разные форматы (пример: у AIG это BDD/ANF/Thr, у BDD —
AIG/ANF/Thr), общий заголовок пришлось бы делать нечестным.

## Детали (последний прогон)

- `tt_to_aig` — PASS: все проверки пройдены (ground_truth + metamorphic + sat_encoding)
- `aig_to_bdd` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `aig_to_anf` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `aig_to_thr` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `aig_to_tt` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `tt_to_bdd` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `bdd_to_aig` — PASS: все проверки пройдены (ground_truth + metamorphic + sat_encoding)
- `bdd_to_anf` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `bdd_to_thr` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `bdd_to_tt` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `tt_to_anf` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `anf_to_aig` — PASS: все проверки пройдены (ground_truth + metamorphic + sat_encoding)
- `anf_to_bdd` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `anf_to_thr` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `anf_to_tt` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `tt_to_thr` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `thr_to_aig` — PASS: все проверки пройдены (ground_truth + metamorphic + sat_encoding)
- `thr_to_bdd` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `thr_to_anf` — PASS: все проверки пройдены (ground_truth + metamorphic)
- `thr_to_tt` — PASS: все проверки пройдены (ground_truth + metamorphic)
