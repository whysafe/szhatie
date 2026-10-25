#include <gtest/gtest.h>

#include <random>
#include <string>

#include "../src/deflate.hpp"
#include "../src/gzip.hpp"
#include "../src/huffman.hpp"
#include "vectors.hpp"

using namespace szhatie;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes bytes(std::string_view s) { return {s.begin(), s.end()}; }

Bytes random_bytes(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    return b;
}

// Текст с повторами, похожий на лог: на нём хорошо видны совпадения и деревья Хаффмана
Bytes log_like(std::size_t lines, unsigned seed) {
    std::mt19937 rng(seed);
    const char* level[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    const char* path[] = {"/api/orders", "/api/users", "/login", "/static/app.js", "/health"};
    std::string s;
    for (std::size_t i = 0; i < lines; ++i) {
        s += "2026-10-25 12:" + std::to_string(10 + rng() % 50) + ":" + std::to_string(10 + rng() % 50) + " " +
             level[rng() % 4] + " GET " + path[rng() % 5] + " " + std::to_string(100 + rng() % 400) + " ms\n";
    }
    return bytes(s);
}

void expect_roundtrip(const Bytes& data, int level) {
    auto packed = deflate(data, level);
    EXPECT_EQ(inflate(packed), data) << "уровень " << level << ", размер " << data.size();
}

}  // namespace

TEST(Crc32, KnownValues) {
    EXPECT_EQ(crc32(bytes("")), 0u);
    EXPECT_EQ(crc32(bytes("123456789")), 0xCBF43926u);  // контрольное значение из стандарта
    EXPECT_EQ(crc32(bytes("The quick brown fox jumps over the lazy dog")), 0x414FA339u);
}

TEST(Huffman, LengthsRespectLimit) {
    // Частоты Фибоначчи дают самое глубокое дерево: без ограничения длина ушла бы за 15
    std::vector<std::uint32_t> freq(30);
    freq[0] = freq[1] = 1;
    for (std::size_t i = 2; i < freq.size(); ++i) freq[i] = freq[i - 1] + freq[i - 2];
    auto len = code_lengths(freq, 15);
    double kraft = 0;
    for (auto l : len) {
        EXPECT_LE(l, 15);
        EXPECT_GE(l, 1);
        kraft += 1.0 / static_cast<double>(1u << l);
    }
    EXPECT_DOUBLE_EQ(kraft, 1.0);  // дерево полное
}

TEST(Huffman, SingleSymbolGetsCompleteTree) {
    std::vector<std::uint32_t> freq(10, 0);
    freq[4] = 100;
    auto len = code_lengths(freq, 15);
    EXPECT_EQ(len[4], 1);
    int used = 0;
    for (auto l : len) used += l > 0;
    EXPECT_EQ(used, 2);
}

TEST(Inflate, ZlibStreams) {
    EXPECT_TRUE(inflate(k_empty).empty());
    EXPECT_EQ(inflate(k_fixed), bytes("hello hello hello hello\n"));
    Bytes seq(100);
    for (int i = 0; i < 100; ++i) seq[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    EXPECT_EQ(inflate(k_stored), seq);
    EXPECT_EQ(inflate(k_dynamic), bytes(k_dynamic_text));
}

TEST(Inflate, RejectsCorruptData) {
    Bytes bad(std::begin(k_dynamic), std::end(k_dynamic));
    EXPECT_THROW(inflate(std::span(bad).first(bad.size() / 2)), FormatError);  // оборванный поток
    EXPECT_THROW(inflate(Bytes{0x07}), FormatError);                          // тип блока 3
    EXPECT_THROW(inflate(Bytes{0x01, 0x05, 0x00, 0x00, 0x00}), FormatError);  // LEN и NLEN не сходятся
}

TEST(Inflate, NeverCrashesOnGarbage) {
    // Случайные порчи сжатого потока: допустимы исключение или другой результат, но не падение
    auto packed = deflate(log_like(200, 3), 6);
    std::mt19937 rng(11);
    for (int i = 0; i < 500; ++i) {
        Bytes b = packed;
        for (int k = 0; k < 3; ++k) b[rng() % b.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        try {
            (void)inflate(b);
        } catch (const FormatError&) {
        }
    }
    SUCCEED();
}

TEST(Deflate, RoundtripAllLevels) {
    std::vector<Bytes> inputs = {
        {},
        bytes("a"),
        bytes("abc"),
        Bytes(100000, 'x'),                // длинные перекрывающиеся совпадения
        log_like(3000, 1),                 // несколько блоков
        random_bytes(70000, 2),            // несжимаемое: несколько несжатых блоков по 65535
        bytes(k_dynamic_text),
    };
    for (int level = 0; level <= 9; ++level)
        for (const auto& in : inputs) expect_roundtrip(in, level);
}

TEST(Deflate, CompressesAndHigherLevelIsNotWorse) {
    auto data = log_like(5000, 4);
    auto l1 = deflate(data, 1).size();
    auto l6 = deflate(data, 6).size();
    auto l9 = deflate(data, 9).size();
    EXPECT_LT(l6, data.size() / 4);
    EXPECT_LE(l9, l6);
    EXPECT_LE(l6, l1);
}

TEST(Deflate, RandomDataBarelyGrows) {
    auto data = random_bytes(200000, 5);
    // Каждый блок уходит несжатым: несколько байт заголовков, не больше 0.1 % сверху
    EXPECT_LE(deflate(data, 9).size(), data.size() + data.size() / 1000 + 64);
}

TEST(Gzip, RoundtripWithName) {
    auto data = log_like(500, 6);
    auto gz = gzip_compress(data, {9, "access.log", 1700000000});
    EXPECT_EQ(gz[0], 0x1F);
    EXPECT_EQ(gz[1], 0x8B);
    auto r = gzip_decompress(gz);
    EXPECT_EQ(r.data, data);
    ASSERT_EQ(r.members.size(), 1u);
    EXPECT_EQ(r.members[0].name, "access.log");
    EXPECT_EQ(r.members[0].mtime, 1700000000u);
}

TEST(Gzip, ReadsPythonGzipWithFilename) {
    auto r = gzip_decompress(k_gzip_named);
    EXPECT_EQ(r.data, bytes("szhatie\n"));
    EXPECT_EQ(r.members[0].name, "a.txt");
    EXPECT_EQ(r.members[0].mtime, 1700000000u);
}

TEST(Gzip, ConcatenatedMembersAndZeroPadding) {
    auto a = gzip_compress(bytes("первая часть\n"));
    auto b = gzip_compress(bytes("вторая часть\n"));
    Bytes both = a;
    both.insert(both.end(), b.begin(), b.end());
    both.insert(both.end(), 512, 0);
    auto r = gzip_decompress(both);
    EXPECT_EQ(r.data, bytes("первая часть\nвторая часть\n"));
    EXPECT_EQ(r.members.size(), 2u);
}

TEST(Gzip, DetectsCorruption) {
    auto gz = gzip_compress(log_like(100, 7));
    Bytes bad_crc = gz;
    bad_crc[bad_crc.size() - 6] ^= 0xFF;
    EXPECT_THROW(gzip_decompress(bad_crc), FormatError);
    Bytes bad_magic = gz;
    bad_magic[0] = 0;
    EXPECT_THROW(gzip_decompress(bad_magic), FormatError);
    EXPECT_THROW(gzip_decompress(std::span(gz).first(gz.size() - 3)), FormatError);
}
