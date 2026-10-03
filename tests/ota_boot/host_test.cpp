#include "boot_health.hpp"
#include <cassert>
#include <initializer_list>
int main() {
    using namespace satori::ble;
    assert(BootImageCanValidate(static_cast<unsigned>(BootImageState::Valid)));
    assert(BootImageCanValidate(static_cast<unsigned>(BootImageState::PendingVerify)));
    for (const auto state : {BootImageState::New, BootImageState::Invalid,
                             BootImageState::Aborted, BootImageState::Undefined}) {
        assert(!BootImageCanValidate(static_cast<unsigned>(state)));
    }
    assert(!BootImageCanValidate(42));
    BootHealthWindow healthy;
    for (unsigned i=0; i<4; ++i) assert(healthy.Observe(i*100,true,true)==BootDecision::Wait);
    assert(healthy.Observe(400,true,true)==BootDecision::Confirm);
    BootHealthWindow interrupted;
    for (unsigned i=0; i<4; ++i) assert(interrupted.Observe(i*100,true,true)==BootDecision::Wait);
    assert(interrupted.Observe(400,false,true)==BootDecision::Wait);
    for (unsigned i=0; i<4; ++i) assert(interrupted.Observe(500+i*100,true,true)==BootDecision::Wait);
    assert(interrupted.Observe(900,true,true)==BootDecision::Confirm);
    BootHealthWindow stalled;
    for (unsigned i=0; i<150; ++i) assert(stalled.Observe(i*100,true,false)==BootDecision::Wait);
    assert(stalled.Observe(15000,true,true)==BootDecision::Fail);
    BootHealthWindow late;
    for (unsigned i=0; i<4; ++i) assert(late.Observe(14600+i*100,true,true)==BootDecision::Wait);
    assert(late.Observe(15000,true,true)==BootDecision::Fail);
    BootHealthWindow lost_tick;
    for (unsigned i=0; i<4; ++i) assert(lost_tick.Observe(i*100,true,true)==BootDecision::Wait);
    assert(lost_tick.Observe(400,true,false)==BootDecision::Wait);
    for (unsigned i=0; i<4; ++i) assert(lost_tick.Observe(500+i*100,true,true)==BootDecision::Wait);
    assert(lost_tick.Observe(900,true,true)==BootDecision::Confirm);
}
