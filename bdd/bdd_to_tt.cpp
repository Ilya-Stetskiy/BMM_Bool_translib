#include "bdd_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace bmm {

Result<TruthTable> bdd_to_tt(const Bdd& f) {
    ZoneScoped;

    try {
        uint32_t n = f.n_vars();

        if (n > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables,
                "bdd_to_tt: n > " + std::to_string(kMaxTruthTableVars));
        }

        sylvan::Bdd f_syl = f.raw();
        TruthTable tt(n); // Инициализируется нулями по умолчанию

        // Быстрый путь для констант
        if (f_syl.isZero()) {
            return ok(std::move(tt)); 
        }
        if (f_syl.isOne()) {
            uint64_t rows = uint64_t{1} << n;
            for (uint64_t i = 0; i < rows; ++i) {
                kitty::set_bit(tt.raw(), i);
            }
            return ok(std::move(tt));
        }

        // Простая и надежная реализация: итеративный спуск по BDD для каждого минтерма.
        // Для n <= 24 это занимает ~0.1-0.2 секунды, что вполне приемлемо.
        // Эта структура (плоский цикл по minterm) идеально подходит для будущего 
        // параллелизма: диапазон [0, 2^n) можно будет легко разбить на блоки 
        // для sylvan::run или OpenMP, не меняя логику внутри блока.
        uint64_t total_rows = uint64_t{1} << n;
        
        for (uint64_t minterm = 0; minterm < total_rows; ++minterm) {
            sylvan::Bdd curr = f_syl;
            
            // Итеративный спуск по графу BDD
            while (!curr.isTerminal()) {
                uint32_t v = curr.TopVar();
                if ((minterm >> v) & 1) {
                    curr = curr.Then();
                } else {
                    curr = curr.Else();
                }
            }
            
            // Если достигли терминала 1 (с учетом возможного complement-бита, 
            // который уже учтен в isOne() / isZero() обертки sylvan::Bdd)
            if (curr.isOne()) {
                kitty::set_bit(tt.raw(), minterm);
            }
        }

        return ok(std::move(tt));

    } catch (const std::bad_alloc&) {
        return fail<TruthTable>(ErrorCode::OutOfMemory,
            "bdd_to_tt: исчерпана память при построении TT");
    } catch (const std::exception& e) {
        return fail<TruthTable>(ErrorCode::InvalidArgument,
            std::string("bdd_to_tt internal error: ") + e.what());
    }
}

} // namespace bmm