// Коды Хаффмана для DEFLATE: длины с ограничением, канонические коды и декодер.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "bits.hpp"

namespace szhatie {

// Оптимальные длины кодов не длиннее max_len (алгоритм package-merge).
// Символы с нулевой частотой получают длину 0. Если используемый символ один,
// добавляется второй, чтобы дерево было полным: так строже декодеры не жалуются.
std::vector<std::uint8_t> code_lengths(std::span<const std::uint32_t> freq, int max_len);

// Канонические коды по длинам (RFC 1951, 3.2.2), уже развёрнутые для записи младшим битом вперёд.
std::vector<std::uint16_t> canonical_codes(std::span<const std::uint8_t> lengths);

// Декодер по схеме puff: число кодов каждой длины и символы в каноническом порядке.
class Decoder {
public:
    // allow_incomplete: для деревьев литералов и расстояний допустим одиночный код (как в zlib)
    Decoder(std::span<const std::uint8_t> lengths, bool allow_incomplete);

    int decode(BitReader& in) const;

private:
    std::uint16_t count_[16] = {};
    std::vector<std::uint16_t> symbols_;
};

}  // namespace szhatie
