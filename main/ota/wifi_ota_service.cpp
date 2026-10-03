#include "sdkconfig.h"
#include "wifi_ota_service.hpp"
#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
#include <atomic>
#include <cstring>
#include <cmath>
#include <new>
#include "ota_idf_sink.hpp"
#include "ble_server.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "mbedtls/platform_util.h"

namespace satori::ota {
namespace {
std::atomic<bool> active{false},cancelled{false},restart_requested{false};
std::uint32_t started_ms{0};
SessionCredentials credentials{};
httpd_handle_t server{nullptr};
esp_netif_t* ap{nullptr};
constexpr std::uint32_t kSessionMs=120000;
std::uint32_t Now() { return static_cast<std::uint32_t>(esp_timer_get_time()/1000); }
bool Alive() {
    return active&&!cancelled&&!restart_requested&&static_cast<std::uint32_t>(Now()-started_ms)<kSessionMs&&
        BleOtaMaintenanceStopped();
}
bool ExactFields(const cJSON* object,std::initializer_list<const char*> fields) {
    if (!cJSON_IsObject(object)||cJSON_GetArraySize(object)!=static_cast<int>(fields.size())) return false;
    for (const auto* field:fields) {
        unsigned count=0;
        for (const auto* item=object->child;item;item=item->next) if (item->string&&std::strcmp(item->string,field)==0) ++count;
        if (count!=1) return false;
    }
    return true;
}
const char* Text(const cJSON* obj,const char* name) {
    const auto* item=cJSON_GetObjectItemCaseSensitive(obj,name);
    return cJSON_IsString(item)?item->valuestring:nullptr;
}
bool ReadManifest(httpd_req_t* req,ImageManifest& manifest) {
    const auto length=httpd_req_get_hdr_value_len(req,"X-Satori-Manifest");
    if (length==0||length>1024) return false;
    char json[1025]{};
    if (httpd_req_get_hdr_value_str(req,"X-Satori-Manifest",json,sizeof(json))!=ESP_OK||std::strchr(json,'\\')) return false;
    const char* end=nullptr;
    auto* obj=cJSON_ParseWithOpts(json,&end,true);
    if (!obj) return false;
    const auto* schema=cJSON_GetObjectItemCaseSensitive(obj,"schema");
    const auto* size=cJSON_GetObjectItemCaseSensitive(obj,"image_length");
    const auto* board=Text(obj,"board");const auto* chip=Text(obj,"chip");
    const auto* version=Text(obj,"version");const auto* sha=Text(obj,"sha256");
    const auto* format=Text(obj,"image_format");
    bool ok=ExactFields(obj,{"schema","board","chip","image_length","sha256","version","image_format"})&&cJSON_IsNumber(schema)&&schema->valuedouble==1&&
        cJSON_IsNumber(size)&&std::isfinite(size->valuedouble)&&size->valuedouble>=1&&
        size->valuedouble<=kSlotSize&&std::floor(size->valuedouble)==size->valuedouble&&
        board&&std::strcmp(board,"satori_c3_v1")==0&&chip&&std::strcmp(chip,"esp32c3")==0&&
        version&&sha&&format&&std::strcmp(format,"esp-idf-sbv2-rsa3072")==0;
    if (ok) {
        manifest.image_size=static_cast<std::uint32_t>(size->valuedouble);manifest.version=version;
        ok=DecodeLowerHex(sha,manifest.sha256.data(),32)&&ManifestValid(manifest);
    }
    cJSON_Delete(obj);return ok;
}
esp_err_t Upload(httpd_req_t* req) {
    if (!Alive()) return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Session not authorized");
    ImageManifest manifest;
    if (!ReadManifest(req,manifest)||req->content_len!=manifest.image_size||
        httpd_req_get_hdr_value_len(req,"Transfer-Encoding")!=0) {
        cancelled=true;return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Package rejected");
    }
    char confirmation[16]{};
    if (httpd_req_get_hdr_value_str(req,"X-Satori-Install",confirmation,sizeof(confirmation))!=ESP_OK||
        std::strcmp(confirmation,"confirm-restart")!=0) {
        cancelled=true;return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Explicit install confirmation required");
    }
    IdfOtaSink sink(manifest,Alive);Transfer transfer(sink);
    const Manifest core{"satori_c3_v1","esp32c3",sink.update_slot(),manifest.image_size,manifest.sha256};
    if (!transfer.Start(core,{true,BleOtaMaintenanceStopped(),sink.running_slot(),sink.running_valid()},Now(),kSessionMs)) {
        cancelled=true;return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Signature/image/slot policy rejected");
    }
    std::array<std::uint8_t,2048> buffer{};
    std::size_t remaining=manifest.image_size;
    while (remaining) {
        if (!Alive()) { transfer.Cancel();cancelled=true;return ESP_FAIL; }
        const auto count=httpd_req_recv(req,reinterpret_cast<char*>(buffer.data()),
            remaining<buffer.size()?remaining:buffer.size());
        if (count<=0||!transfer.Write(buffer.data(),static_cast<std::size_t>(count),Now())) {
            transfer.Cancel();cancelled=true;return ESP_FAIL; // no timeout retry
        }
        remaining-=static_cast<std::size_t>(count);
    }
    if (!Alive()||!transfer.Finalize(Now())) {
        cancelled=true;return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Image validation failed");
    }
    // The explicit request permits install+restart. The supervisor closes the
    // server/AP before reboot; an ACK lost here must never auto-retry upload.
    const auto result=httpd_resp_sendstr(req,"Installed; restarting for startup validation");
    restart_requested=true;return result;
}
void Teardown() {
    if (server) { (void)httpd_stop(server);server=nullptr; }
    (void)esp_wifi_stop();(void)esp_wifi_deinit();
    if (ap) { esp_netif_destroy_default_wifi(ap);ap=nullptr; }
    mbedtls_platform_zeroize(credentials.password,sizeof(credentials.password));
    credentials={};active=false;
    // BLE maintenance remains latched; no lease/queued target is restored.
}
void Supervisor(void*) {
    while (Alive()&&!restart_requested) vTaskDelay(pdMS_TO_TICKS(100));
    const bool restart=restart_requested.load();cancelled=true;Teardown();
    if (restart) { vTaskDelay(pdMS_TO_TICKS(100));esp_restart(); }
    vTaskDelete(nullptr);
}
esp_err_t Page(httpd_req_t* req) {
    if (!Alive()) return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Maintenance window closed");
    constexpr const char page[]=R"HTML(<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>觉瞳固件升级</title><style>body{font:18px sans-serif;max-width:40rem;margin:3rem auto;padding:1rem}button,input{font:inherit;margin:1rem 0}#status{white-space:pre-wrap}</style><h1>觉瞳固件升级</h1><p>维护窗口限时两分钟。升级期间运动已停止。上传设备无需 BLE 配对；镜像仍须通过设备的验签与型号检查。</p><input id="file" type="file" accept=".sota"><br><button id="install">安装并重启</button><p id="status" role="status"></p><script>
const file=document.getElementById('file'),button=document.getElementById('install'),status=document.getElementById('status');
button.onclick=async()=>{button.disabled=true;try{const f=file.files[0];if(!f||f.size>1507328+1028)throw Error('请选择正确大小的 .sota 包');const h=new DataView(await f.slice(0,4).arrayBuffer());const n=h.getUint32(0,true);if(n<1||n>1024||f.size<=4+n)throw Error('包头错误');const m=JSON.parse(await f.slice(4,4+n).text());const image=f.slice(4+n);if(image.size!==m.image_length)throw Error('镜像长度不匹配');if(!confirm('安装 '+m.version+' 并重启？运动不会自动恢复。'))return;status.textContent='正在上传，请勿断电。';const r=await fetch('/v1/ota/image',{method:'POST',headers:{'X-Satori-Manifest':JSON.stringify(m),'X-Satori-Install':'confirm-restart'},body:image});status.textContent=await r.text();if(!r.ok)throw Error('设备拒绝升级；请查看结果，不要自动重试。');}catch(e){status.textContent=String(e);}finally{button.disabled=false;}};
</script></html>)HTML";
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_send(req,page,HTTPD_RESP_USE_STRLEN);
}
}
esp_err_t StartExplicitWifiOta(std::uint16_t peer,SessionCredentials& output) {
    // An unsigned running seed or disabled native policy fails closed
    // before motion gating, RNG credentials, Wi-Fi init or network listeners.
    if (!OfficialSignaturePolicyReady()) return ESP_ERR_NOT_SUPPORTED;
    bool expected=false;
    if (!active.compare_exchange_strong(expected,true)) return ESP_ERR_INVALID_STATE;
    cancelled=false;restart_requested=false;started_ms=Now();
    if (!BeginBleOtaMaintenance(peer)) { active=false;return ESP_ERR_INVALID_STATE; }
    const auto stop_begin=Now();
    while (!BleOtaMaintenanceStopped()&&static_cast<std::uint32_t>(Now()-stop_begin)<1000)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!BleOtaMaintenanceStopped()||!BleOtaPeerStillAuthorized(peer)) { active=false;return ESP_ERR_INVALID_STATE; }
    std::array<std::uint8_t,16> random{};
    esp_fill_random(random.data(),random.size());const auto password=LowerHex(random.data(),random.size());
    esp_fill_random(random.data(),random.size());const auto bearer=LowerHex(random.data(),random.size());
    std::snprintf(credentials.ssid,sizeof(credentials.ssid),"SatoriEye-OTA-%.8s",bearer.c_str());
    std::memcpy(credentials.password,password.c_str(),password.size()+1);
    auto rc=esp_netif_init();
    if (rc!=ESP_OK) { Teardown();return rc; }
    rc=esp_event_loop_create_default();
    if (rc!=ESP_OK&&rc!=ESP_ERR_INVALID_STATE) { Teardown();return rc; }
    ap=esp_netif_create_default_wifi_ap();
    if (!ap) { Teardown();return ESP_ERR_NO_MEM; }
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();init.nvs_enable=0;
    rc=esp_wifi_init(&init);
    if (rc!=ESP_OK) { Teardown();return rc; }
    wifi_config_t config{};
    std::memcpy(config.ap.ssid,credentials.ssid,std::strlen(credentials.ssid));
    config.ap.ssid_len=std::strlen(credentials.ssid);
    std::memcpy(config.ap.password,credentials.password,std::strlen(credentials.password));
    config.ap.channel=1;config.ap.max_connection=1;config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    if ((rc=esp_wifi_set_storage(WIFI_STORAGE_RAM))!=ESP_OK||
        (rc=esp_wifi_set_mode(WIFI_MODE_AP))!=ESP_OK||
        (rc=esp_wifi_set_config(WIFI_IF_AP,&config))!=ESP_OK||
        (rc=esp_wifi_start())!=ESP_OK) { Teardown();return rc; }
    httpd_config_t http=HTTPD_DEFAULT_CONFIG();http.stack_size=8192;http.max_open_sockets=1;
    http.recv_wait_timeout=3;http.send_wait_timeout=3;http.max_uri_handlers=2;http.lru_purge_enable=false;
    if ((rc=httpd_start(&server,&http))!=ESP_OK) { Teardown();return rc; }
    const httpd_uri_t handler{.uri="/v1/ota/image",.method=HTTP_POST,.handler=Upload,.user_ctx=nullptr};
    if ((rc=httpd_register_uri_handler(server,&handler))!=ESP_OK) { Teardown();return rc; }
    const httpd_uri_t page{.uri="/",.method=HTTP_GET,.handler=Page,.user_ctx=nullptr};
    if ((rc=httpd_register_uri_handler(server,&page))!=ESP_OK) { Teardown();return rc; }
    if (xTaskCreate(Supervisor,"ota_lifetime",4096,nullptr,5,nullptr)!=pdPASS) { Teardown();return ESP_ERR_NO_MEM; }
    output=credentials;return ESP_OK;
}
void CancelWifiOta() { cancelled=true; }
bool WifiOtaSessionActive() { return active.load(); }
} // namespace satori::ota
// Link-only candidate anchor; no runtime caller/registration is installed.
extern "C" esp_err_t satori_ota_prototype_link_anchor(std::uint16_t peer,satori::ota::SessionCredentials* output) {
    return output?satori::ota::StartExplicitWifiOta(peer,*output):ESP_ERR_INVALID_ARG;
}
#else
namespace satori::ota {
esp_err_t StartExplicitWifiOta(std::uint16_t,SessionCredentials&) { return ESP_ERR_NOT_SUPPORTED; }
void CancelWifiOta() {}
bool WifiOtaSessionActive() { return false; }
}
#endif
