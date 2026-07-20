#include "thr/thr_to_aig.hpp"

#include <mockturtle/networks/aig.hpp>

#include <vector>
#include <algorithm>
#include <cstdint>
#include <array>
#include <functional>
#include <unordered_map>

namespace bmm {

using Signal = mockturtle::aig_network::signal;

// Прячем вспомогательные структуры в анонимный namespace,
// чтобы они были видны только внутри этого .cpp файла
namespace {
    struct FA_Input {
        uint32_t a, b, c;
        bool operator==(const FA_Input& other) const {
            return a == other.a && b == other.b && c == other.c;
        }
    };

    // ИСПРАВЛЕНО: раньше это был TBB-совместимый HashCompare (.hash()/.equal())
    // для tbb::concurrent_hash_map — теперь просто std::hash-функтор
    // (operator()) для std::unordered_map, см. большой комментарий у
    // build_tree ниже про переход на полностью последовательную сборку.
    struct FA_Hash {
        std::size_t operator()(const FA_Input& i) const {
            return std::hash<uint32_t>()(i.a) ^
                  (std::hash<uint32_t>()(i.b) << 1) ^
                  (std::hash<uint32_t>()(i.c) << 2);
        }
    };
} // namespace

Result<Aig> thr_to_aig(const Thr& thr) {
    try {
        const uint32_t n = thr.n_vars();
        mockturtle::aig_network ntk;
        
        if (n == 0) {
            auto sig = ntk.get_constant(thr.theta() <= 0);
            ntk.create_po(sig);
            return ok<Aig>(Aig{ntk});
        }

        std::vector<Signal> pis(n);
        for (uint32_t i = 0; i < n; ++i) {
            pis[i] = ntk.create_pi();
        }

        // 1. Приведение к неотрицательным весам
        int64_t theta_adj = thr.theta();
        std::vector<uint64_t> pos_weights(n);
        std::vector<Signal> var_signals(n);
        
        uint64_t max_sum = 0;

        for (uint32_t i = 0; i < n; ++i) {
            int64_t current_weight = thr.weights()[i];
            if (current_weight < 0) {
                // Замена x_i -> (1 - x_i)
                pos_weights[i] = static_cast<uint64_t>(-current_weight);
                theta_adj -= current_weight;
                var_signals[i] = ntk.create_not(pis[i]);
            } else {
                pos_weights[i] = static_cast<uint64_t>(current_weight);
                var_signals[i] = pis[i];
            }
            max_sum += pos_weights[i];
        }

        // Раннее отсечение
        if (theta_adj <= 0) {
            auto sig = ntk.get_constant(true);
            ntk.create_po(sig);
            return ok<Aig>(Aig{ntk});
        }
        if (theta_adj > static_cast<int64_t>(max_sum)) {
            auto sig = ntk.get_constant(false);
            ntk.create_po(sig);
            return ok<Aig>(Aig{ntk});
        }

        // Разрядность для хранения сумм (B)
        int B = max_sum > 0 ? (64 - __builtin_clzll(max_sum)) : 1;

        // ИСПРАВЛЕНО (см. thr/README.md §5 — большой раздел с честным
        // замером и историей обеих итераций фикса): изначально дерево
        // сумматоров строилось через TBB `tbb::task_group` (правило 2
        // core/CONVENTIONS.md п.6 формально требует TBB здесь). Level 1
        // (порог гранулярности `kMinParallelChunk` + консолидация 9 локов в
        // 1 на сумматор) был реализован и подтверждён на числах, но при
        // независимой ПОВТОРНОЙ проверке `benchmarks/large_scale_bench.cpp`
        // честно показал: на n=50 (ниже порога, TBB-задачи вообще не
        // спавнились) — паритет 1.01x, но начиная с n=100 (порог пройден,
        // задачи реально спавнятся) — снова 3-5 раз МЕДЛЕННЕЕ (0.19x-0.30x),
        // причём БЕЗ улучшения при росте n вплоть до n=2000. Level 2
        // (шардинг на изолированные локальные сети + слияние) рассмотрен и
        // отклонён отдельно — mockturtle не даёт быстрого примитива слияния
        // сетей, любой перенос узлов стоит столько же, сколько исходное
        // создание (`create_and`), то есть удвоил бы работу вместо
        // распараллеливания её.
        //
        // Корень проблемы — не гранулярность и не число локов, а сам факт,
        // что `mockturtle::aig_network::create_and` не потокобезопасен и
        // ТРЕБУЕТ единственного общего мьютекса на ВСЮ сеть: как только
        // несколько поддеревьев реально исполняются параллельно, они
        // сериализуются на этом одном мьютексе при каждом гейте — это
        // contention, а не оверхед на сам lock/unlock, и он НЕ уменьшается
        // с ростом n (наоборот, больше параллельных поддеревьев — больше
        // конкуренции за тот же один мьютекс). Ни на одном протестированном
        // размере (n=50..2000, три независимых прогона) не нашлось точки,
        // где параллельный путь стабильно обгоняет последовательный.
        //
        // Решение: как и `aig_to_anf`/`anf_to_aig` (см. aig/README.md §1.3,
        // anf/README.md §4 — тот же класс находки, BRiAl вместо
        // mockturtle-мьютекса), `thr_to_aig` сознательно НЕ использует
        // TBB — построение дерева сумматоров полностью последовательное.
        // Это единственный способ ГАРАНТИРОВАННО не проигрывать
        // последовательной версии, раз честного выигрыша от параллелизма
        // при данной архитектуре mockturtle не существует ни при каком
        // протестированном n. Как следствие, мьютекс и TBB concurrent
        // hash map тоже не нужны — единственный поток, который когда-либо
        // мутирует `ntk` или `fa_memo`, всегда один и тот же.
        auto not_ = [&](Signal a) {
            return ntk.create_not(a);
        };

        auto and_ = [&](Signal a, Signal b) {
            return ntk.create_and(a, b);
        };

        auto or_ = [&](Signal a, Signal b) {
            return not_(and_(not_(a), not_(b)));
        };

        auto xor_ = [&](Signal a, Signal b) {
            Signal term1 = and_(a, not_(b));
            Signal term2 = and_(not_(a), b);
            return or_(term1, term2);
        };

        using FAMemoMap = std::unordered_map<FA_Input, std::pair<Signal, Signal>, FA_Hash>;
        FAMemoMap fa_memo;

        // Полный сумматор с мемоизацией.
        //
        // ИСПРАВЛЕНО: ключ раньше строился из ntk.node_to_index(ntk.get_node(s))
        // — это отбрасывает complement-бит сигнала, т.к. get_node() возвращает
        // один и тот же узел и для s, и для !s. full_adder(a,b,cin) и
        // full_adder(!a,b,cin) в общем случае дают РАЗНЫЙ результат (sum
        // инвертируется, но cout = maj(a,b,c) не связан с maj(!a,b,c) простой
        // инверсией — см. thr/README.md, находка про потерю полярности) — при
        // совпадении отсортированных индексов узлов, но разной полярности
        // хотя бы одного входа, старый ключ мог тихо вернуть результат для
        // ЧУЖОЙ комбинации полярностей. Кодируем индекс и полярность вместе
        // (idx<<1 | complement) ДО сортировки — sum/cout симметричны при
        // перестановке любой пары входов (a,b,cin), поэтому сортировка по
        // такому расширенному ключу остаётся корректной канонизацией, просто
        // больше не теряет полярность.
        auto encode_signal = [&](Signal s) -> uint32_t {
            uint32_t idx = ntk.node_to_index(ntk.get_node(s));
            return (idx << 1) | (ntk.is_complemented(s) ? 1u : 0u);
        };

        auto full_adder = [&](Signal a, Signal b, Signal cin) -> std::pair<Signal, Signal> {
            std::array<uint32_t, 3> idxs = {encode_signal(a), encode_signal(b), encode_signal(cin)};
            std::sort(idxs.begin(), idxs.end());
            FA_Input key{idxs[0], idxs[1], idxs[2]};

            auto it = fa_memo.find(key);
            if (it != fa_memo.end()) {
                return it->second;
            }

            Signal s1 = xor_(a, b);
            Signal sum = xor_(s1, cin);

            Signal c1 = and_(a, b);
            Signal c2 = and_(cin, s1);
            Signal cout = or_(c1, c2);

            auto [inserted_it, _] = fa_memo.emplace(key, std::make_pair(sum, cout));
            return inserted_it->second;
        };

        // Сложение двух векторов (шириной B)
        auto add_bitvectors = [&](const std::vector<Signal>& vecA, const std::vector<Signal>& vecB) -> std::vector<Signal> {
            std::vector<Signal> sum(vecB.size());
            Signal cin = ntk.get_constant(false);
            
            for (size_t i = 0; i < vecB.size(); ++i) {
                auto [s, cout] = full_adder(vecA[i], vecB[i], cin);
                sum[i] = s;
                cin = cout;
            }
            return sum;
        };

        // 2. Инициализация листьев дерева сумматоров
        std::vector<std::vector<Signal>> bitvecs(n, std::vector<Signal>(B, ntk.get_constant(false)));
        for (uint32_t i = 0; i < n; ++i) {
            for (int bit = 0; bit < B; ++bit) {
                if ((pos_weights[i] >> bit) & 1) {
                    bitvecs[i][bit] = var_signals[i];
                }
            }
        }

        // 3. Рекурсивное построение дерева сумматоров — последовательно
        // (см. большой комментарий выше про TBB/mockturtle-мьютекс).
        std::function<std::vector<Signal>(int, int)> build_tree = [&](int l, int r) -> std::vector<Signal> {
            if (l == r) return bitvecs[l];

            int mid = l + (r - l) / 2;
            std::vector<Signal> left_sum = build_tree(l, mid);
            std::vector<Signal> right_sum = build_tree(mid + 1, r);

            return add_bitvectors(left_sum, right_sum);
        };

        std::vector<Signal> final_sum = build_tree(0, n - 1);

        // 4. Компаратор: проверяем, что final_sum >= theta_adj
        uint64_t T = static_cast<uint64_t>(theta_adj);
        Signal ge = ntk.get_constant(true);

        for (int i = 0; i < B; ++i) {
            bool t_bit = (T >> i) & 1;
            if (t_bit) {
                ge = and_(final_sum[i], ge);
            } else {
                ge = or_(final_sum[i], ge);
            }
        }

        ntk.create_po(ge);

        return ok<Aig>(Aig{ntk});
    } 
    catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "Memory exhausted during adder-tree AIG construction");
    }
}

} // namespace bmm