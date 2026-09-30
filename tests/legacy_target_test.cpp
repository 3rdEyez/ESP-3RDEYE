#include "legacy_target_parser.h"
#include <cassert>
#include <iostream>
int main() {
    std::array<int, 3> out{1000,1000,1000};
    assert(ParseLegacyTarget("CH1:500CH2:1500CH3:2500", out));
    const std::array<int,3> expected{500,1500,2500};
    assert(out == expected);
    const char* bad[] = {"", "CH1:1500", "CH1:1500CH2:1500", "CH1:499CH2:1500CH3:1500",
        "CH1:1500CH2:2501CH3:1500", "CH1:-1CH2:1500CH3:1500", "CH1:999999999999CH2:1500CH3:1500",
        "CH1:+500CH2:1500CH3:1500", "CH1: 500CH2:1500CH3:1500", "CH1:500CH2:1500CH3:2500junk",
        "CH1:500CH2:1500CH3:2500\n"};
    for (const auto* value: bad) { assert(!ParseLegacyTarget(value, out)); assert(out == expected); }
    std::cout << "Legacy target bounds and atomic parse: passed\n";
}
