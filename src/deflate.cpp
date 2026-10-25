#include <algorithm>
#include <array>
#include <iterator>

#include "deflate.hpp"
#include "huffman.hpp"

namespace szhatie {

namespace {

constexpr int kWindow = 32768;
constexpr int kWindowMask = kWindow - 1;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
constexpr std::size_t kBlockTokens = 1 << 15;  // токенов в одном блоке: у каждого блока своё дерево
constexpr int kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Token {
    std::uint16_t len;   // 0 — литерал
    std::uint16_t dist;  // для литерала здесь байт
};

// Для длины 3..258 и расстояния 1..32768 — номер кода по таблицам RFC
int len_code(int len) {
    int c = 0;
    while (c < 28 && kLenBase[c + 1] <= len) ++c;
    return c;
}

int dist_code(int dist) {
    int c = 0;
    while (c < 29 && kDistBase[c + 1] <= dist) ++c;
    return c;
}

// Таблицы для быстрого перевода: длина -> код, расстояние -> код
struct CodeTables {
    std::array<std::uint8_t, 259> len{};
    std::array<std::uint8_t, 512> dist_small{};  // для расстояний 1..512
    CodeTables() {
        for (int l = 3; l <= 258; ++l) len[l] = static_cast<std::uint8_t>(len_code(l));
        for (int d = 1; d <= 512; ++d) dist_small[d - 1] = static_cast<std::uint8_t>(dist_code(d));
    }
    int dist(int d) const {
        if (d <= 512) return dist_small[d - 1];
        // Коды больших расстояний растут по степеням двойки: 7 бит отбрасываются и смотрим таблицу
        return dist_small[(d - 1) >> 7] + 14;
    }
};

const CodeTables& tables() {
    static const CodeTables t;
    return t;
}

// ---------- LZ77 ----------

class Matcher {
public:
    Matcher(std::span<const std::uint8_t> in, Level lv) : in_(in), lv_(lv), head_(kHashSize, -1), prev_(kWindow, -1) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        out.reserve(in_.size() / 2 + 16);
        std::size_t i = 0;
        const std::size_t n = in_.size();
        while (i < n) {
            auto [len, dist] = find(i);
            if (len >= kMinMatch && lv_.lazy && len < lv_.nice_len && i + 1 < n) {
                insert(i);
                auto [len2, dist2] = find(i + 1);
                if (len2 > len) {
                    // Совпадение со следующего байта длиннее: текущий байт уходит литералом
                    out.push_back({0, in_[i]});
                    ++i;
                    len = len2;
                    dist = dist2;
                    insert_range(i, i + static_cast<std::size_t>(len));
                    out.push_back({static_cast<std::uint16_t>(len), static_cast<std::uint16_t>(dist)});
                    i += static_cast<std::size_t>(len);
                    continue;
                }
                insert_range(i + 1, i + static_cast<std::size_t>(len));
            } else if (len >= kMinMatch) {
                insert_range(i, i + static_cast<std::size_t>(len));
            } else {
                insert(i);
                out.push_back({0, in_[i]});
                ++i;
                continue;
            }
            out.push_back({static_cast<std::uint16_t>(len), static_cast<std::uint16_t>(dist)});
            i += static_cast<std::size_t>(len);
        }
        return out;
    }

private:
    std::uint32_t hash(std::size_t i) const {
        std::uint32_t v = in_[i] | (in_[i + 1] << 8) | (in_[i + 2] << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    }

    void insert(std::size_t i) {
        if (i + kMinMatch > in_.size()) return;
        std::uint32_t h = hash(i);
        prev_[i & kWindowMask] = head_[h];
        head_[h] = static_cast<std::int64_t>(i);
    }

    void insert_range(std::size_t from, std::size_t to) {
        for (std::size_t j = from; j < to; ++j) insert(j);
    }

    std::pair<int, int> find(std::size_t i) const {
        if (i + kMinMatch > in_.size()) return {0, 0};
        const std::size_t limit = std::min<std::size_t>(kMaxMatch, in_.size() - i);
        int best = 0, best_dist = 0;
        int chain = lv_.max_chain;
        std::int64_t cand = head_[hash(i)];
        while (cand >= 0 && chain-- > 0) {
            auto c = static_cast<std::size_t>(cand);
            std::size_t dist = i - c;
            if (dist == 0 || dist > kWindow) break;
            // Сначала сверяем байт, который должен продлить лучшее совпадение: отсекает большинство кандидатов
            if (in_[c + static_cast<std::size_t>(best)] == in_[i + static_cast<std::size_t>(best)] || best == 0) {
                std::size_t l = 0;
                while (l < limit && in_[c + l] == in_[i + l]) ++l;
                if (static_cast<int>(l) > best) {
                    best = static_cast<int>(l);
                    best_dist = static_cast<int>(dist);
                    if (best >= lv_.nice_len || l == limit) break;
                }
            }
            std::int64_t next = prev_[c & kWindowMask];
            if (next >= cand) break;  // ячейка уже перезаписана более новой позицией
            cand = next;
        }
        return best >= kMinMatch ? std::pair{best, best_dist} : std::pair{0, 0};
    }

    std::span<const std::uint8_t> in_;
    Level lv_;
    std::vector<std::int64_t> head_;
    std::vector<std::int64_t> prev_;
};

// ---------- Запись блоков ----------

struct Block {
    std::span<const Token> tokens;
    std::span<const std::uint8_t> raw;  // исходные байты блока: на случай несжатого варианта
};

struct Trees {
    std::vector<std::uint8_t> lit_len, dist_len;
    std::vector<std::uint16_t> lit_code, dist_code;
};

void count_freq(const Block& b, std::array<std::uint32_t, 286>& lit, std::array<std::uint32_t, 30>& dist) {
    lit.fill(0);
    dist.fill(0);
    const auto& t = tables();
    for (const auto& tk : b.tokens) {
        if (tk.len == 0) {
            ++lit[tk.dist];
        } else {
            ++lit[257 + t.len[tk.len]];
            ++dist[t.dist(tk.dist)];
        }
    }
    lit[256] = 1;
}

// Сколько бит займут данные блока с этими деревьями (без заголовка)
std::uint64_t data_bits(const Block& b, std::span<const std::uint8_t> lit_len, std::span<const std::uint8_t> dist_len) {
    const auto& t = tables();
    std::uint64_t bits = lit_len[256];
    for (const auto& tk : b.tokens) {
        if (tk.len == 0) {
            bits += lit_len[tk.dist];
        } else {
            int lc = t.len[tk.len], dc = t.dist(tk.dist);
            bits += lit_len[static_cast<std::size_t>(257 + lc)] + kLenExtra[lc] + dist_len[static_cast<std::size_t>(dc)] + kDistExtra[dc];
        }
    }
    return bits;
}

void write_tokens(BitWriter& w, const Block& b, const Trees& tr) {
    const auto& t = tables();
    for (const auto& tk : b.tokens) {
        if (tk.len == 0) {
            w.put(tr.lit_code[tk.dist], tr.lit_len[tk.dist]);
            continue;
        }
        int lc = t.len[tk.len], dc = t.dist(tk.dist);
        auto ls = static_cast<std::size_t>(257 + lc);
        w.put(tr.lit_code[ls], tr.lit_len[ls]);
        w.put(static_cast<std::uint32_t>(tk.len - kLenBase[lc]), kLenExtra[lc]);
        w.put(tr.dist_code[static_cast<std::size_t>(dc)], tr.dist_len[static_cast<std::size_t>(dc)]);
        w.put(static_cast<std::uint32_t>(tk.dist - kDistBase[dc]), kDistExtra[dc]);
    }
    w.put(tr.lit_code[256], tr.lit_len[256]);
}

const Trees& fixed_trees() {
    static const Trees tr = [] {
        Trees t;
        t.lit_len.assign(288, 8);
        for (int i = 144; i < 256; ++i) t.lit_len[i] = 9;
        for (int i = 256; i < 280; ++i) t.lit_len[i] = 7;
        t.dist_len.assign(30, 5);
        t.lit_code = canonical_codes(t.lit_len);
        t.dist_code = canonical_codes(t.dist_len);
        return t;
    }();
    return tr;
}

// Длины кодов литералов и расстояний подряд сжимаются кодами 16/17/18 (повторы и нули)
struct ClSymbol {
    std::uint8_t sym, extra;
};

std::vector<ClSymbol> rle_lengths(std::span<const std::uint8_t> lens) {
    std::vector<ClSymbol> out;
    std::size_t i = 0;
    while (i < lens.size()) {
        std::uint8_t v = lens[i];
        std::size_t run = 1;
        while (i + run < lens.size() && lens[i + run] == v) ++run;
        if (v == 0 && run >= 3) {
            std::size_t r = std::min<std::size_t>(run, 138);
            if (r >= 11) out.push_back({18, static_cast<std::uint8_t>(r - 11)});
            else out.push_back({17, static_cast<std::uint8_t>(r - 3)});
            i += r;
        } else if (v != 0 && run >= 4) {
            out.push_back({v, 0});
            std::size_t r = std::min<std::size_t>(run - 1, 6);
            out.push_back({16, static_cast<std::uint8_t>(r - 3)});
            i += 1 + r;
        } else {
            out.push_back({v, 0});
            ++i;
        }
    }
    return out;
}

struct Dynamic {
    Trees tr;
    int hlit, hdist, hclen;
    std::vector<ClSymbol> rle;
    std::array<std::uint8_t, 19> cl_len{};
    std::vector<std::uint16_t> cl_code;
    std::uint64_t header_bits;
};

Dynamic build_dynamic(const Block& b) {
    std::array<std::uint32_t, 286> lit{};
    std::array<std::uint32_t, 30> dist{};
    count_freq(b, lit, dist);

    Dynamic d;
    d.tr.lit_len = code_lengths(lit, 15);
    d.tr.dist_len = code_lengths(dist, 15);
    d.tr.lit_code = canonical_codes(d.tr.lit_len);
    d.tr.dist_code = canonical_codes(d.tr.dist_len);

    d.hlit = 286;
    while (d.hlit > 257 && d.tr.lit_len[static_cast<std::size_t>(d.hlit - 1)] == 0) --d.hlit;
    d.hdist = 30;
    while (d.hdist > 1 && d.tr.dist_len[static_cast<std::size_t>(d.hdist - 1)] == 0) --d.hdist;

    std::vector<std::uint8_t> all(d.tr.lit_len.begin(), d.tr.lit_len.begin() + d.hlit);
    all.insert(all.end(), d.tr.dist_len.begin(), d.tr.dist_len.begin() + d.hdist);
    d.rle = rle_lengths(all);

    std::array<std::uint32_t, 19> cl_freq{};
    for (auto s : d.rle) ++cl_freq[s.sym];
    auto cl = code_lengths(cl_freq, 7);
    std::copy(cl.begin(), cl.end(), d.cl_len.begin());
    d.cl_code = canonical_codes(d.cl_len);

    d.hclen = 19;
    while (d.hclen > 4 && d.cl_len[static_cast<std::size_t>(kClOrder[d.hclen - 1])] == 0) --d.hclen;

    d.header_bits = 5 + 5 + 4 + 3 * static_cast<std::uint64_t>(d.hclen);
    for (auto s : d.rle) d.header_bits += d.cl_len[s.sym] + (s.sym == 16 ? 2 : s.sym == 17 ? 3 : s.sym == 18 ? 7 : 0);
    return d;
}

void write_dynamic(BitWriter& w, const Block& b, const Dynamic& d) {
    w.put(static_cast<std::uint32_t>(d.hlit - 257), 5);
    w.put(static_cast<std::uint32_t>(d.hdist - 1), 5);
    w.put(static_cast<std::uint32_t>(d.hclen - 4), 4);
    for (int i = 0; i < d.hclen; ++i) w.put(d.cl_len[static_cast<std::size_t>(kClOrder[i])], 3);
    for (auto s : d.rle) {
        w.put(d.cl_code[s.sym], d.cl_len[s.sym]);
        if (s.sym == 16) w.put(s.extra, 2);
        else if (s.sym == 17) w.put(s.extra, 3);
        else if (s.sym == 18) w.put(s.extra, 7);
    }
    write_tokens(w, b, d.tr);
}

void write_stored(BitWriter& w, std::span<const std::uint8_t> raw, bool last) {
    // Несжатый блок вмещает не больше 65535 байт: длинные куски делим
    std::size_t pos = 0;
    do {
        std::size_t len = std::min<std::size_t>(raw.size() - pos, 65535);
        bool final_piece = pos + len == raw.size();
        w.put(last && final_piece ? 1 : 0, 1);
        w.put(0, 2);
        w.align();
        w.put(static_cast<std::uint32_t>(len), 16);
        w.put(static_cast<std::uint32_t>(~len & 0xFFFF), 16);
        for (std::size_t i = 0; i < len; ++i) w.put(raw[pos + i], 8);
        pos += len;
    } while (pos < raw.size());
}

// Для каждого блока выбирается самый короткий из трёх вариантов: динамические деревья,
// фиксированные или без сжатия (случайные данные и уже сжатые файлы)
void write_block(BitWriter& w, const Block& b, bool last) {
    Dynamic d = build_dynamic(b);
    std::uint64_t dyn = 3 + d.header_bits + data_bits(b, d.tr.lit_len, d.tr.dist_len);
    std::uint64_t fix = 3 + data_bits(b, fixed_trees().lit_len, fixed_trees().dist_len);
    std::uint64_t stored = (b.raw.size() / 65535 + 1) * (3 + 7 + 32) + 8 * static_cast<std::uint64_t>(b.raw.size());

    if (stored < dyn && stored < fix) {
        write_stored(w, b.raw, last);
    } else if (fix <= dyn) {
        w.put(last ? 1 : 0, 1);
        w.put(1, 2);
        write_tokens(w, b, fixed_trees());
    } else {
        w.put(last ? 1 : 0, 1);
        w.put(2, 2);
        write_dynamic(w, b, d);
    }
}

}  // namespace

Level level_params(int level) {
    static constexpr Level table[10] = {
        {0, 0, false},     {4, 8, false},     {8, 16, false},     {16, 32, false},    {16, 32, true},
        {32, 64, true},    {128, 128, true},  {256, 258, true},   {1024, 258, true},  {4096, 258, true},
    };
    return table[std::clamp(level, 0, 9)];
}

std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> input, int level) {
    std::vector<std::uint8_t> out;
    BitWriter w(out);
    if (level <= 0 || input.empty()) {
        write_stored(w, input, true);
        w.align();
        return out;
    }

    std::vector<Token> tokens = Matcher(input, level_params(level)).run();
    std::size_t raw_pos = 0;
    for (std::size_t start = 0; start < tokens.size(); start += kBlockTokens) {
        std::size_t end = std::min(tokens.size(), start + kBlockTokens);
        std::span<const Token> part(tokens.data() + start, end - start);
        std::size_t raw_len = 0;
        for (const auto& t : part) raw_len += t.len == 0 ? 1 : t.len;
        write_block(w, {part, input.subspan(raw_pos, raw_len)}, end == tokens.size());
        raw_pos += raw_len;
    }
    w.align();
    return out;
}

}  // namespace szhatie
