// Формат gzip (RFC 1952) поверх DEFLATE: заголовок, имя файла, CRC-32 и размер.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bits.hpp"  // FormatError: ошибки формата бросаются наружу

namespace szhatie {

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

struct GzipOptions {
    int level = 6;
    std::string name;          // сохраняется в заголовке, как это делает gzip
    std::uint32_t mtime = 0;   // время изменения исходного файла, 0 — неизвестно
};

std::vector<std::uint8_t> gzip_compress(std::span<const std::uint8_t> data, const GzipOptions& opt = {});

struct Member {
    std::string name;
    std::uint32_t mtime = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;  // байт потока DEFLATE
    std::uint64_t size = 0;
};

struct GzipResult {
    std::vector<std::uint8_t> data;
    std::vector<Member> members;  // gzip допускает несколько склеенных частей: cat a.gz b.gz
};

// Распаковка с проверкой CRC-32 и длины каждой части. Ошибки — исключение FormatError.
GzipResult gzip_decompress(std::span<const std::uint8_t> file);

}  // namespace szhatie
