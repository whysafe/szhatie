// DEFLATE (RFC 1951): сжатие и распаковка «сырого» потока без обёртки.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "bits.hpp"

namespace szhatie {

struct Level {
    int max_chain;   // сколько предыдущих позиций с тем же хешем проверять
    int nice_len;    // совпадение такой длины считается достаточно хорошим, поиск прекращается
    bool lazy;       // ленивое сопоставление: не брать совпадение, если со следующего байта есть длиннее
};

// Уровни 1–9 по смыслу как у gzip: дольше ищем — лучше сжимаем. 0 — без сжатия.
Level level_params(int level);

std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> input, int level = 6);

// Распаковывает поток с текущей позиции reader; после возврата reader стоит на границе байта за потоком.
std::vector<std::uint8_t> inflate(BitReader& reader);

std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> raw);

// Таблицы длин и расстояний из RFC 1951, 3.2.5
inline constexpr std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,    5,    7,    9,    13,   17,    25,
                                                33,  49,  65,  97,   129,  193,  257,  385,  513,   769,
                                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace szhatie
