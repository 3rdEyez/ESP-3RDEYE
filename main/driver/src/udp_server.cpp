#include "udp_server.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_log.h"

#include "data_proc_obj.h"

static const char *TAG = "UDP_SERVER";

UdpServer::UdpServer(int port) : m_dataProc(new DataProcObj())
{
    int ip_protocol = 0;
    int addr_family = AF_INET;
    struct sockaddr_in6 dest_addr;
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(port);
    ip_protocol = IPPROTO_IP;
    m_sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (m_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    }
    ESP_LOGI(TAG, "Socket created");
    int err = bind(m_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    }
    ESP_LOGI(TAG, "Socket bound, port %d", port);
}

UdpServer::~UdpServer()
{
    ESP_LOGE(TAG, "Shutting down socket and restarting...");
    shutdown(m_sock, 0);
    close(m_sock);
}

bool UdpServer::SendBytes(const std::string& bytes, uint8_t ip[4], uint16_t port)
{
    struct sockaddr_in6 source_addr;
    struct sockaddr_in *source_addr_ip4 = (struct sockaddr_in *)&source_addr;
    source_addr_ip4->sin_addr.s_addr = htonl(ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3]);
    source_addr_ip4->sin_family = AF_INET;
    source_addr_ip4->sin_port = htons(port);
    int err = sendto(
        m_sock, bytes.data(), bytes.size(), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
        return false;
    }
    return true;
}

void UdpServer::StartListening()
{
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt (m_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    socklen_t socklen = sizeof(source_addr);
    char addr_str[128];
    m_running.store(true);

    while (m_running.load()) {
        int len = recvfrom(m_sock, m_rxBuff, sizeof(m_rxBuff) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        // Error occurred during receiving
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            continue;
        }
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
        m_rxBuff[std::min(RX_BUF_SIZE - 1, len)] = 0; // Null-terminate whatever we received and treat like a string...
        std::string message = m_dataProc->ProcessMessage(m_rxBuff);
        int err = sendto(m_sock, message.c_str(), message.length(), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            break;
        }
    }
}

void UdpServer::StopListening()
{
    m_running.store(false);
}
