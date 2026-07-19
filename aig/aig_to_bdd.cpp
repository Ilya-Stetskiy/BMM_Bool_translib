#include "aig_to_bdd.hpp"

#include <tracy/Tracy.hpp>
#include <vector>
#include <array>
#include <cstdint>

namespace bmm {

namespace {

// Максимальный "бюджет" пар переменных (|support(fanin0)| * |support(fanin1)|),
// для которого на ОДНОМ гейте ещё генерируются рёбра графа взаимодействия
// (см. build_interaction_graph ниже). Без этого предела построение графа само
// по себе становится узким местом на широких реальных схемах: на voter.aig
// (EPFL, n=1000, ~14760 гейтов) гейты в верхней части схемы легко зависят от
// сотен переменных каждый — Θ(|support0|*|support1|) рёбер с одного такого
// гейта уже сотни тысяч, а таких гейтов много. Порог 4096 — тот же порядок
// величины, что и типичное число мономов в ANF-корпусе, на котором FORCE уже
// проверена (anf/anf_to_bdd.cpp) — гейты, превышающие бюджет, просто не дают
// вклада в граф (эвристика деградирует к MinIndex локально для этих пар), но
// не останавливают построение целиком. Не влияет на корректность итогового
// BDD — это только эвристика ПОРЯДКА переменных, любой порядок даёт
// семантически верный результат (см. convert()/Bdd::var_at_level в
// core/common.hpp).
constexpr uint64_t kMaxCrossProductPerGate = 4096;

// Носитель узла AIG — какие входные переменные влияют на его значение,
// накапливается снизу вверх (PI -> гейты) объединением носителей фанинов.
// count кэшируется рядом с битовым вектором, чтобы не пересчитывать popcount
// на каждом гейте, который использует этот узел как фанин (а в DAG один узел
// обычно используется несколькими гейтами выше по схеме).
struct NodeSupport {
    std::vector<bool> bits;
    uint32_t count = 0;
};

// Граф взаимодействия переменных по фанин-структуре AIG — аналог того, что
// anf/anf_to_bdd.cpp строит по мономам ANF (build_interaction_graph там), но
// источник пар другой: вместо "переменные одного монома" здесь "переменные,
// от которых совместно зависят два фанина одного AND-гейта" — тот же
// содержательный сигнал (какие переменные структурно связаны и стоит
// располагать рядом в порядке BDD), выражен через естественную для AIG
// единицу структуры (гейт с двумя входами) вместо монома.
//
// Обход — net.foreach_gate, который mockturtle гарантированно отдаёт в
// топологическом порядке (PI перед использующими их гейтами), поэтому к
// моменту обработки гейта носители обоих его фанинов уже посчитаны.
//
// Стоимость: O(n_vars) на гейт для объединения носителей (дёшево — n_vars
// обычно <= 1000 даже на крупных EPFL-схемах) + O(|support0|*|support1|) на
// генерацию рёбер, ограничено kMaxCrossProductPerGate за гейт.
InteractionGraph build_interaction_graph(const Aig& aig, uint32_t n_vars) {
    const auto& net = aig.raw();
    InteractionGraph graph(n_vars);

    std::vector<NodeSupport> support(net.size());

    uint32_t pi_idx = 0;
    net.foreach_pi([&](auto node) {
        auto& s = support[net.node_to_index(node)];
        s.bits.assign(n_vars, false);
        s.bits[pi_idx] = true;
        s.count = 1;
        ++pi_idx;
    });

    std::vector<uint32_t> idx0;
    std::vector<uint32_t> idx1;

    net.foreach_gate([&](auto node) {
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        const auto& s0 = support[net.node_to_index(net.get_node(fanins[0]))];
        const auto& s1 = support[net.node_to_index(net.get_node(fanins[1]))];

        const uint64_t cross = static_cast<uint64_t>(s0.count) * static_cast<uint64_t>(s1.count);
        if (cross > 0 && cross <= kMaxCrossProductPerGate) {
            idx0.clear();
            idx1.clear();
            for (uint32_t v = 0; v < n_vars; ++v) {
                if (s0.bits[v]) idx0.push_back(v);
                if (s1.bits[v]) idx1.push_back(v);
            }
            for (uint32_t a : idx0) {
                for (uint32_t b : idx1) {
                    graph.add_edge(a, b);
                }
            }
        }

        // Объединение носителей — ПОСЛЕ генерации рёбер (не влияет на
        // порядок, но s0/s1 выше — ссылки в support[], а sd ниже создаётся в
        // той же таблице; узел гейта имеет свой собственный индекс, отличный
        // от индексов обоих фанинов, поэтому алиасинга при записи нет).
        auto& sd = support[net.node_to_index(node)];
        sd.bits.assign(n_vars, false);
        uint32_t cnt = 0;
        for (uint32_t v = 0; v < n_vars; ++v) {
            const bool bit = s0.bits[v] || s1.bits[v];
            sd.bits[v] = bit;
            if (bit) ++cnt;
        }
        sd.count = cnt;
    });

    return graph;
}

// Эвристика — степень вершины (VariableOrderHeuristic::Degree). Алгоритм —
// bmm::compute_rank_by_degree (core/bdd_order_heuristics.hpp), здесь только
// построение графа по фанин-структуре AIG.
VariableRank compute_rank_by_degree(const Aig& aig, uint32_t n_vars) {
    InteractionGraph graph = build_interaction_graph(aig, n_vars);
    return ::bmm::compute_rank_by_degree(graph, n_vars);
}

// Эвристика — FORCE (Aloul, Markov, Sakallah, GLSVLSI 2003). Тот же
// проверенный на anf_to_bdd.cpp алгоритм (см. историю сравнения эвристик
// там — FORCE не хуже 1.95x от лучшей на 5 разных семействах структур, тогда
// как у альтернатив минимаксное сожаление доходит до 8-12x), здесь применён
// к графу взаимодействия, построенному по фанинам AIG вместо мономов ANF.
VariableRank compute_rank_force(const Aig& aig, uint32_t n_vars, uint32_t iterations = 20) {
    InteractionGraph graph = build_interaction_graph(aig, n_vars);
    return ::bmm::compute_rank_force(graph, n_vars, iterations);
}

VariableRank select_rank(VariableOrderHeuristic heuristic, const Aig& aig, uint32_t n_vars) {
    switch (heuristic) {
        case VariableOrderHeuristic::MinIndex:
            return identity_rank(n_vars);
        case VariableOrderHeuristic::LengthFreqRank:
            // Не имеет естественного смысла для AIG: у anf_to_bdd эта
            // эвристика ранжирует переменные по длине монома, в который они
            // входят, но AND-гейт AIG всегда бинарный (арность 2 у всех
            // гейтов одинакова) — понятие "длины" вырождено. Используем
            // MinIndex как безопасную реализацию для этой ветки общего
            // enum, вместо того чтобы делать функцию частичной.
            return identity_rank(n_vars);
        case VariableOrderHeuristic::Degree:
            return compute_rank_by_degree(aig, n_vars);
        case VariableOrderHeuristic::Force:
            return compute_rank_force(aig, n_vars);
    }
    return identity_rank(n_vars);  // недостижимо при корректном enum, но без UB на всякий случай
}

}  // namespace

Result<Bdd> aig_to_bdd_with_heuristic(const Aig& aig, VariableOrderHeuristic heuristic) {
    ZoneScoped;
    const auto& net = aig.raw();
    if (net.num_pos() != 1) {
        return fail<Bdd>(ErrorCode::Unsupported, "aig_to_bdd: ожидается ровно один PO");
    }

  try {
    VariableRank rank = select_rank(heuristic, aig, aig.n_vars());

    std::vector<sylvan::Bdd> node_bdds(net.size(), sylvan::Bdd::bddZero());

    // Constant false node
    node_bdds[net.node_to_index(net.get_node(net.get_constant(false)))] = sylvan::Bdd::bddZero();

    // Map PIs — level = rank[pi_idx], НЕ pi_idx: физическая перестановка
    // уровня Sylvan (см. комментарий у aig_to_bdd_with_heuristic в .hpp и у
    // convert()/ЧАСТЬ 2 в anf/anf_to_bdd.cpp за тем, почему просто менять
    // порядок ПОСТРОЕНИЯ узлов недостаточно — level == bddVar index жёстко
    // фиксирован Sylvan, значение имеет только САМ индекс, переданный в
    // bddVar).
    uint32_t pi_idx = 0;
    net.foreach_pi([&](auto node) {
        node_bdds[net.node_to_index(node)] = sylvan::Bdd::bddVar(rank[pi_idx++]);
    });

    // Traverse gates in topological order
    net.foreach_gate([&](auto node) {
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        auto get_signal_bdd = [&](mockturtle::aig_network::signal s) {
            auto node_idx = net.node_to_index(net.get_node(s));
            auto bdd = node_bdds[node_idx];
            if (net.is_complemented(s)) {
                return !bdd;
            }
            return bdd;
        };

        auto bdd0 = get_signal_bdd(fanins[0]);
        auto bdd1 = get_signal_bdd(fanins[1]);
        node_bdds[net.node_to_index(node)] = bdd0 & bdd1;
    });

    // PO signal
    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });

    auto po_node_idx = net.node_to_index(net.get_node(po_sig));
    auto final_bdd = node_bdds[po_node_idx];
    if (net.is_complemented(po_sig)) {
        final_bdd = !final_bdd;
    }

    // var_to_level = rank — тот же явный контракт, что anf_to_bdd.cpp
    // использует для Bdd(root, n_vars, var_to_level) (core/common.hpp):
    // потребители (bdd_to_aig.cpp, bdd_to_tt.cpp, bdd_to_anf.cpp,
    // bdd_to_thr.cpp) обязаны обращаться к переменной по уровню через
    // Bdd::var_at_level(), а не считать level==var — это уже соблюдается
    // ими для результата anf_to_bdd, тот же путь кода теперь корректно
    // работает и для результата aig_to_bdd_with_heuristic.
    return ok<Bdd>(Bdd(final_bdd, aig.n_vars(), rank));

  } catch (const std::bad_alloc&) {
      return fail<Bdd>(ErrorCode::OutOfMemory, "aig_to_bdd: исчерпана память");
  }
}

// Умолчание — Force, тем же обоснованием, что и anf_to_bdd() (anf/
// anf_to_bdd.cpp): везде не хуже 1.95x от лучшей эвристики среди
// протестированных структурных семейств, тогда как альтернативы уходят в
// 8-12x на своих худших случаях. Здесь дополнительно эмпирически мотивировано
// этой же сессией: натуральный (MinIndex) порядок вызвал зависание при
// построении BDD для реальной схемы EPFL router.aig (n=60,
// verify/real_datasets_tests.cpp, epfl_router_po0) — Force с графом
// взаимодействия по фанин-структуре AIG (см. build_interaction_graph выше)
// закрывает этот же класс риска, что уже был закрыт для anf_to_bdd.
Result<Bdd> aig_to_bdd(const Aig& aig) {
    return aig_to_bdd_with_heuristic(aig, VariableOrderHeuristic::Force);
}

}  // namespace bmm
