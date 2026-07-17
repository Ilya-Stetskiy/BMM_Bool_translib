#include "bdd_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>

namespace bmm {

constexpr sylvan::BDD SYLVAN_COMPLEMENT = 0x8000000000000000ULL;

inline sylvan::BDD sylvan_regular(sylvan::BDD n) {
    return n & ~SYLVAN_COMPLEMENT;
}

struct BddFlatNode {
    uint16_t level;
    uint16_t _pad;
    uint32_t high_edge;
    uint32_t low_edge;
};

class FastBddMap {
    struct Entry { sylvan::BDD key; uint32_t val; };
    std::vector<Entry> table;
    size_t mask;
    size_t used;

    static size_t home_slot(sylvan::BDD key, size_t m) {
        sylvan::BDD reg = sylvan_regular(key);
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

struct BddSerializer {
    std::vector<BddFlatNode> nodes;
    FastBddMap cache;
    uint32_t n;

    BddSerializer(size_t estimated, uint32_t n_vars)
        : cache(estimated), n(n_vars) {
        nodes.reserve(estimated);
        nodes.push_back({0xFFFF, 0, 0, 0});
    }

    uint32_t serialize(sylvan::Bdd root) {
        if (root.isTerminal()) {
            bool is_logically_zero = (root == sylvan::Bdd::bddZero()) || (root == !sylvan::Bdd::bddOne());
            return (0u << 1) | (is_logically_zero ? 1u : 0u);
        }

        sylvan::BDD root_reg = sylvan_regular(root.GetBDD());

        std::vector<sylvan::Bdd> stack;
        std::unordered_set<sylvan::BDD> seen;
        stack.reserve(nodes.capacity());
        seen.reserve(nodes.capacity());

        stack.push_back(root);
        seen.insert(root_reg);

        auto edge_of = [&](const sylvan::Bdd& child) -> uint32_t {
            if (child.isTerminal()) {
                bool is_logically_zero = (child == sylvan::Bdd::bddZero()) || (child == !sylvan::Bdd::bddOne());
                return (0u << 1) | (is_logically_zero ? 1u : 0u);
            }
            
            sylvan::BDD craw = child.GetBDD();
            sylvan::BDD creg = sylvan_regular(craw);
            uint32_t id = cache.get(creg);
            if (id == 0u) {
                throw std::runtime_error("BddSerializer::serialize: узел не найден в кэше");
            }
            
            bool is_comp = (craw & SYLVAN_COMPLEMENT) != 0;
            return (id << 1) | (is_comp ? 1u : 0u);
        };

        while (!stack.empty()) {
            sylvan::Bdd node = stack.back();
            sylvan::BDD reg = sylvan_regular(node.GetBDD());

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
                if (!T_ready && seen.find(T_reg) == seen.end()) {
                    seen.insert(T_reg);
                    stack.push_back(T);
                }
                if (!E_ready && seen.find(E_reg) == seen.end()) {
                    seen.insert(E_reg);
                    stack.push_back(E);
                }
            }
        }

        sylvan::BDD root_raw = root.GetBDD();
        bool root_is_comp = (root_raw & SYLVAN_COMPLEMENT) != 0;
        uint32_t root_id = cache.get(root_reg);
        return (root_id << 1) | (root_is_comp ? 1u : 0u);
    }
};

inline bool evaluate_minterm(
    const BddFlatNode* nodes,
    uint32_t root_edge,
    uint64_t minterm
) {
    uint32_t curr_id = root_edge >> 1;
    bool is_comp = root_edge & 1;

    while (curr_id != 0) {
        const BddFlatNode& node = nodes[curr_id];
        bool decision = (minterm >> node.level) & 1;
        uint32_t next_edge = decision ? node.high_edge : node.low_edge;
        
        is_comp ^= (next_edge & 1);
        curr_id = next_edge >> 1;
    }

    return !is_comp;
}

Result<TruthTable> bdd_to_tt(const Bdd& f) {
    ZoneScoped;

    try {
        uint32_t n = f.n_vars();

        if (n > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables,
                "bdd_to_tt: n > " + std::to_string(kMaxTruthTableVars));
        }

        sylvan::Bdd f_syl = f.raw();

        if (f_syl.isTerminal()) {
            TruthTable tt(n);
            bool is_logically_zero = (f_syl == sylvan::Bdd::bddZero()) || (f_syl == !sylvan::Bdd::bddOne());
            
            if (!is_logically_zero) {
                uint64_t rows = uint64_t{1} << n;
                for (uint64_t i = 0; i < rows; ++i) {
                    kitty::set_bit(tt.raw(), i);
                }
            }
            return ok(std::move(tt));
        }

        size_t dag_size = std::max<size_t>(f_syl.NodeCount(), 64);
        BddSerializer serializer(dag_size, n);
        uint32_t root_edge = serializer.serialize(f_syl);

        const BddFlatNode* flat_nodes = serializer.nodes.data();
        uint64_t total_rows = uint64_t{1} << n;

        TruthTable tt(n);

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
        return fail<TruthTable>(ErrorCode::InvalidArgument,
            std::string("bdd_to_tt internal error: ") + e.what());
    }
}

} // namespace bmm