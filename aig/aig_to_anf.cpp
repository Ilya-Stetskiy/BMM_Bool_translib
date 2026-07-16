#include "aig_to_anf.hpp"

#include <tracy/Tracy.hpp>
#include <vector>

namespace bmm {

Result<Anf> aig_to_anf(const Aig& aig) {
    ZoneScoped;
    
    // 1. Переводим AIG в TruthTable (используя встроенную симуляцию mockturtle)
    auto tt_res = aig.to_tt();
    if (!is_ok(tt_res)) {
        return fail<Anf>(error(tt_res).code, error(tt_res).message);
    }
    const auto& tt = value(tt_res);
    const uint32_t n = aig.n_vars();
    const uint64_t rows = uint64_t{1} << n;

    // 2. Копируем значения таблицы истинности
    std::vector<uint8_t> a(rows);
    #pragma omp parallel for
    for (uint64_t idx = 0; idx < rows; ++idx) {
        a[idx] = kitty::get_bit(tt.raw(), idx) ? 1 : 0;
    }

    // 3. Выполняем быстрое преобразование Мёбиуса (Fast Mobius Transform)
    for (uint32_t i = 0; i < n; ++i) {
        #pragma omp parallel for
        for (uint64_t mask = 0; mask < rows; ++mask) {
            if ((mask >> i) & 1u) {
                a[mask] ^= a[mask ^ (uint64_t{1} << i)];
            }
        }
    }

    // 4. Собираем ANF-полином на основе коэффициентов Мёбиуса
#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n == 0 ? 1 : n);
    BoolePolynomial poly(ring);
    for (uint64_t mask = 0; mask < rows; ++mask) {
        if (!a[mask]) continue;
        BooleMonomial mono(ring);
        for (uint32_t i = 0; i < n; ++i) {
            if ((mask >> i) & 1u) {
                mono *= ring.variable(i);
            }
        }
        poly += mono;
    }
    return ok<Anf>(Anf(std::move(poly), n));
#else
    AnfFallback poly;
    for (uint64_t mask = 0; mask < rows; ++mask) {
        if (!a[mask]) continue;
        Monomial m;
        for (uint32_t i = 0; i < n; ++i) {
            if ((mask >> i) & 1u) {
                m.push_back(i);
            }
        }
        poly.add_monomial(std::move(m));
    }
    return ok<Anf>(Anf(std::move(poly), n));
#endif
}

}  // namespace bmm
