// generate_sbox.cpp — генератор тестовых S-box'ов (векторных булевых
// функций) для benchmarks/. Не downloader, в отличие от соседних
// download_*.sh: для S-box нет единого архива с постоянным URL — это
// маленькие (обычно 2^4..2^8 записей) таблицы, которые либо стандартизованы
// в конкретном документе (AES/DES/ГОСТ и т.п. — переписывать их сюда по
// памяти рискованно, легко ошибиться в одном байте и получить незаметно
// неверные тестовые данные), либо генерируются. Этот генератор делает
// второе — случайную БИЕКЦИЮ (перестановку) на 2^k значениях с
// детерминированным seed, что даёт корректные тестовые векторные булевы
// функции (каждая координатная функция — обычная n=k Boolean-функция,
// пригодная для tt_to_anf/tt_to_thr/tt_to_aig и т.п.) без риска опечатки
// в стандартной таблице.
//
// Если нужен именно конкретный стандартный S-box (AES, ГОСТ...) — возьмите
// его из первоисточника (текст стандарта/RFC) и впишите as-is, сверив
// каждый байт, а не из вторых рук.
//
// Сборка (не часть основного CMakeLists.txt — вспомогательный инструмент):
//   g++ -std=c++23 -O2 generate_sbox.cpp -o generate_sbox
// Запуск:
//   ./generate_sbox <k> <seed> > out.sbox
// Формат вывода: k, затем 2^k строк "index -> value" (обе в двоичном виде,
// LSB_FIRST — то же соглашение, что и BitOrder в core/common.hpp).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

std::string to_binary(uint64_t value, uint32_t width) {
    std::string s(width, '0');
    for (uint32_t i = 0; i < width; ++i) {
        if ((value >> i) & 1u) s[i] = '1';  // LSB_FIRST: бит i строки = бит i числа
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Использование: %s <k> <seed>\n", argv[0]);
        std::fprintf(stderr, "  k    — число бит входа/выхода S-box (обычно 4 или 8)\n");
        std::fprintf(stderr, "  seed — для воспроизводимости\n");
        return 1;
    }

    const uint32_t k = static_cast<uint32_t>(std::stoul(argv[1]));
    const uint64_t seed = std::stoull(argv[2]);
    const uint64_t n = uint64_t{1} << k;

    std::vector<uint64_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::mt19937_64 rng(seed);
    std::shuffle(perm.begin(), perm.end(), rng);

    std::printf("%u\n", k);
    for (uint64_t i = 0; i < n; ++i) {
        std::printf("%s -> %s\n", to_binary(i, k).c_str(), to_binary(perm[i], k).c_str());
    }
    return 0;
}
