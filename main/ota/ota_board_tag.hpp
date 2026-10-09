#pragma once
#include <cstdint>
namespace satori::ota {
struct BoardTag { char magic[16];char board[16];char chip[16]; };
constexpr BoardTag kExpectedBoardTag{"SATORI_BOARD_V1","satori_c3_v1","esp32c3"};
static_assert(sizeof(BoardTag)==48);
// Pinned IDF linker places .rodata_custom_desc immediately after esp_app_desc.
constexpr std::uint32_t kBoardTagImageOffset=24+8+256;
} // namespace satori::ota
