/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <cstring>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_partition_param.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi station";

static int s_retry_num = 0;


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    std::string ssid =  EspPartitionParam::GetInstance().GetStringParam("ESP_WIFI_SSID", EXAMPLE_ESP_WIFI_SSID);
    std::string password =  EspPartitionParam::GetInstance().GetStringParam("ESP_WIFI_PASSWORD", EXAMPLE_ESP_WIFI_PASS);
    if (ssid.empty() || ssid.size() > sizeof(wifi_config.sta.ssid) ||
        password.size() > sizeof(wifi_config.sta.password) ||
        strlen(EXAMPLE_H2E_IDENTIFIER) >= sizeof(wifi_config.sta.sae_h2e_identifier)) {
        ESP_LOGE(TAG, "Wi-Fi configuration is missing or exceeds field limits");
        return;
    }
    memcpy(wifi_config.sta.ssid, ssid.data(), ssid.size());
    memcpy(wifi_config.sta.password, password.data(), password.size());
    wifi_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    wifi_config.sta.sae_pwe_h2e = ESP_WIFI_SAE_MODE;
    memcpy(wifi_config.sta.sae_h2e_identifier, EXAMPLE_H2E_IDENTIFIER, strlen(EXAMPLE_H2E_IDENTIFIER));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to configured access point");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to configured access point");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void connect_wifi(void)
{
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
}

#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
#include "connect_wifi.h"
#include <atomic>
#include "mbedtls/platform_util.h"
namespace {
std::atomic<bool> maintenance_wifi_started{false},maintenance_stopping{true},maintenance_ready{false},maintenance_failed{false};
std::atomic<std::uint32_t> maintenance_ip{0};
esp_netif_t* maintenance_sta=nullptr;
esp_event_handler_instance_t maintenance_wifi_handler=nullptr,maintenance_ip_handler=nullptr;
bool maintenance_wifi_initialized=false;
unsigned maintenance_retries=0;
void MaintenanceWifiEvent(void*,esp_event_base_t base,int32_t id,void* data) {
    if (maintenance_stopping.load())return;
    if (base==WIFI_EVENT&&id==WIFI_EVENT_STA_START) {
        if(esp_wifi_connect()!=ESP_OK)maintenance_failed=true;
    } else if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED) {
        const bool had_ip=maintenance_ready.exchange(false);maintenance_ip=0;
        if(had_ip||maintenance_retries++>=2){maintenance_failed=true;return;}
        if(esp_wifi_connect()!=ESP_OK)maintenance_failed=true;
    } else if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP) {
        const auto* event=static_cast<ip_event_got_ip_t*>(data);
        if(event->esp_netif!=maintenance_sta)return;
        maintenance_ip=event->ip_info.ip.addr;maintenance_ready=maintenance_ip.load()!=0;
    } else if(base==IP_EVENT&&id==IP_EVENT_STA_LOST_IP) {
        maintenance_ready=false;maintenance_ip=0;maintenance_failed=true;
    }
}
}
void StopMaintenanceSta() {
    maintenance_stopping=true;maintenance_ready=false;maintenance_ip=0;
    if(maintenance_wifi_handler){(void)esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,maintenance_wifi_handler);maintenance_wifi_handler=nullptr;}
    if(maintenance_ip_handler){(void)esp_event_handler_instance_unregister(IP_EVENT,ESP_EVENT_ANY_ID,maintenance_ip_handler);maintenance_ip_handler=nullptr;}
    if(maintenance_wifi_initialized){if(maintenance_wifi_started)(void)esp_wifi_stop();(void)esp_wifi_deinit();maintenance_wifi_initialized=false;}
    maintenance_wifi_started=false;
    if(maintenance_sta){esp_netif_destroy_default_wifi(maintenance_sta);maintenance_sta=nullptr;}
}
esp_err_t StartMaintenanceSta(const char* ssid,const char* password) {
    if(maintenance_wifi_initialized||maintenance_sta)return ESP_ERR_INVALID_STATE;
    const auto ssid_size=std::strlen(ssid),password_size=std::strlen(password);
    if(!ssid_size||ssid_size>32||password_size<8||password_size>63)return ESP_ERR_INVALID_ARG;
    maintenance_stopping=false;maintenance_failed=false;maintenance_ready=false;maintenance_retries=0;maintenance_ip=0;
    auto rc=esp_netif_init();if(rc!=ESP_OK&&rc!=ESP_ERR_INVALID_STATE){StopMaintenanceSta();return rc;}
    rc=esp_event_loop_create_default();if(rc!=ESP_OK&&rc!=ESP_ERR_INVALID_STATE){StopMaintenanceSta();return rc;}
    maintenance_sta=esp_netif_create_default_wifi_sta();if(!maintenance_sta){StopMaintenanceSta();return ESP_ERR_NO_MEM;}
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();init.nvs_enable=0;
    rc=esp_wifi_init(&init);if(rc!=ESP_OK){StopMaintenanceSta();return rc;}maintenance_wifi_initialized=true;
    if((rc=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,MaintenanceWifiEvent,nullptr,&maintenance_wifi_handler))!=ESP_OK||
       (rc=esp_event_handler_instance_register(IP_EVENT,ESP_EVENT_ANY_ID,MaintenanceWifiEvent,nullptr,&maintenance_ip_handler))!=ESP_OK){StopMaintenanceSta();return rc;}
    wifi_config_t config{};std::memcpy(config.sta.ssid,ssid,ssid_size);std::memcpy(config.sta.password,password,password_size);
    config.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;config.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
    rc=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(rc==ESP_OK)rc=esp_wifi_set_mode(WIFI_MODE_STA);
    if(rc==ESP_OK)rc=esp_wifi_set_config(WIFI_IF_STA,&config);
    mbedtls_platform_zeroize(&config,sizeof(config));
    if(rc==ESP_OK)rc=esp_wifi_start();
    if(rc!=ESP_OK){StopMaintenanceSta();return rc;}maintenance_wifi_started=true;return ESP_OK;
}
bool MaintenanceStaReady(std::array<unsigned char,4>& ip) {
    const auto address=maintenance_ip.load();
    if(!maintenance_ready||maintenance_failed||!address)return false;
    std::memcpy(ip.data(),&address,4);return true;
}
bool MaintenanceStaFailed(){return maintenance_failed.load();}
#endif
