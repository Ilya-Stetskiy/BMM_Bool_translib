#include "anf_to_thr.hpp"

#include "anf_to_tt.hpp"

#include <ortools/linear_solver/linear_solver.h>

#include <cmath>
#include <memory>
#include <variant>
#include <vector>


namespace bmm {


Result<Thr> anf_to_thr(const Anf& anf)
{
    const uint32_t n = anf.n_vars();


    // LP становится огромным после 20-22 переменных
    if (n > 20)
    {
        return fail<Thr>(
            ErrorCode::TooManyVariables,
            "anf_to_thr: too many variables for LP");
    }



    // =====================================
    // 1. ANF -> Truth Table
    // =====================================

    auto tt_result = anf_to_tt(anf);


    if (std::holds_alternative<Error>(tt_result))
    {
        const auto& err =
            std::get<Error>(tt_result);

        return fail<Thr>(
            err.code,
            "anf_to_thr: anf_to_tt failed");
    }


    TruthTable tt =
        std::move(
            std::get<TruthTable>(tt_result));



    // =====================================
    // 2. Создание LP/ILP-солвера
    // =====================================

    using namespace operations_research;

    // Фолбэк-цепочка солверов, как в aig/aig_to_thr.cpp: SCIP обычно
    // быстрее и надёжнее CBC на такого рода MIP, SAT — резерв на случай,
    // если SCIP не собран в образе. CBC — последний резерв (раньше был
    // единственным вариантом вообще, без фолбэка).
    std::unique_ptr<MPSolver> solver(MPSolver::CreateSolver("SCIP"));
    if (!solver) {
        solver.reset(MPSolver::CreateSolver("SAT"));
    }
    if (!solver) {
        solver.reset(MPSolver::CreateSolver("CBC"));
    }
    if (!solver) {
        return fail<Thr>(
            ErrorCode::Unsupported,
            "anf_to_thr: OR-Tools solver backend not available");
    }


    std::vector<const MPVariable*> weights(n);



    for(uint32_t i = 0; i < n; i++)
    {
        weights[i] =
            solver->MakeIntVar(
                -100000,
                100000,
                "w_" + std::to_string(i));
    }


    auto theta =
        solver->MakeIntVar(
            -100000,
            100000,
            "theta");



    // =====================================
    // 3. Ограничения пороговой функции
    //
    // f(x)=1:
    //
    // sum(w_i*x_i) >= theta
    //
    //
    // f(x)=0:
    //
    // sum(w_i*x_i) <= theta - 1
    //
    // =====================================


    const uint64_t rows =
        1ULL << n;



    for(uint64_t mask = 0;
        mask < rows;
        mask++)
    {

        bool value =
            kitty::get_bit(
                tt.raw(),
                mask);



        MPConstraint* c;


        if(value)
        {
            // sum(w*x)-theta >= 0

            c =
                solver->MakeRowConstraint(
                    0,
                    solver->infinity());
        }
        else
        {
            // sum(w*x)-theta <= -1

            c =
                solver->MakeRowConstraint(
                    -solver->infinity(),
                    -1);
        }



        for(uint32_t i = 0;
            i < n;
            i++)
        {
            if(mask & (1ULL << i))
            {
                c->SetCoefficient(
                    weights[i],
                    1);
            }
        }


        c->SetCoefficient(
            theta,
            -1);
    }



    // =====================================
    // 4. Решение
    // =====================================


    auto status =
        solver->Solve();



    if(status != MPSolver::OPTIMAL)
    {
        return fail<Thr>(
            ErrorCode::Unsupported,
            "anf_to_thr: not a threshold function");
    }



    // =====================================
    // 5. Создание Thr
    // =====================================


    std::vector<int64_t> result_weights(n);



    for(uint32_t i = 0;
        i < n;
        i++)
    {
        result_weights[i] =
            static_cast<int64_t>(
                std::llround(
                    weights[i]->solution_value()));
    }



    int64_t result_theta =
        static_cast<int64_t>(
            std::llround(
                theta->solution_value()));



    return ok<Thr>(
        Thr(
            std::move(result_weights),
            result_theta));
}


} // namespace bmm