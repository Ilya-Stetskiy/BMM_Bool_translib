#include "anf_to_tt.hpp"

#include <tracy/Tracy.hpp>

#include <vector>


namespace bmm {

namespace {


void mobius_transform_inplace(
    std::vector<uint64_t>& a,
    uint32_t n)
{
    const uint64_t size = uint64_t{1} << n;


    for (uint32_t i = 0; i < n; ++i)
    {
        const uint64_t bit = uint64_t{1} << i;


        #pragma omp parallel for
        for (uint64_t mask = 0; mask < size; ++mask)
        {
            if (mask & bit)
            {
                a[mask] ^= a[mask ^ bit];
            }
        }
    }
}




}


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


    const uint64_t rows = uint64_t{1} << n;


    // a[mask] = коэффициент монома
    std::vector<uint64_t> values(rows, 0);



    for (auto it = anf.raw().begin();
     it != anf.raw().end();
     ++it)
{
    auto mono = *it;

    uint64_t mask = 0;

    for (auto var : mono)
    {
        mask |= (uint64_t{1} << var);
    }

    values[mask] ^= 1;
}



    // Mobius: коэффициенты ANF -> значения функции
    mobius_transform_inplace(values, n);



    TruthTable tt(n);


    for (uint64_t i = 0; i < rows; ++i)
    {
        if (values[i])
        {
            kitty::set_bit(tt.raw(), i);
        }
    }


    return ok<TruthTable>(std::move(tt));
}


} // namespace bmm