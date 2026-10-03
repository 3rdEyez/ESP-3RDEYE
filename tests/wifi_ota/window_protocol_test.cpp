#include "ota_window_protocol.hpp"
#include "maintenance_stop.hpp"
#include <cassert>
#include <string>

using namespace satori::ota;
int main() {
    using satori::ble::MaintenanceStopAckMatches;
    assert(!MaintenanceStopAckMatches(false, 0, 0, 0));
    assert(!MaintenanceStopAckMatches(true, 0, 0, 0));
    assert(MaintenanceStopAckMatches(true, 1, 1, 1));
    assert(!MaintenanceStopAckMatches(true, 1, 0, 1));
    assert(!MaintenanceStopAckMatches(false, 2, 1, 2)); // closed after first halt
    assert(!MaintenanceStopAckMatches(true, 3, 1, 3)); // reopened, old ACK late
    assert(!MaintenanceStopAckMatches(true, 3, 2, 3)); // ACK from close transition
    assert(MaintenanceStopAckMatches(true, 3, 3, 3));
    assert(!MaintenanceStopAckMatches(true, 1, 1, 2)); // epoch changed during snapshot
    assert(!MaintenanceStopAckMatches(true, 1, 3, 3)); // close/reopen during snapshot
    assert(!MaintenanceStopAckMatches(true, 0xffffffffu, 0xffffffffu, 0));
    assert(!MaintenanceStopAckMatches(true, 1, 0xffffffffu, 1)); // old pre-wrap ACK
    const WindowCommand open{1, 0x78563412, 0};
    const auto encoded = EncodeWindowCommand(open);
    assert((encoded == std::array<std::uint8_t,10>{1,1,0x12,0x34,0x56,0x78,0,0,0,0}));
    WindowCommand decoded{};
    assert(DecodeWindowCommand(encoded.data(), encoded.size(), decoded));
    assert(decoded.action == 1 && decoded.request_id == open.request_id && decoded.window_id == 0);
    const WindowCommand close{2, 9, 0x12345678};
    const auto close_bytes = EncodeWindowCommand(close);
    assert(DecodeWindowCommand(close_bytes.data(), close_bytes.size(), decoded));
    assert(decoded.action == 2 && decoded.window_id == close.window_id);
    assert(!DecodeWindowCommand(nullptr, 10, decoded));
    assert(!DecodeWindowCommand(encoded.data(), 9, decoded));
    assert(!DecodeWindowCommand(encoded.data(), 11, decoded));
    for (unsigned bad = 0; bad < 5; ++bad) {
        auto bytes = encoded;
        if (bad == 0) bytes[0] = 2;
        if (bad == 1) bytes[1] = 3;
        if (bad == 2) for (unsigned i=2;i<6;++i) bytes[i] = 0;
        if (bad == 3) bytes[6] = 1;
        if (bad == 4) bytes[1] = 2;
        assert(!DecodeWindowCommand(bytes.data(), bytes.size(), decoded));
    }
    WindowStatus status{WindowState::Open, WindowResult::Ok, 0x78563412, 0x12345678, 1000};
    auto bytes = EncodeWindowStatus(status, "AP", "secret");
    assert((bytes == std::vector<std::uint8_t>{1,2,0,0,0x12,0x34,0x56,0x78,0x78,0x56,0x34,0x12,
                                              0xe8,3,0,0,2,6,'A','P','s','e','c','r','e','t'}));
    WindowStatus recovered{}; std::string_view ssid, password;
    assert(DecodeWindowStatus(bytes.data(), bytes.size(), recovered, ssid, password));
    assert(recovered.state == WindowState::Open && recovered.window_id == status.window_id);
    assert(recovered.ack_request_id == status.ack_request_id && recovered.remaining_ms == 1000);
    assert(ssid == "AP" && password == "secret");
    const auto empty = EncodeWindowStatus({}, {}, {});
    assert(empty.size() == 18 && DecodeWindowStatus(empty.data(), empty.size(), recovered, ssid, password));
    const auto maximum = EncodeWindowStatus(status, std::string(32,'s'), std::string(64,'p'));
    assert(maximum.size() == 114 && DecodeWindowStatus(maximum.data(), maximum.size(), recovered, ssid, password));
    assert(EncodeWindowStatus(status, std::string(33,'s'), {}).empty());
    assert(EncodeWindowStatus(status, {}, std::string(65,'p')).empty());
    assert(EncodeWindowStatus(status, "line\n", {}).empty());
    assert(EncodeWindowStatus(status, {}, std::string_view("x\0y",3)).empty());
    assert(EncodeWindowStatus(status, "\x7f", {}).empty());
    assert(EncodeWindowStatus(status, "\xc3\xa9", {}).empty());
    for (unsigned bad = 0; bad < 9; ++bad) {
        auto malformed = bytes;
        if (bad == 0) malformed[0] = 2;
        if (bad == 1) malformed[1] = 7;
        if (bad == 2) malformed[2] = 7;
        if (bad == 3) malformed[3] = 1;
        if (bad == 4) malformed[16] = 33;
        if (bad == 5) malformed[17] = 65;
        if (bad == 6) malformed.pop_back();
        if (bad == 7) malformed.push_back(0);
        if (bad == 8) malformed[18] = 0;
        assert(!DecodeWindowStatus(malformed.data(), malformed.size(), recovered, ssid, password));
    }
    assert(AdmitWindowCommand(open, WindowState::Closed, 0, true) == WindowResult::Ok);
    assert(AdmitWindowCommand(open, WindowState::Failed, 42, true) == WindowResult::Ok);
    assert(AdmitWindowCommand(open, WindowState::Closed, 42, false) == WindowResult::Unsupported);
    for (auto state : {WindowState::Opening, WindowState::Open, WindowState::Uploading,
                       WindowState::Closing, WindowState::Committed}) {
        assert(AdmitWindowCommand(open, state, 42, true) == WindowResult::Busy);
        assert(AdmitWindowCommand(open, state, 42, false) == WindowResult::Busy);
    }
    assert(AdmitWindowCommand(close, WindowState::Open, 7, true) == WindowResult::StaleWindow);
    assert(AdmitWindowCommand(close, WindowState::Committed, close.window_id, true) == WindowResult::Busy);
    assert(AdmitWindowCommand(close, WindowState::Closed, close.window_id, false) == WindowResult::Ok);
    assert(AdmitWindowCommand({}, WindowState::Closed, 0, true) == WindowResult::Invalid);
    assert(AdmitWindowCommand(open, static_cast<WindowState>(99), 0, true) == WindowResult::Invalid);
    WindowProtocol protocol;
    auto admission = protocol.Admit(open, WindowState::Closed, 0, true);
    assert(admission.result == WindowResult::Ok && admission.execute && !admission.duplicate);
    admission = protocol.Admit(open, WindowState::Open, 42, false);
    assert(admission.result == WindowResult::Ok && !admission.execute && admission.duplicate);
    admission = protocol.Admit({2, open.request_id, 42}, WindowState::Open, 42, true);
    assert(admission.result == WindowResult::Invalid && !admission.execute && !admission.duplicate);
    admission = protocol.Admit(open, WindowState::Open, 42, true);
    assert(admission.result == WindowResult::Ok && admission.duplicate); // conflict did not replace cache
    admission = protocol.Admit(close, WindowState::Open, close.window_id, true);
    assert(admission.result == WindowResult::Ok && admission.execute);
    admission = protocol.Admit(close, WindowState::Closed, close.window_id, true);
    assert(admission.result == WindowResult::Ok && !admission.execute && admission.duplicate);
    admission = protocol.Admit({2,10,close.window_id}, WindowState::Closed, close.window_id, true);
    assert(admission.result == WindowResult::Ok && !admission.execute && !admission.duplicate);
    admission = protocol.Admit({2,11,close.window_id}, WindowState::Closing, close.window_id, true);
    assert(admission.result == WindowResult::Ok && !admission.execute);
    admission = protocol.Admit({1,12,0}, WindowState::Open, close.window_id, true);
    assert(admission.result == WindowResult::Busy && !admission.execute);
    admission = protocol.Admit({1,12,0}, WindowState::Closed, close.window_id, true);
    assert(admission.result == WindowResult::Busy && admission.duplicate && !admission.execute);
    WindowProtocol interleaved;
    assert(interleaved.Admit({1,1,0}, WindowState::Closed, 0, true).execute);
    assert(interleaved.Admit({2,2,42}, WindowState::Open, 42, true).execute);
    admission = interleaved.Admit({1,1,0}, WindowState::Closed, 42, true);
    assert(admission.result == WindowResult::Ok && admission.duplicate && !admission.execute);
    admission = interleaved.Admit({2,1,42}, WindowState::Closed, 42, true);
    assert(admission.result == WindowResult::Invalid && !admission.duplicate && !admission.execute);
    // Fill all eight entries; retry/conflict/invalid input never evicts entries.
    for (unsigned id = 3; id <= 8; ++id)
        assert(!interleaved.Admit({2,id,42}, WindowState::Closed, 42, true).execute);
    admission = interleaved.Admit({1,1,0}, WindowState::Closed, 42, true);
    assert(admission.duplicate && !admission.execute);
    assert(interleaved.Admit({}, WindowState::Closed, 42, true).result == WindowResult::Invalid);
    assert(interleaved.Admit({2,2,43}, WindowState::Closed, 42, true).result == WindowResult::Invalid);
    assert(interleaved.Admit({1,1,0}, WindowState::Closed, 42, true).duplicate);
    interleaved.Admit({2,9,42}, WindowState::Closed, 42, true); // bounded cache evicts oldest id=1
    admission = interleaved.Admit({1,1,0}, WindowState::Closed, 42, true);
    assert(admission.result == WindowResult::Ok && !admission.duplicate && admission.execute);
}
