// verify/diag_boundary_timing.cpp — ВРЕМЕННЫЙ диагностический инструмент
// (удаляется после того, как числа записаны, тот же принцип, что и у
// остальных verify/diag_*.cpp): замер времени всех X_to_tt/tt_to_X функций
// на границе старого (24) и нового (32) kMaxTruthTableVars, на одной и той
// же простой функции (AND всех переменных — тривиально строится во всех
// пяти представлениях без завязки на код самих 20 функций).

#include <bmm/aig/aig_to_tt.hpp>
#include <bmm/aig/tt_to_aig.hpp>
#include <bmm/anf/anf_to_tt.hpp>
#include <bmm/anf/tt_to_anf.hpp>
#include <bmm/bdd/bdd_to_tt.hpp>
#include <bmm/bdd/tt_to_bdd.hpp>
#include <bmm/thr/thr_to_tt.hpp>
#include <bmm/thr/tt_to_thr.hpp>
#include <bmm/core/anf_repr.hpp>
#include <bmm/core/common.hpp>

#include <mockturtle/networks/aig.hpp>
#include <sylvan_obj.hpp>

#include <chrono>
#include <cstdio>
#include <vector>

using namespace bmm;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

TruthTable make_and_tt(uint32_t n) {
    TruthTable tt(n);
    kitty::set_bit(tt.raw(), (uint64_t{1} << n) - 1);  // единственная 1 — все биты выставлены
    return tt;
}

Aig make_and_aig(uint32_t n) {
    mockturtle::aig_network net;
    std::vector<mockturtle::aig_network::signal> pis(n);
    for (uint32_t i = 0; i < n; ++i) pis[i] = net.create_pi();
    auto acc = pis[0];
    for (uint32_t i = 1; i < n; ++i) acc = net.create_and(acc, pis[i]);
    net.create_po(acc);
    return Aig(std::move(net));
}

Anf make_and_anf(uint32_t n) {
#if BMM_HAVE_BRIAL
    polybori::BoolePolyRing ring(n);
    polybori::BoolePolynomial poly(ring.one());
    for (uint32_t i = 0; i < n; ++i) poly *= ring.variable(i);
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    std::vector<uint32_t> all_vars(n);
    for (uint32_t i = 0; i < n; ++i) all_vars[i] = i;
    poly.add_monomial(std::move(all_vars));
    return Anf(std::move(poly), n);
#endif
}

Thr make_and_thr(uint32_t n) {
    return Thr(std::vector<int64_t>(n, 1), n);  // sum(x_i) >= n  <=>  все x_i=1
}

template <class F>
void row(const char* name, F&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    std::printf("  %-14s %.3f мс\n", name, ms_since(t0));
    std::fflush(stdout);
}

void run_boundary(uint32_t n) {
    std::printf("=== n=%u ===\n", n);

    row("tt_to_aig", [&] { auto r = tt_to_aig(make_and_tt(n)); (void)is_ok(r); });
    row("aig_to_tt", [&] { auto r = aig_to_tt(make_and_aig(n)); (void)is_ok(r); });

    row("tt_to_bdd", [&] { auto r = tt_to_bdd(make_and_tt(n)); (void)is_ok(r); });
    row("bdd_to_tt", [&] {
        auto bdd = tt_to_bdd(make_and_tt(n));
        if (is_ok(bdd)) { auto r = bdd_to_tt(value(bdd)); (void)is_ok(r); }
    });

    row("tt_to_anf", [&] { auto r = tt_to_anf(make_and_tt(n)); (void)is_ok(r); });
    row("anf_to_tt", [&] { auto r = anf_to_tt(make_and_anf(n)); (void)is_ok(r); });

    row("tt_to_thr", [&] { auto r = tt_to_thr(make_and_tt(n)); (void)is_ok(r); });
    row("thr_to_tt", [&] { auto r = thr_to_tt(make_and_thr(n)); (void)is_ok(r); });

    std::printf("\n");
}

int g_result = 0;

VOID_TASK_0(main_task) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    run_boundary(24);
    run_boundary(32);

    sylvan::sylvan_quit();
}

}  // namespace

int main() {
    const int n_workers = 0;
    const size_t deque_size = 1ULL << 21;
    lace_start(n_workers, deque_size);
    RUN(main_task);
    lace_stop();
    return g_result;
}
