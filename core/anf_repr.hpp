// core/anf_repr.hpp — тип Anf (BRiAl BoolePolynomial или AnfFallback), см.
// core/CONVENTIONS.md п.5.
//
// Сознательно ОТДЕЛЁН от core/common.hpp: раньше BRiAl-детектирование
// (__has_include(<polybori.h>), BMM_HAVE_BRIAL, #include <polybori.h>) жило
// прямо в common.hpp, который транзитивно тянут вообще ВСЕ файлы проекта
// через bmm_core (INTERFACE-таргет) — то есть проблема с BRiAl (например,
// конфликт макросов/заголовков с TBB или OR-Tools в одной единице
// трансляции) ломала сборку функций, которые вообще не имеют отношения к
// ANF (aig_to_thr, thr_to_bdd и т.п.), а "выключить BRiAl" было
// решением на весь проект разом, а не на конкретную функцию.
//
// Теперь core/anf_repr.hpp подключают явно только те файлы, которые реально
// строят или читают Anf: aig/aig_to_anf.*, bdd/bdd_to_anf.*, thr/thr_to_anf.*,
// весь anf/*.*, verify/reference_builders.*, verify/config_dump.cpp. Никакой
// другой файл в проекте (в частности — aig_to_thr.cpp, где как раз ловился
// конфликт TBB+OR-Tools+BRiAl в одной единице трансляции) больше не видит
// <polybori.h> вовсе, если сам явно не подключит этот заголовок.
//
// API типа Anf не изменился ни на символ по сравнению с тем, что раньше
// жило в common.hpp — тот же конструктор (BoolePolynomial|AnfFallback, n),
// тот же .raw(), тот же BMM_HAVE_BRIAL-переключатель веток. Код, уже
// написанный против старого common.hpp (например, anf/tt_to_anf.cpp,
// thr/thr_to_anf.cpp), продолжает работать без изменений — он получает Anf
// транзитивно через свой собственный .hpp, который теперь включает этот
// файл вместо голого common.hpp.

#pragma once

#include "core/common.hpp"

// BMM_FORCE_ANF_FALLBACK — тот же паттерн, что BMM_FORCE_VARIANT_FALLBACK у
// Result<T> в core/common.hpp: форсирует AnfFallback-ветку даже если BRiAl
// физически доступен в include-путях. Нужен, чтобы сравнить BRiAl и
// AnfFallback на ОДНОМ и том же образе без пересборки Docker-контейнера без
// BRiAl (см. aig/README.md — эмпирическая проверка, действительно ли BRiAl
// ограничивает параллелизм, и что даёт отказ от него).
#if defined(BMM_FORCE_ANF_FALLBACK)
#  define BMM_HAVE_BRIAL 0
#elif defined(__has_include)
#  if __has_include(<polybori.h>)
#    define BMM_HAVE_BRIAL 1
#    include <polybori.h>
using polybori::BoolePolynomial;
using polybori::BoolePolyRing;
using polybori::BooleMonomial;
#  else
#    define BMM_HAVE_BRIAL 0
#  endif
#else
#  define BMM_HAVE_BRIAL 0
#endif

namespace bmm {

// ---------------------------------------------------------------------------
// Anf (CONVENTIONS.md п.5: BRiAl, если заголовки найдены, иначе fallback)
// ---------------------------------------------------------------------------

#if BMM_HAVE_BRIAL

class Anf {
public:
    explicit Anf(BoolePolynomial poly, uint32_t n_vars)
        : poly_(std::move(poly)), n_vars_(n_vars) {}

    uint32_t n_vars() const { return n_vars_; }

    // Прямой проход по термам полинома вместо substitute_variables(ring,
    // idx2poly, poly_): предыдущая реализация на каждый вызов evaluate()
    // клонировала и перестраивала весь ZDD полинома через
    // substitute_variables — при переборе всех 2^n точек (to_tt(),
    // verify::find_mismatch, verify::compute_chow_parameters и т.д. каждый
    // прогоняют полный перебор) это O(2^n) полных ZDD-перестроений вместо
    // O(2^n * |термов|) простых проверок. Терм (BooleMonomial) истинен на
    // точке assignment ровно тогда, когда все его переменные равны 1 — тот
    // же алгоритм, что и AnfFallback::evaluate ниже, только по
    // BoolePolynomial/BooleMonomial итераторам BRiAl вместо std::set.
    // Не пересобиралось этой сессией (BRiAl доступен только в контейнере
    // сборки, недоступном с этого хоста) — перепроверить test_anf/test_aig
    // перед мёржем.
    bool evaluate(const Assignment& assignment) const {
        bool acc = false;
        for (BoolePolynomial::const_iterator term = poly_.begin(); term != poly_.end(); ++term) {
            bool value = true;
            for (BooleMonomial::const_iterator var = term->begin(); var != term->end(); ++var) {
                const uint32_t idx = *var;
                if (!(idx < assignment.size() && assignment[idx])) {
                    value = false;
                    break;
                }
            }
            acc ^= value;
        }
        return acc;
    }

    Result<TruthTable> to_tt() const {
        if (n_vars_ > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables, "");
        }
        TruthTable tt(n_vars_);
        Assignment assignment(n_vars_, false);
        const uint64_t rows = uint64_t{1} << n_vars_;
        for (uint64_t idx = 0; idx < rows; ++idx) {
            for (uint32_t b = 0; b < n_vars_; ++b) assignment[b] = (idx >> b) & 1u;
            if (evaluate(assignment)) kitty::set_bit(tt.raw(), idx);
        }
        return ok<TruthTable>(std::move(tt));
    }

    BoolePolynomial& raw() { return poly_; }
    const BoolePolynomial& raw() const { return poly_; }

private:
    BoolePolynomial poly_;
    uint32_t n_vars_;
};

#else  // !BMM_HAVE_BRIAL — резервное представление, см. CONVENTIONS.md п.5

// Моном: отсортированный список индексов переменных, входящих в него
// (x_i * x_j, i<j). Пустой список — константный моном "1".
using Monomial = std::vector<uint32_t>;

// Полином Жегалкина — это F2-векторное пространство над свободными
// squarefree-мономами: сложение = XOR, т.е. моном либо присутствует, либо
// нет. std::set с лексикографическим сравнением даёт именно это множество
// при условии, что вставка дубликата **удаляет** существующий элемент
// (см. AnfFallback::add_monomial) — тождество x XOR x = 0.
class AnfFallback {
public:
    void add_monomial(Monomial m) {
        std::sort(m.begin(), m.end());
        auto [it, inserted] = monomials_.insert(m);
        if (!inserted) monomials_.erase(it);  // XOR-сокращение
    }

    bool evaluate(const Assignment& assignment) const {
        bool acc = false;
        for (const auto& mono : monomials_) {
            bool term = true;
            for (uint32_t v : mono) {
                if (!(v < assignment.size() && assignment[v])) { term = false; break; }
            }
            acc ^= term;
        }
        return acc;
    }

    const std::set<Monomial>& monomials() const { return monomials_; }

private:
    std::set<Monomial> monomials_;
};

class Anf {
public:
    Anf(AnfFallback poly, uint32_t n_vars) : poly_(std::move(poly)), n_vars_(n_vars) {}

    uint32_t n_vars() const { return n_vars_; }
    bool evaluate(const Assignment& assignment) const { return poly_.evaluate(assignment); }

    Result<TruthTable> to_tt() const {
        if (n_vars_ > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables, "");
        }
        TruthTable tt(n_vars_);
        Assignment assignment(n_vars_, false);
        const uint64_t rows = uint64_t{1} << n_vars_;
        for (uint64_t idx = 0; idx < rows; ++idx) {
            for (uint32_t b = 0; b < n_vars_; ++b) assignment[b] = (idx >> b) & 1u;
            if (evaluate(assignment)) kitty::set_bit(tt.raw(), idx);
        }
        return ok<TruthTable>(std::move(tt));
    }

    AnfFallback& raw() { return poly_; }
    const AnfFallback& raw() const { return poly_; }

private:
    AnfFallback poly_;
    uint32_t n_vars_;
};

#endif  // BMM_HAVE_BRIAL

static_assert(Representation<Anf>);

}  // namespace bmm
