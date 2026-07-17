#include "anf_to_bdd.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

namespace {

using Poly = polybori::BoolePolynomial;
using Exp  = polybori::BooleExponent;


// получить часть полинома без x_var
Poly split_without(const Poly& poly, uint32_t var)
{
    auto ring = poly.ring();

    Poly result(ring);


    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        auto mono = *it;

        Exp exp;

        bool has_var = false;


        for (auto v : mono)
        {
            if (v == var)
            {
                has_var = true;
                break;
            }

            exp.push_back(v);
        }


        if (!has_var)
        {
            result += Poly(exp, ring);
        }
    }


    return result;
}


// часть с x_var, но x_var удалён
Poly split_with(const Poly& poly, uint32_t var)
{
    auto ring = poly.ring();

    Poly result(ring);


    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        auto mono = *it;


        Exp exp;

        bool has_var = false;


        for (auto v : mono)
        {
            if (v == var)
            {
                has_var = true;
            }
            else
            {
                exp.push_back(v);
            }
        }


        if (has_var)
        {
            result += Poly(exp, ring);
        }
    }


    return result;
}



// поиск переменной для разложения
uint32_t choose_variable(const Poly& poly)
{
    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        auto mono = *it;

        for (auto v : mono)
        {
            return v;
        }
    }

    return 0;
}



sylvan::Bdd convert(const Poly& poly)
{

    if (poly == 0)
        return sylvan::Bdd::bddZero();


    if (poly == 1)
        return sylvan::Bdd::bddOne();



    uint32_t var = choose_variable(poly);



    /*
        p = p0 XOR x*p1

        low  = p0

        high = p0 XOR p1
    */


    auto p0 = split_without(poly, var);

    auto p1 = split_with(poly, var);


    auto low = convert(p0);


    auto high_poly = p0 + p1;

    auto high = convert(high_poly);



    return sylvan::Bdd::bddVar(var)
        .Ite(high, low);
}


}



Result<Bdd> anf_to_bdd(const Anf& anf)
{
    ZoneScoped;


#ifdef BMM_HAVE_BRIAL

    auto root = convert(anf.raw());


    return ok<Bdd>(
        Bdd(root, anf.n_vars())
    );


#else

    return fail<Bdd>(
        ErrorCode::NotImplemented,
        "anf_to_bdd requires BRiAl"
    );

#endif

}


}