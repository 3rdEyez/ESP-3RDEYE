#include "sdkconfig.h"
#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
#include "ota_board_tag.hpp"
extern "C" const satori::ota::BoardTag satori_ota_board_tag __attribute__((section(".rodata_custom_desc"),used))=satori::ota::kExpectedBoardTag;
#endif
