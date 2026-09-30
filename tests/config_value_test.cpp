#include "config_value_parser.h"
#include <cassert>
#include <iostream>
int main() {
    int number = 123; float real = 42;
    assert(ParseConfigInt("-40", number) && number == -40);
    assert(!ParseConfigInt("500junk", number) && number == -40);
    assert(!ParseConfigInt("99999999999999999", number));
    assert(!ParseConfigInt("", number));
    assert(ParseConfigFloat("0.333333", real));
    for (const char* bad : {"", "nan", "inf", "-inf", "1e999", "0.5junk"}) assert(!ParseConfigFloat(bad, real));
    for (const char* good : {"", "   ", "# comment", " ; comment", "[DEFAULT]", "GPIO=7", " WIFI ="})
        assert(IsWellFormedConfigLine(good));
    for (const char* bad : {"garbage", "[broken", "[]", "[a[b]", "=value"})
        assert(!IsWellFormedConfigLine(bad));
    std::cout << "Nonthrowing board configuration and syntax validation: passed\n";
}
