#include "anf_to_tt.hpp"

#include <tracy/Tracy.hpp>

#include <vector>

namespace bmm {

namespace {

// Ниже которого размера (в элементах) параллелить преобразование Мёбиуса
// не имеет смысла: накладные расходы на synchронизацию/omp for-барьеры
// перевешивают полезную работу. Порог подобран эмпирически, при желании
// можно вынести в конфиг/бенчмарк.
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

Result<TruthTable> anf_to_tt(const Anf& anf)
{
    ZoneScoped;

    const uint32_t n = anf.n_vars();

    if (n > kMaxTruthTableVars)
    {
        return fail<TruthTable>(
            ErrorCode::TooManyVariables,
            "anf_to_tt: too many variables");
    }

    const uint64_t rows = 1ULL << n;

    // uint8_t вместо uint64_t: сокращает потребление памяти в 8 раз
    // (для n=24: 128 МБ -> 16 МБ), что позволяет данным поместиться в
    // L3-кэш процессора.
    std::vector<uint8_t> values(rows, 0);

    // =============================
    // Обход полинома BRiAl
    // =============================
    for (auto it = anf.raw().begin(); it != anf.raw().end(); ++it)
    {
        uint64_t mask = 0;
        for (auto var : *it)
        {
            mask |= (1ULL << var);
        }
        values[mask] ^= 1;
    }

    // =============================
    // Преобразование Мёбиуса
    // =============================
    mobius_transform_inplace(values, n);

    // =============================
    // Запись TT
    // =============================
    TruthTable tt(n);
    auto& raw_tt = tt.raw();

    for (uint64_t i = 0; i < rows; ++i)
    {
        if (values[i])
        {
            kitty::set_bit(raw_tt, i);
        }
    }

    return ok<TruthTable>(std::move(tt));
}

} // namespace bmm