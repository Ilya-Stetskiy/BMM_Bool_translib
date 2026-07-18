// verify/ground_truth/ground_truth.hpp — независимый источник истины.
//
// Намеренно не включает core/common.hpp и не использует kitty/mockturtle/
// sylvan/BRiAl: если бы эталонные значения функции считались тем же кодом
// (kitty::dynamic_truth_table), который использует TruthTable в common.hpp,
// баг в этом общем коде остался бы незамеченным при сравнении "перевод vs
// эталон". Эталон здесь — голый std::vector<bool>, посчитанный элементарной
// битовой арифметикой.
//
// Проверяется тестами до kMaxGroundTruthVars (n<=16 — жёсткий лимит для
// exhaustive-тестов в CI, зафиксирован явно во второй итерации задания;
// сознательно меньше kMaxTruthTableVars=24 в common.hpp: 2^16=65536 точек
// перебираются на каждую из 20 функций трансляции в каждом прогоне PR,
// 2^20 и тем более 2^24 были бы уже заметно дороже без дополнительной
// пользы для покрытия багов на каждый PR. Проверка корректности на большем
// n, если она вообще нужна, — это не задача exhaustive-теста; используйте
// benchmarks/ для того, чтобы гонять функции на больших n не по
// корректности, а по производительности).

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bmm::verify {

inline constexpr uint32_t kMaxGroundTruthVars = 16;

// LSB_FIRST-декодирование индекса минтерма в присвоение переменным — та же
// конвенция, что и BitOrder::LSB_FIRST в core/common.hpp (см. CONVENTIONS.md
// core/), но выражена здесь заново, без #include common.hpp.
inline std::vector<bool> decode_assignment(uint64_t index, uint32_t n_vars) {
    std::vector<bool> assignment(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) assignment[i] = (index >> i) & 1u;
    return assignment;
}

// Эталонная функция малой размерности: её значения известны по построению
// (не вычислены переводом из другого представления).
struct GroundTruthFunction {
    std::string name;         // для сообщений об ошибках, напр. "xor3", "random_n5_seed7"
    uint32_t n_vars;
    std::vector<bool> table;  // table[index] — значение на точке decode_assignment(index, n_vars)

    bool evaluate(uint64_t index) const { return table.at(index); }
};

// Растущий набор тестовых функций от n=1 до max_n включительно: для каждого n
// — константы 0/1, все n проекций (x_i), AND-всех, OR-всех, XOR-всех
// (parity), MAJ (при нечётном n), и несколько псевдослучайных функций на
// детерминированном seed (воспроизводимость прогонов CI). Это единственное
// место, где решается "сколько и каких функций тестировать" — test_runner.hpp
// просто перебирает то, что здесь возвращено.
std::vector<GroundTruthFunction> growing_test_functions(uint32_t max_n = kMaxGroundTruthVars);

// Точечная сверка: перебирает все 2^gt.n_vars точек, на каждой сравнивает
// gt.evaluate(index) с evaluate_under_test(assignment). При первом
// расхождении возвращает индекс несовпавшей точки (для печати контрпримера
// в test_runner.hpp), иначе std::nullopt.
std::optional<uint64_t> find_mismatch(
    const GroundTruthFunction& gt,
    const std::function<bool(const std::vector<bool>&)>& evaluate_under_test);

}  // namespace bmm::verify
