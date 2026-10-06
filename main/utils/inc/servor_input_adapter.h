/**
 * @file servor_input_adapter.h
 * @author Fyra-BH (2472708282@qq.com)
 * @brief 为什么要做这个adapter？因为之前脑子抽了，把三路舵机的输入揉到了一个报文, 而写用的是us脉宽的格式, 
 *        后来需求变化, 要改用传输角度了，不得已做了这个适配器
 * @version 0.1
 * @date 2024-12-11
 * 
 * @copyright Copyright (c) 2024
 * 
 */
#ifndef SERVO_INPUT_ADAPTER_H
#define SERVO_INPUT_ADAPTER_H

/**
 * @brief 舵机数据预处理
 * @note scale将输入0~180度映射到一个小于180度的范围, offset配合scale设置角度偏置
 *       min和max用于将舵机的角度限制在某个范围内
 *       isReverse用于将舵机的角度反转
 */
struct ServoDataConfig {
    float scale;
    float offset;
    float zeroPoint;
    float minAngle;
    float maxAngle;
    bool isReverse;
};


// Shared by both output profiles and host mechanical regression tests.
// Preserve the historical order: reversal, scale about zero, offset, clamp.
inline float CalibratedServoAngle(float theta, const ServoDataConfig& config) {
    theta = config.isReverse ? 180.0f - theta : theta;
    theta = (theta - config.zeroPoint) * config.scale + config.zeroPoint - config.offset;
    if (theta > config.maxAngle) return config.maxAngle;
    if (theta < config.minAngle) return config.minAngle;
    return theta;
}


class ServoInputAdapter {
public:
    inline float PulseWidth2Angle(int pulseWidth)
    { // 500us ~ 2500us
        return (pulseWidth - 500.0) / 2000.0 * 180.0;
    }
    inline int Angle2PulseWidth(float theta)
    { // 500us ~ 2500us
        return theta / 180.0 * 2000.0 + 500;
    }
    inline float DutyCycle2Angle(float dutyCycle)
    { // 0.025 ~ 0.125
        return (dutyCycle - 0.025) / 0.1 * 180.0;
    }
};

#endif