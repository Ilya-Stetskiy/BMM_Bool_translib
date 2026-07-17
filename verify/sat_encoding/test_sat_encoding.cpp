// verify/sat_encoding/test_sat_encoding.cpp — прямая проверка Tseitin-
// кодирования и SAT-сравнения эквивалентности (encode_aig_tseitin,
// check_aig_equivalence_via_sat), которая иначе выполняется только
// косвенно, изнутри test_<format>.cpp, и только когда translate() для
// To=Aig УЖЕ успешно завершился (см. test_runner.hpp) — пока все функции
// трансляции стабы (NotImplemented), этот путь не выполняется вообще. Баг,
// из-за которого входы схем A и B в miter-формуле не были связаны между
// собой (см. отчёт от 17.07.2026 — любая пара схем, включая идентичные,
// давала SAT с бессмысленным "контрпримером"), оставался бы незамеченным
// сколь угодно долго именно из-за этого слепого пятна. Этот файл — прямая,
// независимая от стабов трансляции проверка.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "verify/ground_truth/ground_truth.hpp"
#include "verify/sat_encoding/sat_encoding.hpp"

using namespace bmm::verify;

namespace {

// Ограничение размера ради времени прогона: в отличие от exhaustive-сравнения
// с ground truth, здесь не нужно покрывать n вплоть до kMaxReferenceAigVars —
// баги в связывании входов A/B (единственный класс уязвимости, специфичный
// именно для CNF-кодирования, см. отчёт от 17.07.2026) проявляются уже на
// n=1-2, дальше только замедляют прогон SAT-солвером.
constexpr uint32_t kTestMaxVars = 6;

bool eval_gt(const GroundTruthFunction& gt, const std::vector<bool>& a) {
    uint64_t idx = 0;
    for (uint32_t i = 0; i < a.size(); ++i) {
        if (a[i]) idx |= (uint64_t{1} << i);
    }
    return gt.evaluate(idx);
}

}  // namespace

TEST_CASE("check_aig_equivalence_via_sat: identical circuits are equivalent", "[core][sat]") {
    for (const auto& gt : growing_test_functions(kTestMaxVars)) {
        auto f = [&gt](const std::vector<bool>& a) { return eval_gt(gt, a); };
        // Две НЕЗАВИСИМЫЕ постройки одной и той же функции — не переиспользуем
        // один и тот же mockturtle::aig_network, чтобы проверить именно случай
        // "разные объекты, одна семантика" (как reference vs результат
        // перевода в реальном использовании), а не тривиальное сравнение
        // объекта с самим собой.
        auto aig_a = reference_aig_from_table(gt.n_vars, f);
        auto aig_b = reference_aig_from_table(gt.n_vars, f);

        auto result = check_aig_equivalence_via_sat(aig_a, aig_b);
        if (!result.checked) {
            WARN("SAT-солвер недоступен -- пропуск " + gt.name);
            continue;
        }
        INFO(gt.name);
        REQUIRE(result.equivalent);
    }
}

TEST_CASE("check_aig_equivalence_via_sat: different circuits are not equivalent", "[core][sat]") {
    const auto functions = growing_test_functions(kTestMaxVars);
    int checked_pairs = 0;
    for (size_t i = 0; i < functions.size() && checked_pairs < 30; ++i) {
        for (size_t j = i + 1; j < functions.size() && checked_pairs < 30; ++j) {
            const auto& gt_a = functions[i];
            const auto& gt_b = functions[j];
            if (gt_a.n_vars != gt_b.n_vars) continue;

            // Интересен именно случай "реально разные функции" -- пропускаем
            // пары, которые (несмотря на разные имена в growing_test_functions)
            // совпадают на всех точках.
            bool actually_different = false;
            for (uint64_t idx = 0; idx < (uint64_t{1} << gt_a.n_vars); ++idx) {
                if (gt_a.evaluate(idx) != gt_b.evaluate(idx)) {
                    actually_different = true;
                    break;
                }
            }
            if (!actually_different) continue;
            ++checked_pairs;

            auto f_a = [&gt_a](const std::vector<bool>& a) { return eval_gt(gt_a, a); };
            auto f_b = [&gt_b](const std::vector<bool>& a) { return eval_gt(gt_b, a); };
            auto aig_a = reference_aig_from_table(gt_a.n_vars, f_a);
            auto aig_b = reference_aig_from_table(gt_b.n_vars, f_b);

            auto result = check_aig_equivalence_via_sat(aig_a, aig_b);
            if (!result.checked) {
                WARN("SAT-солвер недоступен -- пропуск " + gt_a.name + " vs " + gt_b.name);
                continue;
            }
            INFO(gt_a.name + " vs " + gt_b.name);
            REQUIRE_FALSE(result.equivalent);

            // Контрпример должен быть НАСТОЯЩИМ: подставленный в обе функции,
            // обязан дать разные значения -- иначе "нашли расхождение" ничего
            // не доказывает. Именно это было сломано в исходном баге:
            // контрпример существовал, но был бессмысленным, т.к. входы A и B
            // были никак не связаны в CNF.
            REQUIRE(result.counterexample.has_value());
            if (result.counterexample) {
                INFO(gt_a.name + " vs " + gt_b.name + ": проверка контрпримера");
                REQUIRE(f_a(*result.counterexample) != f_b(*result.counterexample));
            }
        }
    }
    INFO("проверено пар: " + std::to_string(checked_pairs));
    REQUIRE(checked_pairs > 0);
}
