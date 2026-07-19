// benchmarks/anf_dimacs_loader.hpp — загрузчик текстового формата DIMACS ANF
// (persons.iis.nsk.su, см. benchmarks/scripts/download_iis_nsk.sh,
// benchmarks/data/iis-nsk/100-10k-rnd/*.anf и т.п.) в bmm::Anf.
//
// Формат (тот же дух, что классический DIMACS CNF, только термы вместо
// клозов):
//   c <произвольный комментарий>      (ноль и более строк, игнорируются)
//   p anf <n_vars> <n_monomials>      (ровно одна строка-заголовок)
//   <v1> <v2> ... <vk> 0              (по одной строке на моном; переменные
//                                      1-индексированные, терминатор 0;
//                                      пустая строка перед "0" — то есть
//                                      строка, состоящая ровно из "0", —
//                                      это моном "1" (константа))
//
// <n_monomials> в заголовке не проверяется на равенство фактическому числу
// прочитанных строк — используется только как подсказка для reserve()
// (тот же необязательный статус, что у "число клозов" в DIMACS CNF).
//
// Переменные внутри файла 1-индексированы (1..n_vars) — на входе в bmm::Anf
// вычитается 1, т.к. весь остальной проект использует 0-индексацию
// (core/CONVENTIONS.md, LSB_FIRST).

#pragma once

#include "core/anf_repr.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace bmm::benchmarks {

inline std::optional<Anf> load_anf_dimacs(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;

    uint32_t n_vars = 0;
    uint32_t n_monoms_declared = 0;
    bool have_header = false;
    std::vector<std::vector<uint32_t>> monomials;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == 'c') continue;

        if (!have_header) {
            if (line[0] != 'p') return std::nullopt;
            std::istringstream iss(line);
            std::string tag, fmt;
            iss >> tag >> fmt >> n_vars >> n_monoms_declared;
            if (!iss || tag != "p" || fmt != "anf") return std::nullopt;
            have_header = true;
            monomials.reserve(n_monoms_declared);
            continue;
        }

        std::istringstream iss(line);
        std::vector<uint32_t> mono;
        int64_t tok = 0;
        while (iss >> tok) {
            if (tok == 0) break;
            if (tok < 1 || static_cast<uint64_t>(tok) > n_vars) return std::nullopt;
            mono.push_back(static_cast<uint32_t>(tok - 1));  // 1-индекс -> 0-индекс
        }
        monomials.push_back(std::move(mono));
    }
    if (!have_header) return std::nullopt;

#if BMM_HAVE_BRIAL
    polybori::BoolePolyRing ring(n_vars);
    polybori::BoolePolynomial poly(ring);
    for (const auto& mono : monomials) {
        BooleMonomial m(ring);
        for (uint32_t v : mono) m *= ring.variable(v);
        poly += m;
    }
    return Anf(std::move(poly), n_vars);
#else
    AnfFallback poly;
    for (auto mono : monomials) {
        std::sort(mono.begin(), mono.end());
        poly.add_monomial(std::move(mono));
    }
    return Anf(std::move(poly), n_vars);
#endif
}

}  // namespace bmm::benchmarks
