#include "thr_to_anf.hpp"
#include "../core/common.hpp"

#include <tracy/Tracy.hpp>
#include <vector>
#include <omp.h>

namespace bmm {

Result<Anf> thr_to_anf(const Thr& thr) {
    ZoneScoped;

    try {
        uint32_t n = thr.n_vars();

        // Базовый случай для функции без переменных
        if (n == 0) {
            bool is_one = (thr.theta() <= 0);
#if BMM_HAVE_BRIAL
            polybori::BoolePolyRing ring(1); // Кольцо требует хотя бы 1 переменную
            return ok(Anf(is_one ? ring.one() : ring.zero(), 0));
#else
            AnfFallback p;
            if (is_one) p.add_monomial({});
            return ok(Anf(std::move(p), 0));
#endif
        }

        // Защита от исчерпания памяти при построении плоского массива (согласно контракту)
        if (n > kMaxTruthTableVars) {
            return fail<Anf>(ErrorCode::OutOfMemory, "thr_to_anf: n > 24 exceeds flat array memory limits");
        }

        uint64_t size = 1ull << n;
        
        // Используем int8_t вместо std::vector<bool> для безопасного конкурентного 
        // доступа в OpenMP (std::vector<bool> упаковывает биты и ломает потокобезопасность!)
        std::vector<int8_t> tt(size, 0);

        const auto& w = thr.weights();
        int64_t theta = thr.theta();

        // 1. Построение таблицы истинности (OpenMP над плоским массивом, Правило №3)
        #pragma omp parallel for
        for (uint64_t i = 0; i < size; ++i) {
            int64_t sum = 0;
            for (uint32_t b = 0; b < n; ++b) {
                if ((i >> b) & 1) {
                    sum += w[b];
                }
            }
            tt[i] = (sum >= theta) ? 1 : 0;
        }

        // 2. Быстрое преобразование Мёбиуса in-place (OpenMP над плоским массивом)
        for (uint32_t step = 1; step < size; step <<= 1) {
            #pragma omp parallel for
            for (uint64_t i = 0; i < size; i += (step << 1)) {
                for (uint32_t j = 0; j < step; ++j) {
                    tt[i + j + step] ^= tt[i + j];
                }
            }
        }

        // 3. Сборка ANF из полученных коэффициентов Жегалкина
        // (Выполняется строго последовательно, что гарантирует безопасность CUDD/PolyBoRi)
#if BMM_HAVE_BRIAL
        polybori::BoolePolyRing ring(n);
        polybori::BoolePolynomial poly(ring.zero());
        
        for (uint64_t i = 0; i < size; ++i) {
            if (tt[i]) {
                polybori::BoolePolynomial m(ring.one());
                for (uint32_t b = 0; b < n; ++b) {
                    if ((i >> b) & 1) {
                        m = m * ring.variable(b);
                    }
                }
                poly = poly + m;
            }
        }
        return ok(Anf(std::move(poly), n));
#else
        AnfFallback poly;
        for (uint64_t i = 0; i < size; ++i) {
            if (tt[i]) {
                Monomial m;
                for (uint32_t b = 0; b < n; ++b) {
                    if ((i >> b) & 1) m.push_back(b);
                }
                poly.add_monomial(m); // add_monomial делает std::sort, вектор уже отсортирован
            }
        }
        return ok(Anf(std::move(poly), n));
#endif

    } catch (const std::bad_alloc&) {
        // Правило 2а: Graceful degradation при нехватке памяти 
        return fail<Anf>(ErrorCode::OutOfMemory, "OutOfMemory: исчерпана память при построении ANF");
    } catch (...) {
        return fail<Anf>(ErrorCode::OutOfMemory, "OutOfMemory/Exception: неизвестная ошибка аллокации");
    }
}

} // namespace bmm