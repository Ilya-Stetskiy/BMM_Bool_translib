#include "aig_to_thr.hpp"

#include <tracy/Tracy.hpp>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <vector>
#include <array>
#include <cmath>
#include <memory>
#include "ortools/linear_solver/linear_solver.h"

namespace bmm {

Result<Thr> aig_to_thr(const Aig& aig) {
    ZoneScoped;
    const auto& net = aig.raw();
    if (net.num_pos() != 1) {
        return fail<Thr>(ErrorCode::Unsupported, "aig_to_thr: ожидается ровно один PO");
    }
    const uint32_t n = aig.n_vars();
    if (n > kMaxTruthTableVars) {
        return fail<Thr>(ErrorCode::TooManyVariables, "aig_to_thr: слишком много переменных");
    }

  try {
    uint64_t num_states = uint64_t{1} << n;

    // We use a flat vector of uint8_t for thread safety (no bit-packing data races)
    std::vector<uint8_t> tt(num_states, 0);

    // Pre-extract gates in topological order
    struct FlatGate {
        uint32_t dest;
        uint32_t src0;
        bool inv0;
        uint32_t src1;
        bool inv1;
    };
    std::vector<FlatGate> gates;
    gates.reserve(net.num_gates());
    net.foreach_gate([&](auto node) {
        uint32_t dest = net.node_to_index(node);
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });
        gates.push_back({
            dest,
            static_cast<uint32_t>(net.node_to_index(net.get_node(fanins[0]))),
            net.is_complemented(fanins[0]),
            static_cast<uint32_t>(net.node_to_index(net.get_node(fanins[1]))),
            net.is_complemented(fanins[1])
        });
    });

    // PO info
    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });
    uint32_t po_node = net.node_to_index(net.get_node(po_sig));
    bool po_inv = net.is_complemented(po_sig);

    // PI node indices
    std::vector<uint32_t> pi_nodes(n);
    uint32_t pi_idx = 0;
    net.foreach_pi([&](auto node) {
        pi_nodes[pi_idx++] = net.node_to_index(node);
    });

    uint32_t net_size = net.size();
    uint32_t const_node = net.node_to_index(net.get_node(net.get_constant(false)));

    // TBB parallel simulation to satisfy TBB scaling requirement
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_states), [&](const tbb::blocked_range<size_t>& r) {
        std::vector<uint8_t> node_vals(net_size, 0);
        for (size_t idx = r.begin(); idx < r.end(); ++idx) {
            node_vals[const_node] = 0;
            for (uint32_t i = 0; i < n; ++i) {
                node_vals[pi_nodes[i]] = (idx >> i) & 1;
            }
            for (const auto& gate : gates) {
                uint8_t val0 = node_vals[gate.src0] ^ (gate.inv0 ? 1 : 0);
                uint8_t val1 = node_vals[gate.src1] ^ (gate.inv1 ? 1 : 0);
                node_vals[gate.dest] = val0 & val1;
            }
            tt[idx] = node_vals[po_node] ^ (po_inv ? 1 : 0);
        }
    });

    // Initialize OR-Tools MIP solver (SCIP, fallback to SAT or CBC if SCIP is not available)
    std::unique_ptr<operations_research::MPSolver> solver(
        operations_research::MPSolver::CreateSolver("SCIP"));
    if (!solver) {
        solver.reset(operations_research::MPSolver::CreateSolver("SAT"));
        if (!solver) {
            solver.reset(operations_research::MPSolver::CreateSolver("CBC"));
        }
    }
    if (!solver) {
        return fail<Thr>(ErrorCode::Unsupported, "aig_to_thr: OR-Tools solver backend not available");
    }

    const double infinity = solver->infinity();

    // Create variables w_0..w_{n-1} and theta (bounds: [-1000.0, 1000.0], integer)
    const double max_val = 1000.0;
    std::vector<operations_research::MPVariable*> w(n);
    for (uint32_t i = 0; i < n; ++i) {
        w[i] = solver->MakeIntVar(-max_val, max_val, "w_" + std::to_string(i));
    }
    operations_research::MPVariable* theta = solver->MakeIntVar(-max_val, max_val, "theta");

    // Add inequalities
    // If f(x) = 1: sum(w_i * x_i) - theta >= 0   ->   sum(w_i * x_i) - theta in [0, infinity]
    // If f(x) = 0: sum(w_i * x_i) - theta <= -1  ->   sum(w_i * x_i) - theta in [-infinity, -1]
    for (uint64_t idx = 0; idx < num_states; ++idx) {
        double lb, ub;
        if (tt[idx]) {
            lb = 0.0;
            ub = infinity;
        } else {
            lb = -infinity;
            ub = -1.0;
        }
        operations_research::MPConstraint* const c = solver->MakeRowConstraint(lb, ub);
        for (uint32_t i = 0; i < n; ++i) {
            if ((idx >> i) & 1ULL) {
                c->SetCoefficient(w[i], 1.0);
            }
        }
        c->SetCoefficient(theta, -1.0);
    }

    // Solve
    const operations_research::MPSolver::ResultStatus result_status = solver->Solve();
    if (result_status == operations_research::MPSolver::OPTIMAL ||
        result_status == operations_research::MPSolver::FEASIBLE) {
        std::vector<int64_t> weights(n);
        for (uint32_t i = 0; i < n; ++i) {
            weights[i] = static_cast<int64_t>(std::round(w[i]->solution_value()));
        }
        int64_t theta_val = static_cast<int64_t>(std::round(theta->solution_value()));

        // ДОБАВЛЕНО: верификация решения солвера перед выдачей — тот же
        // паттерн, что bdd/bdd_to_thr.cpp::verify_threshold_from_tt (там —
        // единственное место в проекте, где это уже было сделано). tt уже
        // построена выше, повторный проход по num_states точкам — та же
        // цена, что и построение ограничений, на порядок дешевле самого
        // ILP-solve.
        for (uint64_t idx = 0; idx < num_states; ++idx) {
            int64_t sum = 0;
            for (uint32_t i = 0; i < n; ++i) {
                if ((idx >> i) & 1ULL) sum += weights[i];
            }
            bool predicted = (sum >= theta_val);
            if (predicted != (tt[idx] != 0)) {
                return fail<Thr>(ErrorCode::Unsupported,
                    "aig_to_thr: решение солвера не проходит верификацию по исходной функции");
            }
        }

        return ok<Thr>(Thr(std::move(weights), theta_val));
    }

    return fail<Thr>(ErrorCode::Unsupported, "aig_to_thr: функция не является пороговой");

  } catch (const std::bad_alloc&) {
      return fail<Thr>(ErrorCode::OutOfMemory, "aig_to_thr: исчерпана память при построении ILP-модели");
  } catch (const std::exception& e) {
      return fail<Thr>(ErrorCode::InvalidArgument, std::string("aig_to_thr error: ") + e.what());
  }
}

}  // namespace bmm
