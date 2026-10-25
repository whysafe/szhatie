// szhatie: совместимая с gzip утилита сжатия, написанная с нуля.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "gzip.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;
using szhatie::FormatError;

namespace {

constexpr const char* kUsage = R"(szhatie — сжатие и распаковка в формате gzip

  szhatie [опции] ФАЙЛ...      сжать каждый файл в ФАЙЛ.gz (исходник остаётся)
  szhatie -d ФАЙЛ.gz...        распаковать
  szhatie -t ФАЙЛ.gz...        проверить целостность
  szhatie -l ФАЙЛ.gz...        показать имя, размеры и степень сжатия
  ... | szhatie -c > out.gz    без файлов: stdin -> stdout

  -1 … -9   уровень: 1 быстрее, 9 сильнее (по умолчанию 6)
  -c        писать в stdout
  -f        перезаписывать существующие файлы
  -v        подробно: размер, процент и время
)";

struct Options {
    bool decompress = false, test = false, list = false, to_stdout = false, force = false, verbose = false;
    int level = 6;
    std::vector<std::string> files;
};

std::vector<std::uint8_t> read_all(std::istream& in) {
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("не удалось открыть " + p.string());
    return read_all(in);
}

void write_file(const fs::path& p, const std::vector<std::uint8_t>& data, bool force) {
    if (fs::exists(p) && !force) throw std::runtime_error(p.string() + " уже существует, добавьте -f, чтобы перезаписать");
    // Пишем во временный файл и переименовываем: при ошибке не останется наполовину записанного архива
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) throw std::runtime_error("не удалось записать " + tmp.string());
    }
    fs::rename(tmp, p);
}

void write_stdout(const std::vector<std::uint8_t>& data) {
    std::fwrite(data.data(), 1, data.size(), stdout);
    std::fflush(stdout);
}

std::string human(std::uint64_t n) {
    const char* units[] = {"Б", "КБ", "МБ", "ГБ"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

double percent(std::uint64_t packed, std::uint64_t original) {
    return original == 0 ? 0.0 : 100.0 * (1.0 - static_cast<double>(packed) / static_cast<double>(original));
}

int run_file(const Options& o, const std::string& name) {
    auto t0 = std::chrono::steady_clock::now();
    fs::path path(name);
    auto input = read_file(path);

    if (o.list) {
        auto r = szhatie::gzip_decompress(input);
        for (const auto& m : r.members) {
            std::printf("%12llu %12llu %6.1f%%  %s\n", static_cast<unsigned long long>(input.size()),
                        static_cast<unsigned long long>(m.size), percent(input.size(), m.size),
                        m.name.empty() ? path.stem().string().c_str() : m.name.c_str());
        }
        return 0;
    }
    if (o.test) {
        auto r = szhatie::gzip_decompress(input);
        std::printf("%s: OK, %s в %zu част%s\n", name.c_str(), human(r.data.size()).c_str(), r.members.size(),
                    r.members.size() == 1 ? "и" : "ях");
        return 0;
    }

    std::vector<std::uint8_t> output;
    fs::path target;
    if (o.decompress) {
        if (path.extension() != ".gz" && !o.to_stdout)
            throw std::runtime_error(name + ": не оканчивается на .gz, непонятно, как назвать результат (используйте -c)");
        output = szhatie::gzip_decompress(input).data;
        target = path;
        target.replace_extension();
    } else {
        if (path.extension() == ".gz" && !o.force) throw std::runtime_error(name + " уже сжат (.gz), добавьте -f, если нужно");
        szhatie::GzipOptions g;
        g.level = o.level;
        g.name = path.filename().string();
        // clock_cast есть не во всех стандартных библиотеках (libc++ на macOS), поэтому переводим через разницу с now()
        auto ftime = fs::last_write_time(path);
        auto sys = std::chrono::system_clock::now() +
                   std::chrono::duration_cast<std::chrono::system_clock::duration>(ftime - fs::file_time_type::clock::now());
        g.mtime = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
        output = szhatie::gzip_compress(input, g);
        target = path;
        target += ".gz";
    }

    if (o.to_stdout) write_stdout(output);
    else write_file(target, output, o.force);

    if (o.verbose) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        auto packed = o.decompress ? input.size() : output.size();
        auto orig = o.decompress ? output.size() : input.size();
        std::fprintf(stderr, "%s: %s -> %s, %.1f%%, %.0f мс\n", name.c_str(), human(input.size()).c_str(),
                     human(output.size()).c_str(), percent(packed, orig), ms);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (a.size() >= 2 && a[0] == '-' && a != "-") {
            for (char c : a.substr(1)) {
                if (c >= '0' && c <= '9') o.level = c - '0';
                else if (c == 'd') o.decompress = true;
                else if (c == 't') o.test = true;
                else if (c == 'l') o.list = true;
                else if (c == 'c') o.to_stdout = true;
                else if (c == 'f') o.force = true;
                else if (c == 'v') o.verbose = true;
                else {
                    std::fprintf(stderr, "неизвестная опция -%c\n\n%s", c, kUsage);
                    return 2;
                }
            }
        } else {
            o.files.push_back(a);
        }
    }

    try {
        if (o.files.empty() || (o.files.size() == 1 && o.files[0] == "-")) {
            auto input = read_all(std::cin);
            if (o.test) {
                szhatie::gzip_decompress(input);
                std::fputs("stdin: OK\n", stderr);
                return 0;
            }
            write_stdout(o.decompress ? szhatie::gzip_decompress(input).data
                                      : szhatie::gzip_compress(input, {o.level, "", 0}));
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "szhatie: %s\n", e.what());
        return 1;
    }

    if (o.list) std::printf("%12s %12s %7s  %s\n", "сжатый", "исходный", "сжатие", "имя");
    int rc = 0;
    for (const auto& f : o.files) {
        try {
            run_file(o, f);
        } catch (const FormatError& e) {
            std::fprintf(stderr, "szhatie: %s: повреждён или не gzip: %s\n", f.c_str(), e.what());
            rc = 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "szhatie: %s\n", e.what());
            rc = 1;
        }
    }
    return rc;
}
