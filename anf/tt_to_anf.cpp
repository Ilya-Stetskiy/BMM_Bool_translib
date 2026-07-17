#include "tt_to_anf.hpp"

#include <tracy/Tracy.hpp>

#include <vector>

namespace bmm {

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

    for (uint32_t i = 0; i < n; ++i)
    {
        const uint64_t bit = 1ULL << i;
        const uint64_t step = bit << 1;

        for (uint64_t j = 0; j < rows; j += step)
        {
            for (uint64_t k = 0; k < bit; ++k)
            {
                coeff[j + k + bit] ^= coeff[j + k];
            }
        }
    }


    // ===================================
    // Построение BRiAl polynomial
    // ===================================

    // ===================================
// Построение BRiAl polynomial
// ===================================

BoolePolynomial::ring_type ring(n);
BoolePolynomial poly(ring);


for (uint64_t mask = 0; mask < rows; ++mask)
{
    if (!coeff[mask])
        continue;


    BooleMonomial mono(ring);


    for (uint32_t var = 0; var < n; ++var)
    {
        if (mask & (1ULL << var))
        {
            mono *= ring.variable(var);
        }
    }


    poly += mono;
}


    return ok<Anf>(Anf(std::move(poly), n));
}


} // namespace bmm