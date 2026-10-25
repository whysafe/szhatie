#include "huffman.hpp"

#include <algorithm>
#include <iterator>

namespace szhatie {

namespace {

struct Item {
    std::uint64_t weight;
    std::vector<std::uint16_t> symbols;  // какие листья входят в пакет
};

}  // namespace

std::vector<std::uint8_t> code_lengths(std::span<const std::uint32_t> freq_in, int max_len) {
    std::vector<std::uint32_t> freq(freq_in.begin(), freq_in.end());
    std::vector<std::uint8_t> lengths(freq.size(), 0);

    std::vector<std::uint16_t> used;
    for (std::size_t i = 0; i < freq.size(); ++i)
        if (freq[i] > 0) used.push_back(static_cast<std::uint16_t>(i));
    // Меньше двух символов — дерево неполное. Дополняем фиктивным символом с частотой 1.
    for (std::uint16_t s = 0; used.size() < 2 && s < freq.size(); ++s) {
        if (freq[s] == 0) {
            freq[s] = 1;
            used.push_back(s);
        }
    }
    std::sort(used.begin(), used.end());
    if (used.size() < 2) return lengths;

    std::vector<Item> leaves;
    leaves.reserve(used.size());
    for (auto s : used) leaves.push_back({freq[s], {s}});
    std::stable_sort(leaves.begin(), leaves.end(), [](const Item& a, const Item& b) { return a.weight < b.weight; });

    // package-merge: max_len раз объединяем соседние пары и сливаем с листьями
    std::vector<Item> list = leaves;
    for (int level = 1; level < max_len; ++level) {
        std::vector<Item> packages;
        for (std::size_t i = 0; i + 1 < list.size(); i += 2) {
            Item p{list[i].weight + list[i + 1].weight, list[i].symbols};
            p.symbols.insert(p.symbols.end(), list[i + 1].symbols.begin(), list[i + 1].symbols.end());
            packages.push_back(std::move(p));
        }
        std::vector<Item> merged;
        merged.reserve(leaves.size() + packages.size());
        std::merge(leaves.begin(), leaves.end(), packages.begin(), packages.end(), std::back_inserter(merged),
                   [](const Item& a, const Item& b) { return a.weight < b.weight; });
        list = std::move(merged);
    }

    // Длина символа — сколько раз он встретился в первых 2n-2 элементах
    std::size_t take = 2 * used.size() - 2;
    for (std::size_t i = 0; i < take && i < list.size(); ++i)
        for (auto s : list[i].symbols) ++lengths[s];
    return lengths;
}

std::vector<std::uint16_t> canonical_codes(std::span<const std::uint8_t> lengths) {
    std::uint16_t bl_count[16] = {};
    for (auto l : lengths) ++bl_count[l];
    bl_count[0] = 0;

    std::uint16_t next[16] = {};
    std::uint16_t code = 0;
    for (int bits = 1; bits < 16; ++bits) {
        code = static_cast<std::uint16_t>((code + bl_count[bits - 1]) << 1);
        next[bits] = code;
    }

    std::vector<std::uint16_t> codes(lengths.size(), 0);
    for (std::size_t s = 0; s < lengths.size(); ++s) {
        int len = lengths[s];
        if (len == 0) continue;
        std::uint16_t c = next[len]++;
        // Коды Хаффмана в потоке идут старшим битом вперёд, а BitWriter пишет младшим: разворачиваем
        std::uint16_t rev = 0;
        for (int i = 0; i < len; ++i) rev = static_cast<std::uint16_t>((rev << 1) | ((c >> i) & 1));
        codes[s] = rev;
    }
    return codes;
}

Decoder::Decoder(std::span<const std::uint8_t> lengths, bool allow_incomplete) {
    for (auto l : lengths) {
        if (l > 15) throw FormatError("длина кода Хаффмана больше 15");
        ++count_[l];
    }
    int used = static_cast<int>(lengths.size()) - count_[0];
    if (used == 0) {
        // Пустое дерево допустимо только для расстояний, когда в блоке одни литералы
        if (!allow_incomplete) throw FormatError("пустое дерево Хаффмана");
        return;
    }
    // Проверка Крафта: дерево не должно быть переполнено
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= count_[len];
        if (left < 0) throw FormatError("переполненное дерево Хаффмана");
    }
    if (left > 0 && !(allow_incomplete && used == 1)) throw FormatError("неполное дерево Хаффмана");

    std::uint16_t offs[16] = {};
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count_[len]);
    symbols_.assign(lengths.size(), 0);
    for (std::size_t s = 0; s < lengths.size(); ++s)
        if (lengths[s]) symbols_[offs[lengths[s]]++] = static_cast<std::uint16_t>(s);
}

int Decoder::decode(BitReader& in) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(in.bits(1));
        int count = count_[len];
        if (code - count < first) return symbols_[static_cast<std::size_t>(index + (code - first))];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    throw FormatError("неизвестный код Хаффмана");
}

}  // namespace szhatie
