#include "ota_transfer.hpp"
#include <cassert>
#include <limits>
#include <vector>

using namespace satori::ota;
struct Fake : Sink {
    unsigned begins{0}, writes{0}, aborts{0}, finishes{0}, selections{0};
    bool begin_ok{true}, write_ok{true}, select_ok{true};
    Verification verification{Verification::Verified};
    Slot selected{Slot::Other};
    std::vector<int> events;
    std::size_t count{0};
    bool Begin(const Manifest&) override { ++begins; events.push_back(1); return begin_ok; }
    bool Write(const std::uint8_t*, std::size_t n) override {
        ++writes; events.push_back(2); if (!write_ok) return false; count += n; return true;
    }
    void Abort() override { ++aborts; events.push_back(5); }
    Verification FinishVerifyAuthenticity() override { ++finishes; events.push_back(3); return verification; }
    bool SetBootTarget(Slot slot) override {
        ++selections; events.push_back(4); if (!select_ok) return false; selected = slot; return true;
    }
};
Manifest Good(std::uint32_t size = 4) { return {"satori_c3_v1", "esp32c3", Slot::Ota1, size, {}}; }
StartContext Idle() { return {true, true, Slot::Ota0, true}; }
const std::uint8_t bytes[] = {0, 1, 2, 3};
void AssertTerminal(Transfer& transfer, Fake& sink, Error error, unsigned aborts) {
    assert(transfer.state() == State::Failed && transfer.error() == error);
    const auto calls = sink.events.size();
    assert(!transfer.Write(bytes, 1, 0));
    assert(!transfer.Finalize(0));
    assert(!transfer.Start(Good(), Idle(), 0));
    transfer.Cancel(); transfer.Cancel();
    assert(sink.events.size() == calls && sink.aborts == aborts);
    assert(sink.selected == Slot::Other);
}
int main() {
    {
        Fake sink; Transfer transfer(sink);
        assert(transfer.Start(Good(), Idle(), 10));
        assert(!transfer.Start(Good(), Idle(), 10) && sink.begins == 1);
        assert(transfer.Write(bytes, 1, 11));
        assert(transfer.Write(nullptr, 0, 12));
        assert(transfer.Write(bytes + 1, 3, 13));
        assert(sink.selections == 0 && sink.finishes == 0);
        assert(transfer.Finalize(14));
        assert(transfer.state() == State::Committed && transfer.received() == 4);
        assert(sink.selected == Slot::Ota1 && sink.aborts == 0);
        assert((sink.events == std::vector<int>{1, 2, 2, 3, 4}));
        assert(!transfer.Finalize(15)); transfer.Cancel();
        assert(sink.selections == 1 && sink.aborts == 0);
    }
    for (unsigned bad = 0; bad < 12; ++bad) {
        Fake sink; Transfer transfer(sink); auto manifest = Good(); auto context = Idle();
        Error expected = Error::ManifestMismatch; unsigned timeout = 100;
        switch (bad) {
        case 0: context.explicit_request = false; expected = Error::RequestRequired; break;
        case 1: context.control_idle_or_stopped = false; expected = Error::ControlActive; break;
        case 2: context.running_slot_valid = false; expected = Error::RunningSlotInvalid; break;
        case 3: context.running_slot = Slot::Other; expected = Error::RunningSlotInvalid; break;
        case 4: manifest.board = "satori_s3_v1"; break;
        case 5: manifest.chip = "ESP32-S3"; break;
        case 6: manifest.target_slot = Slot::Ota0; expected = Error::TargetInvalid; break;
        case 7: manifest.target_slot = Slot::Other; expected = Error::TargetInvalid; break;
        case 8: manifest.image_size = 0; expected = Error::ImageSizeInvalid; break;
        case 9: manifest.image_size = kSlotSize + 1; expected = Error::ImageSizeInvalid; break;
        case 10: timeout = 0; expected = Error::TimeoutInvalid; break;
        case 11: timeout = 0x80000000u; expected = Error::TimeoutInvalid; break;
        }
        assert(!transfer.Start(manifest, context, 0, timeout));
        AssertTerminal(transfer, sink, expected, 0);
        assert(sink.begins == 0);
    }
    {
        Fake sink; Transfer transfer(sink); sink.begin_ok = false;
        assert(!transfer.Start(Good(), Idle(), 0));
        AssertTerminal(transfer, sink, Error::BeginFailed, 1);
    }
    {
        Fake sink; Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0));
        sink.write_ok = false; assert(!transfer.Write(bytes, 4, 1));
        AssertTerminal(transfer, sink, Error::WriteFailed, 1);
        assert(sink.finishes == 0 && sink.selections == 0 && transfer.received() == 0);
    }
    for (auto size : {std::size_t{5}, std::numeric_limits<std::size_t>::max()}) {
        Fake sink; Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0));
        assert(!transfer.Write(bytes, size, 1));
        AssertTerminal(transfer, sink, Error::Overlong, 1); assert(sink.writes == 0);
    }
    {
        Fake sink; Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0));
        assert(transfer.Write(bytes, 4, 1)); assert(!transfer.Write(bytes, 1, 2));
        AssertTerminal(transfer, sink, Error::Overlong, 1);
        assert(sink.writes == 1 && sink.finishes == 0 && sink.selections == 0);
    }
    {
        Fake sink; Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0));
        assert(!transfer.Write(nullptr, 1, 1)); AssertTerminal(transfer, sink, Error::InvalidData, 1);
    }
    for (unsigned count : {0u, 3u}) {
        Fake sink; Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0));
        assert(transfer.Write(bytes, count, 1)); assert(!transfer.Finalize(2));
        AssertTerminal(transfer, sink, Error::Truncated, 1); assert(sink.finishes == 0 && sink.selections == 0);
    }
    for (auto verify : {Verification::IntegrityFailure, Verification::AuthenticityFailure,
                        static_cast<Verification>(99)}) {
        Fake sink; Transfer transfer(sink); sink.verification = verify;
        assert(transfer.Start(Good(), Idle(), 0) && transfer.Write(bytes, 4, 1));
        assert(!transfer.Finalize(2));
        AssertTerminal(transfer, sink,
                       verify == Verification::IntegrityFailure ? Error::IntegrityFailed : Error::AuthenticityFailed, 1);
        assert(sink.finishes == 1 && sink.selections == 0);
    }
    {
        Fake sink; Transfer transfer(sink); sink.select_ok = false;
        assert(transfer.Start(Good(), Idle(), 0) && transfer.Write(bytes, 4, 1));
        assert(!transfer.Finalize(2)); AssertTerminal(transfer, sink, Error::BootSelectionFailed, 1);
        assert(sink.finishes == 1 && sink.selections == 1);
    }
    for (unsigned operation = 0; operation < 3; ++operation) {
        Fake sink; Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 100, 20));
        assert(transfer.Tick(119));
        if (operation == 0) assert(!transfer.Tick(120));
        if (operation == 1) assert(!transfer.Write(bytes, 4, 120));
        if (operation == 2) assert(!transfer.Finalize(120));
        AssertTerminal(transfer, sink, Error::Timeout, 1);
        assert(sink.writes == 0 && sink.finishes == 0 && sink.selections == 0);
    }
    {
        Fake sink; Transfer transfer(sink);
        assert(transfer.Start(Good(), Idle(), 0xfffffff0u, 32));
        assert(transfer.Tick(0x0fu)); assert(!transfer.Tick(0x10u));
        AssertTerminal(transfer, sink, Error::Timeout, 1);
    }
    for (bool start : {false, true}) {
        Fake sink;
        { Transfer transfer(sink);
          if (start) assert(transfer.Start(Good(), Idle(), 0));
          transfer.Cancel(); transfer.Cancel();
          assert(transfer.state() == State::Cancelled && transfer.error() == Error::Cancelled);
          assert(!transfer.Start(Good(), Idle(), 0) && !transfer.Finalize(1)); }
        assert(sink.aborts == (start ? 1u : 0u) && sink.selections == 0);
    }
    {
        Fake sink; { Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0)); }
        assert(sink.aborts == 1 && sink.selections == 0);
    }
    {
        // Verify that omitting platform authenticity implementation fails closed.
        struct DefaultVerifier : Fake {
            Verification FinishVerifyAuthenticity() override { return Sink::FinishVerifyAuthenticity(); }
        } sink;
        Transfer transfer(sink); assert(transfer.Start(Good(), Idle(), 0) && transfer.Write(bytes, 4, 1));
        assert(!transfer.Finalize(2)); AssertTerminal(transfer, sink, Error::AuthenticityFailed, 1);
        assert(sink.selections == 0);
    }
    {
        Fake sink; Transfer transfer(sink);
        auto manifest = Good(kSlotSize); manifest.target_slot = Slot::Ota0;
        auto context = Idle(); context.running_slot = Slot::Ota1;
        assert(transfer.Start(manifest, context, 0));
        std::vector<std::uint8_t> image(kSlotSize, 0xff);
        assert(transfer.Write(image.data(), image.size(), 1));
        assert(transfer.Finalize(2) && sink.selected == Slot::Ota0);
        assert(sink.count == kSlotSize && transfer.received() == kSlotSize);
    }
}
