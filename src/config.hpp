#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace t99 {
// RF defaults only. Output paths, overwrite and channel selection stay explicit.
inline std::vector<std::string> read_rf_config(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open RF config: " + path.string());
    const std::set<std::string> allowed{"device", "gain", "ppm", "freq-error", "sample-rate", "pfb"};
    std::set<std::string> seen;
    std::vector<std::string> args;
    auto trim = [](std::string s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string{} : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    };
    std::string line;
    unsigned number = 0;
    while (std::getline(stream, line)) {
        ++number;
        if (number == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3); // UTF-8 BOM
        if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;
        const auto equal = line.find('=');
        const auto key = trim(line.substr(0, equal));
        const auto value = equal == std::string::npos ? std::string{} : trim(line.substr(equal + 1));
        const auto where = "RF config line " + std::to_string(number) + ": ";
        if (equal == std::string::npos || value.empty()) throw std::invalid_argument(where + "expected key = value");
        if (!allowed.contains(key)) throw std::invalid_argument(where + "unknown setting " + key);
        if (!seen.insert(key).second) throw std::invalid_argument(where + "duplicate setting " + key);
        args.push_back("--" + key);
        args.push_back(value);
    }
    if (stream.bad()) throw std::runtime_error("Cannot read RF config: " + path.string());
    return args;
}
}
