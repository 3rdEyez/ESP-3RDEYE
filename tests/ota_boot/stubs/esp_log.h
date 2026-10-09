#pragma once
template<class... Args> inline void MockLog(const char*, const char*, Args...) {}
#define ESP_LOGE(...) MockLog(__VA_ARGS__)
#define ESP_LOGI(...) MockLog(__VA_ARGS__)
