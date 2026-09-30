#pragma once
#include <charconv>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>

inline std::string_view TrimConfigView(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline bool IsWellFormedConfigLine(std::string_view line) {
    line = TrimConfigView(line);
    if (line.empty() || line.front() == '#' || line.front() == ';') return true;
    if (line.front() == '[') {
        if (line.size() < 3 || line.back() != ']') return false;
        const auto name = TrimConfigView(line.substr(1, line.size() - 2));
        return !name.empty() && name.find_first_of("[]") == std::string_view::npos;
    }
    const auto separator = line.find('=');
    return separator != std::string_view::npos &&
           !TrimConfigView(line.substr(0, separator)).empty();
}

inline bool ParseConfigInt(std::string_view value, int& result) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return false;
    value.remove_prefix(first);
    value = value.substr(0, value.find_last_not_of(" \t\r\n") + 1);
    int parsed{};
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()) return false;
    result = parsed;
    return true;
}
inline bool ParseConfigFloat(const std::string& value, float& result) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtof(value.c_str(), &end);
    if (errno == ERANGE || end != value.c_str() + value.size() || !std::isfinite(parsed)) return false;
    result = parsed;
    return true;
}
