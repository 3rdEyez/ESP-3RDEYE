#pragma once
#include <array>
#include <charconv>
#include <string_view>

// Validate the complete request before any channel reaches the actuator.
inline bool ParseLegacyTarget(std::string_view input, std::array<int, 3>& output) {
    constexpr std::string_view labels[] = {"CH1:", "CH2:", "CH3:"};
    std::array<int, 3> parsed{};
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto label = labels[i];
        if (input.substr(0, label.size()) != label) return false;
        input.remove_prefix(label.size());
        std::size_t digits = 0;
        while (digits < input.size() && input[digits] >= '0' && input[digits] <= '9') ++digits;
        if (digits == 0 || digits > 4) return false;
        const auto result = std::from_chars(input.data(), input.data() + digits, parsed[i]);
        if (result.ec != std::errc{} || parsed[i] < 500 || parsed[i] > 2500) return false;
        input.remove_prefix(digits);
    }
    if (!input.empty()) return false;
    output = parsed;
    return true;
}
