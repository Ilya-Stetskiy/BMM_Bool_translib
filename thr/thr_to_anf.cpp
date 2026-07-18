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
        // ИСПРАВЛЕНО (см. thr/README.md §6.1/§6.2): раньше OpenMP включался
        // безусловно, даже для n=8 (256 точек) — накладные расходы fork/join
        // давали speedup=0.058 (в 17 раз медленнее последовательной версии).
        // Отсекаем накладные расходы OpenMP для массивов до n=16 (size <= 65536) включительно.
        #pragma omp parallel for if(size > 65536)
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
        // ИСПРАВЛЕНО (см. thr/README.md §6.1/§6.2): раньше #pragma omp parallel for
        // стоял на ВНЕШНЕМ цикле по блокам — на последнем уровне трансформа
        // (step=size/2) внешний цикл имел РОВНО одну итерацию, и весь этот
        // уровень (полноценная 1/log2(size) доля общей работы) выполнялся на
        // одном потоке, пока остальные простаивали; плюс log2(size) отдельных
        // omp-регионов на вызов вместо одного. Теперь — один parallel-регион на
        // весь трансформ (роспуск/создание thread team не на каждый уровень) +
        // collapse(2), сливающий блочный и внутриблочный циклы в одно
        // итерационное пространство — устраняет и накладные расходы, и
        // дисбаланс последнего уровня.
        #pragma omp parallel if(size > 65536)
        {
            for (uint32_t step = 1; step < size; step <<= 1) {
                uint64_t step_x2 = step << 1;

                #pragma omp for collapse(2) schedule(static)
                for (uint64_t i = 0; i < size; i += step_x2) {
                    for (uint32_t j = 0; j < step; ++j) {
                        tt[i + j + step] ^= tt[i + j];
                    }
                }
            }
        }

        // 3. Сборка ANF из полученных коэффициентов Жегалкина
        // (Выполняется строго последовательно, что гарантирует безопасность CUDD/PolyBoRi)
        //
        // ИСПРАВЛЕНО (см. thr/README.md §6.1/§6.2, тот же паттерн, что и
        // anf/README.md §5.3/§9.5 для tt_to_anf): раньше каждый моном строился
        // через n последовательных умножений (m = m * ring.variable(b)) — O(n)
        // операций BoolePolynomial::operator* на каждый из до 4.4М мономов при
        // n=24. Теперь — polybori::BooleExponent (вектор индексов переменных) +
        // BooleMonomial(vars, ring) за один вызов вместо n умножений. Число
        // мономов на выходе не меняется (проверено на реальном корпусе,
        // thr/README.md §6.2) — меняется только скорость построения.
#if BMM_HAVE_BRIAL
        polybori::BoolePolyRing ring(n);
        polybori::BoolePolynomial poly(ring.zero());

        for (uint64_t i = 0; i < size; ++i) {
            if (tt[i]) {
                // Создаем BooleExponent заново на каждой итерации.
                // У него есть reserve() и push_back(), но нет clear(),
                // поэтому локальное создание — самый чистый обход API PolyBoRi.
                polybori::BooleExponent vars;
                vars.reserve(n);

                // Заполняем индексы в порядке возрастания
                for (uint32_t b = 0; b < n; ++b) {
                    if ((i >> b) & 1) {
                        vars.push_back(b);
                    }
                }

                // Создаем моном за один вызов, обходя C++ operator*
                poly = poly + polybori::BooleMonomial(vars, ring);
            }
        }
        return ok(Anf(std::move(poly), n));
#else
        // Fallback-реализация для систем без BRIAL/PolyBoRi
        AnfFallback p;
        for (uint64_t i = 0; i < size; ++i) {
            if (tt[i]) {
                std::vector<uint32_t> vars;
                vars.reserve(n);
                for (uint32_t b = 0; b < n; ++b) {
                    if ((i >> b) & 1) {
                        vars.push_back(b);
                    }
                }
                p.add_monomial(vars);
            }
        }
        return ok(Anf(std::move(p), n));
#endif
    } catch (const std::bad_alloc&) {
        return fail<Anf>(ErrorCode::OutOfMemory, "thr_to_anf: исчерпана память");
    } catch (const std::exception& e) {
        return fail<Anf>(ErrorCode::InvalidArgument, e.what());
    }
}

} // namespace bmm
