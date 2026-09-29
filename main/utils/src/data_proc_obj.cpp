#include "data_proc_obj.h"
#include "servo_group.h"
#include "legacy_target_parser.h"
#include <cstring>
#include "esp_log.h"

static const char* TAG = "DataProcObj";

std::string DataProcObj::ProcessMessage(const std::string& message)
{
    if (message == "SatoriEye_DISCOVERY_REQUEST") {
        return HandleDiscoveryRequest(message);
    }
    if (message == "SatoriEye_HEARTBEAT_REQUEST") {
        return "SatoriEye_HEARTBEAT_RESPONSE";
    }
    if (message.starts_with("CH1")) {
        return HandleMoveRequest(message);
    }
    return message;
}

std::string DataProcObj::HandleDiscoveryRequest(const std::string& message)
{
    ESP_LOGI(TAG, "Discovery request received");
    return "SatoriEye_DISCOVERY_RESPONSE";
}

std::string DataProcObj::HandleMoveRequest(const std::string& message)
{
    std::array<int, 3> pulseWidth{};
    if (!ParseLegacyTarget(message, pulseWidth)) {
        ESP_LOGW(TAG, "Rejected malformed/out-of-range legacy target");
        return "ERROR:BAD_TARGET";
    }
    float theta[3];
    // ESP_LOGI(TAG, "Move request received, pulse width: CH1:%d, CH2:%d, CH3:%d", pulseWidth[0], pulseWidth[1], pulseWidth[2]);
    for (int i = 0; i < 3; i++) {
        theta[i] = m_servoInputAdapter.PulseWidth2Angle(pulseWidth[i]);
    }
    auto str = "CH1:" + std::to_string(theta[0]) + ",CH2:" + std::to_string(theta[1]) + ",CH3:" + std::to_string(theta[2]);
    ESP_LOGI(TAG, "Move request response: %s", str.c_str());
    ServoGroup::GetInstance().SetAngle((int)SERVO_IDX_CH1, theta[0]);
    ServoGroup::GetInstance().SetAngle((int)SERVO_IDX_CH2, theta[1]);
    ServoGroup::GetInstance().SetAngle((int)SERVO_IDX_CH3, theta[2] - (theta[1] - 90.0f) * 0.8f); // TODO: magic number
    return str;
}
