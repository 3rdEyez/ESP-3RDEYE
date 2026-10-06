#include "servo_group.h"
#include <thread>
#include <chrono>
#include <cmath>

#include "driver/ledc.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_partition_param.h"

static const char *TAG = "SERVO";

constexpr int SERVO_NUM = 3;

// 舵机数据预处理配置
constexpr float SERVO_SCALE_CH1 = 1.0f / 3.0f;
constexpr float SERVO_OFFSET_CH1 = 0.0f;
constexpr float SERVO_ZEROPOINT_CH1 = 90.0f;
constexpr float SERVO_MIN_ANGLE_CH1 = 45.0f;
constexpr float SERVO_MAX_ANGLE_CH1 = 135.0f;
constexpr bool SERVO_IS_REVERSE_CH1 = true;

constexpr float SERVO_SCALE_CH2 = 1.0f / 3.0;
constexpr float SERVO_OFFSET_CH2 = 40.0f;
constexpr float SERVO_ZEROPOINT_CH2 = 60.0f;
constexpr float SERVO_MIN_ANGLE_CH2 = 0.0f;
constexpr float SERVO_MAX_ANGLE_CH2 = 90.0f;
constexpr bool SERVO_IS_REVERSE_CH2 = true;

constexpr float SERVO_SCALE_CH3 = 1.0f / 3.0f;
constexpr float SERVO_OFFSET_CH3 = -40.0f;
constexpr float SERVO_ZEROPOINT_CH3 = 120.0f;
constexpr float SERVO_MIN_ANGLE_CH3 = 120.0f;
constexpr float SERVO_MAX_ANGLE_CH3 = 180.0f;
constexpr bool SERVO_IS_REVERSE_CH3 = true;

namespace {

ServoDataConfig g_ServoConfigCH1;
ServoDataConfig g_ServoConfigCH2;
ServoDataConfig g_ServoConfigCH3;
std::array<int, SERVO_NUM> g_ServoPwmGpios;

void SetUpServoDataPreprocessor()
{
    g_ServoConfigCH1 = {
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_SCALE_CH1", SERVO_SCALE_CH1),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_OFFSET_CH1", SERVO_OFFSET_CH1),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_ZEROPOINT_CH1", SERVO_ZEROPOINT_CH1),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_MIN_ANGLE_CH1", SERVO_MIN_ANGLE_CH1),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_MAX_ANGLE_CH1", SERVO_MAX_ANGLE_CH1),
        EspPartitionParam::GetInstance().GetBoolParam("SERVO_IS_REVERSE_CH1", SERVO_IS_REVERSE_CH1)
    };

    g_ServoConfigCH2 = {
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_SCALE_CH2", SERVO_SCALE_CH2),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_OFFSET_CH2", SERVO_OFFSET_CH2),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_ZEROPOINT_CH2", SERVO_ZEROPOINT_CH2),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_MIN_ANGLE_CH2", SERVO_MIN_ANGLE_CH2),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_MAX_ANGLE_CH2", SERVO_MAX_ANGLE_CH2),
        EspPartitionParam::GetInstance().GetBoolParam("SERVO_IS_REVERSE_CH2", SERVO_IS_REVERSE_CH2)
    };

    g_ServoConfigCH3 = {
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_SCALE_CH3", SERVO_SCALE_CH3),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_OFFSET_CH3", SERVO_OFFSET_CH3),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_ZEROPOINT_CH3", SERVO_ZEROPOINT_CH3),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_MIN_ANGLE_CH3", SERVO_MIN_ANGLE_CH3),
        EspPartitionParam::GetInstance().GetFloatParam("SERVO_MAX_ANGLE_CH3", SERVO_MAX_ANGLE_CH3),
        EspPartitionParam::GetInstance().GetBoolParam("SERVO_IS_REVERSE_CH3", SERVO_IS_REVERSE_CH3)
    };

    g_ServoPwmGpios = {
        EspPartitionParam::GetInstance().GetIntParam("SERVO_PULSE_GPIO_CH1", CONFIG_SERVO_PULSE_GPIO_CH1),
        EspPartitionParam::GetInstance().GetIntParam("SERVO_PULSE_GPIO_CH2", CONFIG_SERVO_PULSE_GPIO_CH2),
        EspPartitionParam::GetInstance().GetIntParam("SERVO_PULSE_GPIO_CH3", CONFIG_SERVO_PULSE_GPIO_CH3)
    };
}

}


ServoGroup &ServoGroup::GetInstance()
{
    static ServoGroup instance;
    return instance;
}

ServoGroup::ServoGroup()
{
    SetUpServoDataPreprocessor();
    /*########################### PWM #################################*/
    // 对于ESP32C3，使用LEDC驱动PWM
    // Prepare and then apply the LEDC PWM timer configuration
    const std::array<int, SERVO_NUM> &gpio_array=g_ServoPwmGpios;
    
    ledc_timer_config_t ledc_timer{};

    ledc_timer.speed_mode = LEDC_MODE;
    ledc_timer.duty_resolution = LEDC_DUTY_RES;
    ledc_timer.timer_num = LEDC_TIMER;
    ledc_timer.freq_hz = LEDC_FREQUENCY;  // Set output frequency at 50Hz
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer.deconfigure = false;

    if (ledc_timer_config(&ledc_timer) != ESP_OK) {
        ESP_LOGE(TAG, "PWM timer initialization failed");
        return;
    }
    
    ledc_channel_config_t ledc_channel{};
    ledc_channel.speed_mode     = LEDC_MODE;
    ledc_channel.timer_sel      = LEDC_TIMER;
    ledc_channel.intr_type      = LEDC_INTR_DISABLE;
    ledc_channel.duty           = 0; // Set duty to 0%
    ledc_channel.hpoint         = 0;

    ledc_channel.gpio_num = gpio_array[SERVO_IDX_CH1];
    ledc_channel.channel = static_cast<ledc_channel_t>(SERVO_IDX_CH1);
    if (ledc_channel_config(&ledc_channel) != ESP_OK) {
        ESP_LOGE(TAG, "PWM channel initialization failed");
        return;
    }
    SetServoDataPreprocessor(SERVO_IDX_CH1, g_ServoConfigCH1);

    ledc_channel.gpio_num = gpio_array[SERVO_IDX_CH2];
    ledc_channel.channel = static_cast<ledc_channel_t>(SERVO_IDX_CH2);
    if (ledc_channel_config(&ledc_channel) != ESP_OK) {
        ESP_LOGE(TAG, "PWM channel initialization failed");
        return;
    }
    SetServoDataPreprocessor(SERVO_IDX_CH2, g_ServoConfigCH2);

    ledc_channel.gpio_num = gpio_array[SERVO_IDX_CH3];
    ledc_channel.channel = static_cast<ledc_channel_t>(SERVO_IDX_CH3);
    if (ledc_channel_config(&ledc_channel) != ESP_OK) {
        ESP_LOGE(TAG, "PWM channel initialization failed");
        return;
    }
    SetServoDataPreprocessor(SERVO_IDX_CH3, g_ServoConfigCH3);
    ready_ = true;
}

/**
 * @brief 设置舵机的角度
 * 
 * @param servoIdx 舵机索引
 * @param theta 舵机角度
 */
bool ServoGroup::SetAngle(int servoIdx, float theta)
{
    if (!ready_ || !std::isfinite(theta) || servoIdx < 0 || servoIdx >= SERVO_NUM) {
        ESP_LOGE(TAG, "PWM output unavailable or invalid input");
        return false;
    }
    theta = CalibratedServoAngle(theta, m_servoDataPreprocessorMap.at(servoIdx));
    float duty = (theta * 2.0 / 180.0 + 0.5) / 20.0; // 将角度转换为占空比;20.0代表20ms
    int pulse_width = (uint32_t)(duty * (float)(1 << LEDC_DUTY_RES));
    // ESP_LOGI(TAG, "servoIdx: %d, theta: %f, duty: %f, pulse_width: %d", servoIdx, theta, duty, pulse_width);
    if (ledc_set_duty(LEDC_MODE, static_cast<ledc_channel_t>(servoIdx), pulse_width) != ESP_OK ||
        ledc_update_duty(LEDC_MODE, static_cast<ledc_channel_t>(servoIdx)) != ESP_OK) {
        ESP_LOGE(TAG, "PWM output update failed");
        ready_ = false;
        return false;
    }
    return true;
}


/**
 * @brief 设置舵机的预处理参数
 * 
 * @param servoIdx 舵机索引
 * @param servoDataPreprocessor 舵机预处理配置
 */
void ServoGroup::SetServoDataPreprocessor(int servoIdx, const ServoDataConfig &servoDataPreprocessor)
{
    m_servoDataPreprocessorMap.insert({servoIdx, servoDataPreprocessor});
}

/**
 * @brief 五次插值函数，输入为目标舵机、初始角度、目标角度
 * 
 * @param servoIdx 舵机索引
 * @param angleStart 初始角度
 * @param angleEnd 目标角度
 * @param duration 插值时间 (s)
 */
void ServoGroup::FiveTimesInterpolation(int servoIdx, float angleStart, float angleEnd, float duration)
{
    // 定义插值时间总长度 (us)，可以根据需求调整
    float T = duration * 1000.0 * 1000.0; // 插值时间为2秒 (2000_000 us)
    int steps = 50;   // 插值步数，越多越平滑
    float dt = T / steps;

    // 五次插值多项式系数
    float a0 = angleStart;
    float a1 = 0;
    float a2 = 0;
    float a3 = 10 * (angleEnd - angleStart) / (T * T * T);
    float a4 = -15 * (angleEnd - angleStart) / (T * T * T * T);
    float a5 = 6 * (angleEnd - angleStart) / (T * T * T * T * T);

    // 插值过程
    for (int i = 0; i <= steps; i++) {
        float t = i * dt; // 当前时间
        float theta = a0 + a1 * t + a2 * t * t + a3 * t * t * t + a4 * t * t * t * t + a5 * t * t * t * t * t;
        
        // 将插值角度设置到舵机
        SetAngle(servoIdx, theta);
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(dt)));
    }
}
