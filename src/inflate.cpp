#include <array>

#include "deflate.hpp"
#include "huffman.hpp"

namespace szhatie {

namespace {

// Порядок длин кодов в заголовке динамического блока (RFC 1951, 3.2.7)
constexpr int kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

void inflate_codes(BitReader& in, std::vector<std::uint8_t>& out, const Decoder& lit, const Decoder& dist) {
    for (;;) {
        int sym = lit.decode(in);
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
            continue;
        }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) throw FormatError("недопустимый код длины");
        std::size_t len = kLenBase[sym] + in.bits(kLenExtra[sym]);

        int dsym = dist.decode(in);
        if (dsym >= 30) throw FormatError("недопустимый код расстояния");
        std::size_t d = kDistBase[dsym] + in.bits(kDistExtra[dsym]);
        if (d > out.size()) throw FormatError("ссылка на данные до начала потока");

        // Копируем побайтно: совпадение может перекрывать само себя (d < len), так кодируются повторы
        std::size_t from = out.size() - d;
        for (std::size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    }
}

void inflate_stored(BitReader& in, std::vector<std::uint8_t>& out) {
    in.align();
    auto data = in.data();
    std::size_t p = in.raw_pos();
    if (p + 4 > data.size()) throw FormatError("данные оборвались в заголовке несжатого блока");
    unsigned len = data[p] | (data[p + 1] << 8);
    unsigned nlen = data[p + 2] | (data[p + 3] << 8);
    if ((len ^ 0xFFFFu) != nlen) throw FormatError("повреждена длина несжатого блока");
    if (p + 4 + len > data.size()) throw FormatError("данные оборвались в несжатом блоке");
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(p + 4),
               data.begin() + static_cast<std::ptrdiff_t>(p + 4 + len));
    in.skip_bytes(4 + len);
}

const std::pair<Decoder, Decoder>& fixed_decoders() {
    static const auto pair = [] {
        std::array<std::uint8_t, 288> lit{};
        for (int i = 0; i < 144; ++i) lit[i] = 8;
        for (int i = 144; i < 256; ++i) lit[i] = 9;
        for (int i = 256; i < 280; ++i) lit[i] = 7;
        for (int i = 280; i < 288; ++i) lit[i] = 8;
        // 32 кода, а не 30: так дерево полное, а коды 30 и 31 отсекаются при декодировании
        std::array<std::uint8_t, 32> dist{};
        dist.fill(5);
        return std::pair<Decoder, Decoder>{Decoder(lit, false), Decoder(dist, true)};
    }();
    return pair;
}

void inflate_dynamic(BitReader& in, std::vector<std::uint8_t>& out) {
    int hlit = static_cast<int>(in.bits(5)) + 257;
    int hdist = static_cast<int>(in.bits(5)) + 1;
    int hclen = static_cast<int>(in.bits(4)) + 4;
    if (hlit > 286 || hdist > 30) throw FormatError("слишком много кодов в динамическом блоке");

    std::array<std::uint8_t, 19> cl{};
    for (int i = 0; i < hclen; ++i) cl[kClOrder[i]] = static_cast<std::uint8_t>(in.bits(3));
    Decoder cl_dec(cl, false);

    std::vector<std::uint8_t> lengths(static_cast<std::size_t>(hlit + hdist), 0);
    std::size_t i = 0;
    while (i < lengths.size()) {
        int sym = cl_dec.decode(in);
        if (sym < 16) {
            lengths[i++] = static_cast<std::uint8_t>(sym);
            continue;
        }
        std::uint8_t value = 0;
        std::size_t repeat;
        if (sym == 16) {
            if (i == 0) throw FormatError("повтор длины без предыдущей");
            value = lengths[i - 1];
            repeat = 3 + in.bits(2);
        } else if (sym == 17) {
            repeat = 3 + in.bits(3);
        } else {
            repeat = 11 + in.bits(7);
        }
        if (i + repeat > lengths.size()) throw FormatError("повтор длин выходит за таблицу");
        while (repeat--) lengths[i++] = value;
    }
    if (lengths[256] == 0) throw FormatError("в блоке нет кода конца блока");

    std::span<const std::uint8_t> all(lengths);
    Decoder lit(all.first(static_cast<std::size_t>(hlit)), true);
    Decoder dist(all.subspan(static_cast<std::size_t>(hlit)), true);
    inflate_codes(in, out, lit, dist);
}

}  // namespace

std::vector<std::uint8_t> inflate(BitReader& in) {
    std::vector<std::uint8_t> out;
    bool last = false;
    while (!last) {
        last = in.bits(1) == 1;
        switch (in.bits(2)) {
            case 0: inflate_stored(in, out); break;
            case 1: inflate_codes(in, out, fixed_decoders().first, fixed_decoders().second); break;
            case 2: inflate_dynamic(in, out); break;
            default: throw FormatError("неизвестный тип блока 3");
        }
    }
    in.align();
    return out;
}

std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> raw) {
    BitReader in(raw);
    return inflate(in);
}

}  // namespace szhatie
