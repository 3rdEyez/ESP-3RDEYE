#include "sdkconfig.h"
#include "wifi_ota_service.hpp"
#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
#include <atomic>
#include <cstring>
#include <cmath>
#include <new>
#include "ota_idf_sink.hpp"
#include "ota_http_policy.hpp"
#include "maintenance_lifetime.hpp"
#include "ble_server.h"
#include "connect_wifi.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

namespace satori::ota {
namespace {
std::atomic<bool> active{false},cancelled{false},restart_requested{false};
std::atomic<std::uint32_t> started_ms{0};
SessionCredentials credentials{};
std::array<char,33> lan_token{};
std::array<std::uint8_t,4> lan_ip{};
std::atomic<bool> lan_mode{false};
std::uint8_t lan_detail=0;
httpd_handle_t server{nullptr};
esp_netif_t* ap{nullptr};
constexpr std::uint32_t kSessionMs=kMaintenanceUploadMs;
std::atomic<bool> uploading{false};
std::atomic<std::uint32_t> upload_started_ms{0};
std::atomic<std::uint16_t> maintenance_peer{0xffff};
std::atomic<std::uint32_t> maintenance_connection_epoch{0};
std::uint32_t Now();
std::uint32_t Remaining(){
    const bool in_upload=uploading.load(std::memory_order_acquire);
    const auto base=in_upload?upload_started_ms.load(std::memory_order_acquire):started_ms.load(std::memory_order_acquire);
    const auto now=Now();
    return MaintenanceRemaining(now,base,in_upload,base);
}
portMUX_TYPE status_lock=portMUX_INITIALIZER_UNLOCKED;
WindowStatus window_status[2]{};
QueueHandle_t commands{nullptr};
SemaphoreHandle_t commit_lock{nullptr};
struct CommandWork { std::uint16_t peer;WindowCommand command;std::uint32_t connection_epoch;bool lan;LanCommand* configuration;std::array<std::uint8_t,32> fingerprint; };
void ClearCommand(CommandWork& work) {
    if(work.configuration){mbedtls_platform_zeroize(work.configuration,sizeof(LanCommand));delete work.configuration;}
    mbedtls_platform_zeroize(&work,sizeof(work));
}
struct WipeOnExit {
    void* bytes;std::size_t size;
    ~WipeOnExit(){mbedtls_platform_zeroize(bytes,size);}
};
LanReplayGuard lan_replay{};
WindowProtocol protocol[2]{}; // command worker only
void Status(WindowState state,WindowResult result=WindowResult::Ok) {
    portENTER_CRITICAL(&status_lock);window_status[lan_mode?1:0].state=state;window_status[lan_mode?1:0].result=result;portEXIT_CRITICAL(&status_lock);
}
WindowStatus Snapshot(bool lan) {
    portENTER_CRITICAL(&status_lock);const auto value=window_status[lan?1:0];portEXIT_CRITICAL(&status_lock);return value;
}
std::uint32_t Now() { return static_cast<std::uint32_t>(esp_timer_get_time()/1000); }
bool Alive() {
    return active&&!cancelled&&!restart_requested&&Remaining()>0&&maintenance_connection_epoch==BleOtaConnectionEpoch()&&BleOtaPeerStillAuthorized(maintenance_peer)&&
        BleOtaMaintenanceStopped()&&(!lan_mode||!MaintenanceStaFailed());
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
bool AuthorizedUpload(httpd_req_t* req) {
    if(!lan_mode)return true;
    char received[33]{};std::array<char,33> expected{};
    portENTER_CRITICAL(&status_lock);expected=lan_token;portEXIT_CRITICAL(&status_lock);
    const auto rc=httpd_req_get_hdr_value_str(req,"X-Satori-Window",received,sizeof(received));
    const bool ok=rc==ESP_OK&&WindowBearerMatches(std::string_view(expected.data(),32),received);
    mbedtls_platform_zeroize(received,sizeof(received));mbedtls_platform_zeroize(expected.data(),expected.size());return ok;
}
esp_err_t UploadBody(httpd_req_t* req) {
    if (!OfficialUpdateLayoutReady()) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Dual OTA layout required; run explicit USB tools/migrate_layout.py; unknown layouts are unsupported");
    if (!Alive()||!AuthorizedUpload(req)) return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Session not authorized");
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
    upload_started_ms.store(Now(),std::memory_order_relaxed);uploading.store(true,std::memory_order_release);Status(WindowState::Uploading);
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
    // Serialize close with the final verification/boot selection. A close that
    // loses to a successful commit receives Busy, never a false cancellation.
    xSemaphoreTake(commit_lock,portMAX_DELAY);
    const bool finalized=Alive()&&transfer.Finalize(Now());
    if (finalized) { restart_requested=true;Status(WindowState::Committed); }
    xSemaphoreGive(commit_lock);
    if (!finalized) {
        cancelled=true;Status(WindowState::Failed,WindowResult::Invalid);
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Image validation failed");
    }
    // The explicit request permits install+restart. The supervisor closes the
    // server/AP before reboot; an ACK lost here must never auto-retry upload.
    const auto result=httpd_resp_sendstr(req,"镜像已验签并写入，正在重启；请重新连接后确认固件版本与启动结果。");
    return result;
}
esp_err_t Upload(httpd_req_t* req) {
    httpd_resp_set_hdr(req,"Connection","close");
    return CloseHttpResponse(UploadBody(req));
}
void Teardown() {
    if (server) { (void)httpd_stop(server);server=nullptr; }
    if(lan_mode)StopMaintenanceSta();
    else {(void)esp_wifi_stop();(void)esp_wifi_deinit();}
    if (ap) { esp_netif_destroy_default_wifi(ap);ap=nullptr; }
    portENTER_CRITICAL(&status_lock);
    mbedtls_platform_zeroize(credentials.password,sizeof(credentials.password));
    credentials={};mbedtls_platform_zeroize(lan_token.data(),lan_token.size());lan_ip={};window_status[lan_mode?1:0].remaining_ms=0;
    portEXIT_CRITICAL(&status_lock);
    if (!restart_requested) EndBleOtaMaintenance();
    if (!restart_requested) Status(WindowState::Closed);
    uploading=false;maintenance_peer=0xffff;
    active=false; // release only after resources and this transport state are cleaned
    // Motion is not restored; normal control requires a new CLAIM and ARM.
}
void Supervisor(void*) {
    while (Alive()&&!restart_requested) vTaskDelay(pdMS_TO_TICKS(100));
    xSemaphoreTake(commit_lock,portMAX_DELAY);
    const bool restart=restart_requested.load();cancelled=true;
    xSemaphoreGive(commit_lock);
    if(lan_mode&&MaintenanceStaFailed()){portENTER_CRITICAL(&status_lock);lan_detail=4;portEXIT_CRITICAL(&status_lock);}
    Teardown();
    if (restart) { vTaskDelay(pdMS_TO_TICKS(100));esp_restart(); }
    vTaskDelete(nullptr);
}
esp_err_t Layout(httpd_req_t* req) {
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_type(req,"application/json");
    if (!Alive()) return CloseHttpResponse(httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Maintenance window closed"));
    const char* body=OfficialUpdateLayoutReady()?"{\"layout\":\"dual-ota\",\"update_allowed\":true}":"{\"layout\":\"unsupported\",\"update_allowed\":false}";
    return CloseHttpResponse(httpd_resp_send(req,body,HTTPD_RESP_USE_STRLEN));
}
esp_err_t Page(httpd_req_t* req) {
    httpd_resp_set_hdr(req,"Connection","close");
    if (!Alive()) return CloseHttpResponse(httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Maintenance window closed"));
    constexpr const char page[]=R"HTML(<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>觉瞳固件升级</title><style>body{font:18px sans-serif;max-width:40rem;margin:3rem auto;padding:1rem}button,input{font:inherit;margin:1rem 0}#status{white-space:pre-wrap}</style><h1>觉瞳固件升级</h1><p>维护窗口空闲十分钟后关闭；上传单独限时两分钟。升级期间运动已停止。电脑可直接上传签名包；LAN 模式需当前窗口授权码，AP 为显式备用。镜像仍须通过设备的验签与型号检查。</p><label>LAN 窗口授权码（仅本次使用；AP 模式留空）<input id="token" type="password" autocomplete="off"></label><br><input id="file" type="file" accept=".sota"><br><button id="install">安装并重启</button><p id="status" role="status"></p><script>
const layoutMessage='设备须先完成双槽迁移；未知布局或旧固件无法确认时，请使用 USB 工具检查，禁止自动迁移。';
const file=document.getElementById('file'),button=document.getElementById('install'),status=document.getElementById('status');
button.onclick=async()=>{button.disabled=true;try{const check=await fetch('/v1/ota/layout',{cache:'no-store'});let layout;try{layout=await check.json();}catch(e){throw Error(layoutMessage);}if(!check.ok||layout.layout!=='dual-ota'||layout.update_allowed!==true)throw Error(layoutMessage);const f=file.files[0];if(!f||f.size>1507328+1028)throw Error('请选择正确大小的 .sota 包');const h=new DataView(await f.slice(0,4).arrayBuffer());const n=h.getUint32(0,true);if(n<1||n>1024||f.size<=4+n)throw Error('包头错误');const m=JSON.parse(await f.slice(4,4+n).text());const image=f.slice(4+n);if(image.size!==m.image_length)throw Error('镜像长度不匹配');if(!confirm('安装 '+m.version+' 并重启？运动不会自动恢复。'))return;status.textContent='正在上传，请勿断电。';const r=await fetch('/v1/ota/image',{method:'POST',headers:{'X-Satori-Manifest':JSON.stringify(m),'X-Satori-Install':'confirm-restart','X-Satori-Window':document.getElementById('token').value},body:image});status.textContent=await r.text();if(!r.ok)throw Error('设备拒绝升级；请查看结果，不要自动重试。');}catch(e){status.textContent=String(e);}finally{button.disabled=false;}};
</script></html>)HTML";
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return CloseHttpResponse(httpd_resp_send(req,page,HTTPD_RESP_USE_STRLEN));
}
}
esp_err_t StartNetworkOta(std::uint16_t peer,SessionCredentials& output,std::uint32_t connection_epoch,LanCommand* lan) {
    // An unsigned running seed or disabled native policy fails closed
    // before motion gating, RNG credentials, Wi-Fi init or network listeners.
    if (connection_epoch!=BleOtaConnectionEpoch()) return ESP_ERR_INVALID_STATE;
    if (!OfficialUpdateLayoutReady()||!OfficialSignaturePolicyReady()) return ESP_ERR_NOT_SUPPORTED;
    bool expected=false;
    if (!active.compare_exchange_strong(expected,true)) return ESP_ERR_INVALID_STATE;
    lan_mode=lan!=nullptr;cancelled=false;restart_requested=false;uploading=false;started_ms=Now();maintenance_peer=peer;maintenance_connection_epoch=connection_epoch;
    portENTER_CRITICAL(&status_lock);
    auto& status=window_status[lan_mode?1:0];status.window_id=esp_random();if (!status.window_id) status.window_id=1;lan_detail=0;lan_ip={};
    portEXIT_CRITICAL(&status_lock);
    Status(WindowState::Opening);
    if (!BeginBleOtaMaintenance(peer)) { Status(WindowState::Failed,WindowResult::NotReady);active=false;return ESP_ERR_INVALID_STATE; }
    const auto stop_begin=Now();
    while (!BleOtaMaintenanceStopped()&&static_cast<std::uint32_t>(Now()-stop_begin)<1000)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!BleOtaMaintenanceStopped()||connection_epoch!=BleOtaConnectionEpoch()||!BleOtaPeerStillAuthorized(peer)) { Teardown();return ESP_ERR_INVALID_STATE; }
    esp_err_t rc=ESP_OK;
    if(lan) {
        std::array<std::uint8_t,16> random{};esp_fill_random(random.data(),random.size());
        auto token=LowerHex(random.data(),random.size());
        portENTER_CRITICAL(&status_lock);std::memcpy(lan_token.data(),token.data(),32);portEXIT_CRITICAL(&status_lock);
        mbedtls_platform_zeroize(token.data(),token.size());mbedtls_platform_zeroize(random.data(),random.size());
        rc=lan->use_saved_network?StartSavedMaintenanceSta():StartMaintenanceSta(lan->ssid.data(),lan->password.data());
        const WipeOnExit wipe_ssid{lan->ssid.data(),lan->ssid.size()};
        const WipeOnExit wipe_pass{lan->password.data(),lan->password.size()};
        if(rc!=ESP_OK){portENTER_CRITICAL(&status_lock);lan_detail=rc==ESP_ERR_NOT_FOUND?5:3;portEXIT_CRITICAL(&status_lock);Teardown();return rc;}
        const auto connect_start=Now();std::array<std::uint8_t,4> ip{};
        while(!MaintenanceStaReady(ip)&&!MaintenanceStaFailed()&&Alive()&&static_cast<std::uint32_t>(Now()-connect_start)<20000)
            vTaskDelay(pdMS_TO_TICKS(50));
        if(!MaintenanceStaReady(ip)||!Alive()) {
            portENTER_CRITICAL(&status_lock);lan_detail=MaintenanceStaFailed()?3:2;portEXIT_CRITICAL(&status_lock);
            Teardown();return ESP_ERR_TIMEOUT;
        }
        if(lan->remember_network&&(rc=SaveMaintenanceNetwork(lan->ssid.data(),lan->password.data()))!=ESP_OK){
            portENTER_CRITICAL(&status_lock);lan_detail=6;portEXIT_CRITICAL(&status_lock);Teardown();return rc;
        }
        portENTER_CRITICAL(&status_lock);lan_ip=ip;portEXIT_CRITICAL(&status_lock);
    } else {
    std::array<std::uint8_t,16> random{};
    esp_fill_random(random.data(),random.size());auto password=LowerHex(random.data(),random.size());
    const WipeOnExit wipe_password{password.data(),password.size()};
    esp_fill_random(random.data(),random.size());const auto bearer=LowerHex(random.data(),random.size());
    portENTER_CRITICAL(&status_lock);
    std::snprintf(credentials.ssid,sizeof(credentials.ssid),"SatoriEye-OTA-%.8s",bearer.c_str());
    std::memcpy(credentials.password,password.c_str(),password.size()+1);
    portEXIT_CRITICAL(&status_lock);
    rc=esp_netif_init();
    if (rc!=ESP_OK) { Teardown();return rc; }
    rc=esp_event_loop_create_default();
    if (rc!=ESP_OK&&rc!=ESP_ERR_INVALID_STATE) { Teardown();return rc; }
    ap=esp_netif_create_default_wifi_ap();
    if (!ap) { Teardown();return ESP_ERR_NO_MEM; }
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();init.nvs_enable=0;
    rc=esp_wifi_init(&init);
    if (rc!=ESP_OK) { Teardown();return rc; }
    wifi_config_t config{};const WipeOnExit wipe_config{&config,sizeof(config)};
    std::memcpy(config.ap.ssid,credentials.ssid,std::strlen(credentials.ssid));
    config.ap.ssid_len=std::strlen(credentials.ssid);
    std::memcpy(config.ap.password,credentials.password,std::strlen(credentials.password));
    config.ap.channel=1;config.ap.max_connection=1;config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    if ((rc=esp_wifi_set_storage(WIFI_STORAGE_RAM))!=ESP_OK||
        (rc=esp_wifi_set_mode(WIFI_MODE_AP))!=ESP_OK||
        (rc=esp_wifi_set_config(WIFI_IF_AP,&config))!=ESP_OK||
        (rc=esp_wifi_start())!=ESP_OK) { Teardown();return rc; }
    }
    httpd_config_t http=HTTPD_DEFAULT_CONFIG();http.stack_size=8192;http.max_open_sockets=SATORI_OTA_HTTP_MAX_OPEN_SOCKETS;
    http.recv_wait_timeout=3;http.send_wait_timeout=3;http.max_uri_handlers=3;http.lru_purge_enable=SATORI_OTA_HTTP_LRU_PURGE;
    if ((rc=httpd_start(&server,&http))!=ESP_OK) { Teardown();return rc; }
    const httpd_uri_t handler{.uri="/v1/ota/image",.method=HTTP_POST,.handler=Upload,.user_ctx=nullptr};
    if ((rc=httpd_register_uri_handler(server,&handler))!=ESP_OK) { Teardown();return rc; }
    const httpd_uri_t page{.uri="/",.method=HTTP_GET,.handler=Page,.user_ctx=nullptr};
    if ((rc=httpd_register_uri_handler(server,&page))!=ESP_OK) { Teardown();return rc; }
    const httpd_uri_t layout{.uri="/v1/ota/layout",.method=HTTP_GET,.handler=Layout,.user_ctx=nullptr};
    if ((rc=httpd_register_uri_handler(server,&layout))!=ESP_OK) { Teardown();return rc; }
    if(!Alive()){Teardown();return ESP_ERR_INVALID_STATE;}
    output=credentials;Status(WindowState::Open);
    if (xTaskCreate(Supervisor,"ota_lifetime",4096,nullptr,5,nullptr)!=pdPASS) { Teardown();return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
namespace {
void CommandWorker(void*) {
    CommandWork work{};
    for (;;) {
        if (xQueueReceive(commands,&work,portMAX_DELAY)!=pdTRUE) continue;
        if (work.connection_epoch!=BleOtaConnectionEpoch()||!BleOtaPeerStillAuthorized(work.peer)) {ClearCommand(work);continue;}
        // Close cannot report cancellation once a finalized image was selected.
        xSemaphoreTake(commit_lock,portMAX_DELAY);
        const auto before=Snapshot(work.lan);
        const auto admission=work.lan&&!lan_replay.Accept(work.command.request_id,work.fingerprint)?WindowAdmission{WindowResult::Invalid,false,false}:
            protocol[work.lan?1:0].Admit(work.command,active&&lan_mode!=work.lan?WindowState::Committed:before.state,before.window_id,OfficialUpdateLayoutReady()&&OfficialSignaturePolicyReady());
        portENTER_CRITICAL(&status_lock);
        window_status[work.lan?1:0].ack_request_id=work.command.request_id;window_status[work.lan?1:0].result=admission.result;
        portEXIT_CRITICAL(&status_lock);
        if (!admission.execute) { xSemaphoreGive(commit_lock);ClearCommand(work);continue; }
        if (work.command.action==2) {
            if (!active) { EndBleOtaMaintenance();portENTER_CRITICAL(&status_lock);window_status[work.lan?1:0].state=WindowState::Closed;portEXIT_CRITICAL(&status_lock);xSemaphoreGive(commit_lock);ClearCommand(work);continue; }
            cancelled=true;Status(WindowState::Closing);xSemaphoreGive(commit_lock);
            while (active) vTaskDelay(pdMS_TO_TICKS(10));
            ClearCommand(work);
            // Teardown publishes Closed only after AP and control-session cleanup.
            continue;
        }
        xSemaphoreGive(commit_lock);
        SessionCredentials ignored{};
        const bool requested_lan=work.lan;
        const auto rc=StartNetworkOta(work.peer,ignored,work.connection_epoch,work.configuration);
        ClearCommand(work);
        mbedtls_platform_zeroize(&ignored,sizeof(ignored));
        if (rc!=ESP_OK) {
            portENTER_CRITICAL(&status_lock);
            window_status[requested_lan?1:0].state=WindowState::Failed;
            window_status[requested_lan?1:0].result=rc==ESP_ERR_NOT_SUPPORTED?WindowResult::Unsupported:WindowResult::Internal;
            portEXIT_CRITICAL(&status_lock);
        }
    }
}
}
esp_err_t StartWifiOtaControlWorker() {
    if (commands) return ESP_OK;
    commit_lock=xSemaphoreCreateMutex();if (!commit_lock) return ESP_ERR_NO_MEM;
    commands=xQueueCreate(4,sizeof(CommandWork));
    if (!commands) { vSemaphoreDelete(commit_lock);commit_lock=nullptr;return ESP_ERR_NO_MEM; }
    if (xTaskCreate(CommandWorker,"ota_commands",6144,nullptr,5,nullptr)!=pdPASS) {
        vQueueDelete(commands);commands=nullptr;vSemaphoreDelete(commit_lock);commit_lock=nullptr;return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool SubmitWifiOtaCommand(std::uint16_t peer,const WindowCommand& command) {
    const CommandWork work{peer,command,BleOtaConnectionEpoch(),false,nullptr,{}};
    return commands&&xQueueSend(commands,&work,0)==pdTRUE;
}
std::size_t ReadWifiOtaStatus(std::uint8_t* output,std::size_t capacity) {
    if (!output) return 0;
    WindowStatus status{};SessionCredentials copy{};
    portENTER_CRITICAL(&status_lock);status=window_status[0];copy=credentials;portEXIT_CRITICAL(&status_lock);
    if (active&&!lan_mode&&!cancelled&&!restart_requested) {
        status.remaining_ms=Remaining();
    } else status.remaining_ms=0;
    if(active&&lan_mode){status.state=WindowState::Closed;status.result=WindowResult::Busy;status.remaining_ms=0;copy={};}
    else if (status.ack_request_id==0&&!OfficialSignaturePolicyReady()) status.result=WindowResult::Unsupported;
    if (status.state!=WindowState::Open&&status.state!=WindowState::Uploading) copy={};
    auto bytes=EncodeWindowStatus(status,copy.ssid,copy.password);
    const WipeOnExit wipe_encoded{bytes.data(),bytes.size()};
    mbedtls_platform_zeroize(&copy,sizeof(copy));
    if (bytes.size()>capacity) return 0;
    std::memcpy(output,bytes.data(),bytes.size());return bytes.size();
}
esp_err_t StartExplicitWifiOta(std::uint16_t peer,SessionCredentials& output,std::uint32_t epoch) {return StartNetworkOta(peer,output,epoch,nullptr);}
bool SubmitLanOtaCommand(std::uint16_t peer,const LanCommand& command) {
    auto* configuration=new(std::nothrow) LanCommand(command);if(!configuration)return false;
    CommandWork work{peer,command.window,BleOtaConnectionEpoch(),true,configuration,{}};
    std::array<std::uint8_t,107> canonical{};const auto wire=EncodeWindowCommand(command.window);
    std::memcpy(canonical.data(),wire.data(),wire.size());
    canonical[0]=command.use_saved_network?2:(command.remember_network?3:1);
    const auto ssid_size=std::strlen(command.ssid.data()),password_size=std::strlen(command.password.data());
    canonical[10]=ssid_size;canonical[11]=password_size;
    std::memcpy(canonical.data()+12,command.ssid.data(),ssid_size);std::memcpy(canonical.data()+12+ssid_size,command.password.data(),password_size);
    const auto digest_result=mbedtls_sha256(canonical.data(),12+ssid_size+password_size,work.fingerprint.data(),0);
    mbedtls_platform_zeroize(canonical.data(),canonical.size());
    if(digest_result!=0){ClearCommand(work);return false;}
    const bool accepted=commands&&xQueueSend(commands,&work,0)==pdTRUE;
    if(!accepted)ClearCommand(work);
    return accepted;
}
std::size_t ReadLanOtaStatus(std::uint8_t* output,std::size_t capacity) {
    WindowStatus status{};std::array<std::uint8_t,4> ip{};std::array<char,33> token{};std::uint8_t detail;
    portENTER_CRITICAL(&status_lock);status=window_status[1];ip=lan_ip;token=lan_token;detail=lan_detail;portEXIT_CRITICAL(&status_lock);
    const bool open=active&&lan_mode&&!cancelled&&!restart_requested;
    status.remaining_ms=open?Remaining():0;
    if(active&&!lan_mode){status.state=WindowState::Closed;status.result=WindowResult::Busy;status.remaining_ms=0;detail=0;ip={};token={};}
    else if(status.ack_request_id==0&&!OfficialSignaturePolicyReady())status.result=WindowResult::Unsupported;
    const bool expose=open&&(status.state==WindowState::Open||status.state==WindowState::Uploading);
    if(expose&&!MaintenanceStaReady(ip))ip={};
    auto bytes=EncodeLanStatus(status,detail,expose?ip:std::array<std::uint8_t,4>{},expose?std::string_view(token.data(),32):std::string_view{});
    mbedtls_platform_zeroize(token.data(),token.size());
    const auto size=bytes.size();
    if(!output||size>capacity){mbedtls_platform_zeroize(bytes.data(),size);return 0;}
    std::memcpy(output,bytes.data(),size);mbedtls_platform_zeroize(bytes.data(),size);return size;
}
void CancelWifiOta() { cancelled=true; }
bool WifiOtaSessionActive() { return active.load(); }
} // namespace satori::ota
#else
namespace satori::ota {
esp_err_t StartWifiOtaControlWorker() { return ESP_ERR_NOT_SUPPORTED; }
bool SubmitWifiOtaCommand(std::uint16_t,const WindowCommand&) { return false; }
bool SubmitLanOtaCommand(std::uint16_t,const LanCommand&) { return false; }
std::size_t ReadLanOtaStatus(std::uint8_t*,std::size_t) { return 0; }
std::size_t ReadWifiOtaStatus(std::uint8_t*,std::size_t) { return 0; }
esp_err_t StartExplicitWifiOta(std::uint16_t,SessionCredentials&,std::uint32_t) { return ESP_ERR_NOT_SUPPORTED; }
void CancelWifiOta() {}
bool WifiOtaSessionActive() { return false; }
}
#endif
