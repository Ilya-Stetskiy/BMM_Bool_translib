// core/common.hpp — единый контракт bmm-translib.
//
// Обоснования решений (порядок бит, Result vs исключения, скоуп "один выход",
// BRiAl fallback, требования к параллелизму) — см. CONVENTIONS.md рядом с этим
// файлом. Здесь только типы и их реализация.
//
// Это не стаб: core/ полностью готов к использованию, студенты его не трогают
// (кроме как читают контракт перед тем как писать тела функций трансляции).

#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(BMM_FORCE_VARIANT_FALLBACK)
#  define BMM_HAS_STD_EXPECTED 0
#elif defined(__has_include)
#  if __has_include(<expected>) && defined(__cpp_lib_expected)
#    define BMM_HAS_STD_EXPECTED 1
#  else
#    define BMM_HAS_STD_EXPECTED 0
#  endif
#else
#  define BMM_HAS_STD_EXPECTED 0
#endif

#if BMM_HAS_STD_EXPECTED
#  include <expected>
#else
#  include <variant>
#endif

// mockturtle/kitty — header-only, поставляются mockturtle (см. CMakeLists.txt,
// MOCKTURTLE_ROOT). aig_network и dynamic_truth_table подключаем напрямую,
// это единственное место в репозитории, где common.hpp обязан знать о
// конкретной внешней библиотеке AIG-представления.
#include <kitty/dynamic_truth_table.hpp>
#include <kitty/operations.hpp>
#include <mockturtle/networks/aig.hpp>
#include <mockturtle/algorithms/simulation.hpp>

// Sylvan — sylvan_obj.hpp даёт RAII C++ обёртку sylvan::Bdd с автоматическим
// рефкаунтингом (bddVar/bddOne/bddZero/Ite/операторы). Именно её и используем,
// а не сырые BDD-хэндлы из sylvan.h — см. CONVENTIONS.md п.6 про Sylvan/Lace.
#include <sylvan_obj.hpp>

#if defined(__has_include)
#  if __has_include(<polybori.h>)
#    define BMM_HAVE_BRIAL 1
#    include <polybori.h>
#  else
#    define BMM_HAVE_BRIAL 0
#  endif
#else
#  define BMM_HAVE_BRIAL 0
#endif

namespace bmm {

// ---------------------------------------------------------------------------
// 1. Порядок переменных (CONVENTIONS.md п.1)
// ---------------------------------------------------------------------------

enum class BitOrder { LSB_FIRST };
inline constexpr BitOrder kBitOrder = BitOrder::LSB_FIRST;

// Присвоение значений переменным x0..x_{n-1}. assignment[i] — значение x_i.
// Индекс минтерма (для to_tt()/сравнения с TruthTable) — это assignment,
// прочитанный как двоичное число с x0 в младшем разряде (kBitOrder).
using Assignment = std::vector<bool>;

// Таблицы истинности материализуются только до этого числа переменных
// (2^24 строк — разумный потолок для in-memory kitty::dynamic_truth_table).
inline constexpr uint32_t kMaxTruthTableVars = 24;

// ---------------------------------------------------------------------------
// 2. Тип ошибок и Result<T> (CONVENTIONS.md п.2)
// ---------------------------------------------------------------------------

enum class ErrorCode {
    Success,          // используется вне Result<T> — там, где нужен голый статус
    NotImplemented,    // функция трансляции ещё не написана студентом
    Unsupported,       // вход вне скоупа контракта (напр. AIG с >1 PO)
    TooManyVariables,  // n_vars() > kMaxTruthTableVars при попытке to_tt()
    InvalidArgument,
    // Промежуточная структура (граф/BDD/CNF) не поместилась в память —
    // отдельно от TooManyVariables: TooManyVariables — это статическая,
    // предсказуемая по n_vars() граница ДО начала работы; OutOfMemory — это
    // std::bad_alloc, пойманный УЖЕ в процессе построения, когда
    // неконтролируемый рост графа (типичный пример — bdd_to_aig) исчерпал
    // память по факту, не по заранее известной оценке. См.
    // CONVENTIONS.md п.2а про паттерн graceful degradation.
    OutOfMemory,
};

struct Error {
    ErrorCode code;
    std::string message;
};

#if BMM_HAS_STD_EXPECTED
template <class T>
using Result = std::expected<T, Error>;

template <class T>
constexpr bool is_ok(const Result<T>& r) { return r.has_value(); }

template <class T>
const T& value(const Result<T>& r) { return r.value(); }

template <class T>
const Error& error(const Result<T>& r) { return r.error(); }

template <class T>
Result<T> ok(T v) { return Result<T>(std::move(v)); }

template <class T>
Result<T> fail(ErrorCode code, std::string message = {}) {
    return std::unexpected(Error{code, std::move(message)});
}
#else
template <class T>
using Result = std::variant<T, Error>;

template <class T>
constexpr bool is_ok(const Result<T>& r) { return std::holds_alternative<T>(r); }

template <class T>
const T& value(const Result<T>& r) { return std::get<T>(r); }

template <class T>
const Error& error(const Result<T>& r) { return std::get<Error>(r); }

template <class T>
Result<T> ok(T v) { return Result<T>(std::in_place_type<T>, std::move(v)); }

template <class T>
Result<T> fail(ErrorCode code, std::string message = {}) {
    return Result<T>(std::in_place_type<Error>, Error{code, std::move(message)});
}
#endif

// ---------------------------------------------------------------------------
// 3. TruthTable — вспомогательное представление (n <= kMaxTruthTableVars)
// ---------------------------------------------------------------------------

class TruthTable {
public:
    explicit TruthTable(uint32_t n_vars) : tt_(n_vars) {}
    explicit TruthTable(kitty::dynamic_truth_table tt) : tt_(std::move(tt)) {}

    uint32_t n_vars() const { return tt_.num_vars(); }

    bool evaluate(const Assignment& assignment) const {
        uint64_t index = 0;
        for (uint32_t i = 0; i < assignment.size(); ++i) {
            if (assignment[i]) index |= (uint64_t{1} << i);  // kBitOrder = LSB_FIRST
        }
        return kitty::get_bit(tt_, index) != 0;
    }

    // Тождественная операция — присутствует ради единообразия интерфейса
    // (generic-код в verify/ вызывает to_tt() на любом представлении не глядя
    // на его конкретный тип).
    Result<TruthTable> to_tt() const { return ok<TruthTable>(*this); }

    kitty::dynamic_truth_table& raw() { return tt_; }
    const kitty::dynamic_truth_table& raw() const { return tt_; }

private:
    kitty::dynamic_truth_table tt_;
};

// ---------------------------------------------------------------------------
// 4. Aig (CONVENTIONS.md п.4: ровно один PO)
// ---------------------------------------------------------------------------

class Aig {
public:
    explicit Aig(mockturtle::aig_network net) : net_(std::move(net)) {}

    uint32_t n_vars() const { return net_.num_pis(); }

    bool evaluate(const Assignment& assignment) const {
        // Явный поэлементный проход вместо std::copy(assignment...,
        // sim_values.begin()): assignment.size() == n_vars() — предусловие
        // (см. core/CONVENTIONS.md п.3), но std::copy при его нарушении
        // (assignment длиннее num_pis()) переполнил бы sim_values, а
        // std::vector<bool>::operator[] с проверкой границ — нет.
        std::vector<bool> sim_values(net_.num_pis());
        for (uint32_t i = 0; i < sim_values.size(); ++i) {
            sim_values[i] = i < assignment.size() && assignment[i];
        }
        const auto outputs = mockturtle::simulate<bool>(
            net_, mockturtle::default_simulator<bool>(sim_values));
        return outputs.at(0);
    }

    Result<TruthTable> to_tt() const {
        if (net_.num_pos() != 1) {
            return fail<TruthTable>(ErrorCode::Unsupported,
                                     "Aig::to_tt: ожидается ровно один PO");
        }
        if (n_vars() > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables, "");
        }
        const auto outputs = mockturtle::simulate<kitty::dynamic_truth_table>(
            net_, mockturtle::default_simulator<kitty::dynamic_truth_table>(
                      n_vars()));
        return ok<TruthTable>(TruthTable(outputs.at(0)));
    }

    mockturtle::aig_network& raw() { return net_; }
    const mockturtle::aig_network& raw() const { return net_; }

private:
    mockturtle::aig_network net_;
};

// ---------------------------------------------------------------------------
// 5. Bdd — обёртка над sylvan::Bdd (CONVENTIONS.md п.6: параллелизм — только
//    через Lace, никакого собственного). n_vars хранится явно: Sylvan
//    привязывает BDD к глобальным индексам переменных, а не к диапазону
//    [0, n) конкретной функции.
// ---------------------------------------------------------------------------

class Bdd {
public:
    Bdd(sylvan::Bdd root, uint32_t n_vars) : root_(std::move(root)), n_vars_(n_vars) {}

    uint32_t n_vars() const { return n_vars_; }

    // Точечное вычисление через "кубик"-BDD (конъюнкция литералов, заданных
    // assignment) вместо предполагаемого Eval(...) с неизвестной точной
    // сигнатурой: root_ истинен в точке assignment ровно тогда, когда cube
    // влечёт root_, т.е. (root_ AND cube) == cube. Используются только
    // документированные операторы sylvan::Bdd (bddVar/bddOne/!/&/==).
    bool evaluate(const Assignment& assignment) const {
        sylvan::Bdd cube = sylvan::Bdd::bddOne();
        for (uint32_t i = 0; i < n_vars_; ++i) {
            const bool v = i < assignment.size() && assignment[i];
            sylvan::Bdd lit = sylvan::Bdd::bddVar(i);
            if (!v) lit = !lit;
            cube = cube & lit;
        }
        return (root_ & cube) == cube;
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

    sylvan::Bdd& raw() { return root_; }
    const sylvan::Bdd& raw() const { return root_; }

private:
    sylvan::Bdd root_;
    uint32_t n_vars_;
};

// ---------------------------------------------------------------------------
// 6. Anf (CONVENTIONS.md п.5: BRiAl, если заголовки найдены, иначе fallback)
// ---------------------------------------------------------------------------

#if BMM_HAVE_BRIAL

class Anf {
public:
    explicit Anf(BoolePolynomial poly, uint32_t n_vars)
        : poly_(std::move(poly)), n_vars_(n_vars) {}

    uint32_t n_vars() const { return n_vars_; }

    // Подстановка константы в каждую переменную через BoolePolynomial::set(idx,
    // value) (стандартный элиминационный примитив PolyBoRi/BRiAl), затем
    // проверка hasConstantPart() на результате — после подстановки константы
    // во все n переменных полином вырождается либо в 0, либо в константу "1".
    bool evaluate(const Assignment& assignment) const {
        BoolePolynomial p = poly_;
        for (uint32_t i = 0; i < n_vars_; ++i) {
            const bool v = i < assignment.size() && assignment[i];
            p = p.set(i, v ? 1 : 0);
        }
        return p.hasConstantPart();
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
// squarefree-мономами: сложение = XOR, т.е. монoм либо присутствует, либо
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

// ---------------------------------------------------------------------------
// 7. Thr — пороговая функция f(x) = [sum(w_i * x_i) >= theta]
// ---------------------------------------------------------------------------

class Thr {
public:
    Thr(std::vector<int64_t> weights, int64_t theta)
        : weights_(std::move(weights)), theta_(theta) {}

    uint32_t n_vars() const { return static_cast<uint32_t>(weights_.size()); }

    bool evaluate(const Assignment& assignment) const {
        int64_t sum = 0;
        for (uint32_t i = 0; i < weights_.size(); ++i) {
            if (i < assignment.size() && assignment[i]) sum += weights_[i];
        }
        return sum >= theta_;
    }

    Result<TruthTable> to_tt() const {
        const uint32_t n = n_vars();
        if (n > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables, "");
        }
        TruthTable tt(n);
        Assignment assignment(n, false);
        const uint64_t rows = uint64_t{1} << n;
        for (uint64_t idx = 0; idx < rows; ++idx) {
            for (uint32_t b = 0; b < n; ++b) assignment[b] = (idx >> b) & 1u;
            if (evaluate(assignment)) kitty::set_bit(tt.raw(), idx);
        }
        return ok<TruthTable>(std::move(tt));
    }

    const std::vector<int64_t>& weights() const { return weights_; }
    int64_t theta() const { return theta_; }

private:
    std::vector<int64_t> weights_;
    int64_t theta_;
};

// ---------------------------------------------------------------------------
// 8. Концепт "представление" — компилятор проверяет, что все пять типов
//    реализуют один и тот же контракт (CONVENTIONS.md п.3).
// ---------------------------------------------------------------------------

template <class T>
concept Representation = requires(const T& t, const Assignment& a) {
    { t.n_vars() } -> std::convertible_to<uint32_t>;
    { t.evaluate(a) } -> std::convertible_to<bool>;
    { t.to_tt() } -> std::same_as<Result<TruthTable>>;
};

static_assert(Representation<TruthTable>);
static_assert(Representation<Aig>);
static_assert(Representation<Bdd>);
static_assert(Representation<Anf>);
static_assert(Representation<Thr>);

}  // namespace bmm
