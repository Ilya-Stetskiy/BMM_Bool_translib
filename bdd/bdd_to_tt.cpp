#include "bdd_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>

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
    uint32_t n;  // Число переменных (для проверки level < n)

    BddSerializer(size_t estimated, uint32_t n_vars)
        : cache(estimated), n(n_vars) {
        nodes.reserve(estimated);
        nodes.push_back({0xFFFF, 0, 0, 0});  // Узел 0: терминал
    }

    // ИЗМЕНЕНО: было рекурсивным (serialize вызывал сам себя для
    // node.Then()/node.Else()). Контракт проекта требует итеративного
    // обхода с явным стеком именно из-за риска переполнения стека на
    // глубоких BDD (см. bdd_to_aig.cpp и CONVENTIONS.md). Глубина здесь и
    // так ограничена kMaxTruthTableVars (проверяется в bdd_to_tt() до
    // вызова serialize()), поэтому падение по стеку было маловероятно на
    // практике — но только благодаря этому внешнему, специфичному для TT
    // ограничению; было бы небезопасно, если BddSerializer когда-нибудь
    // переиспользуют в контексте без такого потолка на n. Ниже — тот же
    // итеративный post-order паттерн, что и в bdd_to_aig.cpp, адаптированный
    // под плоский формат BddFlatNode и FastBddMap вместо unordered_map.
    uint32_t serialize(sylvan::Bdd root) {
        sylvan::BDD root_raw = root.GetBDD();

        // Корень тоже может оказаться терминалом, если вызвать serialize()
        // напрямую, минуя быстрый путь в bdd_to_tt().
        if (root.isTerminal()) {
            return (0u << 1) | (sylvan_is_comp(root_raw) ? 1u : 0u);
        }

        sylvan::BDD root_reg = sylvan_regular(root_raw);

        std::vector<sylvan::Bdd> stack;
        std::unordered_set<sylvan::BDD> seen;
        stack.reserve(nodes.capacity());
        seen.reserve(nodes.capacity());

        stack.push_back(root);
        seen.insert(root_reg);

        // Возвращает упакованное ребро (id<<1 | complement) для уже
        // готового (терминального либо ранее сериализованного) узла.
        auto edge_of = [&](const sylvan::Bdd& child) -> uint32_t {
            sylvan::BDD craw = child.GetBDD();
            if (child.isTerminal()) {
                return (0u << 1) | (sylvan_is_comp(craw) ? 1u : 0u);
            }
            sylvan::BDD creg = sylvan_regular(craw);
            uint32_t id = cache.get(creg);
            if (id == 0u) {
                throw std::runtime_error(
                    "BddSerializer::serialize: узел не найден в кэше при "
                    "топологической сборке (нарушен инвариант post-order обхода)");
            }
            return (id << 1) | (sylvan_is_comp(craw) ? 1u : 0u);
        };

        while (!stack.empty()) {
            sylvan::Bdd node = stack.back();
            sylvan::BDD raw = node.GetBDD();
            sylvan::BDD reg = sylvan_regular(raw);

            // Уже полностью сериализован — просто убираем со стека.
            if (cache.get(reg) != 0u) {
                stack.pop_back();
                continue;
            }

            sylvan::Bdd T = node.Then();
            sylvan::Bdd E = node.Else();

            sylvan::BDD T_reg = sylvan_regular(T.GetBDD());
            sylvan::BDD E_reg = sylvan_regular(E.GetBDD());

            bool T_ready = T.isTerminal() || cache.get(T_reg) != 0u;
            bool E_ready = E.isTerminal() || cache.get(E_reg) != 0u;

            if (T_ready && E_ready) {
                uint32_t var_idx = node.TopVar();
                if (var_idx >= n) {
                    throw std::runtime_error("BDD variable level out of range");
                }

                uint32_t new_id = static_cast<uint32_t>(nodes.size());
                nodes.push_back({
                    static_cast<uint16_t>(var_idx),
                    0,
                    edge_of(T),
                    edge_of(E)
                });

                cache.set(reg, new_id);
                stack.pop_back();
            } else {
                // Проверка 'seen' перед push — не даёт одному и тому же узлу
                // попасть в стек дважды, пока он не досчитан (иначе на
                // сильно шаренных DAG возможен квадратичный рост числа
                // обращений — тот же паттерн, что и в bdd_to_aig.cpp).
                if (!T_ready && !seen.count(T_reg)) {
                    seen.insert(T_reg);
                    stack.push_back(T);
                }
                if (!E_ready && !seen.count(E_reg)) {
                    seen.insert(E_reg);
                    stack.push_back(E);
                }
            }
        }

        return edge_of(root);
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

    // ИЗМЕНЕНО: n_vars() и проверка лимита переменных перенесены внутрь
    // try — контракт требует оборачивать в try всё тело функции, а не
    // только часть после проверки kMaxTruthTableVars.
    try {
        uint32_t n = f.n_vars();

        // Контракт: TT только до kMaxTruthTableVars переменных
        if (n > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables,
                "bdd_to_tt: n > " + std::to_string(kMaxTruthTableVars));
        }

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
        // ИЗМЕНЕНО: ErrorCode::InternalError вместо ErrorCode::Unsupported.
        // Это исключение сигнализирует о нарушенном внутреннем инварианте
        // (испорченный BDD, рассинхрон var_idx/уровней, повреждённая
        // FastBddMap), а не о заведомо неподдерживаемом, но легитимном
        // случае. Если тест-раннер трактует Unsupported так же мягко, как
        // NotImplemented (SKIP, а не FAIL) — реальный баг рисковал остаться
        // незамеченным под видом "не поддерживается".
        return fail<TruthTable>(ErrorCode::InternalError,
            std::string("bdd_to_tt internal error: ") + e.what());
    }
}

} // namespace bmm