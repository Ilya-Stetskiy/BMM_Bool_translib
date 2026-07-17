#include "anf_to_aig.hpp"
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <vector>
 
#include <tracy/Tracy.hpp>
 
namespace bmm {
 
namespace {
 
using Signal = mockturtle::aig_network::signal;
using Node   = mockturtle::aig_network::node;
 
 
// ------------------------------------------------------------
// BRiAl BooleMonomial -> список индексов переменных
// ------------------------------------------------------------
 
std::vector<uint32_t> monomial_to_vector(
        const polybori::BooleMonomial& mono)
{
    std::vector<uint32_t> result;
 
    for (auto it = mono.begin();
         it != mono.end();
         ++it)
    {
        result.push_back(static_cast<uint32_t>(*it));
    }
 
    return result;
}
 
 
// ------------------------------------------------------------
// Построение AND дерева для одного монома
// ------------------------------------------------------------
 
Signal build_monomial(
        mockturtle::aig_network& aig,
        const std::vector<uint32_t>& mono,
        const std::vector<Signal>& vars)
{
    if (mono.empty())
    {
        return aig.get_constant(true);
    }
 
    std::vector<Signal> nodes;
    nodes.reserve(mono.size());
 
    for (uint32_t v : mono)
    {
        nodes.push_back(vars[v]);
    }
 
    while (nodes.size() > 1)
    {
        std::vector<Signal> next;
        next.reserve((nodes.size() + 1) / 2);
 
        for (size_t i = 0; i < nodes.size(); i += 2)
        {
            if (i + 1 < nodes.size())
            {
                next.push_back(
                    aig.create_and(
                        nodes[i],
                        nodes[i + 1]
                    )
                );
            }
            else
            {
                next.push_back(nodes[i]);
            }
        }
 
        nodes.swap(next);
    }
 
    return nodes[0];
}
 
 
// ------------------------------------------------------------
// XOR через AIG
// ------------------------------------------------------------
 
Signal xor_aig(
        mockturtle::aig_network& aig,
        Signal a,
        Signal b)
{
    auto t1 = aig.create_and(a, !b);
    auto t2 = aig.create_and(!a, b);
 
    return !aig.create_and(!t1, !t2);
}
 
 
// ------------------------------------------------------------
// XOR редукция дерева
// ------------------------------------------------------------
 
Signal xor_reduce(
        mockturtle::aig_network& aig,
        std::vector<Signal> nodes)
{
    if (nodes.empty())
    {
        return aig.get_constant(false);
    }
 
    while (nodes.size() > 1)
    {
        std::vector<Signal> next;
        next.reserve((nodes.size() + 1) / 2);
 
        for (size_t i = 0; i < nodes.size(); i += 2)
        {
            if (i + 1 < nodes.size())
            {
                next.push_back(
                    xor_aig(aig, nodes[i], nodes[i + 1])
                );
            }
            else
            {
                next.push_back(nodes[i]);
            }
        }
 
        nodes.swap(next);
    }
 
    return nodes[0];
}
 
 
// ------------------------------------------------------------
// Перенос сигнала (вместе с конусом) из локальной сети src
// в главную сеть dst.
// ------------------------------------------------------------
 
Signal clone_into(
        mockturtle::aig_network& dst,
        const mockturtle::aig_network& src,
        Signal src_signal,
        const std::unordered_map<uint32_t, Signal>& pi_map,
        std::unordered_map<uint32_t, Signal>& memo)
{
    Node n = src.get_node(src_signal);
    bool comp = src.is_complemented(src_signal);
 
    if (src.is_constant(n))
    {
        Signal c = dst.get_constant(false);
        return comp ? !c : c;
    }
 
    auto pi_it = pi_map.find(src.node_to_index(n));
    if (pi_it != pi_map.end())
    {
        return comp ? !pi_it->second : pi_it->second;
    }
 
    auto memo_it = memo.find(src.node_to_index(n));
    if (memo_it != memo.end())
    {
        return comp ? !memo_it->second : memo_it->second;
    }
 
    std::vector<Signal> fanins;
    fanins.reserve(2);
 
    src.foreach_fanin(n, [&](Signal const& fi) {
        fanins.push_back(
            clone_into(dst, src, fi, pi_map, memo)
        );
    });
 
    Signal base = dst.clone_node(src, n, fanins);
 
    memo[src.node_to_index(n)] = base;
 
    return comp ? !base : base;
}
 
 
} // namespace
 
 
Result<Aig> anf_to_aig(const Anf& anf)
{
    ZoneScoped;
 
    mockturtle::aig_network aig;
 
    std::vector<Signal> vars;
    vars.reserve(anf.n_vars());
 
    for (uint32_t i = 0; i < anf.n_vars(); ++i)
    {
        vars.push_back(aig.create_pi());
    }
 
    // --------------------------------------------------------
    // 1. Собираем мономы и сортируем по убыванию степени
    //    (для более равномерной нагрузки между потоками).
    // --------------------------------------------------------
 
    std::vector<std::vector<uint32_t>> mono_list;
 
    {
        ZoneScopedN("extract_monomials");
 
#if BMM_HAVE_BRIAL
        const auto& poly = anf.raw();
        for (auto it = poly.begin(); it != poly.end(); ++it)
        {
            mono_list.push_back(monomial_to_vector(*it));
        }
#else
        for (const auto& mono : anf.raw().monomials())
        {
            mono_list.push_back(mono);
        }
#endif
 
        std::sort(
            mono_list.begin(), mono_list.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); }
        );
    }
 
    // Если мономов мало — параллелизм не окупается
    constexpr size_t kMinMonomialsPerThread = 32;
 
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4; // разумное значение по умолчанию, если система не сообщила
 
    const int n_threads = std::max<int>(
        1,
        std::min<int>(
            static_cast<int>(hw),
            static_cast<int>(mono_list.size() / kMinMonomialsPerThread)
        )
    );
 
    // --------------------------------------------------------
    // 2. Параллельно строим частичные результаты в локальных
    //    сетях — по одной сети на поток, через std::thread.
    // --------------------------------------------------------
 
    std::vector<mockturtle::aig_network> local_aigs(n_threads);
    std::vector<std::vector<Signal>>     local_vars(n_threads);
    std::vector<Signal>                  local_results(n_threads);
    std::vector<bool>                    local_has_result(n_threads, false);
 
    for (int t = 0; t < n_threads; ++t)
    {
        local_vars[t].reserve(anf.n_vars());
        for (uint32_t i = 0; i < anf.n_vars(); ++i)
        {
            local_vars[t].push_back(local_aigs[t].create_pi());
        }
    }
 
    {
        ZoneScopedN("parallel_build");
 
        auto worker = [&](int t)
        {
            // Мономы отсортированы по убыванию степени; раздаём их
            // потокам по кругу (round-robin), это даёт неплохую
            // балансировку без динамического диспетчера задач.
            std::vector<Signal> my_terms;
 
            for (size_t i = static_cast<size_t>(t);
                 i < mono_list.size();
                 i += static_cast<size_t>(n_threads))
            {
                my_terms.push_back(
                    build_monomial(
                        local_aigs[t],
                        mono_list[i],
                        local_vars[t]
                    )
                );
            }
 
            if (!my_terms.empty())
            {
                local_results[t] = xor_reduce(local_aigs[t], my_terms);
                local_has_result[t] = true;
            }
        };
 
        if (n_threads == 1)
        {
            worker(0);
        }
        else
        {
            std::vector<std::thread> pool;
            pool.reserve(n_threads);
 
            for (int t = 0; t < n_threads; ++t)
            {
                pool.emplace_back(worker, t);
            }
 
            for (auto& th : pool)
            {
                th.join();
            }
        }
    }
 
    // --------------------------------------------------------
    // 3. Последовательно переносим частичные результаты в
    //    главную сеть и сводим их через XOR.
    // --------------------------------------------------------
 
    std::vector<Signal> merged_terms;
    merged_terms.reserve(n_threads);
 
    {
        ZoneScopedN("merge");
 
        for (int t = 0; t < n_threads; ++t)
        {
            if (!local_has_result[t])
            {
                continue;
            }
 
            std::unordered_map<uint32_t, Signal> pi_map;
            pi_map.reserve(anf.n_vars());
 
            for (uint32_t i = 0; i < anf.n_vars(); ++i)
            {
                pi_map[local_aigs[t].node_to_index(
                    local_aigs[t].get_node(local_vars[t][i])
                )] = vars[i];
            }
 
            std::unordered_map<uint32_t, Signal> memo;
 
            merged_terms.push_back(
                clone_into(
                    aig,
                    local_aigs[t],
                    local_results[t],
                    pi_map,
                    memo
                )
            );
        }
    }
 
    Signal result = xor_reduce(aig, merged_terms);
 
    aig.create_po(result);
 
    return ok<Aig>(Aig(std::move(aig)));
}
 
 
} // namespace bmm
 
