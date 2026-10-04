#pragma once
#include "ota_window_protocol.hpp"
#include <array>
#include <cstring>
namespace satori::ota {
// Credentials are transient input, never a status payload or persistent config.
struct LanCommand {
    WindowCommand window{};
    bool use_saved_network{false};
    bool remember_network{false};
    std::array<char,33> ssid{};
    std::array<char,64> password{};
};
inline bool DecodeLanCommand(const std::uint8_t* b,std::size_t n,LanCommand& out) {
    if (!b||n<12||(b[0]!=1&&b[0]!=2&&b[0]!=3)||b[10]>32||b[11]>63||n!=12u+b[10]+b[11]) return false;
    WindowCommand window{b[1],WindowReadLe32(b+2),WindowReadLe32(b+6)};
    if (!ValidWindowCommand(window)) return false;
    // v2 is an explicit saved-network OPEN, never an empty v1 fallback.
    if (b[0]==2) {
        if(window.action!=1||n!=12||b[10]||b[11])return false;
        out={};out.window=window;out.use_saved_network=true;return true;
    }
    if(b[0]==3&&window.action!=1)return false;
    if (window.action==2) { if (n!=12) return false;out={};out.window=window;return true; }
    out={};
    if (!b[10]||b[11]<8) return false;
    // UTF-8 SSID is opaque bytes; NUL and control bytes are not accepted.
    for (std::size_t i=12;i<12u+b[10];++i) if (b[i]<0x20||b[i]==0x7f) return false;
    for (std::size_t i=12u+b[10];i<n;++i) if (b[i]<0x20||b[i]>0x7e) return false;
    out={};out.window=window;out.remember_network=b[0]==3;
    std::memcpy(out.ssid.data(),b+12,b[10]);std::memcpy(out.password.data(),b+12+b[10],b[11]);return true;
}
inline std::vector<std::uint8_t> EncodeLanStatus(const WindowStatus& s,std::uint8_t detail,
        const std::array<std::uint8_t,4>& ip,std::string_view token) {
    if (!WindowEnumsValid(s)||detail>6||(!token.empty()&&token.size()!=32)) return {};
    for (const auto c:token) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return {};
    if (!token.empty()&&(s.state!=WindowState::Open&&s.state!=WindowState::Uploading)) return {};
    std::vector<std::uint8_t> b(24+token.size(),0);b[0]=1;b[1]=static_cast<std::uint8_t>(s.state);
    b[2]=static_cast<std::uint8_t>(s.result);b[3]=detail;WindowWriteLe32(b.data()+4,s.ack_request_id);
    WindowWriteLe32(b.data()+8,s.window_id);WindowWriteLe32(b.data()+12,s.remaining_ms);
    for (unsigned i=0;i<4;++i)b[16+i]=ip[i];
    b[20]=token.size();
    std::memcpy(b.data()+24,token.data(),token.size());return b;
}
// Bounded RAM-only request fingerprints prevent a retry ID changing credentials.
// Digests are never exposed in status/logs or saved to NVS.
class LanReplayGuard {
public:
    bool Accept(std::uint32_t id,const std::array<std::uint8_t,32>& fingerprint) {
        for(const auto& entry:entries_)if(entry.occupied&&entry.id==id)return entry.fingerprint==fingerprint;
        entries_[next_]={id,fingerprint,true};next_=(next_+1)%entries_.size();return true;
    }
private:
    struct Entry{std::uint32_t id{0};std::array<std::uint8_t,32> fingerprint{};bool occupied{false};};
    std::array<Entry,8> entries_{};std::size_t next_{0};
};
inline bool WindowBearerMatches(std::string_view expected,std::string_view supplied) {
    if (expected.size()!=32||supplied.size()!=32)return false;
    unsigned mismatch=0;for(unsigned i=0;i<32;++i)mismatch|=static_cast<unsigned char>(expected[i])^static_cast<unsigned char>(supplied[i]);
    return mismatch==0;
}
} // namespace satori::ota
