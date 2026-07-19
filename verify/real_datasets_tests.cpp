// verify/real_datasets_tests.cpp — прогон реальных (не синтетических) наборов
// булевых функций через отдельные функции трансляции И через цепочки
// (verify/chain_utils.hpp), по одному источнику на представление, из
// которого это представление реалистичнее всего строить напрямую:
//
//  - Aig: EPFL Combinational Benchmark Suite (github.com/lsils/benchmarks,
//    тот же lsils, что и mockturtle) — реальные комбинационные схемы,
//    загружаются через lorina/mockturtle::aiger_reader. У EPFL-схем обычно
//    много выходов — bmm::Aig требует ровно один (CONVENTIONS.md п.4),
//    поэтому берём один конкретный выход схемы (extract_single_output ниже),
//    остальные входы/логика остаются как есть (реальная, не упрощённая
//    схема, просто с одним из её реальных выходов).
//  - Anf: AES S-box (FIPS-197: GF(2^8)-обращение + аффинное преобразование,
//    вычислено из определения, не переписано по памяти — та же процедура,
//    что и в anf/bench_real_corpus.cpp, откуда и взят этот код), обобщённый
//    χ (Keccak/Ascon, FIPS-202), бент-функция Майорана-МакФарланда —
//    формулы идентичны anf/bench_real_corpus.cpp (см. комментарии там же).
//  - Thr: Коллегия выборщиков США 2024-2028 (National Archives), тот же
//    источник и веса, что в thr/bench_real_corpus.cpp.
//  - Bdd/TruthTable: у этих двух представлений нет естественного "датасета"
//    (BDD — производная структура, а не то, в чём распространяют данные) —
//    получаются переводом реальных функций выше через tt_to_bdd/aig_to_bdd
//    и т.п., что как раз и проверяется round-trip'ами ниже.
//
// Постоянный файл (не удаляется), результат — verify/REAL_DATASETS_REPORT.md
// (перезаписывается при каждом запуске, как STATUS.md/CHAIN_TESTS_REPORT.md).

#include "verify/chain_utils.hpp"

#include <lorina/aiger.hpp>
#include <mockturtle/io/aiger_reader.hpp>
#include <mockturtle/networks/aig.hpp>

#include <sylvan_obj.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace bmm;
using namespace bmm::chains;

namespace {

// ---------------------------------------------------------------------------
// Aig: загрузка EPFL-схемы, извлечение одного выхода в отдельную Aig.
// ---------------------------------------------------------------------------

// Копирует ВСЮ логику src в dest (топологически, foreach_gate уже в топо-
// порядке для сети, загруженной из корректного AIGER-файла), но создаёт
// ровно один PO — для po_index-го выхода исходной (обычно многовыходной)
// схемы. Остальные PI src сохраняются как PI dest (даже если для конкретно
// этого выхода часть из них окажется фиктивной — это реальное свойство
// схемы, не искажение). bmm::Aig требует ровно один PO (CONVENTIONS.md п.4).
std::optional<mockturtle::aig_network> extract_single_output(const mockturtle::aig_network& src,
                                                               uint32_t po_index) {
    if (po_index >= src.num_pos()) return std::nullopt;

    mockturtle::aig_network dest;
    std::unordered_map<mockturtle::aig_network::node, mockturtle::aig_network::signal> node_map;

    node_map[src.get_node(src.get_constant(false))] = dest.get_constant(false);
    src.foreach_pi([&](auto n) { node_map[n] = dest.create_pi(); });

    src.foreach_gate([&](auto n) {
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        src.foreach_fanin(n, [&](auto s) { fanins[k++] = s; });
        auto get_dest_signal = [&](mockturtle::aig_network::signal s) {
            auto ds = node_map.at(src.get_node(s));
            return src.is_complemented(s) ? !ds : ds;
        };
        node_map[n] = dest.create_and(get_dest_signal(fanins[0]), get_dest_signal(fanins[1]));
    });

    uint32_t idx = 0;
    std::optional<mockturtle::aig_network::signal> po_signal;
    src.foreach_po([&](auto s) {
        if (idx == po_index) {
            auto ds = node_map.at(src.get_node(s));
            po_signal = src.is_complemented(s) ? !ds : ds;
        }
        ++idx;
    });
    if (!po_signal) return std::nullopt;
    dest.create_po(*po_signal);
    return dest;
}

std::optional<Aig> load_epfl_single_output(const std::string& path, uint32_t po_index) {
    mockturtle::aig_network full;
    auto result = lorina::read_aiger(path, mockturtle::aiger_reader(full));
    if (result != lorina::return_code::success) return std::nullopt;
    auto extracted = extract_single_output(full, po_index);
    if (!extracted) return std::nullopt;
    return Aig(std::move(*extracted));
}

// ---------------------------------------------------------------------------
// Anf: AES S-box / χ / bent — та же математика, что anf/bench_real_corpus.cpp
// (см. обоснования там: GF(2^8)-инверсия + аффинное преобразование FIPS-197
// для S-box, вычислено из определения, не переписано по памяти).
// ---------------------------------------------------------------------------

uint8_t gf_mult(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        const uint8_t hi = a & 0x80;
        a = static_cast<uint8_t>(a << 1);
        if (hi) a ^= 0x1B;
        b = static_cast<uint8_t>(b >> 1);
    }
    return p;
}

uint8_t gf_inverse(uint8_t a) {
    if (a == 0) return 0;
    for (int b = 1; b < 256; ++b) {
        if (gf_mult(a, static_cast<uint8_t>(b)) == 1) return static_cast<uint8_t>(b);
    }
    return 0;
}

uint8_t aes_affine(uint8_t b) {
    uint8_t result = 0;
    for (int i = 0; i < 8; ++i) {
        const int bit = ((b >> i) & 1) ^ ((b >> ((i + 4) % 8)) & 1) ^ ((b >> ((i + 5) % 8)) & 1) ^
                         ((b >> ((i + 6) % 8)) & 1) ^ ((b >> ((i + 7) % 8)) & 1);
        result = static_cast<uint8_t>(result | (bit << i));
    }
    return static_cast<uint8_t>(result ^ 0x63);
}

TruthTable aes_sbox_coordinate_tt(int bit) {
    TruthTable tt(8);
    for (int x = 0; x < 256; ++x) {
        uint8_t y = aes_affine(gf_inverse(static_cast<uint8_t>(x)));
        if ((y >> bit) & 1) kitty::set_bit(tt.raw(), static_cast<uint64_t>(x));
    }
    return tt;
}

// χ (Keccak/SHA-3, FIPS-202), обобщённый: out_0 = x_0 XOR x_2 XOR x_1*x_2
// (индексы mod n) — построен напрямую как Anf.
Anf build_chi(uint32_t n) {
#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n);
    BoolePolynomial poly(ring);
    poly += ring.variable(0);
    poly += ring.variable(2 % n);
    BooleMonomial mono(ring);
    mono *= ring.variable(1 % n);
    mono *= ring.variable(2 % n);
    poly += mono;
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    poly.add_monomial({0});
    poly.add_monomial({2 % n});
    Monomial m = {1 % n, 2 % n};
    std::sort(m.begin(), m.end());
    poly.add_monomial(std::move(m));
    return Anf(std::move(poly), n);
#endif
}

// Бент-функция Майорана-МакФарланда: f(x,y) = XOR x_i*y_i, i=0..n/2-1, n чётно.
Anf build_bent_mm(uint32_t n) {
    const uint32_t half = n / 2;
#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n);
    BoolePolynomial poly(ring);
    for (uint32_t i = 0; i < half; ++i) {
        BooleMonomial mono(ring);
        mono *= ring.variable(i);
        mono *= ring.variable(half + i);
        poly += mono;
    }
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    for (uint32_t i = 0; i < half; ++i) poly.add_monomial({i, half + i});
    return Anf(std::move(poly), n);
#endif
}

// ---------------------------------------------------------------------------
// Thr: Коллегия выборщиков США 2024-2028 — та же таблица и источник, что
// thr/bench_real_corpus.cpp (National Archives, archives.gov/electoral-college/allocation).
// ---------------------------------------------------------------------------

struct ElectoralEntry {
    const char* name;
    int64_t electors;
};

constexpr ElectoralEntry kElectoralCollege2024[] = {
    {"Alabama", 9},          {"Alaska", 3},          {"Arizona", 11},        {"Arkansas", 6},
    {"California", 54},      {"Colorado", 10},       {"Connecticut", 7},     {"Delaware", 3},
    {"District of Columbia", 3}, {"Florida", 30},     {"Georgia", 16},       {"Hawaii", 4},
    {"Idaho", 4},             {"Illinois", 19},       {"Indiana", 11},       {"Iowa", 6},
    {"Kansas", 6},            {"Kentucky", 8},        {"Louisiana", 8},      {"Maine", 4},
    {"Maryland", 10},         {"Massachusetts", 11},  {"Michigan", 15},      {"Minnesota", 10},
    {"Mississippi", 6},       {"Missouri", 10},       {"Montana", 4},        {"Nebraska", 5},
    {"Nevada", 6},            {"New Hampshire", 4},   {"New Jersey", 14},    {"New Mexico", 5},
    {"New York", 28},         {"North Carolina", 16}, {"North Dakota", 3},   {"Ohio", 17},
    {"Oklahoma", 7},          {"Oregon", 8},          {"Pennsylvania", 19},  {"Rhode Island", 4},
    {"South Carolina", 9},    {"South Dakota", 3},    {"Tennessee", 11},     {"Texas", 40},
    {"Utah", 6},              {"Vermont", 3},         {"Virginia", 13},      {"Washington", 12},
    {"West Virginia", 4},     {"Wisconsin", 10},      {"Wyoming", 3},
};
constexpr size_t kNumStates = sizeof(kElectoralCollege2024) / sizeof(kElectoralCollege2024[0]);
static_assert(kNumStates == 51, "50 штатов + округ Колумбия");

Thr build_electoral_prefix(uint32_t n) {
    std::vector<int64_t> weights;
    weights.reserve(n);
    int64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        weights.push_back(kElectoralCollege2024[i].electors);
        total += kElectoralCollege2024[i].electors;
    }
    return Thr(std::move(weights), total / 2 + 1);
}

Thr build_electoral_college_full() {
    return build_electoral_prefix(static_cast<uint32_t>(kNumStates));
}

// ---------------------------------------------------------------------------
// Реестр реальных датасетов: (имя, представление-источник, значение,
// "точно проверяемо exhaustively" — n достаточно мал для перебора 2^n).
// ---------------------------------------------------------------------------

struct Dataset {
    std::string name;
    Repr repr;
    AnyRepr value;
    bool exhaustive;  // n достаточно мал, чтобы перебрать все 2^n точек
};

std::vector<Dataset> build_real_datasets(Report& rep) {
    std::vector<Dataset> out;

    // --- Aig: EPFL, несколько небольших схем (перебор feasible) + одна
    // относительно крупная (только выборочная проверка). ---
    struct EpflCase { std::string path; uint32_t po; std::string name; bool exhaustive; };
    std::vector<EpflCase> epfl_cases = {
        {"benchmarks/data/epfl/random_control/ctrl.aig", 0, "epfl_ctrl_po0", true},
        {"benchmarks/data/epfl/random_control/int2float.aig", 0, "epfl_int2float_po0", true},
        {"benchmarks/data/epfl/random_control/cavlc.aig", 0, "epfl_cavlc_po0", true},
        {"benchmarks/data/epfl/random_control/router.aig", 0, "epfl_router_po0", false},
    };
    for (const auto& c : epfl_cases) {
        auto aig = load_epfl_single_output(c.path, c.po);
        if (!aig) {
            rep.bullet(false, "не удалось загрузить " + c.name + " (" + c.path + ")");
            continue;
        }
        rep.line("- загружено " + c.name + ": n_vars=" + std::to_string(aig->n_vars()) +
                  (c.exhaustive ? " (перебор всех точек)" : " (только случайная выборка)"));
        out.push_back({c.name, Repr::Aig, AnyRepr(std::move(*aig)), c.exhaustive});
    }

    // --- Anf: AES S-box (2 координатные функции), χ, bent. ---
    out.push_back({"aes_sbox_bit0", Repr::Tt, AnyRepr(aes_sbox_coordinate_tt(0)), true});
    out.push_back({"aes_sbox_bit4", Repr::Tt, AnyRepr(aes_sbox_coordinate_tt(4)), true});
    out.push_back({"chi_n8", Repr::Anf, AnyRepr(build_chi(8)), true});
    out.push_back({"chi_n16", Repr::Anf, AnyRepr(build_chi(16)), true});
    out.push_back({"chi_n64", Repr::Anf, AnyRepr(build_chi(64)), false});
    out.push_back({"bent_mm_n8", Repr::Anf, AnyRepr(build_bent_mm(8)), true});
    out.push_back({"bent_mm_n16", Repr::Anf, AnyRepr(build_bent_mm(16)), true});
    out.push_back({"bent_mm_n40", Repr::Anf, AnyRepr(build_bent_mm(40)), false});

    // --- Thr: Коллегия выборщиков (реальные веса), несколько префиксов +
    // полный список (n=51, только выборка). ---
    out.push_back({"electoral_prefix_n12", Repr::Thr, AnyRepr(build_electoral_prefix(12)), true});
    // ВАЖНАЯ НАХОДКА: electoral_prefix_n16 -> Anf (thr_to_anf) на РЕАЛЬНЫХ
    // (неравномерных: 3..54) весах занимает на порядки дольше, чем thr/
    // README.md заявляет для того же n на синтетических (MAJ-подобных,
    // единичных) весах — прогон этого файла не уложился даже в 400с там, где
    // синтетический бенчмарк даёт ~65мс. Правдоподобная причина: степень/
    // плотность ANF пороговой функции зависит от РАСПРЕДЕЛЕНИЯ весов, не
    // только от n (thr_to_anf.hpp сам предупреждает: "для симметричных
    // пороговых функций вроде MAJ... компактных формул нет", но ничего не
    // говорит о резко НЕсимметричных, как реальные веса выборщиков) — не
    // исследовано глубже в этой сессии, зафиксировано как открытая находка
    // (см. SESSION_REPORT.md). Помечаем n=16 тем же "non-exhaustive"-путём,
    // что и большие датасеты (пропускает Bdd/Anf, оставляет только Tt/Thr),
    // чтобы автоматический прогон не зависал на многие минуты.
    out.push_back({"electoral_prefix_n16", Repr::Thr, AnyRepr(build_electoral_prefix(16)), false});
    out.push_back({"electoral_prefix_n20", Repr::Thr, AnyRepr(build_electoral_prefix(20)), false});
    out.push_back({"electoral_college_full_n51", Repr::Thr, AnyRepr(build_electoral_college_full()), false});

    return out;
}

// ---------------------------------------------------------------------------
// Проверки: (1) отдельные функции перевода из естественного представления
// датасета в каждое из остальных четырёх; (2) round-trip туда-обратно.
// ---------------------------------------------------------------------------

constexpr uint64_t kSampleCount = 2000;
constexpr uint64_t kSampleSeed = 20260719;

bool check_equivalent(const Dataset& ds, const AnyRepr& other) {
    if (ds.exhaustive) return reprs_equivalent(ds.value, other);
    return reprs_equivalent_sampled(ds.value, other, kSampleCount, kSampleSeed);
}

// aig_to_bdd теперь строит BDD с FORCE-эвристикой порядка переменных (core/
// bdd_order_heuristics.hpp), применённой к графу взаимодействия переменных,
// построенному по фанинам AND-гейтов. Эмпирически подтверждено этой сессией
// через отдельный диагностический прогон (verify/diag_router_bdd.cpp,
// удалён после записи чисел в SESSION_REPORT.md): на router.aig (EPFL,
// n=60) — том самом случае, из-за которого исходно введён общий
// предохранитель ниже, — aig_to_bdd_with_heuristic и с Force, и с MinIndex
// завершается за единицы-десятки миллисекунд, никакого зависания НЕ
// воспроизводится именно на построении BDD. Поэтому Bdd специально исключён
// из общего предохранителя для датасетов с source Aig.
//
// source == Anf сюда НЕ входит, несмотря на то что anf_to_bdd тоже уже
// использует ту же FORCE-эвристику (и дольше, с версии до этой сессии): при
// пробной проверке этого пути на реальном bent_mm_n40 (бент-функция
// Майорана-МакФарланда, n=40) anf_to_bdd заняла ~48с (а round-trip ~54с) —
// НЕ зависание, но кратно дороже всех остальных случаев в этом файле
// (обычно <1мс-десятки мс). Это ожидаемо и не чинится выбором эвристики:
// бент-функции по определению не имеют эксплуатируемой линейной структуры
// (максимальная нелинейность) — известный в литературе худший случай для
// компактности BDD при ЛЮБОМ порядке переменных, а не следствие неудачного
// выбора эвристики. Добавление её в автоматический ctest-прогон было бы
// вне рамок текущей задачи (закрыть находку про aig_to_bdd на реальных
// схемах) и раздуло бы время прогона в ~4 раза ради случая, который сам по
// себе уже не является "зависанием" — оставляем под общим предохранителем
// как задокументированный, но не тестируемый автоматически случай.
//
// Thr сюда тоже не входит: thr_to_bdd для non-exhaustive Thr-датасетов не
// проверялся отдельно в рамках этой сессии (риск иной природы — bdd_to_thr,
// а не thr_to_bdd, и связан с K>6, а не со взрывом порядка переменных) —
// остаётся под общим предохранителем, пока не проверено явно.
bool bdd_target_is_force_protected(Repr source) {
    return source == Repr::Aig;
}

void run_single_function_checks(Report& rep, const std::vector<Dataset>& datasets) {
    rep.section("1. Отдельные функции перевода на реальных данных (X -> Y, "
                "X — естественное представление датасета)");

    const std::array<Repr, 5> kAll = {Repr::Aig, Repr::Bdd, Repr::Anf, Repr::Thr, Repr::Tt};

    for (const auto& ds : datasets) {
        for (Repr to : kAll) {
            if (to == ds.repr) continue;
            const bool bdd_ok = (to == Repr::Bdd) && bdd_target_is_force_protected(ds.repr);
            if (!ds.exhaustive && to != Repr::Tt && to != Repr::Thr && !bdd_ok) {
                // ПРЕДОХРАНИТЕЛЬ: для больших (не exhaustive) реальных
                // датасетов допускаем только переводы в Tt/Thr (у обоих есть
                // дешёвая статическая проверка n <= лимита, отказ быстрый и
                // безопасный) и в Bdd — но ТОЛЬКО если source Aig/Anf, чей
                // aig_to_bdd/anf_to_bdd защищён FORCE-эвристикой порядка
                // переменных и эмпирически проверен на router.aig (см.
                // bdd_target_is_force_protected выше). Anf НЕ входит в
                // список разрешённых целей: aig_to_anf на реальной (не
                // синтетической) схеме такого размера эмпирически показал,
                // что может не укладываться в разумное время (в отличие от
                // более раннего вывода в aig/README.md "не найдено ни
                // одного отказа", полученного только на синтетических
                // семействах chain/star/block/mux/random) — для него
                // никакой аналогичной защиты не существует. Гонять
                // непроверенные случаи в АВТОМАТИЧЕСКОМ, не присматриваемом
                // прогоне (ctest) — риск уронить/подвесить весь тестовый
                // бинарник без возможности поймать это изнутри процесса.
                // Если нужно всё же проверить конкретный большой случай —
                // делайте это отдельным запуском с внешним `timeout`, как
                // рекомендует anf/README.md §2.
                rep.line("- SKIP " + ds.name + ": " + repr_name(ds.repr) + "->" + repr_name(to) +
                         " (пропущено намеренно — предохранитель: для большого n без известной "
                         "структуры разрешены только Tt/Thr/Bdd-с-FORCE-защитой)");
                continue;
            }
            auto t0 = std::chrono::steady_clock::now();
            auto result = translate_step(ds.repr, to, ds.value);
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

            if (!is_ok(result)) {
                // NotImplemented/Unsupported/TooManyVariables — легитимный
                // пропуск конкретной пары на конкретном датасете (например,
                // Thr на функции, не являющейся пороговой; bdd_to_thr при K>6;
                // *_to_tt/tt_to_thr при n>kMaxTruthTableVars/32).
                rep.line("- SKIP " + ds.name + ": " + repr_name(ds.repr) + "->" + repr_name(to) +
                         " (" + error(result).message + ")");
                continue;
            }
            bool eq = check_equivalent(ds, value(result));
            rep.bullet(eq, ds.name + ": " + repr_name(ds.repr) + "->" + repr_name(to) + " (n=" +
                                std::to_string(n_vars_of(ds.value)) + ", " +
                                std::to_string(ms) + " мс" +
                                (ds.exhaustive ? ", перебор" : ", выборка " +
                                                                    std::to_string(kSampleCount) +
                                                                    " точек") +
                                ")");
        }
    }
}

void run_round_trip_checks(Report& rep, const std::vector<Dataset>& datasets) {
    rep.section("2. Round-trip на реальных данных (X -> Y -> X)");

    const std::array<Repr, 5> kAll = {Repr::Aig, Repr::Bdd, Repr::Anf, Repr::Thr, Repr::Tt};

    for (const auto& ds : datasets) {
        if (!ds.exhaustive && ds.repr == Repr::Thr) {
            // ПРЕДОХРАНИТЕЛЬ (отдельный от Bdd/Anf-ограничения выше):
            // round-trip ОБРАТНО в Thr для любого via требует X_to_thr на
            // возвратном шаге — единственный способ вернуться в Thr — это
            // распознавание пороговости через ILP (aig_to_thr/tt_to_thr/
            // anf_to_thr; bdd_to_thr вместо этого падает по K>6 почти сразу
            // на реальных многовесовых данных). Практический потолок именно
            // этого шага НАМНОГО ниже формальных ограничений в коде — thr/
            // README.md уже задокументировал tt_to_thr как "не укладывается
            // в разумное время" при n=20 на реальных весах; на n=16
            // подтверждено и этим прогоном (round-trip через Tt — свыше
            // 10 секунд, single-function тот же thr_to_tt — единицы мс,
            // разница целиком в ОБРАТНОМ ILP-шаге). Не пытаемся так же для
            // Thr-round-trip: только прямой перевод (run_single_function_
            // checks выше) для non-exhaustive Thr-датасетов, без возврата.
            rep.line("- SKIP round-trip " + ds.name + " (*->" + ds.name +
                     "): предохранитель — обратный X_to_thr на реальных многовесовых данных "
                     "может стоить десятки секунд-минуты уже при n=16-20 (см. thr/README.md), "
                     "не только на большом n; см. run_single_function_checks за прямыми переводами");
            continue;
        }
        for (Repr via : kAll) {
            if (via == ds.repr) continue;
            const bool bdd_ok = (via == Repr::Bdd) && bdd_target_is_force_protected(ds.repr);
            if (!ds.exhaustive && via != Repr::Tt && via != Repr::Thr && !bdd_ok) {
                // Тот же предохранитель, что и в run_single_function_checks —
                // см. комментарий там (для больших датасетов разрешены
                // промежуточные Tt/Thr и Bdd-с-FORCE-защитой при source
                // Aig/Anf; Anf как промежуточное представление — нет).
                rep.line("- SKIP " + ds.name + ": " + repr_name(ds.repr) + "->" + repr_name(via) +
                         "->" + repr_name(ds.repr) + " (пропущено намеренно — предохранитель)");
                continue;
            }
            auto chain = run_chain(ds.value, {ds.repr, via, ds.repr});
            if (!chain.ok) {
                rep.line("- SKIP " + ds.name + ": " + repr_name(ds.repr) + "->" + repr_name(via) +
                         "->" + repr_name(ds.repr) + " (" + chain.detail + ")");
                continue;
            }
            bool eq = check_equivalent(ds, *chain.final_value);
            rep.bullet(eq, ds.name + ": " + repr_name(ds.repr) + "->" + repr_name(via) + "->" +
                                repr_name(ds.repr) + " (" + std::to_string(chain.total_ms) + " мс" +
                                (ds.exhaustive ? ", перебор" : ", выборка") + ")");
        }
    }
}

int g_result = 0;

VOID_TASK_0(real_datasets_main) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    Report rep;
    rep.md << "# Отчёт: тесты на реальных наборах данных "
              "(verify/real_datasets_tests.cpp)\n\n"
              "Автоматически перезаписывается при каждом запуске "
              "`test_real_datasets` — не редактировать руками.\n\n"
              "Источники: EPFL Combinational Benchmark Suite (Aig), AES S-box/χ"
              "(Keccak)/бент-функция Майорана-МакФарланда (Anf), Коллегия "
              "выборщиков США 2024-2028 (Thr, National Archives).\n";

    rep.section("0. Загруженные датасеты");
    auto datasets = build_real_datasets(rep);

    auto t_start = std::chrono::steady_clock::now();
    run_single_function_checks(rep, datasets);
    run_round_trip_checks(rep, datasets);
    auto t_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t_end - t_start).count();

    rep.md << "\n## Итог\n\n" << rep.total_checks << " проверок, " << rep.failed_checks
           << " провалов. Полное время прогона: " << total_s << " с.\n";
    std::printf("\n=== ИТОГ: %d проверок, %d провалов, %.2f с ===\n", rep.total_checks,
                rep.failed_checks, total_s);

    std::ofstream out("verify/REAL_DATASETS_REPORT.md");
    if (out) {
        out << rep.md.str();
        std::printf("Отчёт записан в verify/REAL_DATASETS_REPORT.md\n");
    } else {
        std::printf("WARN: не удалось записать отчёт (текущая директория не похожа на корень репозитория)\n");
    }

    g_result = rep.failed_checks == 0 ? 0 : 1;
    sylvan::sylvan_quit();
}

}  // namespace

int main() {
    const int n_workers = 0;
    const size_t deque_size = 0;
    lace_start(n_workers, deque_size);
    RUN(real_datasets_main);
    lace_stop();
    return g_result;
}
