// anf/bench_real_corpus.cpp — ПОСТОЯННЫЙ инструмент (в отличие от всех
// предыдущих bench_*.cpp в истории этого проекта — bench_bdd_heuristics.cpp/
// bench_aig_corpus.cpp/bench_anf_fallback.cpp и т.д. — которые были
// одноразовыми и удалялись после того, как числа записывались в README; этот
// оставлен по прямому запросу пользователя: "давай подцепим датасет реальных
// ANF функций и будем с ними проводить тестирования" — предполагается, что
// корпус и бенчмарк будут использоваться повторно, не только в этой сессии).
//
// Цель: прогнать все 5 функций anf/ на РЕАЛЬНЫХ (не синтетических) булевых
// функциях, чтобы иметь честную оценку эффективности не только на
// искусственных семействах (chain/star/block/random/one-hot из прошлых
// раундов, см. §5 в anf/README.md). Находки этого корпуса — anf/README.md
// §9 (главная — экспоненциальный взрыв anf_to_bdd на бент-функциях с
// "разнесённой" парой переменных, см. diag_bent_order() ниже).
//
// Источники "реальных" функций (все — по формуле/математическому
// определению, НЕ по таблице, вписанной в код по памяти, — см.
// benchmarks/scripts/generate_sbox.cpp за тем, почему переписывание
// стандартных S-box таблиц по памяти сознательно избегается в этом
// проекте):
//
//  1. AES S-box (n=8, 8 координатных функций) — таблица НЕ переписана по
//     памяти, а ВЫЧИСЛЕНА напрямую из математического определения Rijndael
//     (мультипликативный обратный в GF(2^8) = GF(2)[x]/(x^8+x^4+x^3+x+1),
//     затем аффинное преобразование с константой 0x63) прямо в этом файле
//     — та же процедура, что и в самом стандарте FIPS-197. Отдельно
//     сверено (вне этого файла, при подготовке корпуса) с независимым
//     алгебраическим вычислением на PowerShell и с первыми 7 строками
//     таблицы Wikipedia — совпадает побайтово.
//  2. Обобщённый шаг χ (Keccak/SHA-3, Ascon) — НЕ таблица, чистая формула:
//     out_i = a_i XOR a_{i+2} XOR a_{i+1}*a_{i+2} (индексы по модулю n).
//     Настоящий, стандартизованный (FIPS 202) нелинейный примитив,
//     обобщённый с канонических 5 бит на произвольный n (стандартная в
//     крипто-литературе техника изучения χ "at scale" — семейство χ_n).
//     Квадратичный, разреженный ANF (3 монома на выходной бит) — растёт до
//     n=64 без проблем.
//  3. Бент-функции Майорана-МакФарланда (IP-конструкция): f(x,y) =
//     XOR_i x_i * y_i, x,y — по n/2 переменных каждая. Каноническая
//     бент-функция из крипто-литературы (степень 2, n/2 мономов), стандартный
//     тестовый объект именно для BDD/decision-diagram инструментов (см.
//     литературу по cutwidth в anf/README.md §6). Чётные n от 8 до 64.
//
// Сборка (временный CMake-таргет добавлен вручную в конце этого запуска,
// не через основной CMakeLists.txt — см. команду в конце файла).

#include "anf/anf_to_aig.hpp"
#include "anf/anf_to_bdd.hpp"
#include "anf/anf_to_thr.hpp"
#include "anf/anf_to_tt.hpp"
#include "anf/tt_to_anf.hpp"

#include <sylvan_obj.hpp>

#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace bmm;

namespace {

// =============================================================================
// Методология замера — тот же паттерн, что во всех предыдущих раундах этого
// проекта (см. anf/README.md §5): 1 прогрев + медиана kRuns повторов.
// =============================================================================

constexpr int kRuns = 11;

double median_ms(const std::function<void()>& work) {
    work();  // прогрев, не входит в измерение
    std::vector<double> samples;
    samples.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        work();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

size_t count_monomials(const Anf& anf) {
#if BMM_HAVE_BRIAL
    size_t c = 0;
    for (auto it = anf.raw().begin(); it != anf.raw().end(); ++it) ++c;
    return c;
#else
    return anf.raw().monomials().size();
#endif
}

// =============================================================================
// AES S-box — ВЫЧИСЛЕН из математического определения (GF(2^8) + аффинное
// преобразование), не переписан по памяти. Возвращает Anf для одной
// координатной функции (output bit) через TruthTable -> tt_to_anf (это
// ОДНОВРЕМЕННО и способ получить вход для остальных 4 функций, и часть
// самого замера tt_to_anf).
// =============================================================================

uint8_t gf_mult(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        const uint8_t hi = a & 0x80;
        a = static_cast<uint8_t>(a << 1);
        if (hi) a ^= 0x1B;
        b = static_cast<uint8_t>(b >> 1);
    }
    return p;
}

uint8_t gf_inverse(uint8_t a) {
    if (a == 0) return 0;
    for (int b = 1; b < 256; ++b) {
        if (gf_mult(a, static_cast<uint8_t>(b)) == 1) return static_cast<uint8_t>(b);
    }
    return 0;
}

uint8_t aes_affine(uint8_t b) {
    uint8_t result = 0;
    for (int i = 0; i < 8; ++i) {
        const int bit = ((b >> i) & 1) ^ ((b >> ((i + 4) % 8)) & 1) ^ ((b >> ((i + 5) % 8)) & 1) ^
                         ((b >> ((i + 6) % 8)) & 1) ^ ((b >> ((i + 7) % 8)) & 1);
        result = static_cast<uint8_t>(result | (bit << i));
    }
    return static_cast<uint8_t>(result ^ 0x63);
}

std::vector<uint8_t> build_aes_sbox() {
    std::vector<uint8_t> sbox(256);
    for (int x = 0; x < 256; ++x) {
        sbox[x] = aes_affine(gf_inverse(static_cast<uint8_t>(x)));
    }
    return sbox;
}

// Одна координатная функция AES S-box (n=8) как TruthTable.
TruthTable aes_coordinate_tt(const std::vector<uint8_t>& sbox, int bit) {
    TruthTable tt(8);
    for (int x = 0; x < 256; ++x) {
        if ((sbox[static_cast<size_t>(x)] >> bit) & 1) kitty::set_bit(tt.raw(), static_cast<uint64_t>(x));
    }
    return tt;
}

// =============================================================================
// χ (Keccak/SHA-3/Ascon), обобщённый на n переменных: out_0 = x_0 XOR x_2 XOR
// x_1*x_2 (индексы mod n). Построен НАПРЯМУЮ как Anf (минуя truth table —
// работает при любом n, в т.ч. > kMaxTruthTableVars).
// =============================================================================

Anf build_chi(uint32_t n) {
#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n);
    BoolePolynomial poly(ring);
    poly += ring.variable(0);
    poly += ring.variable(2 % n);
    BooleMonomial mono(ring);
    mono *= ring.variable(1 % n);
    mono *= ring.variable(2 % n);
    poly += mono;
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    poly.add_monomial({0});
    poly.add_monomial({2 % n});
    Monomial m = {1 % n, 2 % n};
    std::sort(m.begin(), m.end());
    poly.add_monomial(std::move(m));
    return Anf(std::move(poly), n);
#endif
}

// =============================================================================
// Бент-функция Майорана-МакФарланда (IP-конструкция): f(x,y) = XOR x_i*y_i,
// i=0..n/2-1. Построена напрямую как Anf, чётный n.
// =============================================================================

Anf build_bent_mm(uint32_t n) {
    const uint32_t half = n / 2;
#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n);
    BoolePolynomial poly(ring);
    for (uint32_t i = 0; i < half; ++i) {
        BooleMonomial mono(ring);
        mono *= ring.variable(i);
        mono *= ring.variable(half + i);
        poly += mono;
    }
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    for (uint32_t i = 0; i < half; ++i) {
        poly.add_monomial({i, half + i});
    }
    return Anf(std::move(poly), n);
#endif
}

// То же самое f(x,y) = XOR x_i*y_i, но пары (x_i,y_i) занимают СОСЕДНИЕ
// индексы (2i, 2i+1) вместо (i, half+i) — единственное отличие от
// build_bent_mm(). Диагностика гипотезы "graph interaction — идеальное
// паросочетание (степень 1 у каждой вершины), FORCE/MinIndex/Degree на нём
// не сближают пару, а bent/IP-функция экспоненциальна по размеру BDD именно
// когда пара разнесена далеко в порядке переменных (классический факт из
// литературы по decision diagrams, см. anf/README.md §6 про cutwidth)" — см.
// diag_bent_order() ниже.
Anf build_bent_mm_interleaved(uint32_t n) {
    const uint32_t half = n / 2;
#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n);
    BoolePolynomial poly(ring);
    for (uint32_t i = 0; i < half; ++i) {
        BooleMonomial mono(ring);
        mono *= ring.variable(2 * i);
        mono *= ring.variable(2 * i + 1);
        poly += mono;
    }
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    for (uint32_t i = 0; i < half; ++i) {
        poly.add_monomial({2 * i, 2 * i + 1});
    }
    return Anf(std::move(poly), n);
#endif
}

// =============================================================================
// Диагностика находки "anf_to_bdd падает на bent_mm(n=48)" (см. anf/README.md
// §9): один целевой замер за один запуск процесса. Причина — отдельный
// процесс на каждый (n, вариант): у Lace fatal error (task stack overflow)
// нет способа поймать его внутри процесса (это abort()), поэтому если один
// размер уронит процесс, это не должно стоить результатов уже полученных
// меньших размеров — оркестрация (растущий n, остановка на первом крахе)
// делается снаружи, из shell-обвязки, а не из этого бинаря.
// =============================================================================

void diag_bent_order(uint32_t n, const std::string& variant) {
    Anf anf = (variant == "interleaved") ? build_bent_mm_interleaved(n) : build_bent_mm(n);
    const auto start = std::chrono::steady_clock::now();
    auto result = anf_to_bdd(anf);
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (!is_ok(result)) {
        std::printf("BMM_DIAG bent_order variant=%s n=%u FAILED\n", variant.c_str(), n);
        return;
    }
    const size_t node_count = value(result).raw().NodeCount();
    std::printf("BMM_DIAG bent_order variant=%s n=%u nodecount=%zu ms=%.3f\n", variant.c_str(), n,
                node_count, ms);
}

// =============================================================================
// Диагностика "какой профит от OpenMP-параллелизма в самом Мёбиус-
// трансформе" (см. anf/README.md §9.5) — ИЗОЛИРОВАННО от tt_to_anf()/
// anf_to_tt() целиком. Первая попытка измерить это через сами
// tt_to_anf_openmp_scaling/anf_to_tt_openmp_scaling (test_anf.cpp) дала шум
// (0.69-1.11x без закономерности) — подозрение: измерялся не только
// трансформ, а ВЕСЬ вызов, включая последовательную сборку BoolePolynomial
// из коэффициентов после трансформа (`poly += mono` на каждый ненулевой
// коэффициент) — при плотном входе (make_bench_function в test_anf.cpp:
// popcount(idx)>n/2, у порядка половины коэффициентов после трансформа
// ненулевые) эта часть могла доминировать по времени и по закону Амдала
// маскировать эффект от параллельного трансформа. Функции ниже —
// НАМЕРЕННАЯ копия mobius_transform_sequential/parallel из
// anf/tt_to_anf.cpp (тот же алгоритм, побайтово) — не переиспользуют
// оригинал, т.к. он в анонимном namespace .cpp и не виден отсюда; сделано
// специально ради изоляции: работает мимо tt_to_anf()/anf_to_tt() и мимо
// BRiAl вообще, чистая скорость самого трансформа на массиве.
namespace mobius_diag {

void transform_sequential(std::vector<uint8_t>& a, uint32_t n) {
    const size_t size = 1ULL << n;
    for (uint32_t i = 0; i < n; ++i) {
        const size_t bit = 1ULL << i;
        const size_t step = bit << 1;
        for (size_t j = 0; j < size; j += step) {
            for (size_t k = 0; k < bit; ++k) {
                a[j + k + bit] ^= a[j + k];
            }
        }
    }
}

void transform_parallel(std::vector<uint8_t>& a, uint32_t n) {
    const size_t size = 1ULL << n;
    #pragma omp parallel
    {
        for (uint32_t i = 0; i < n; ++i) {
            const size_t bit = 1ULL << i;
            const size_t step = bit << 1;
            #pragma omp for schedule(static)
            for (size_t j = 0; j < size; j += step) {
                for (size_t k = 0; k < bit; ++k) {
                    a[j + k + bit] ^= a[j + k];
                }
            }
        }
    }
}

std::vector<uint8_t> make_dense_coeffs(uint32_t n) {
    const uint64_t rows = uint64_t{1} << n;
    std::vector<uint8_t> a(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        a[idx] = static_cast<uint32_t>(__builtin_popcountll(idx)) > n / 2 ? 1 : 0;
    }
    return a;
}

// Изолированный замер трансформа: 1 поток vs omp_get_max_threads(), 1
// прогрев + медиана kRuns повторов (та же дисциплина, что и везде в этом
// файле). Возвращает {single_ms, parallel_ms}.
std::pair<double, double> bench_transform_only(uint32_t n) {
    auto base = make_dense_coeffs(n);
    const int default_threads = omp_get_max_threads();

    auto run_seq = [&] {
        auto a = base;
        transform_sequential(a, n);
    };
    auto run_par = [&] {
        auto a = base;
        transform_parallel(a, n);
    };

    // Прогрев на КАЖДУЮ сторону отдельно — omp_set_num_threads()
    // глобальное состояние, прогрев под чужим числом потоков не разогревает
    // нужный путь (тот же принцип, что measure_scaling_omp в
    // benchmarks/openmp_scaling.hpp).
    omp_set_num_threads(1);
    run_seq();
    std::vector<double> seq_samples;
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        run_seq();
        const auto end = std::chrono::steady_clock::now();
        seq_samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(seq_samples.begin(), seq_samples.end());

    omp_set_num_threads(default_threads);
    run_par();
    std::vector<double> par_samples;
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        run_par();
        const auto end = std::chrono::steady_clock::now();
        par_samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(par_samples.begin(), par_samples.end());

    return {seq_samples[seq_samples.size() / 2], par_samples[par_samples.size() / 2]};
}

// Изолированный замер ТОЛЬКО сборки BoolePolynomial/AnfFallback из уже
// готового (после трансформа) массива коэффициентов — то самое "что
// осталось непроверенным" из anf/README.md §9.3. Тот же плотный вход
// (make_dense_coeffs), но здесь коэффициенты используются НАПРЯМУЮ как
// результат трансформа (без реального трансформа) — синтетически, но это
// нормально: цена сборки зависит только от ТОГО, СКОЛЬКО коэффициентов
// ненулевые и какой у них mask, не от того, откуда массив взялся.
double bench_poly_construction_only(uint32_t n) {
    auto coeff = make_dense_coeffs(n);
    const uint64_t rows = uint64_t{1} << n;

    auto build = [&] {
#if BMM_HAVE_BRIAL
        BoolePolyRing ring(n);
        BoolePolynomial poly(ring);
        for (uint64_t mask = 0; mask < rows; ++mask) {
            if (!coeff[mask]) continue;
            BooleMonomial mono(ring);
            for (uint32_t var = 0; var < n; ++var) {
                if (mask & (1ULL << var)) mono *= ring.variable(var);
            }
            poly += mono;
        }
#else
        AnfFallback poly;
        for (uint64_t mask = 0; mask < rows; ++mask) {
            if (!coeff[mask]) continue;
            Monomial mono;
            for (uint32_t var = 0; var < n; ++var) {
                if (mask & (1ULL << var)) mono.push_back(var);
            }
            poly.add_monomial(std::move(mono));
        }
#endif
    };

    build();  // прогрев
    std::vector<double> samples;
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        build();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void run(uint32_t n) {
    const auto [single_ms, parallel_ms] = bench_transform_only(n);
    const double speedup = parallel_ms > 0.0 ? single_ms / parallel_ms : 0.0;
    const double construction_ms = bench_poly_construction_only(n);
    std::printf(
        "BMM_MOBIUS_DIAG n=%-3u transform_single_ms=%-10.4f transform_parallel_ms=%-10.4f "
        "speedup=%-8.4f poly_construction_ms=%.4f\n",
        n, single_ms, parallel_ms, speedup, construction_ms);
}

}  // namespace mobius_diag

// =============================================================================
// Замер одной функции трансляции на одном входе, с печатью строки в формате,
// сопоставимом с BMM_BENCH из benchmarks/scaling.hpp (но без TBB single/
// parallel — здесь сравниваются РАЗНЫЕ входы, не потоки).
// =============================================================================

template <class F, class In>
void bench_one(const std::string& label, uint32_t n, size_t n_mono, F&& fn, const In& input) {
    const double ms = median_ms([&] {
        auto r = fn(input);
        (void)r;
    });
    std::printf("BMM_REAL_BENCH %-14s n=%-3u monomials=%-6zu median_ms=%.4f\n", label.c_str(), n,
                n_mono, ms);
}

}  // namespace

// Sylvan/Lace требует, чтобы весь код, трогающий sylvan::Bdd (внутри
// anf_to_bdd), выполнялся внутри зарегистрированной Lace-задачи — иначе
// SIGSEGV в llmsset_lookup (тот же паттерн, что verify/test_main.cpp,
// см. комментарий там). run_all() — тело, run_all_task — обёртка VOID_TASK_0.

void run_all() {
    std::printf("=== anf/ real-corpus benchmark ===\n\n");

    // -------------------------------------------------------------------
    // 1. AES S-box, n=8, все 8 координатных функций.
    //    tt_to_anf замеряется здесь напрямую (это и есть построение входа).
    // -------------------------------------------------------------------
    std::printf("--- AES S-box (n=8, 8 output bits) ---\n");
    auto sbox = build_aes_sbox();
    for (int bit = 0; bit < 8; ++bit) {
        TruthTable tt = aes_coordinate_tt(sbox, bit);

        const double tt_to_anf_ms = median_ms([&] {
            auto r = tt_to_anf(tt);
            (void)r;
        });
        auto anf_result = tt_to_anf(tt);
        if (!is_ok(anf_result)) {
            std::printf("bit %d: tt_to_anf FAILED\n", bit);
            continue;
        }
        Anf anf = value(anf_result);
        const size_t n_mono = count_monomials(anf);
        std::printf("BMM_REAL_BENCH tt_to_anf     n=8   bit=%d monomials=%-6zu median_ms=%.4f\n",
                    bit, n_mono, tt_to_anf_ms);

        bench_one("anf_to_aig", 8, n_mono, [](const Anf& a) { return anf_to_aig(a); }, anf);
        bench_one("anf_to_bdd", 8, n_mono, [](const Anf& a) { return anf_to_bdd(a); }, anf);
        bench_one("anf_to_thr", 8, n_mono, [](const Anf& a) { return anf_to_thr(a); }, anf);
        bench_one("anf_to_tt", 8, n_mono, [](const Anf& a) { return anf_to_tt(a); }, anf);
    }

    // -------------------------------------------------------------------
    // 2. χ (Keccak/SHA-3/Ascon), обобщённый — растёт до n=64.
    //    anf_to_thr/anf_to_tt/tt_to_anf ограничены n<=20/24 контрактом
    //    функций (см. core/common.hpp kMaxTruthTableVars, anf_to_thr.cpp) —
    //    для них корпус урезан отдельно.
    // -------------------------------------------------------------------
    std::printf("\n--- chi (Keccak/SHA-3/Ascon), обобщённый на n переменных ---\n");
    for (uint32_t n : {5u, 10u, 16u, 20u, 24u, 32u, 48u, 64u}) {
        Anf anf = build_chi(n);
        const size_t n_mono = count_monomials(anf);

        bench_one("anf_to_aig", n, n_mono, [](const Anf& a) { return anf_to_aig(a); }, anf);
        bench_one("anf_to_bdd", n, n_mono, [](const Anf& a) { return anf_to_bdd(a); }, anf);
        // n<=16, не 20: n=20 chi/bent (не пороговые функции, солвер обязан
        // ДОКАЗАТЬ инфизибильность, а не просто найти решение) заняли 34с/точку
        // (chi) в первом прогоне этого бенчмарка — 12 повторов (1 прогрев + 11
        // измерений) на такую точку кладут весь прогон в тайм-аут. n=16 (1.7-5.5с
        // median) — уже достаточно, чтобы увидеть экспоненциальный рост.
        if (n <= 16) {
            bench_one("anf_to_thr", n, n_mono, [](const Anf& a) { return anf_to_thr(a); }, anf);
        }
        if (n <= 24) {
            bench_one("anf_to_tt", n, n_mono, [](const Anf& a) { return anf_to_tt(a); }, anf);
        }
    }

    // -------------------------------------------------------------------
    // 3. Бент-функции Майорана-МакФарланда (IP), чётный n до 64.
    // -------------------------------------------------------------------
    std::printf("\n--- bent (Maiorana-McFarland IP), чётный n ---\n");
    for (uint32_t n : {8u, 16u, 20u, 24u, 32u, 48u, 64u}) {
        Anf anf = build_bent_mm(n);
        const size_t n_mono = count_monomials(anf);

        bench_one("anf_to_aig", n, n_mono, [](const Anf& a) { return anf_to_aig(a); }, anf);
        // anf_to_bdd на build_bent_mm(n) — ТОЛЬКО до n=40: NodeCount растёт
        // как 2^(n/2+1)-1 (подтверждено diag_bent_order(), см. anf/README.md
        // §9 — x_i и y_i=x_{n/2+i} разнесены на n/2 позиций в порядке
        // переменных, ни одна из 4 эвристик не устраняет это для чистого
        // паросочетания). n=44 и n=48 воспроизводимо роняют весь процесс
        // (Lace fatal error: task stack overflow — abort(), не ловится).
        // n=40 уже занимает ~50с (2097151 узлов) — не гоняем даже это в
        // штатном run_all(), используйте `bent-diag <n> orig` явно, если
        // нужно именно это измерить.
        if (n <= 32) {
            bench_one("anf_to_bdd", n, n_mono, [](const Anf& a) { return anf_to_bdd(a); }, anf);
        } else {
            std::printf(
                "BMM_REAL_BENCH anf_to_bdd     n=%-3u ПРОПУЩЕНО (известный экспоненциальный "
                "взрыв на build_bent_mm, см. anf/README.md §9 — используйте bent-diag)\n",
                n);
        }
        // n<=16, не 20: n=20 chi/bent (не пороговые функции, солвер обязан
        // ДОКАЗАТЬ инфизибильность, а не просто найти решение) заняли 34с/точку
        // (chi) в первом прогоне этого бенчмарка — 12 повторов (1 прогрев + 11
        // измерений) на такую точку кладут весь прогон в тайм-аут. n=16 (1.7-5.5с
        // median) — уже достаточно, чтобы увидеть экспоненциальный рост.
        if (n <= 16) {
            bench_one("anf_to_thr", n, n_mono, [](const Anf& a) { return anf_to_thr(a); }, anf);
        }
        if (n <= 24) {
            bench_one("anf_to_tt", n, n_mono, [](const Anf& a) { return anf_to_tt(a); }, anf);
        }
    }

    std::printf("\n=== done ===\n");
}

int g_diag_n = 0;
std::string g_diag_variant;
bool g_diag_mode = false;

namespace {
VOID_TASK_0(run_all_task) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    if (g_diag_mode) {
        diag_bent_order(static_cast<uint32_t>(g_diag_n), g_diag_variant);
    } else {
        run_all();
    }

    sylvan::sylvan_quit();
}
}  // namespace

// Режимы запуска:
//   ./bench_real_corpus                         — полный корпус (см. run_all()).
//   ./bench_real_corpus bent-diag <n> <variant>  — один целевой замер
//     anf_to_bdd на build_bent_mm(n) (variant="orig") или
//     build_bent_mm_interleaved(n) (variant="interleaved"), см.
//     diag_bent_order()/anf/README.md §9. Один запуск — один процесс:
//     Lace fatal error (task stack overflow) — это abort(), поймать внутри
//     процесса нельзя, поэтому подбор границы взрыва делается снаружи
//     (растущий n, отдельный запуск на каждое значение).
//   ./bench_real_corpus mobius-diag <n>          — изолированный замер
//     Мёбиус-трансформа + сборки полинома, см. mobius_diag::run()/
//     anf/README.md §9.5. НЕ проходит через lace_start/RUN — этот режим не
//     трогает sylvan::Bdd вообще, а lace_start(0,0) сам поднимает по
//     воркеру на каждое ядро контейнера, которые бы конкурировали с
//     OpenMP-потоками за те же 8 ядер и искажали замер.
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "mobius-diag") {
        mobius_diag::run(static_cast<uint32_t>(std::stoi(argv[2])));
        return 0;
    }

    if (argc == 4 && std::string(argv[1]) == "bent-diag") {
        g_diag_mode = true;
        g_diag_n = std::stoi(argv[2]);
        g_diag_variant = argv[3];
    }

    const int n_workers = 0;
    const size_t deque_size = 0;
    lace_start(n_workers, deque_size);
    RUN(run_all_task);
    lace_stop();
    return 0;
}
