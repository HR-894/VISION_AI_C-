/**
 * @file device_profiler.cpp
 * @brief Hardware detection and performance tier classification
 */

#include "device_profiler.h"
#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>
#include <string>
#include <sstream>
#include <algorithm>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#endif

namespace vision {

DeviceProfiler::DeviceProfiler() {
    detectHardware();
    tier_ = determineTier();
    LOG_INFO("Device tier: {}", getTierString());
}

std::string DeviceProfiler::getTierString() const {
    switch (tier_) {
        case Tier::High: return "high";
        case Tier::Mid:  return "mid";
        case Tier::Low:  return "low";
    }
    return "unknown";
}

void DeviceProfiler::detectHardware() {
    profile_["cpu"] = detectCPU();
    profile_["ram"] = detectRAM();
    profile_["gpu"] = detectGPU();
}

nlohmann::json DeviceProfiler::detectCPU() {
    nlohmann::json cpu;
    
    // Get CPU info from registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char name[256] = {};
        DWORD size = sizeof(name);
        if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                             (LPBYTE)name, &size) == ERROR_SUCCESS) {
            cpu["name"] = std::string(name);
        }
        
        DWORD mhz = 0;
        size = sizeof(mhz);
        if (RegQueryValueExA(hKey, "~MHz", nullptr, nullptr,
                             (LPBYTE)&mhz, &size) == ERROR_SUCCESS) {
            cpu["speed_mhz"] = mhz;
        }
        RegCloseKey(hKey);
    }
    
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    cpu["cores"] = static_cast<int>(si.dwNumberOfProcessors);
    
    return cpu;
}

nlohmann::json DeviceProfiler::detectRAM() {
    nlohmann::json ram;
    
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        ram["total_mb"] = static_cast<int>(mem.ullTotalPhys / (1024 * 1024));
        ram["available_mb"] = static_cast<int>(mem.ullAvailPhys / (1024 * 1024));
        ram["usage_percent"] = static_cast<int>(mem.dwMemoryLoad);
    }
    
    return ram;
}

nlohmann::json DeviceProfiler::detectGPU() {
    // Try NVIDIA first, then AMD, then WMI fallback
    nlohmann::json gpu = detectNvidia();
    if (gpu.contains("name") && !gpu["name"].get<std::string>().empty()) {
        gpu["vendor"] = "nvidia";
        return gpu;
    }
    
    gpu = detectAMD();
    if (gpu.contains("name") && !gpu["name"].get<std::string>().empty()) {
        gpu["vendor"] = "amd";
        return gpu;
    }
    
    gpu = detectGPU_WMI();
    return gpu;
}

nlohmann::json DeviceProfiler::detectNvidia() {
    nlohmann::json gpu;
    
    // Try nvidia-smi
    FILE* pipe = _popen("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>nul", "r");
    if (!pipe) return gpu;
    
    char buffer[256];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    int ret = _pclose(pipe);
    
    if (ret == 0 && !result.empty()) {
        // Parse "GPU Name, MemoryMB"
        auto comma = result.find(',');
        if (comma != std::string::npos) {
            gpu["name"] = result.substr(0, comma);
            // Trim
            auto& n = gpu["name"].get_ref<std::string&>();
            n.erase(n.find_last_not_of(" \t\r\n") + 1);
            
            std::string mem_str = result.substr(comma + 1);
            mem_str.erase(0, mem_str.find_first_not_of(" \t"));
            mem_str.erase(mem_str.find_last_not_of(" \t\r\n") + 1);
            try {
                gpu["memory_mb"] = std::stoi(mem_str);
            } catch (...) {
                gpu["memory_mb"] = 0;
            }
        }
    }
    
    return gpu;
}

nlohmann::json DeviceProfiler::detectAMD() {
    nlohmann::json gpu;
    // AMD detection via WMI (handled in detectGPU_WMI)
    return gpu;
}

nlohmann::json DeviceProfiler::detectGPU_WMI() {
    nlohmann::json gpu;
    
    // Initialize COM on this worker thread (MTA) since main() no longer does it
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com_inited = SUCCEEDED(hr);
    
    IWbemLocator* pLoc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (void**)&pLoc);
    if (FAILED(hr) || !pLoc) {
        if (com_inited) CoUninitialize();
        return gpu;
    }
    
    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr,
                              nullptr, 0, nullptr, nullptr, &pSvc);
    if (FAILED(hr) || !pSvc) {
        pLoc->Release();
        if (com_inited) CoUninitialize();
        return gpu;
    }
    
    // Set security on this specific proxy (replaces CoInitializeSecurity)
    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
        nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE);
    
    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT Name, AdapterRAM FROM Win32_VideoController"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnum);
    
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* pObj = nullptr;
        ULONG uRet = 0;
        
        if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &uRet) == S_OK) {
            VARIANT vtName;
            if (pObj->Get(L"Name", 0, &vtName, nullptr, nullptr) == S_OK) {
                if (vtName.vt == VT_BSTR) {
                    _bstr_t bstr(vtName.bstrVal);
                    gpu["name"] = std::string((const char*)bstr);
                }
                VariantClear(&vtName);
            }
            
            VARIANT vtRam;
            if (pObj->Get(L"AdapterRAM", 0, &vtRam, nullptr, nullptr) == S_OK) {
                if (vtRam.vt == VT_I4 || vtRam.vt == VT_UI4) {
                    gpu["memory_mb"] = static_cast<int>(vtRam.ulVal / (1024 * 1024));
                }
                VariantClear(&vtRam);
            }
            
            // Determine vendor from name
            std::string name = gpu.value("name", "");
            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            if (lower_name.find("nvidia") != std::string::npos) gpu["vendor"] = "nvidia";
            else if (lower_name.find("amd") != std::string::npos || lower_name.find("radeon") != std::string::npos) gpu["vendor"] = "amd";
            else if (lower_name.find("intel") != std::string::npos) gpu["vendor"] = "intel";
            else gpu["vendor"] = "unknown";
            
            pObj->Release();
        }
        pEnum->Release();
    }
    
    pSvc->Release();
    pLoc->Release();
    if (com_inited) CoUninitialize();
    
    return gpu;
}

DeviceProfiler::Tier DeviceProfiler::determineTier() {
    int ram_mb = getRAMTotalMB();
    int gpu_mb = getGPUMemoryMB();
    std::string vendor = profile_.value("/gpu/vendor"_json_pointer, "unknown");
    
    // High tier: GPU ≥ 4GB (NVIDIA or AMD)
    if ((vendor == "nvidia" || vendor == "amd") && gpu_mb >= 4096) {
        return Tier::High;
    }
    
    // Mid tier: 8-16GB RAM
    if (ram_mb >= 8192) {
        return Tier::Mid;
    }
    
    return Tier::Low;
}

nlohmann::json DeviceProfiler::getRecommendedConfig() const {
    nlohmann::json cfg;

    int ram_mb = getRAMTotalMB();
    int gpu_mb = getGPUMemoryMB();
    int total_mb = ram_mb + gpu_mb;

    // ── Dynamic context size based on total available memory ──
    int context_size;
    if (total_mb <= 8192) {        // ≤ 8 GB total
        context_size = 1024;        // Req 1: aggressive cap to avoid swap death
    } else if (total_mb <= 16384) { // ≤ 16 GB total
        context_size = 4096;
    } else {                        // > 16 GB (High tier, e.g. RTX 4050)
        context_size = 8192;
    }
    cfg["context_size"] = context_size;

    // Req 2: Reserve 2 cores for OS + Qt event loop — prevents "Not Responding"
    unsigned int hw_threads = std::thread::hardware_concurrency();
    cfg["thread_count"] = std::max(1u, hw_threads > 2 ? hw_threads - 2 : 1u);

    switch (tier_) {
        case Tier::High:
            cfg["whisper_model"] = "small";
            cfg["llm_model"] = "3B";
            cfg["gpu_layers"] = 99;
            break;
        case Tier::Mid:
            cfg["whisper_model"] = "base";
            cfg["llm_model"] = "3B";
            cfg["gpu_layers"] = 20;
            break;
        case Tier::Low:
            cfg["whisper_model"] = "tiny";
            cfg["llm_model"] = "1B";
            cfg["gpu_layers"] = 0;
            break;
    }

    cfg["tier"] = getTierString();
    return cfg;
}

std::string DeviceProfiler::getStatusString() const {
    std::ostringstream ss;
    ss << "Tier: " << getTierString()
       << " | CPU: " << getCPUName()
       << " | RAM: " << getRAMTotalMB() << "MB"
       << " | GPU: " << getGPUName()
       << " (" << getGPUMemoryMB() << "MB)";
    return ss.str();
}

nlohmann::json DeviceProfiler::getFullReport() const {
    nlohmann::json report = profile_;
    report["tier"] = getTierString();
    report["recommended"] = getRecommendedConfig();
    return report;
}

std::string DeviceProfiler::getCPUName() const {
    return profile_.value("/cpu/name"_json_pointer, "Unknown CPU");
}

int DeviceProfiler::getRAMTotalMB() const {
    return profile_.value("/ram/total_mb"_json_pointer, 0);
}

std::string DeviceProfiler::getGPUName() const {
    return profile_.value("/gpu/name"_json_pointer, "Unknown GPU");
}

int DeviceProfiler::getGPUMemoryMB() const {
    return profile_.value("/gpu/memory_mb"_json_pointer, 0);
}

bool DeviceProfiler::hasNvidiaGPU() const {
    return profile_.value("/gpu/vendor"_json_pointer, "") == "nvidia";
}

bool DeviceProfiler::hasAMDGPU() const {
    return profile_.value("/gpu/vendor"_json_pointer, "") == "amd";
}

} // namespace vision
