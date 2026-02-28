#pragma once
/**
 * @file device_profiler.h
 * @brief Hardware detection and performance tier classification
 * 
 * Detects CPU, GPU (NVIDIA/AMD/Intel), RAM, and classifies the system
 * into performance tiers to auto-select optimal model sizes.
 */

#include <string>
#include <nlohmann/json.hpp>

namespace vision {

class DeviceProfiler {
public:
    /// Performance tier classification
    enum class Tier { Low, Mid, High };

    DeviceProfiler();

    /// Get the detected performance tier
    Tier getTier() const { return tier_; }
    std::string getTierString() const;

    /// Get recommended configuration based on detected hardware
    nlohmann::json getRecommendedConfig() const;

    /// Get human-readable status string
    std::string getStatusString() const;

    /// Get full hardware report
    nlohmann::json getFullReport() const;

    /// Get specific hardware info
    std::string getCPUName() const;
    int getRAMTotalMB() const;
    std::string getGPUName() const;
    int getGPUMemoryMB() const;
    bool hasNvidiaGPU() const;
    bool hasAMDGPU() const;

private:
    nlohmann::json profile_;
    Tier tier_ = Tier::Low;

    void detectHardware();
    nlohmann::json detectCPU();
    nlohmann::json detectRAM();
    nlohmann::json detectGPU();
    nlohmann::json detectNvidia();
    nlohmann::json detectAMD();
    nlohmann::json detectGPU_WMI();
    Tier determineTier();
};

} // namespace vision
