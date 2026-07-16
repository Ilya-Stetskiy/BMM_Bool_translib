#include "anf_to_aig.hpp"
#include <iostream>

#include <tracy/Tracy.hpp>

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
//
// x0*x2*x5:
//
//          AND
//         /   \
//       AND    x5
//      /   \
//    x0     x2
//
// ------------------------------------------------------------

Signal build_monomial(
        mockturtle::aig_network& aig,
        const std::vector<uint32_t>& mono,
        const std::vector<Signal>& vars)
{

    // константа 1
    if (mono.empty())
    {
        return aig.get_constant(true);
    }


    std::vector<Signal> nodes;


    for (uint32_t v : mono)
    {
        nodes.push_back(vars[v]);
    }



    // балансированное AND дерево

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
// XOR через AIG:
//
// a XOR b
//
// = (a & !b) | (!a & b)
//
// = !( !(a&!b) & !(!a&b) )
//
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
// XOR редукция дерева:
//
// m1 xor m2 xor m3 xor m4
//
//        xor
//       /   \
//     xor   xor
//    / \    / \
//   m1 m2 m3 m4
//
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



    // --------------------------------------------------------
    // 1. Создаем входные переменные
    // --------------------------------------------------------

    std::vector<Signal> vars;

    vars.reserve(anf.n_vars());


    for (uint32_t i = 0; i < anf.n_vars(); ++i)
    {
        vars.push_back(
            aig.create_pi()
        );
    }



    // --------------------------------------------------------
    // 2. Каждый моном ANF -> AND
    // --------------------------------------------------------

    std::vector<Signal> terms;



#if BMM_HAVE_BRIAL

    const auto& poly = anf.raw();

    std::cerr << "n_vars = "
              << anf.n_vars()
              << "\n";


    std::cerr << "ANF polynomial:\n";
    std::cerr << poly << "\n";


    for (auto it = poly.begin();
         it != poly.end();
         ++it)
    {
        auto mono = monomial_to_vector(*it);


        std::cerr << "MONO SIZE = "
                  << mono.size()
                  << "\n";


        for(auto x : mono)
        {
            std::cerr << x << " ";
        }

        std::cerr << "\n";


        // ВАЖНО: вот этого у тебя сейчас нет
        terms.push_back(
            build_monomial(
                aig,
                mono,
                vars
            )
        );
    }


    std::cerr << "terms count = "
              << terms.size()
              << "\n";


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



    // --------------------------------------------------------
    // 3. XOR всех мономов
    // --------------------------------------------------------

    Signal result =
        xor_reduce(
            aig,
            terms
        );



    // --------------------------------------------------------
    // 4. Один выход
    // --------------------------------------------------------

    aig.create_po(result);



    return ok<Aig>(
        Aig(std::move(aig))
    );
}


} // namespace bmm