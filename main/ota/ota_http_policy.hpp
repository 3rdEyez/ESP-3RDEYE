#pragma once
// Only synchronous URI handlers: HTTPD cannot admit/evict a connection while
// the upload handler is receiving, writing or verifying. Revisit for async OTA.
#define SATORI_OTA_HTTP_MAX_OPEN_SOCKETS 1
#define SATORI_OTA_HTTP_LRU_PURGE true
namespace satori::ota {
// After fully sending a response, ESP_FAIL tells IDF to close this session.
inline int CloseHttpResponse(int result) { return result==0?-1:result; }
}
