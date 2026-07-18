#include "tt_to_thr.hpp"

#include <tracy/Tracy.hpp>
#include <ortools/linear_solver/linear_solver.h>
#include <omp.h>
#include <vector>
#include <memory>
#include <new>
#include <string>

namespace bmm {

Result<Thr> tt_to_thr(const TruthTable& tt) {
    // Обязательный макрос для профилирования через Tracy
    ZoneScoped;

    // Глобальный перехват bad_alloc для защиты от OOM
    try {
        const uint32_t n = tt.n_vars();
        
        // Снижаем лимит: прямой ILP-подход нежизнеспособен выше n=16
        if (n > 16) {
            return fail<Thr>(ErrorCode::Unsupported, "n > 16 requires a heuristic or incremental ILP solver");
        }

        const uint64_t num_minterms = 1ULL << n;
        std::vector<int8_t> is_on(num_minterms);

        // Отслеживание унитарности (Unateness)
        std::vector<bool> acts_pos(n, false);
        std::vector<bool> acts_neg(n, false);

        // Параллельный обход таблицы истинности через OpenMP
        #pragma omp parallel
        {
            Assignment ass(n); 
            std::vector<bool> local_pos(n, false);
            std::vector<bool> local_neg(n, false);

            #pragma omp for schedule(static)
            for (uint64_t i = 0; i < num_minterms; ++i) {
                for (uint32_t b = 0; b < n; ++b) {
                    ass[b] = (i >> b) & 1; // BitOrder::LSB_FIRST
                }
                
                bool val = tt.evaluate(ass);
                is_on[i] = val ? 1 : 0;
                
                if (val) {
                    for (uint32_t b = 0; b < n; ++b) {
                        if (!ass[b]) local_pos[b] = true; 
                    }
                } else {
                    for (uint32_t b = 0; b < n; ++b) {
                        if (ass[b]) local_neg[b] = true;
                    }
                }
            }

            #pragma omp critical
            {
                for (uint32_t b = 0; b < n; ++b) {
                    if (local_pos[b]) acts_pos[b] = true;
                    if (local_neg[b]) acts_neg[b] = true;
                }
            }
        }

        // ПРЕДФИЛЬТР: Проверка на унитарность
        // Если переменная действует и положительно, и отрицательно, это не пороговая функция.
        for (uint32_t b = 0; b < n; ++b) {
            if (acts_pos[b] && acts_neg[b]) {
                return fail<Thr>(ErrorCode::Unsupported, "Failed unateness pre-filter: not a threshold function");
            }
        }

        // Построение ILP-модели через Google OR-Tools
        std::unique_ptr<operations_research::MPSolver> solver(
            operations_research::MPSolver::CreateSolver("SCIP"));
        
        if (!solver) {
            return fail<Thr>(ErrorCode::Unsupported, "OR-Tools SCIP solver is not available");
        }

        std::vector<operations_research::MPVariable*> w(n);
        for (uint32_t b = 0; b < n; ++b) {
            w[b] = solver->MakeIntVar(-100000, 100000, "w_" + std::to_string(b));
        }
        
        operations_research::MPVariable* T_var = solver->MakeIntVar(-100000, 100000, "T");

        for (uint64_t i = 0; i < num_minterms; ++i) {
            operations_research::MPConstraint* ct = nullptr;
            
            if (is_on[i] == 1) {
                ct = solver->MakeRowConstraint(0, solver->infinity());
            } else {
                ct = solver->MakeRowConstraint(-solver->infinity(), -1);
            }
            
            ct->SetCoefficient(T_var, -1.0);
            
            for (uint32_t b = 0; b < n; ++b) {
                if ((i >> b) & 1) {
                    ct->SetCoefficient(w[b], 1.0);
                }
            }
        }

        operations_research::MPSolver::ResultStatus status = solver->Solve();

        if (status == operations_research::MPSolver::OPTIMAL || 
            status == operations_research::MPSolver::FEASIBLE) {
            
            std::vector<int64_t> weights(n);
            for (uint32_t b = 0; b < n; ++b) {
                weights[b] = static_cast<int64_t>(w[b]->solution_value());
            }
            int64_t threshold = static_cast<int64_t>(T_var->solution_value());

            return ok(Thr{weights, threshold});
        } else {
            return fail<Thr>(ErrorCode::Unsupported, "The given truth table is not a threshold function");
        }

    } catch (const std::bad_alloc&) {
        return fail<Thr>(ErrorCode::OutOfMemory, "Not enough memory to build ILP constraints for the TruthTable");
    } catch (const std::exception& e) {
        return fail<Thr>(ErrorCode::Unsupported, e.what());
    }
}

} // namespace bmm