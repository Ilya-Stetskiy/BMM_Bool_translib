#include "tt_to_anf.hpp"

#include <tracy/Tracy.hpp>

#include <vector>

namespace bmm {

namespace {

// Ниже которого размера (в элементах) параллелить преобразование Мёбиуса
// не имеет смысла: накладные расходы на synchронизацию/omp for-барьеры
// перевешивают полезную работу. Тот же порог и та же реализация, что и в
// anf_to_tt.cpp — это буквально тот же трансформ (Мёбиус/Жегалкин над GF(2)
// — инволюция), применённый в обратную сторону, см. anf_to_tt.hpp.
constexpr size_t kParallelThreshold = 1ULL << 18;

void mobius_transform_sequential(std::vector<uint8_t>& a, uint32_t n)
{
    const size_t size = 1ULL << n;

    for (uint32_t i = 0; i < n; ++i)
    {
        const size_t bit = 1ULL << i;
        const size_t step = bit << 1;

        for (size_t j = 0; j < size; j += step)
        {
            for (size_t k = 0; k < bit; ++k)
            {
                a[j + k + bit] ^= a[j + k];
            }
        }
    }
}

void mobius_transform_parallel(std::vector<uint8_t>& a, uint32_t n)
{
    const size_t size = 1ULL << n;

    // Один parallel-регион на весь трансформ, а не n отдельных регионов:
    // создание/роспуск thread team стоит десятки-сотни микросекунд и при
    // n=24 (24 региона на вызов) на большом числе тестов даёт заметный
    // паразитный оверхед между тестами. #pragma omp for внутри имеет
    // неявный барьер в конце, что и нужно — уровни трансформа зависимы
    // по данным и обязаны идти строго последовательно.
    #pragma omp parallel
    {
        for (uint32_t i = 0; i < n; ++i)
        {
            const size_t bit = 1ULL << i;
            const size_t step = bit << 1;

            // Параллелизм на уровне блоков, а не отдельных элементов.
            // Это устраняет накладные расходы OpenMP на уровне элементов
            // и гарантирует последовательный доступ к памяти внутри блока,
            // что эффективно использует кэш-линии и аппаратную предвыборку.
            #pragma omp for schedule(static)
            for (size_t j = 0; j < size; j += step)
            {
                for (size_t k = 0; k < bit; ++k)
                {
                    a[j + k + bit] ^= a[j + k];
                }
            }
        }
    }
}

void mobius_transform_inplace(std::vector<uint8_t>& a, uint32_t n)
{
    const size_t size = 1ULL << n;

    if (size < kParallelThreshold)
    {
        mobius_transform_sequential(a, n);
    }
    else
    {
        mobius_transform_parallel(a, n);
    }
}

} // namespace

Result<Anf> tt_to_anf(const TruthTable& tt)
{
    ZoneScoped;

    const uint32_t n = tt.n_vars();

    if (n > kMaxTruthTableVars)
    {
        return fail<Anf>(
            ErrorCode::TooManyVariables,
            "tt_to_anf: too many variables");
    }


    const uint64_t rows = 1ULL << n;


    // ===================================
    // Копирование truth table
    // ===================================

    std::vector<uint8_t> coeff(rows, 0);

    auto& raw_tt = tt.raw();

    for (uint64_t i = 0; i < rows; ++i)
    {
        coeff[i] = kitty::get_bit(raw_tt, i);
    }


    // ===================================
    // Обратное преобразование Мёбиуса
    // TT -> ANF coefficients
    // ===================================

    mobius_transform_inplace(coeff, n);


    // ===================================
    // Построение полинома (BRiAl BoolePolynomial либо fallback AnfFallback
    // — см. core/anf_repr.hpp)
    // ===================================

#if BMM_HAVE_BRIAL
    BoolePolynomial::ring_type ring(n);
    BoolePolynomial poly(ring);

    // ИСПРАВЛЕНО (см. anf/README.md §5.3/§9.5, thr/README.md §6.1/§6.2 —
    // тот же антипаттерн уже был найден и исправлен в thr_to_anf.cpp):
    // раньше каждый моном строился через n последовательных умножений
    // (mono *= ring.variable(var)) — O(n) операций BoolePolynomial::operator*
    // на КАЖДЫЙ из до 4.4М мономов при n=24; собственное измерение в
    // anf/README.md §5.3 показывает, что именно эта сборка (не сам
    // Мёбиус-трансформ выше) доминирует по времени на плотных функциях
    // (10.56с из ~10.6с общих на n=22). BooleExponent (вектор индексов) +
    // BooleMonomial(exp, ring) строит моном за один вызов вместо n умножений.
    for (uint64_t mask = 0; mask < rows; ++mask)
    {
        if (!coeff[mask])
            continue;

        polybori::BooleExponent exp;
        exp.reserve(n);

        for (uint32_t var = 0; var < n; ++var)
        {
            if (mask & (1ULL << var))
            {
                exp.push_back(var);
            }
        }

        poly += BooleMonomial(exp, ring);
    }

    return ok<Anf>(Anf(std::move(poly), n));
#else
    AnfFallback poly;

    for (uint64_t mask = 0; mask < rows; ++mask)
    {
        if (!coeff[mask])
            continue;

        Monomial mono;
        for (uint32_t var = 0; var < n; ++var)
        {
            if (mask & (1ULL << var))
            {
                mono.push_back(var);
            }
        }

        poly.add_monomial(std::move(mono));
    }

    return ok<Anf>(Anf(std::move(poly), n));
#endif
}


} // namespace bmm