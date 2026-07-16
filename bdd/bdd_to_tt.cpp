#include "bdd_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace bmm {

// --- Хелперы для работы с сырым Sylvan API ---
inline bool sylvan_is_comp(sylvan::BDD n) {
    return (n & sylvan::sylvan_complement) != 0;
}

inline sylvan::BDD sylvan_regular(sylvan::BDD n) {
    return sylvan_is_comp(n) ? (n ^ sylvan::sylvan_complement) : n;
}

// Плоское представление узла BDD для кэш-дружественного обхода.
// 12 байт на узел (2 байта паддинга после level).
// high_edge/low_edge: младший бит = complement flag, остальное = ID узла.
// ID 0 зарезервирован под терминал-константу (физически не читается).
struct BddFlatNode {
    uint16_t level;      // Уровень переменной (TopVar)
    uint16_t _pad;       // Выравнивание
    uint32_t high_edge;  // Then-ребро (с complement-битом)
    uint32_t low_edge;   // Else-ребро (с complement-битом)
};

// Кастомная плоская хэш-таблица: sylvan::BDD -> uint32_t (ID узла).
// Открытая адресация, линейное пробирование, Фибоначчиево хэширование.
class FastBddMap {
    struct Entry { sylvan::BDD key; uint32_t val; };
    std::vector<Entry> table;
    size_t mask;
    size_t used;

    static size_t home_slot(sylvan::BDD key, size_t m) {
        // Убираем complement-бит для хэширования (регуляризуем)
        sylvan::BDD reg = key & ~sylvan::sylvan_complement;
        return (reg * 0x9E3779B185EBCA87ULL) & m;
    }

    static void raw_insert(std::vector<Entry>& t, size_t m, sylvan::BDD key, uint32_t val) {
        size_t idx = home_slot(key, m);
        while (t[idx].key != 0) idx = (idx + 1) & m;
        t[idx] = {key, val};
    }

    void grow() {
        size_t new_size = table.size() * 2;
        std::vector<Entry> new_table(new_size, {0, 0});
        size_t new_mask = new_size - 1;
        for (const auto& e : table) {
            if (e.key != 0) raw_insert(new_table, new_mask, e.key, e.val);
        }
        table = std::move(new_table);
        mask = new_mask;
    }

public:
    explicit FastBddMap(size_t capacity) : used(0) {
        size_t want = (capacity == 0 ? 1 : capacity) * 2;
        size_t power_of_two = 1;
        while (power_of_two < want) power_of_two <<= 1;
        table.assign(power_of_two, {0, 0});
        mask = power_of_two - 1;
    }

    // 0 = "не найдено" (ID 0 зарезервирован под терминал)
    uint32_t get(sylvan::BDD key) const {
        size_t idx = home_slot(key, mask);
        size_t probes = 0;
        while (table[idx].key != 0 && table[idx].key != key) {
            idx = (idx + 1) & mask;
            if (++probes > table.size())
                throw std::runtime_error("FastBddMap::get: таблица повреждена");
        }
        return (table[idx].key == key) ? table[idx].val : 0u;
    }

    void set(sylvan::BDD key, uint32_t val) {
        if ((used + 1) * 10 >= table.size() * 7) grow();
        size_t idx = home_slot(key, mask);
        while (table[idx].key != 0 && table[idx].key != key)
            idx = (idx + 1) & mask;
        if (table[idx].key == 0) ++used;
        table[idx] = {key, val};
    }
};

// Сериализация BDD в плоский массив.
// Возвращает root_edge (ID корня + complement-бит).
struct BddSerializer {
    std::vector<BddFlatNode> nodes;  // nodes[0] — фиктивный терминал
    FastBddMap cache;
    int n;  // Число переменных (для проверки level < n)

    BddSerializer(size_t estimated, int n_vars)
        : cache(estimated), n(n_vars) {
        nodes.reserve(estimated);
        nodes.push_back({0xFFFF, 0, 0, 0});  // Узел 0: терминал
    }

    uint32_t serialize(sylvan::Bdd node) {
        sylvan::BDD raw = node.GetBDD();

        // Терминал: ID = 0, complement-бит = is_comp
        if (node.isTerminal()) {
            return (0u << 1) | (sylvan_is_comp(raw) ? 1u : 0u);
        }

        // Регуляризуем для хэширования
        sylvan::BDD reg_raw = sylvan_regular(raw);

        // Уже сериализован?
        uint32_t existing = cache.get(reg_raw);
        if (existing != 0) {
            return (existing << 1) | (sylvan_is_comp(raw) ? 1u : 0u);
        }

        // Рекурсивная сериализация детей (DFS)
        uint32_t high_child = serialize(node.Then());
        uint32_t low_child = serialize(node.Else());

        // Уровень переменной (в Sylvan TopVar() = индекс переменной = уровень)
        uint32_t var_idx = node.TopVar();

        if (var_idx >= static_cast<uint32_t>(n)) {
            throw std::runtime_error("BDD variable level out of range");
        }

        uint32_t new_id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({
            static_cast<uint16_t>(var_idx),
            0,  // padding
            high_child,
            low_child
        });

        cache.set(reg_raw, new_id);
        return (new_id << 1) | (sylvan_is_comp(raw) ? 1u : 0u);
    }
};

// Вычисление одного бита TT по плоскому графу BDD.
// BitOrder::LSB_FIRST: x_i соответствует биту i в индексе минтерма.
inline bool evaluate_minterm(
    const BddFlatNode* nodes,
    uint32_t root_edge,
    uint64_t minterm
) {
    uint32_t curr_id = root_edge >> 1;
    bool is_comp = root_edge & 1;

    while (curr_id != 0) {
        const BddFlatNode& node = nodes[curr_id];
        // LSB_FIRST: переменная уровня level = бит level в minterm
        bool decision = (minterm >> node.level) & 1;
        uint32_t next_edge = decision ? node.high_edge : node.low_edge;
        is_comp ^= (next_edge & 1);
        curr_id = next_edge >> 1;
    }

    // curr_id == 0 → терминал. Результат = 1 XOR is_comp
    return !is_comp;
}

Result<TruthTable> bdd_to_tt(const Bdd& f) {
    ZoneScoped;

    uint32_t n = f.n_vars();

    // Контракт: TT только до kMaxTruthTableVars переменных
    if (n > kMaxTruthTableVars) {
        return fail<TruthTable>(ErrorCode::TooManyVariables,
            "bdd_to_tt: n > " + std::to_string(kMaxTruthTableVars));
    }

    try {
        sylvan::Bdd f_syl = f.raw();

        // Быстрый путь для констант
        if (f_syl.isTerminal()) {
            TruthTable tt(n);
            bool val = !sylvan_is_comp(f_syl.GetBDD());
            if (val) {
                // Заполняем все биты в 1
                uint64_t rows = uint64_t{1} << n;
                for (uint64_t i = 0; i < rows; ++i) {
                    kitty::set_bit(tt.raw(), i);
                }
            }
            return ok(std::move(tt));
        }

        // Сериализация BDD в плоский массив
        size_t dag_size = std::max<size_t>(f_syl.NodeCount(), 64);
        BddSerializer serializer(dag_size, n);
        uint32_t root_edge = serializer.serialize(f_syl);

        const BddFlatNode* flat_nodes = serializer.nodes.data();
        uint64_t total_rows = uint64_t{1} << n;

        // Выделение и заполнение TT
        TruthTable tt(n);

        // Побитовое заполнение: для каждого минтерма вычисляем значение
        // и устанавливаем соответствующий бит в kitty::dynamic_truth_table.
        // Для n <= 24 это до 16M итераций — на CPU занимает ~50-200 мс.
        for (uint64_t minterm = 0; minterm < total_rows; ++minterm) {
            if (evaluate_minterm(flat_nodes, root_edge, minterm)) {
                kitty::set_bit(tt.raw(), minterm);
            }
        }

        return ok(std::move(tt));

    } catch (const std::bad_alloc&) {
        return fail<TruthTable>(ErrorCode::OutOfMemory,
            "bdd_to_tt: исчерпана память при построении TT");
    } catch (const std::exception& e) {
        return fail<TruthTable>(ErrorCode::Unsupported,
            std::string("bdd_to_tt internal error: ") + e.what());
    }
}

} // namespace bmm