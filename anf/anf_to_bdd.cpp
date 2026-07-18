#include "anf_to_bdd.hpp"

#include <tracy/Tracy.hpp>

#if BMM_HAVE_BRIAL

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

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


// поиск переменной для разложения — вариант A: минимальный индекс среди
// ВСЕХ мономов (не только первого встреченного). Даёт консистентный
// порядок разложения между разными ветками рекурсии (важно и для
// компактности итогового BDD в Sylvan, и для мемоизации: одинаковые
// подполиномы, до которых дошли разными путями, выбирают одну и ту же
// переменную и поэтому реально совпадают как ключи кэша). Дёшево (один
// проход), но не учитывает структуру полинома вообще. [[maybe_unused]] —
// сейчас не вызывается напрямую (см. переключатель choose_variable ниже),
// оставлена как историческая точка сравнения; эквивалент этой эвристики в
// генерализованной ранговой схеме ниже — identity_rank (VariableOrderHeuristic::MinIndex).
[[maybe_unused]] uint32_t choose_variable_min_index(const Poly& poly)
{
    bool found = false;
    uint32_t best = 0;

    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        for (auto v : *it)
        {
            uint32_t idx = static_cast<uint32_t>(v);

            if (!found || idx < best)
            {
                best = idx;
                found = true;
            }
        }
    }

    return best;
}


// поиск переменной для разложения — вариант B (эвристика, предложена для
// проверки на реальных данных, НЕ подтверждена бенчмарком): переменные,
// которые входят в самые длинные мономы и встречаются чаще остальных,
// разлагаем первыми — длинные мономы сильнее всего определяют структурную
// сложность полинома, а частая переменная режет оставшуюся задачу на
// более сбалансированные куски.
//
// В отличие от первой версии этой эвристики, ранг считается ОДИН РАЗ по
// исходному полиному (см. compute_rank_by_length_freq, вызывается из
// anf_to_bdd()), а не заново на каждом узле рекурсии по текущему
// подполиному. Причины:
//  1) Дешевле: O(size) один раз вместо O(size) на каждом из потенциально
//     экспоненциального числа узлов.
//  2) Важнее производительности — корректность/компактность: пересчёт "с
//     нуля" на каждом узле не гарантирует единый порядок переменных между
//     разными ветками дерева (в одной ветке "самой частой" могла бы
//     оказаться x5, в другой, не проходящей через x5, — x2 раньше x5).
//     У choose_variable_min_index это свойство "Ordered BDD" (порядок
//     переменных вдоль любого пути от корня к листу один и тот же)
//     следовало автоматически из того, что индекс переменной не меняется.
//     Фиксированный ГЛОБАЛЬНЫЙ ранг восстанавливает то же свойство и для
//     этой эвристики: если ранг(a) лучше ранга(b), a выбирается раньше b
//     везде, где обе присутствуют, независимо от ветки.
//
// Ранг переменной — по (её собственной максимальной длине монома, в
// который она входит; затем по частоте) — не тождественно "переменные
// самого длинного монома всего полинома" из первой версии эвристики (тут
// у каждой переменной рассматривается её ЛИЧНЫЙ максимум), но тот же дух
// приоритезации и та же цена вычисления.
//
// ВАЖНАЯ ПОПРАВКА ПО ИТОГАМ ПЕРВОЙ ЭМПИРИЧЕСКОЙ ПРОВЕРКИ (реальная сборка,
// n_vars от 8 до 36, growing_test_functions + специально сконструированные
// мультиплексоры с явной асимметрией частоты переменных): вопреки
// рассуждению выше, порядок разложения в РЕКУРСИИ (при фиксированном
// bddVar(var), т.е. level == var) НЕ определяет порядок переменных в
// итоговом BDD и, соответственно, НЕ влияет на его размер (NodeCount у
// choose_variable_min_index и choose_variable_by_rank оказался БУКВАЛЬНО
// идентичен на каждом из ~65 протестированных случаев, включая специально
// сконструированные асимметричные мультиплексоры). Причина:
// sylvan::Bdd::bddVar(index) вызывает sylvan_ithvar(index) — уровень
// переменной в разделяемой BDD-структуре Sylvan фиксирован её ИНДЕКСОМ, а
// не порядком, в котором наш код вызывает bddVar/Ite; без явного
// физического переприсвоения уровня (см. "ЧАСТЬ 2" ниже, у convert())
// Ite() всегда собирает корректно упорядоченный (по индексу) результат
// независимо от порядка вложенности наших вызовов.
//
// Значит (пока переменные не переставлены физически — см. ЧАСТЬ 2), весь
// эффект от выбора эвристики — это ВРЕМЯ построения (эффективность
// рекурсии/мемоизации: удачный порядок разложения приводит к бОльшему
// числу совпадающих подполиномов между ветками, чаще попадая в memo), а не
// компактность результата. На практике это всё равно ощутимо:
// choose_variable_by_rank на асимметричных структурах (мультиплексор-
// подобных, где несколько переменных явно "важнее" остальных) даёт
// ускорение до ~40-80x относительно choose_variable_min_index; на
// симметричных/случайных функциях (AND/OR/XOR-of-all, равномерно
// случайные) — наоборот, ~1.9x МЕДЛЕННЕЕ (лишний проход подсчёта рангов не
// окупается, когда все переменные и так равнозначны).
using VariableRank = std::vector<uint32_t>;  // rank[var] -> приоритет, меньше = разложить раньше

VariableRank compute_rank_by_length_freq(const Poly& poly, uint32_t n_vars)
{
    std::vector<uint32_t> max_len_for_var(n_vars, 0);
    std::vector<uint32_t> freq(n_vars, 0);

    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        uint32_t len = 0;

        for (auto v : *it)
        {
            ++len;
        }

        for (auto v : *it)
        {
            uint32_t idx = static_cast<uint32_t>(v);
            ++freq[idx];

            if (len > max_len_for_var[idx])
            {
                max_len_for_var[idx] = len;
            }
        }
    }

    std::vector<uint32_t> order(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i)
    {
        order[i] = i;
    }

    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (max_len_for_var[a] != max_len_for_var[b])
        {
            return max_len_for_var[a] > max_len_for_var[b];
        }
        return freq[a] > freq[b];
    });

    VariableRank rank(n_vars);
    for (uint32_t r = 0; r < n_vars; ++r)
    {
        rank[order[r]] = r;
    }

    return rank;
}

// Тривиальный ранг для VariableOrderHeuristic::MinIndex — identity, т.е.
// физический уровень совпадает с индексом переменной (натуральный порядок,
// текущее поведение всех остальных производителей Bdd). choose_variable_by_rank
// поверх identity_rank ведёт себя ТОЧНО как исторический
// choose_variable_min_index (см. выше) — этой эвристике не нужна отдельная
// функция выбора переменной, только отдельный rank-массив.
VariableRank identity_rank(uint32_t n_vars)
{
    VariableRank rank(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i)
    {
        rank[i] = i;
    }
    return rank;
}


// ---------------------------------------------------------------------------
// Граф взаимодействия переменных (узлы — переменные, ребро u-v — переменные
// встретились в одном мономе, вес — число мономов, где встретились вместе).
// Хранится как список смежности (hash map на переменную), не как плотная
// матрица n x n — при n в десятки/сотни переменных и разреженных мономах
// (типичный ANF) это не только дешевле по памяти, но и по времени: обход
// idx2poly.size()^2 плотной матрицы был бы дороже, чем реальное число рёбер.
// Стоимость построения — O(sum L_i^2) по всем мономам длины L_i (C(L,2) пар
// на моном) — дёшево относительно самой постройки BDD при типичных для
// проекта n<=24-36 и десятках-сотнях мономов (см. "Когда реально включать
// параллелизм" в handoff-prompt-anf-bdd-heuristics.md — здесь параллелить не
// стали умышленно, см. итоговую сводку у convert()/select_rank ниже).
// ---------------------------------------------------------------------------
struct InteractionGraph
{
    std::vector<std::unordered_map<uint32_t, uint32_t>> adjacency;  // adjacency[v][u] = вес ребра v-u
};

InteractionGraph build_interaction_graph(const Poly& poly, uint32_t n_vars)
{
    InteractionGraph graph;
    graph.adjacency.resize(n_vars);

    std::vector<uint32_t> mono_vars;

    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        mono_vars.clear();

        for (auto v : *it)
        {
            mono_vars.push_back(static_cast<uint32_t>(v));
        }

        for (size_t i = 0; i < mono_vars.size(); ++i)
        {
            for (size_t j = i + 1; j < mono_vars.size(); ++j)
            {
                ++graph.adjacency[mono_vars[i]][mono_vars[j]];
                ++graph.adjacency[mono_vars[j]][mono_vars[i]];
            }
        }
    }

    return graph;
}

// Эвристика A — степень вершины (VariableOrderHeuristic::Degree): чем больше
// РАЗЛИЧНЫХ переменных встречается вместе с v хотя бы в одном мономе, тем
// раньше её стоит разложить — она сильнее всего "запутана" с остальными.
// Отличие от freq в compute_rank_by_length_freq: freq считает суммарное
// число со-вхождений (повторные пересечения с одной и той же переменной
// увеличивают freq, но не степень) — степень считает только количество
// РАЗЛИЧНЫХ соседей, т.е. насколько широко переменная разбросана по графу, а
// не насколько часто она встречается с одними и теми же партнёрами.
// O(n log n) поверх уже построенного графа (сортировка n переменных).
VariableRank compute_rank_by_degree(const Poly& poly, uint32_t n_vars)
{
    InteractionGraph graph = build_interaction_graph(poly, n_vars);

    std::vector<uint32_t> order(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i)
    {
        order[i] = i;
    }

    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const size_t deg_a = graph.adjacency[a].size();
        const size_t deg_b = graph.adjacency[b].size();
        if (deg_a != deg_b)
        {
            return deg_a > deg_b;
        }
        return a < b;  // детерминированный tie-break
    });

    VariableRank rank(n_vars);
    for (uint32_t r = 0; r < n_vars; ++r)
    {
        rank[order[r]] = r;
    }

    return rank;
}

// Эвристика B — FORCE (Aloul, Markov, Sakallah, "FORCE: A Fast and Easy-To-
// Implement Variable-Ordering Heuristic", GLSVLSI 2003) — стандартный
// алгоритм именно для упорядочивания переменных BDD/схем по гиперграфу
// "сетей" (в оригинале — логические вентили/клозы; здесь роль сети играет
// моном). Классическая формулировка усредняет позиции по НЕТАМ (центр
// тяжести сети = среднее позиций всех переменных сети, затем новая позиция
// переменной = среднее центров тяжести её сетей). Здесь используется
// эквивалентная по духу, но более дешёвая формулировка поверх уже
// построенного графа взаимодействия (как и предписано в задании — не
// пересобирать сетевую структуру заново): "центр тяжести" переменной v — это
// взвешенное среднее позиций всех её соседей в графе, вес ребра = число
// монoмов, где встретились v и сосед вместе (тот же вклад, который дал бы
// проход по нетам при мономах длины <= 3; при более длинных мономах это не
// побитово то же самое, что классический net-based FORCE, но сохраняет его
// ключевое свойство — переменные, которые появляются вместе часто и с
// многими партнёрами, притягиваются друг к другу).
//
// Начальная позиция — исходный индекс переменной (произвольный, но
// детерминированный старт). После N итераций сортируем по финальной позиции.
// Изолированные переменные (нет рёбер, т.е. не входят ни в один немономиальный
// терм) не двигаются — не с кем усредняться.
//
// Сложность: O(итерации * суммарное число рёбер графа) — при графе,
// построенном за O(sum L_i^2), это дёшево (см. общую оценку у convert()).
VariableRank compute_rank_force(const Poly& poly, uint32_t n_vars, uint32_t iterations = 20)
{
    InteractionGraph graph = build_interaction_graph(poly, n_vars);

    std::vector<double> pos(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i)
    {
        pos[i] = static_cast<double>(i);
    }

    std::vector<double> next_pos(n_vars);

    for (uint32_t iter = 0; iter < iterations; ++iter)
    {
        for (uint32_t v = 0; v < n_vars; ++v)
        {
            const auto& neighbors = graph.adjacency[v];

            if (neighbors.empty())
            {
                next_pos[v] = pos[v];
                continue;
            }

            double weighted_sum = 0.0;
            uint64_t weight_total = 0;

            for (const auto& [u, weight] : neighbors)
            {
                weighted_sum += pos[u] * static_cast<double>(weight);
                weight_total += weight;
            }

            next_pos[v] = weighted_sum / static_cast<double>(weight_total);
        }

        pos.swap(next_pos);
    }

    std::vector<uint32_t> order(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i)
    {
        order[i] = i;
    }

    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (pos[a] != pos[b])
        {
            return pos[a] < pos[b];
        }
        return a < b;  // детерминированный tie-break
    });

    VariableRank rank(n_vars);
    for (uint32_t r = 0; r < n_vars; ++r)
    {
        rank[order[r]] = r;
    }

    return rank;
}


// Дешёвый выбор переменной на конкретном узле рекурсии: один проход по
// ТЕКУЩЕМУ (уже уменьшившемуся) подполиному в поисках лучшего по
// предвычисленному глобальному рангу — без пересчёта частот. Полностью
// generic относительно того, ЧЕМ посчитан rank (min_index/length_freq/
// degree/force — см. select_rank ниже) — единственный вызывающий код в
// convert(), не завязан на конкретную эвристику.
uint32_t choose_variable_by_rank(const Poly& poly, const VariableRank& rank)
{
    bool found = false;
    uint32_t best = 0;
    uint32_t best_rank = 0;

    for (auto it = poly.begin(); it != poly.end(); ++it)
    {
        for (auto v : *it)
        {
            uint32_t idx = static_cast<uint32_t>(v);
            uint32_t r = rank[idx];

            if (!found || r < best_rank)
            {
                best = idx;
                best_rank = r;
                found = true;
            }
        }
    }

    return best;
}


// Хэш полинома по его канонической последовательности мономов. BRiAl
// обходит равные полиномы (одна и та же ZDD-структура) в одном и том же
// порядке, поэтому равные объекты гарантированно дают равный хэш — этого
// достаточно для std::unordered_map, без зависимости от внутреннего API
// идентичности узлов BRiAl.
struct PolyHash
{
    size_t operator()(const Poly& poly) const
    {
        size_t h = 0;

        for (auto it = poly.begin(); it != poly.end(); ++it)
        {
            size_t mono_h = 0;

            for (auto v : *it)
            {
                mono_h = mono_h * 1000003u
                       ^ std::hash<uint32_t>{}(static_cast<uint32_t>(v));
            }

            h ^= mono_h + 0x9e3779b9u + (h << 6) + (h >> 2);
        }

        return h;
    }
};

using Memo = std::unordered_map<Poly, sylvan::Bdd, PolyHash>;


// ЧАСТЬ 2 находок (см. handoff-prompt-anf-bdd-heuristics.md): порядок
// разложения в рекурсии сам по себе не меняет размер итогового BDD, потому
// что sylvan::Bdd::bddVar(idx) жёстко привязывает физический УРОВЕНЬ узла к
// idx (см. комментарий у compute_rank_by_length_freq выше). Единственный
// способ реально получить другой, потенциально более компактный порядок
// переменных — строить узлы с bddVar(rank[var]) вместо bddVar(var), т.е.
// физически переиспользовать индексы Sylvan как позиции в желаемом порядке.
// Экспериментально подтверждено (bench_permute_test.cpp, диагностика,
// удалена из финального диффа): на one-hot мультиплексоре это меняет
// NodeCount() (не только время) — в одну или другую сторону в зависимости от
// того, насколько порядок, выбранный эвристикой, лучше/хуже натурального для
// конкретной функции (в проверенном 4x4-случае natural=571,
// rank-переставленный=1215 — ХУЖЕ; сравнение по факту зависит от функции,
// см. "боевое сравнение" в bench_bdd_heuristics.cpp).
//
// Это ломает контракт "level i == переменная i", на котором молча стоят
// Bdd::evaluate() и потребители Bdd::raw() (bdd_to_aig.cpp, bdd_to_tt.cpp) —
// решено через явный var_to_level в core/common.hpp (Bdd(root, n_vars,
// var_to_level) + Bdd::var_at_level()), а не через Bdd::Permute():
// эмпирически (тот же bench_permute_test.cpp) Permute(from, to) НЕ сохраняет
// компактность построенного с переставленными уровнями BDD — после Permute
// обратно к натуральному порядку NodeCount() возвращается ТОЧНО к значению
// натурального построения (25->25, 571->571 на двух проверенных размерах
// one-hot мультиплексора). Это ожидаемо: ROBDD для ФИКСИРОВАННОГО порядка
// уровней (а Permute переводит именно в натуральный, т.к. Sylvan всегда
// хранит level == index) единственен по теореме Брайанта — от какого
// промежуточного представления к нему ни идти, результат один и тот же.
sylvan::Bdd convert(const Poly& poly, Memo& memo, const VariableRank& rank)
{
    if (poly == 0)
        return sylvan::Bdd::bddZero();


    if (poly == 1)
        return sylvan::Bdd::bddOne();


    // Мемоизация: без неё split_without/split_with пересканируют весь
    // полином заново на КАЖДОМ рекурсивном вызове, а повторяющиеся
    // p0/high_poly (частое явление при пересекающихся мономах) считались
    // бы много раз — экспоненциальный риск по числу рекурсивных вызовов.
    auto cached = memo.find(poly);

    if (cached != memo.end())
    {
        return cached->second;
    }


    uint32_t var = choose_variable_by_rank(poly, rank);


    /*
        p = p0 XOR x*p1

        low  = p0

        high = p0 XOR p1
    */


    auto p0 = split_without(poly, var);

    auto p1 = split_with(poly, var);


    auto low = convert(p0, memo, rank);


    auto high_poly = p0 + p1;

    auto high = convert(high_poly, memo, rank);



    // level = rank[var], НЕ var — физическая перестановка уровня, см. ЧАСТЬ 2
    // выше. rank[var] всегда в [0, n_vars), т.к. rank — перестановка индексов
    // переменных (identity_rank/compute_rank_by_*).
    sylvan::Bdd result = sylvan::Bdd::bddVar(rank[var]).Ite(high, low);

    memo.emplace(poly, result);

    return result;
}


VariableRank select_rank(VariableOrderHeuristic heuristic, const Poly& poly, uint32_t n_vars)
{
    switch (heuristic)
    {
        case VariableOrderHeuristic::MinIndex:
            return identity_rank(n_vars);
        case VariableOrderHeuristic::LengthFreqRank:
            return compute_rank_by_length_freq(poly, n_vars);
        case VariableOrderHeuristic::Degree:
            return compute_rank_by_degree(poly, n_vars);
        case VariableOrderHeuristic::Force:
            return compute_rank_force(poly, n_vars);
    }

    return identity_rank(n_vars);  // недостижимо при корректном enum, но без UB на всякий случай
}


}  // namespace



Result<Bdd> anf_to_bdd_with_heuristic(const Anf& anf, VariableOrderHeuristic heuristic)
{
    ZoneScoped;

    VariableRank rank = select_rank(heuristic, anf.raw(), anf.n_vars());

    Memo memo;
    auto root = convert(anf.raw(), memo, rank);

    return ok<Bdd>(
        Bdd(root, anf.n_vars(), rank)
    );
}

// ЧАСТЬ 3 (боевое сравнение — временный anf/bench_bdd_heuristics.cpp этой
// сессии, удалён из финального диффа после того как числа записаны здесь и
// в память проекта; реальная сборка в контейнере, медиана 11 повторов + 1
// прогрев на каждый случай; полная таблица — в памяти проекта
// bmm_translib_anf_parallelism_work.md):
//
// На СИММЕТРИЧНЫХ/случайных функциях (and/or/xor_all, random_n*, n=4/8/12 —
// growing_test_functions) физическая перестановка уровня НЕ помогает ни
// одной из трёх "умных" эвристик (length_freq_rank/degree/force) — NodeCount
// одинаков у всех (порядок переменных тут и так почти не важен для
// компактности), а время у всех трёх на ~10-40% хуже, чем у min_index (цена
// подсчёта ранга/графа, не окупается там, где выигрывать нечему) — все три
// платят примерно ОДИНАКОВЫЙ налог, ни одна не хуже другой качественно.
//
// На АСИММЕТРИЧНОЙ структуре (one-hot мультиплексор, где несколько
// "селекторных" переменных явно важнее остальных) картина другая, и здесь
// впервые проявилась разница именно по NodeCount() (не только по времени —
// в отличие от находок ЧАСТИ 1 до физической перестановки уровня):
//
//   мультиплексор     min_index  length_freq_rank  degree  force
//   3x3 (n=21) nodes       127        151            106      55
//   4x4 (n=36) nodes       571       1215            466     153
//   4x4 (n=36) time,ms    6.0        25.4            8.8     2.9
//
// force — безоговорочный победитель на структурных случаях: в 3.7x (n=36) и
// 2.3x (n=21) компактнее min_index, и настолько же компактнее
// length_freq_rank (прежнего умолчания, выбранного ДО того как появилась
// физическая перестановка уровня — тогда единственным критерием было время,
// и length_freq_rank выигрывала по нему; теперь, когда порядок реально влияет
// на итоговый размер, length_freq_rank оказалась ХУЖЕ даже min_index на этом
// корпусе — её приоритет "самый длинный моном, потом частота" не совпадает с
// тем, что реально минимизирует BDD, в отличие от FORCE, которая напрямую
// моделирует "притяжение" совместно используемых переменных). Меньший BDD у
// force попутно даёт и меньшее время постройки (2.9ms против 6.0ms у
// min_index, 8.8ms у degree, 25.4ms у length_freq_rank) — компактный
// результат означает меньше Ite()-вызовов по дороге, а не только более
// удачные попадания в память.
//
// Итог (на момент ЧАСТИ 3): force не хуже остальных там, где выигрывать
// нечему (симметричные функции), и кратно лучше там, где выигрывать есть на
// чём (структурные, asymmetric ANF). Выбрана как умолчание — но проверено
// только на ОДНОМ семействе структурных функций (one-hot мультиплексор).
// Дальше — расширение на разнообразные семейства, см. ЧАСТЬ 4.
//
// ЧАСТЬ 4 (по запросу пользователя — "больше разнообразных тестов, n до
// 64"; временный anf/bench_bdd_heuristics_v2.cpp этой сессии, удалён после
// записи чисел; та же методология: медиана 11 повторов + 1 прогрев; полная
// таблица по всем n — в памяти проекта): 5 структурных семейств, у каждого
// — крупнейший прогнанный размер (там, где отличия эвристик успевают
// накопиться сильнее всего):
//
//   семейство (крупнейший n)      min_index  length_freq_rank  degree  force
//   chain, path graph (n=64)           127        1103           247    127
//   star/hub, центр на x_{n-1} (n=64)  127          65            65    127
//   block, 16 клик по 4 (n=64)          65         145             65    65
//   random sparse, deg 2-3 (n=24)     1711        1627          1793   149
//   one-hot mux 4x7 (n=60)            4771        9827          3826  1273
//
// НИ ОДНА эвристика не доминирует на всех пяти семействах — у каждой есть
// случай, где она заметно хуже других:
//  - chain (степень графа взаимодействия <= 2 у любой переменной — почти
//    линейная структура): min_index/force побеждают (совпадают с
//    оптимальным натуральным порядком), length_freq_rank — КАТАСТРОФА (8.7x
//    хуже force на n=64: 1103 против 127) — её приоритет "длина монома"
//    вырожден на мономах длины 2, остаётся только частота, которая на
//    цепочке даёт плохой порядок; degree тоже заметно хуже (1.94x).
//  - star/hub (центр намеренно на x_{n-1}, худший случай для min_index):
//    length_freq_rank/degree побеждают (65 против 127 у остальных, 1.95x) —
//    единственный случай, где force НЕ входит в число лучших, и вдобавок
//    совпадает по размеру именно с "плохим" min_index. Так же ожидаемо:
//    star_first (центр на x_0, единственный вариант в таблице выше без
//    этой колонки) — все четыре эвристики дают одинаковый оптимум (65),
//    подтверждает, что дело именно в том, ГДЕ физически расположен важный
//    индекс, а не в самой структуре.
//  - block (несвязные клики — граф взаимодействия распадается на
//    компоненты): min_index/degree/force втроём на оптимуме (65),
//    length_freq_rank снова заметно хуже (2.23x) — та же причина, что и на
//    chain: мономы внутри блока все одной длины (4), приоритет
//    вырождается до частоты, которая здесь не помогает.
//  - random sparse (умеренная плотность, без специальной структуры): force
//    выигрывает с большим отрывом (11.5x у min_index/degree, 10.9x у
//    length_freq_rank) — но на n=16 (не показан в таблице, самый маленький
//    из прогнанных размеров) force был, наоборот, ХУЖЕ остальных (343 узла
//    против 125-221) — на малых n и без структуры "притяжение" FORCE может
//    сходиться в неудачный локальный оптимум, это ожидаемо для эвристики
//    без гарантии глобальной оптимальности; на большем n (>=20) сходится
//    к явно лучшему результату во всех прогнанных случаях.
//  - one-hot mux: как и в ЧАСТИ 3, force кратно лучше всех (3x-7.7x на
//    n=60).
//
// Итог ЧАСТИ 4 — "минимаксное сожаление" (во сколько раз каждая эвристика
// хуже лучшей в САМОМ невыгодном для неё семействе, среди 5 выше):
//   min_index          — до 11.5x хуже (random)
//   length_freq_rank   — до 10.9x хуже (random) и 8.7x (chain)
//   degree             — до 12.0x хуже (random)
//   force              — максимум 1.95x хуже (star/hub), везде остальном —
//                        либо лучший, либо наравне с лучшим
//
// force остаётся умолчанием: её худший случай (star/hub, ~2x) кратно мягче
// худшего случая любой другой эвристики (8x-12x на chain/random). Ни одна
// эвристика не является безопасной "заглушкой на все случаи" — если для
// конкретного прикладного класса ANF заранее известно, что он structурно
// похож на star/hub (доминирующая управляющая переменная с ВЫСОКИМ
// индексом), стоит явно вызвать anf_to_bdd_with_heuristic(anf, Degree) или
// LengthFreqRank вместо умолчания.
Result<Bdd> anf_to_bdd(const Anf& anf)
{
    return anf_to_bdd_with_heuristic(anf, VariableOrderHeuristic::Force);
}


}  // namespace bmm

#else  // !BMM_HAVE_BRIAL

namespace bmm {

Result<Bdd> anf_to_bdd_with_heuristic(const Anf&, VariableOrderHeuristic)
{
    return fail<Bdd>(
        ErrorCode::NotImplemented,
        "anf_to_bdd requires BRiAl"
    );
}

Result<Bdd> anf_to_bdd(const Anf&)
{
    return fail<Bdd>(
        ErrorCode::NotImplemented,
        "anf_to_bdd requires BRiAl"
    );
}

}  // namespace bmm

#endif  // BMM_HAVE_BRIAL
