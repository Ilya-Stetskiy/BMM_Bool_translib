#include "anf_to_aig.hpp"
#include <iostream>

#include <tracy/Tracy.hpp>

// ПАРАЛЛЕЛИЗМ (CONVENTIONS.md п.6, правило 2 формально требует TBB здесь):
// сознательно НЕ используется, после эмпирической проверки нескольких
// вариантов на реальной сборке (не только теоретически). Причина: любая
// параллельная стратегия, безопасная относительно
// mockturtle::aig_network::create_and (который НЕ потокобезопасен при
// конкурентной мутации из нескольких потоков — свои внутренние
// structural-hashing таблицы), требует строить AND-дерево каждого монома в
// изолированной локальной сети (собственные PI, без разделяемого
// состояния), а затем "прививать" готовое дерево в основной aig через
// mockturtle::cleanup_dangling. Проверено на: стандартном бенчмарке
// (test_anf.cpp, n=8/12/16, короткие/средние мономы) — стабильная
// регрессия x0.5-0.8; и на специально сконструированных ANF с n вплоть до
// 64 и мономами длиной 60-63 из 60-64 переменных (минуя truth-table,
// напрямую через BRiAl API) — результат шумный и без устойчивой
// закономерности (от +150% до -40% в зависимости от числа/длины
// мономов), без единого случая надёжного, воспроизводимого выигрыша.
// Причина регрессии — фиксированная цена создания отдельного
// mockturtle::aig_network (свои хэш-таблицы и инфраструктура structural
// hashing) на каждый моном перевешивает экономию от параллельного
// построения AND-дерева, которое само по себе дёшево (десятки вызовов
// create_and даже для монома из полусотни переменных). Подробности и все
// численные результаты экспериментов — см. память
// bmm_translib_anf_parallelism_work.md (или её первоисточник в истории
// сессии, если файл ещё не создан на момент чтения).

namespace bmm {

namespace {

using Signal = mockturtle::aig_network::signal;


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

    for (uint32_t v : mono)
    {
        nodes.push_back(vars[v]);
    }


    while (nodes.size() > 1)
    {
        std::vector<Signal> next;

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
    auto t1 =
        aig.create_and(
            a,
            !b
        );


    auto t2 =
        aig.create_and(
            !a,
            b
        );


    return
        !aig.create_and(
            !t1,
            !t2
        );
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


        for (size_t i = 0; i < nodes.size(); i += 2)
        {
            if (i + 1 < nodes.size())
            {
                next.push_back(
                    xor_aig(
                        aig,
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


} // namespace



Result<Aig> anf_to_aig(const Anf& anf)
{
    ZoneScoped;


    mockturtle::aig_network aig;


    // Создаем входные переменные

    std::vector<Signal> vars;

    vars.reserve(anf.n_vars());


    for (uint32_t i = 0; i < anf.n_vars(); ++i)
    {
        vars.push_back(
            aig.create_pi()
        );
    }



    // Каждый моном ANF -> AND

    std::vector<Signal> terms;


#if BMM_HAVE_BRIAL

    const auto& poly = anf.raw();


    for (auto it = poly.begin();
         it != poly.end();
         ++it)
    {
        auto mono = monomial_to_vector(*it);


        terms.push_back(
            build_monomial(
                aig,
                mono,
                vars
            )
        );
    }


#else

    for (const auto& mono : anf.raw().monomials())
    {
        terms.push_back(
            build_monomial(
                aig,
                mono,
                vars
            )
        );
    }

#endif



    // XOR всех мономов

    Signal result =
        xor_reduce(
            aig,
            terms
        );



    // Один выход

    aig.create_po(result);


    return ok<Aig>(
        Aig(std::move(aig))
    );
}


} // namespace bmm
