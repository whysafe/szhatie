#include "gzip.hpp"

#include <array>

#include "bits.hpp"
#include "deflate.hpp"

namespace szhatie {

namespace {

constexpr std::uint8_t FHCRC = 2, FEXTRA = 4, FNAME = 8, FCOMMENT = 16;

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) throw FormatError("файл оборвался в трейлере gzip");
    return d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (static_cast<std::uint32_t>(d[p + 3]) << 24);
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    const auto& t = crc_table();
    crc = ~crc;
    for (auto b : data) crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::vector<std::uint8_t> gzip_compress(std::span<const std::uint8_t> data, const GzipOptions& opt) {
    std::vector<std::uint8_t> out = {0x1F, 0x8B, 8, static_cast<std::uint8_t>(opt.name.empty() ? 0 : FNAME)};
    put32(out, opt.mtime);
    out.push_back(opt.level >= 9 ? 2 : opt.level <= 1 ? 4 : 0);  // XFL: подсказка «макс. сжатие» / «быстрое»
    out.push_back(255);                                         // ОС неизвестна: архив одинаков на любой системе
    if (!opt.name.empty()) {
        out.insert(out.end(), opt.name.begin(), opt.name.end());
        out.push_back(0);
    }
    auto body = deflate(data, opt.level);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(data));
    put32(out, static_cast<std::uint32_t>(data.size()));  // по стандарту — размер по модулю 2^32
    return out;
}

GzipResult gzip_decompress(std::span<const std::uint8_t> file) {
    GzipResult res;
    std::size_t p = 0;
    while (p < file.size()) {
        // Часть архивов дополнена нулями до размера блока (ленты, tar): это не ошибка
        if (!res.members.empty() && file[p] == 0) {
            while (p < file.size() && file[p] == 0) ++p;
            break;
        }
        if (p + 10 > file.size()) throw FormatError("файл слишком короткий для заголовка gzip");
        if (file[p] != 0x1F || file[p + 1] != 0x8B)
            throw FormatError(res.members.empty() ? "это не gzip: нет сигнатуры 1F 8B" : "мусор после конца архива");
        if (file[p + 2] != 8) throw FormatError("неизвестный метод сжатия, поддерживается только DEFLATE");
        std::uint8_t flags = file[p + 3];
        if (flags & 0xE0) throw FormatError("зарезервированные флаги в заголовке gzip");

        Member m;
        m.mtime = get32(file, p + 4);
        std::size_t q = p + 10;
        if (flags & FEXTRA) {
            if (q + 2 > file.size()) throw FormatError("файл оборвался в поле EXTRA");
            q += 2 + (file[q] | (file[q + 1] << 8));
        }
        auto read_zstring = [&](std::string* to) {
            while (q < file.size() && file[q] != 0) {
                if (to) to->push_back(static_cast<char>(file[q]));
                ++q;
            }
            if (q >= file.size()) throw FormatError("файл оборвался в заголовке gzip");
            ++q;
        };
        if (flags & FNAME) read_zstring(&m.name);
        if (flags & FCOMMENT) read_zstring(nullptr);
        if (flags & FHCRC) q += 2;
        if (q > file.size()) throw FormatError("файл оборвался в заголовке gzip");

        BitReader reader(file, q);
        auto data = inflate(reader);
        std::size_t end = reader.byte_pos();
        m.compressed = end - q;
        m.crc = get32(file, end);
        std::uint32_t isize = get32(file, end + 4);
        if (crc32(data) != m.crc) throw FormatError("контрольная сумма CRC-32 не совпала: архив повреждён");
        if (static_cast<std::uint32_t>(data.size()) != isize) throw FormatError("размер распакованных данных не совпал");
        m.size = data.size();

        res.data.insert(res.data.end(), data.begin(), data.end());
        res.members.push_back(std::move(m));
        p = end + 8;
    }
    if (res.members.empty()) throw FormatError("пустой файл");
    return res;
}

}  // namespace szhatie
