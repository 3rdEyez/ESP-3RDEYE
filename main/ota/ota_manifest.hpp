#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include "ota_transfer.hpp"

namespace satori::ota {
struct ImageManifest {
    std::uint32_t image_size{0};
    std::array<std::uint8_t,32> sha256{};
    std::string version;
};
inline bool DecodeLowerHex(std::string_view text, std::uint8_t* output, std::size_t bytes) {
    if (text.size()!=bytes*2) return false;
    const auto nibble=[](char c)->int { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; };
    for (std::size_t i=0;i<bytes;++i) {
        const int a=nibble(text[i*2]),b=nibble(text[i*2+1]);
        if (a<0||b<0) return false;
        output[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    return true;
}
inline std::string LowerHex(const std::uint8_t* bytes,std::size_t size) {
    constexpr char digits[]="0123456789abcdef";
    std::string result(size*2,'0');
    for (std::size_t i=0;i<size;++i) { result[i*2]=digits[bytes[i]>>4];result[i*2+1]=digits[bytes[i]&15]; }
    return result;
}
inline bool ParseVersion(std::string_view text,std::array<unsigned,3>& parts) {
    std::size_t pos=0;
    for (unsigned field=0;field<3;++field) {
        const auto start=pos;unsigned value=0;
        while (pos<text.size()&&text[pos]>='0'&&text[pos]<='9') {
            value=value*10+static_cast<unsigned>(text[pos++]-'0');
            if (value>65535) return false;
        }
        if (pos==start||(pos-start>1&&text[start]=='0')) return false;
        parts[field]=value;
        if (field<2) { if (pos>=text.size()||text[pos++]!='.') return false; }
    }
    return pos==text.size();
}
inline bool ManifestValid(const ImageManifest& m) {
    std::array<unsigned,3> version{};
    return m.image_size>0&&m.image_size<=kSlotSize&&ParseVersion(m.version,version);
}
} // namespace satori::ota
