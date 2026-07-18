// verify/chain_utils.hpp — общая инфраструктура для тестов КОМПОЗИЦИИ
// переводов между представлениями (verify/chain_tests.cpp,
// verify/real_datasets_tests.cpp): обобщённое "любое из пяти представлений"
// (AnyRepr) + диспетчер по всем 20 функциям трансляции + прогон цепочки
// шагов + сравнение результата с исходным/эталоном.
//
// Вынесено в заголовок (а не продублировано в обоих .cpp), чтобы правка в
// одном месте (например, если появится 21-я функция трансляции) не могла
// разойтись между файлами.

#pragma once

#include "core/anf_repr.hpp"

#include "aig/aig_to_bdd.hpp"
#include "aig/aig_to_anf.hpp"
#include "aig/aig_to_thr.hpp"
#include "aig/aig_to_tt.hpp"
#include "aig/tt_to_aig.hpp"
#include "bdd/bdd_to_aig.hpp"
#include "bdd/bdd_to_anf.hpp"
#include "bdd/bdd_to_thr.hpp"
#include "bdd/bdd_to_tt.hpp"
#include "bdd/tt_to_bdd.hpp"
#include "anf/anf_to_aig.hpp"
#include "anf/anf_to_bdd.hpp"
#include "anf/anf_to_thr.hpp"
#include "anf/anf_to_tt.hpp"
#include "anf/tt_to_anf.hpp"
#include "thr/thr_to_aig.hpp"
#include "thr/thr_to_bdd.hpp"
#include "thr/thr_to_anf.hpp"
#include "thr/thr_to_tt.hpp"
#include "thr/tt_to_thr.hpp"

#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace bmm::chains {

enum class Repr { Aig, Bdd, Anf, Thr, Tt };

inline const char* repr_name(Repr r) {
    switch (r) {
        case Repr::Aig: return "Aig";
        case Repr::Bdd: return "Bdd";
        case Repr::Anf: return "Anf";
        case Repr::Thr: return "Thr";
        case Repr::Tt:  return "Tt";
    }
    return "?";
}

using AnyRepr = std::variant<Aig, Bdd, Anf, Thr, TruthTable>;

inline uint32_t n_vars_of(const AnyRepr& v) {
    return std::visit([](const auto& x) { return x.n_vars(); }, v);
}

inline bool evaluate_of(const AnyRepr& v, const Assignment& a) {
    return std::visit([&](const auto& x) { return x.evaluate(a); }, v);
}

template <class T>
Result<AnyRepr> wrap_result(Result<T> res) {
    if (!is_ok(res)) return fail<AnyRepr>(error(res).code, error(res).message);
    T val = value(res);
    return ok<AnyRepr>(AnyRepr(std::move(val)));
}

inline Result<AnyRepr> translate_step(Repr from, Repr to, const AnyRepr& in) {
    switch (from) {
        case Repr::Aig: {
            const auto& v = std::get<Aig>(in);
            switch (to) {
                case Repr::Bdd: return wrap_result(aig_to_bdd(v));
                case Repr::Anf: return wrap_result(aig_to_anf(v));
                case Repr::Thr: return wrap_result(aig_to_thr(v));
                case Repr::Tt:  return wrap_result(aig_to_tt(v));
                default: break;
            }
            break;
        }
        case Repr::Bdd: {
            const auto& v = std::get<Bdd>(in);
            switch (to) {
                case Repr::Aig: return wrap_result(bdd_to_aig(v));
                case Repr::Anf: return wrap_result(bdd_to_anf(v));
                case Repr::Thr: return wrap_result(bdd_to_thr(v));
                case Repr::Tt:  return wrap_result(bdd_to_tt(v));
                default: break;
            }
            break;
        }
        case Repr::Anf: {
            const auto& v = std::get<Anf>(in);
            switch (to) {
                case Repr::Aig: return wrap_result(anf_to_aig(v));
                case Repr::Bdd: return wrap_result(anf_to_bdd(v));
                case Repr::Thr: return wrap_result(anf_to_thr(v));
                case Repr::Tt:  return wrap_result(anf_to_tt(v));
                default: break;
            }
            break;
        }
        case Repr::Thr: {
            const auto& v = std::get<Thr>(in);
            switch (to) {
                case Repr::Aig: return wrap_result(thr_to_aig(v));
                case Repr::Bdd: return wrap_result(thr_to_bdd(v));
                case Repr::Anf: return wrap_result(thr_to_anf(v));
                case Repr::Tt:  return wrap_result(thr_to_tt(v));
                default: break;
            }
            break;
        }
        case Repr::Tt: {
            const auto& v = std::get<TruthTable>(in);
            switch (to) {
                case Repr::Aig: return wrap_result(tt_to_aig(v));
                case Repr::Bdd: return wrap_result(tt_to_bdd(v));
                case Repr::Anf: return wrap_result(tt_to_anf(v));
                case Repr::Thr: return wrap_result(tt_to_thr(v));
                default: break;
            }
            break;
        }
    }
    return fail<AnyRepr>(ErrorCode::InvalidArgument, "translate_step: from==to");
}

struct ChainResult {
    bool ok = false;
    std::string detail;
    double total_ms = 0.0;
    std::vector<double> step_ms;
    std::optional<AnyRepr> final_value;
};

// Перевод по цепочке представлений path[0] -> path[1] -> ... -> path[k].
// start должен реально хранить тип path[0].
inline ChainResult run_chain(const AnyRepr& start, const std::vector<Repr>& path) {
    ChainResult result;
    AnyRepr current = start;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        auto t0 = std::chrono::steady_clock::now();
        auto next = translate_step(path[i], path[i + 1], current);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        result.step_ms.push_back(ms);
        result.total_ms += ms;
        if (!is_ok(next)) {
            result.ok = false;
            result.detail = std::string(repr_name(path[i])) + "->" + repr_name(path[i + 1]) +
                             " failed: " + error(next).message;
            return result;
        }
        current = value(next);
    }
    result.ok = true;
    result.final_value = current;
    return result;
}

// Точное сравнение (перебор всех 2^n точек) — годится только при n достаточно
// малом (не более ~24, и на практике быстро только до ~16-18).
inline bool reprs_equivalent(const AnyRepr& a, const AnyRepr& b) {
    uint32_t n = n_vars_of(a);
    if (n != n_vars_of(b)) return false;
    uint64_t rows = uint64_t{1} << n;
    Assignment assignment(n, false);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        for (uint32_t i = 0; i < n; ++i) assignment[i] = (idx >> i) & 1u;
        if (evaluate_of(a, assignment) != evaluate_of(b, assignment)) return false;
    }
    return true;
}

// Приближённое сравнение через случайную выборку точек — единственный
// практичный вариант при больших n (реальные схемы вроде EPFL router с 60+
// входами или Коллегия выборщиков целиком, n=51): 2^n точек перебрать
// физически невозможно. num_samples точек, детерминированный seed для
// воспроизводимости. НЕ доказывает эквивалентность (это не SAT-солвер), но
// на практике ловит почти любую реальную ошибку перевода — если на, скажем,
// 2000 случайных точках расхождений нет, шанс, что расходится только на
// оставшихся 2^n-2000 точках и ни разу не попал в выборку, для функции с
// каким-либо структурным дефектом — исчезающе мал.
inline bool reprs_equivalent_sampled(const AnyRepr& a, const AnyRepr& b, uint64_t num_samples,
                                      uint64_t seed) {
    uint32_t n = n_vars_of(a);
    if (n != n_vars_of(b)) return false;
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> dist(0, (n >= 64) ? UINT64_MAX : ((uint64_t{1} << n) - 1));
    Assignment assignment(n, false);
    for (uint64_t s = 0; s < num_samples; ++s) {
        uint64_t idx = dist(rng);
        for (uint32_t i = 0; i < n; ++i) assignment[i] = (idx >> i) & 1u;
        if (evaluate_of(a, assignment) != evaluate_of(b, assignment)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Отчёт: печатает в stdout по ходу дела и копит markdown для файла.
// ---------------------------------------------------------------------------

struct Report {
    std::ostringstream md;
    int total_checks = 0;
    int failed_checks = 0;

    void section(const std::string& title) {
        md << "\n## " << title << "\n\n";
        std::printf("\n=== %s ===\n", title.c_str());
        std::fflush(stdout);
    }

    void bullet(bool ok, const std::string& text) {
        ++total_checks;
        if (!ok) ++failed_checks;
        md << "- **" << (ok ? "PASS" : "FAIL") << "** " << text << "\n";
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text.c_str());
        std::fflush(stdout);  // без этого вывод виден только после завершения
                               // ВСЕГО процесса при перенаправлении в файл/pipe
                               // (block-buffered stdio) — на долгих прогонах
                               // (реальные схемы/большие n) это выглядит как
                               // "зависло", хотя работа реально идёт.
    }

    void line(const std::string& text) {
        md << text << "\n";
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
    }

    void table_row(const std::vector<std::string>& cells) {
        md << "|";
        for (const auto& c : cells) md << " " << c << " |";
        md << "\n";
    }
};

}  // namespace bmm::chains
