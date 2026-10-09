#include "gatt_layout.hpp"
#include <cassert>
#include <vector>
#include <iostream>
using namespace satori::ble;
int main() {
    std::vector<int> calls;
    assert(RegisterStableGatt([&]{calls.push_back(1);}, [&]{calls.push_back(2);}, [&]{calls.push_back(3); return 7;}) == 7);
    assert((calls == std::vector<int>{1,2,3}));
    int hashes=0, changes=0, checks=0;
    auto hash=[&]{++hashes; return true;};
    auto change=[&](std::uint16_t first,std::uint16_t last){assert(first==1 && last==0xffff); ++changes;};
    auto healthy=[&]{++checks; return true;};
    assert(PrepareGattBoot({1,3,10},hash,change,healthy));
    assert(hashes==1 && changes==1 && checks==1);
    for (auto bad : {StableGattLayout{0,3,10},StableGattLayout{1,28,10},StableGattLayout{1,3,0},StableGattLayout{2,3,10}})
        assert(!PrepareGattBoot(bad,hash,change,healthy));
    assert(hashes==1 && changes==1 && checks==1);
    assert(!PrepareGattBoot({1,3,10},[]{return false;},change,healthy));
    assert(changes==1 && checks==1);
    assert(!PrepareGattBoot({1,3,10},hash,change,[]{return false;}));
    assert(changes==2);
    std::cout << "PASS: production registration policy, fixed layout, full range, hash failure, persistence failure (8 cases)\n";
}
