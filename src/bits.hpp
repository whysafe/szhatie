// Побитовые запись и чтение в порядке DEFLATE: младший бит байта идёт первым.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace szhatie {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}

    // Пишет младшие n бит value (n <= 32)
    void put(std::uint32_t value, int n) {
        buf_ |= static_cast<std::uint64_t>(value) << count_;
        count_ += n;
        while (count_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(buf_));
            buf_ >>= 8;
            count_ -= 8;
        }
    }

    // Дописывает неполный байт нулями: перед несжатым блоком и в конце потока
    void align() {
        if (count_ > 0) put(0, 8 - count_);
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t buf_ = 0;
    int count_ = 0;
};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data, std::size_t pos = 0) : data_(data), pos_(pos) {}

    std::uint32_t bits(int n) {
        while (count_ < n) {
            if (pos_ >= data_.size()) throw FormatError("данные оборвались посреди потока DEFLATE");
            buf_ |= static_cast<std::uint64_t>(data_[pos_++]) << count_;
            count_ += 8;
        }
        auto v = static_cast<std::uint32_t>(buf_ & ((std::uint64_t{1} << n) - 1));
        buf_ >>= n;
        count_ -= n;
        return v;
    }

    void align() {
        buf_ = 0;
        count_ = 0;  // недочитанные биты текущего байта отбрасываются
    }

    // Позиция первого непрочитанного байта: с неё начинается трейлер gzip
    std::size_t byte_pos() const { return pos_ - static_cast<std::size_t>(count_ / 8); }

    std::span<const std::uint8_t> data() const { return data_; }

    void skip_bytes(std::size_t n) {
        align();
        if (pos_ + n > data_.size()) throw FormatError("данные оборвались");
        pos_ += n;
    }

    std::size_t raw_pos() const { return pos_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_;
    std::uint64_t buf_ = 0;
    int count_ = 0;
};

}  // namespace szhatie
