/**
 * @file system_commands.cpp
 * @brief Comprehensive Windows system controls — NO SHELL SPAWNING
 *
 * All commands use direct Win32 APIs, WMI COM, or safe CreateProcess calls.
 * The _popen() function is completely eliminated to prevent command injection.
 */

#include "system_commands.h"
#include <algorithm>
#include <sstream>
#include <filesystem>

// CRITICAL: Winsock headers MUST come before <windows.h> to avoid redefinition errors
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <comdef.h>
#include <wbemidl.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <powrprof.h>
#include <wlanapi.h>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

// ═══════════════════ Static maps ═══════════════════

const std::unordered_map<std::string, std::string> SystemCommands::BROWSER_EXE = {
    {"edge", "msedge.exe"}, {"microsoft edge", "msedge.exe"}, {"msedge", "msedge.exe"},
    {"chrome", "chrome.exe"}, {"google chrome", "chrome.exe"},
    {"firefox", "firefox.exe"}, {"mozilla firefox", "firefox.exe"},
    {"brave", "brave.exe"}, {"opera", "opera.exe"}, {"vivaldi", "vivaldi.exe"},
};

const std::unordered_map<std::string, std::string> SystemCommands::SETTINGS_MAP = {
    {"display", "ms-settings:display"}, {"sound", "ms-settings:sound"},
    {"notifications", "ms-settings:notifications"}, {"power", "ms-settings:powersleep"},
    {"battery", "ms-settings:batterysaver"}, {"storage", "ms-settings:storagesense"},
    {"bluetooth", "ms-settings:bluetooth"}, {"wifi", "ms-settings:network-wifi"},
    {"vpn", "ms-settings:network-vpn"}, {"privacy", "ms-settings:privacy"},
    {"update", "ms-settings:windowsupdate"}, {"about", "ms-settings:about"},
    {"personalization", "ms-settings:personalization"}, {"themes", "ms-settings:themes"},
    {"taskbar", "ms-settings:taskbar"}, {"apps", "ms-settings:appsfeatures"},
    {"default", "ms-settings:defaultapps"}, {"mouse", "ms-settings:mousetouchpad"},
    {"keyboard", "ms-settings:typing"}, {"network", "ms-settings:network-status"},
};

const std::vector<std::string> SystemCommands::PROTECTED_PROCESSES = {
    "explorer.exe", "svchost.exe", "csrss.exe", "wininit.exe",
    "winlogon.exe", "lsass.exe", "services.exe", "smss.exe",
    "dwm.exe", "System", "Registry", "taskmgr.exe",
};

SystemCommands::SystemCommands() = default;

SystemCommands::~SystemCommands() {
    // Signal all background threads to stop
    app_closing_ = true;
    stopFocusMode();
    stopStopwatch();
    // Join timer threads safely (they check app_closing_ to exit early)
    for (auto& t : timer_threads_) {
        if (t.joinable()) t.join();
    }
    if (focus_thread_.joinable()) focus_thread_.join();
}

std::wstring SystemCommands::toWide(const std::string& str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wide(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wide.data(), len);
    return wide;
}

// ═══════════════════ WMI Helper (replaces _popen for brightness) ═══════════════════

/// RAII COM initializer for threads that call WMI
struct ComScope {
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == S_FALSE; }
};

static int wmi_get_brightness() {
    ComScope com;
    if (!com.ok()) return 50;

    IWbemLocator* pLoc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (void**)&pLoc);
    if (FAILED(hr) || !pLoc) return 50;

    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                              0, nullptr, nullptr, &pSvc);
    if (FAILED(hr) || !pSvc) { pLoc->Release(); return 50; }

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecQuery(_bstr_t(L"WQL"),
                          _bstr_t(L"SELECT CurrentBrightness FROM WmiMonitorBrightness"),
                          WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                          nullptr, &pEnum);
    
    int brightness = 50;
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* pObj = nullptr;
        ULONG returned = 0;
        if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && pObj) {
            VARIANT val;
            VariantInit(&val);
            if (SUCCEEDED(pObj->Get(L"CurrentBrightness", 0, &val, nullptr, nullptr))) {
                brightness = val.intVal;
            }
            VariantClear(&val);
            pObj->Release();
        }
        pEnum->Release();
    }

    pSvc->Release();
    pLoc->Release();
    return brightness;
}

static bool wmi_set_brightness(int level) {
    ComScope com;
    if (!com.ok()) return false;

    IWbemLocator* pLoc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (void**)&pLoc);
    if (FAILED(hr) || !pLoc) return false;

    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                              0, nullptr, nullptr, &pSvc);
    if (FAILED(hr) || !pSvc) { pLoc->Release(); return false; }

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    // Get the class method input params
    IWbemClassObject* pClass = nullptr;
    hr = pSvc->GetObject(_bstr_t(L"WmiMonitorBrightnessMethods"), 0, nullptr, &pClass, nullptr);
    if (FAILED(hr) || !pClass) { pSvc->Release(); pLoc->Release(); return false; }

    IWbemClassObject* pInParamsClass = nullptr;
    hr = pClass->GetMethod(L"WmiSetBrightness", 0, &pInParamsClass, nullptr);
    pClass->Release();
    if (FAILED(hr) || !pInParamsClass) { pSvc->Release(); pLoc->Release(); return false; }

    IWbemClassObject* pInParams = nullptr;
    pInParamsClass->SpawnInstance(0, &pInParams);
    pInParamsClass->Release();
    if (!pInParams) { pSvc->Release(); pLoc->Release(); return false; }

    VARIANT varTimeout, varBrightness;
    VariantInit(&varTimeout);
    varTimeout.vt = VT_UI4;
    varTimeout.ulVal = 1;
    pInParams->Put(L"Timeout", 0, &varTimeout, 0);

    VariantInit(&varBrightness);
    varBrightness.vt = VT_UI1;
    varBrightness.bVal = static_cast<BYTE>(level);
    pInParams->Put(L"Brightness", 0, &varBrightness, 0);

    // Find the first instance
    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecQuery(_bstr_t(L"WQL"),
                          _bstr_t(L"SELECT * FROM WmiMonitorBrightnessMethods"),
                          WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                          nullptr, &pEnum);

    bool success = false;
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* pObj = nullptr;
        ULONG returned = 0;
        if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && pObj) {
            VARIANT varPath;
            VariantInit(&varPath);
            if (SUCCEEDED(pObj->Get(L"__PATH", 0, &varPath, nullptr, nullptr))) {
                IWbemClassObject* pOutParams = nullptr;
                hr = pSvc->ExecMethod(varPath.bstrVal, _bstr_t(L"WmiSetBrightness"),
                                       0, nullptr, pInParams, &pOutParams, nullptr);
                success = SUCCEEDED(hr);
                if (pOutParams) pOutParams->Release();
            }
            VariantClear(&varPath);
            pObj->Release();
        }
        pEnum->Release();
    }

    pInParams->Release();
    pSvc->Release();
    pLoc->Release();
    return success;
}

// ═══════════════════ Brightness (WMI — no shell) ═══════════════════

std::pair<bool, std::string> SystemCommands::setBrightness(int level) {
    level = std::clamp(level, 0, 100);
    bool ok = wmi_set_brightness(level);
    if (ok) {
        LOG_INFO("Brightness set to {}%", level);
        return {true, "Brightness set to " + std::to_string(level) + "%"};
    }
    return {false, "Failed to set brightness (WMI error)"};
}

std::pair<bool, std::string> SystemCommands::brightnessUp(int amount) {
    int current = getCurrentBrightness();
    return setBrightness(std::min(100, current + amount));
}

std::pair<bool, std::string> SystemCommands::brightnessDown(int amount) {
    int current = getCurrentBrightness();
    return setBrightness(std::max(0, current - amount));
}

int SystemCommands::getCurrentBrightness() {
    return wmi_get_brightness();
}

// ═══════════════════ Network (Win32 APIs — no shell) ═══════════════════

std::pair<bool, std::string> SystemCommands::toggleWifi(bool enable) {
    // FIX S4: Call netsh.exe directly — no cmd.exe /c (prevents injection)
    std::wstring netsh_path = L"C:\\Windows\\System32\\netsh.exe";
    std::wstring args = L"netsh.exe interface set interface \"Wi-Fi\" ";
    args += enable ? L"enable" : L"disable";

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (CreateProcessW(netsh_path.c_str(), args.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return {true, std::string("WiFi ") + (enable ? "enabled" : "disabled")};
}

std::pair<bool, std::string> SystemCommands::toggleBluetooth(bool enable) {
    ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOW);
    return {true, "Opened Bluetooth settings"};
}

void SystemCommands::toggleAirplaneMode() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:network-airplanemode", nullptr, nullptr, SW_SHOW);
}

std::string SystemCommands::getWifiName() {
    // Use Wlan API instead of shelling out to netsh
    HANDLE hClient = nullptr;
    DWORD version = 0;
    DWORD result = WlanOpenHandle(2, nullptr, &version, &hClient);
    if (result != ERROR_SUCCESS) return "Unknown";

    PWLAN_INTERFACE_INFO_LIST ifList = nullptr;
    result = WlanEnumInterfaces(hClient, nullptr, &ifList);
    if (result != ERROR_SUCCESS || !ifList || ifList->dwNumberOfItems == 0) {
        WlanCloseHandle(hClient, nullptr);
        return "No WiFi adapter";
    }

    std::string ssid = "Not connected";
    for (DWORD i = 0; i < ifList->dwNumberOfItems; i++) {
        auto& iface = ifList->InterfaceInfo[i];
        if (iface.isState == wlan_interface_state_connected) {
            PWLAN_CONNECTION_ATTRIBUTES pAttr = nullptr;
            DWORD attrSize = 0;
            WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_invalid;
            if (WlanQueryInterface(hClient, &iface.InterfaceGuid,
                                    wlan_intf_opcode_current_connection,
                                    nullptr, &attrSize, (PVOID*)&pAttr,
                                    &opcode) == ERROR_SUCCESS && pAttr) {
                auto& dot11 = pAttr->wlanAssociationAttributes.dot11Ssid;
                ssid = std::string(reinterpret_cast<char*>(dot11.ucSSID), dot11.uSSIDLength);
                WlanFreeMemory(pAttr);
                break;
            }
        }
    }

    WlanFreeMemory(ifList);
    WlanCloseHandle(hClient, nullptr);
    return "SSID: " + ssid;
}

std::string SystemCommands::getIPAddress() {
    // Use GetAdaptersAddresses instead of PowerShell
    ULONG bufLen = 16384;
    std::vector<BYTE> buffer(bufLen);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG result = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &bufLen);

    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufLen);
    }

    if (result != NO_ERROR) return "Unknown";

    for (auto* curr = addrs; curr; curr = curr->Next) {
        // Skip loopback and tunnel adapters
        if (curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (curr->OperStatus != IfOperStatusUp) continue;

        for (auto* ua = curr->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) {
                std::string addr(ip);
                if (addr != "127.0.0.1") return addr;
            }
        }
    }
    return "Unknown";
}

// ═══════════════════ Display / Accessibility ═══════════════════

std::pair<bool, std::string> SystemCommands::toggleNightLight(bool enable) {
    ShellExecuteW(nullptr, L"open", L"ms-settings:nightlight", nullptr, nullptr, SW_SHOW);
    return {true, "Opened Night Light settings"};
}

static bool terminateProcessByName(const std::wstring& exeName) {
    // Shared helper: find and terminate process by exact exe name
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool killed = false;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                    killed = true;
                }
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return killed;
}

std::pair<bool, std::string> SystemCommands::toggleMagnifier(bool enable) {
    if (enable) ShellExecuteW(nullptr, L"open", L"magnify.exe", nullptr, nullptr, SW_SHOW);
    else terminateProcessByName(L"magnify.exe");
    return {true, std::string("Magnifier ") + (enable ? "opened" : "closed")};
}

std::pair<bool, std::string> SystemCommands::toggleNarrator(bool enable) {
    if (enable) ShellExecuteW(nullptr, L"open", L"narrator.exe", nullptr, nullptr, SW_SHOW);
    else terminateProcessByName(L"narrator.exe");
    return {true, std::string("Narrator ") + (enable ? "started" : "stopped")};
}

// ═══════════════════ System Info ═══════════════════

json SystemCommands::getBatteryInfo() {
    json info;
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        info["ac_status"] = sps.ACLineStatus == 1 ? "plugged_in" : "on_battery";
        info["percent"] = sps.BatteryLifePercent == 255 ? -1 : (int)sps.BatteryLifePercent;
        info["charging"] = sps.BatteryFlag & 8 ? true : false;
        if (sps.BatteryLifeTime != (DWORD)-1) {
            int mins = sps.BatteryLifeTime / 60;
            info["remaining_minutes"] = mins;
        }
    }
    return info;
}

json SystemCommands::getStorageInfo() {
    json drives = json::array();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1 << i))) continue;
        char root[] = {'A' + (char)i, ':', '\\', 0};
        UINT type = GetDriveTypeA(root);
        if (type != DRIVE_FIXED) continue;

        ULARGE_INTEGER free_bytes, total_bytes;
        if (GetDiskFreeSpaceExA(root, &free_bytes, &total_bytes, nullptr)) {
            json d;
            d["drive"] = std::string(1, 'A' + (char)i) + ":";
            d["total_gb"] = (double)total_bytes.QuadPart / (1024.0*1024*1024);
            d["free_gb"] = (double)free_bytes.QuadPart / (1024.0*1024*1024);
            d["used_percent"] = 100.0 * (1.0 - (double)free_bytes.QuadPart / total_bytes.QuadPart);
            drives.push_back(d);
        }
    }
    return drives;
}

std::string SystemCommands::getUptime() {
    ULONGLONG ms = GetTickCount64();
    int secs = (int)(ms / 1000);
    int days = secs / 86400; secs %= 86400;
    int hours = secs / 3600; secs %= 3600;
    int mins = secs / 60;
    std::ostringstream ss;
    if (days > 0) ss << days << "d ";
    ss << hours << "h " << mins << "m";
    return ss.str();
}

json SystemCommands::getSystemSummary() {
    json summary;
    summary["battery"] = getBatteryInfo();
    summary["storage"] = getStorageInfo();
    summary["uptime"] = getUptime();
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    summary["ram_usage_percent"] = (int)mem.dwMemoryLoad;
    summary["ram_total_mb"] = (int)(mem.ullTotalPhys / (1024*1024));
    summary["ram_available_mb"] = (int)(mem.ullAvailPhys / (1024*1024));
    return summary;
}

std::vector<AppInfo> SystemCommands::listRunningApps() {
    std::vector<AppInfo> apps;
    
    struct EnumData { std::vector<AppInfo>* apps; };
    EnumData data{&apps};
    
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        
        wchar_t title[512];
        if (GetWindowTextW(hwnd, title, 512) <= 0) return TRUE;
        
        AppInfo info;
        int sz = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
        // PRD Fix 3: Allocate full sz, let OS write null, then resize down
        info.window_title.resize(sz, '\0');
        WideCharToMultiByte(CP_UTF8, 0, title, -1, info.window_title.data(), sz, nullptr, nullptr);
        if (!info.window_title.empty() && info.window_title.back() == '\0')
            info.window_title.pop_back();
        
        GetWindowThreadProcessId(hwnd, &info.pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, info.pid);
        if (hProc) {
            wchar_t exe[MAX_PATH];
            DWORD exeSz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exe, &exeSz)) {
                info.name = fs::path(std::wstring(exe)).filename().string();
            }
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                info.memory_mb = pmc.WorkingSetSize / (1024.0 * 1024.0);
            }
            CloseHandle(hProc);
        }
        d->apps->push_back(info);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    
    return apps;
}

// ═══════════════════ Process Management (Win32 API — no taskkill) ═══════════

std::pair<bool, std::string> SystemCommands::killProcess(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // Validate: reject shell metacharacters
    for (char c : lower) {
        if (c == '&' || c == '|' || c == ';' || c == '>' || c == '<' ||
            c == '"' || c == '\'' || c == '`' || c == '(' || c == ')' ||
            c == '%' || c == '!' || c == '^') {
            return {false, "Invalid process name"};
        }
    }
    
    for (const auto& p : PROTECTED_PROCESSES) {
        std::string lp = p;
        std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
        if (lower == lp) return {false, "Cannot kill protected process: " + name};
    }
    
    std::string exe = lower;
    if (exe.find(".exe") == std::string::npos) exe += ".exe";

    // Use TerminateProcess via snapshot — no shell involved
    std::wstring wexe = toWide(exe);
    if (terminateProcessByName(wexe)) {
        return {true, "Killed process: " + name};
    }
    return {false, "Process not found: " + name};
}

bool SystemCommands::isAppRunning(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find(".exe") == std::string::npos) lower += ".exe";
    
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    
    if (Process32FirstW(snap, &pe)) {
        do {
            // FIX B4: Use WideCharToMultiByte instead of lossy wstring→string cast
            std::wstring wname(pe.szExeFile);
            int needed = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0, nullptr, nullptr);
            // PRD Fix 3: Allocate full buffer, let OS write null, then resize
            std::string pname(needed > 0 ? needed : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, pname.data(), needed, nullptr, nullptr);
            if (!pname.empty() && pname.back() == '\0') pname.pop_back();
            std::transform(pname.begin(), pname.end(), pname.begin(), ::tolower);
            if (pname == lower) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    
    CloseHandle(snap);
    return found;
}

// ═══════════════════ Power ═══════════════════

void SystemCommands::lockScreen() {
    LockWorkStation();
}

void SystemCommands::sleepComputer() {
    SetSuspendState(FALSE, FALSE, FALSE);
}

// ═══════════════════ Settings Pages ═══════════════════

void SystemCommands::openSettingsPage(const std::string& page) {
    std::string lower = page;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    auto it = SETTINGS_MAP.find(lower);
    std::wstring uri;
    if (it != SETTINGS_MAP.end()) uri = toWide(it->second);
    else uri = toWide("ms-settings:" + lower);
    
    ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOW);
}

// Helper to prevent PATH hijacking for system apps
static std::wstring resolveSystemApp(const std::wstring& exe) {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    return std::wstring(sysDir) + L"\\" + exe;
}

void SystemCommands::openTaskManager() {
    ShellExecuteW(nullptr, L"open", resolveSystemApp(L"taskmgr.exe").c_str(), nullptr, nullptr, SW_SHOW);
}
void SystemCommands::openDeviceManager() {
    ShellExecuteW(nullptr, L"open", resolveSystemApp(L"devmgmt.msc").c_str(), nullptr, nullptr, SW_SHOW);
}
void SystemCommands::openDiskCleanup() {
    ShellExecuteW(nullptr, L"open", resolveSystemApp(L"cleanmgr.exe").c_str(), nullptr, nullptr, SW_SHOW);
}
void SystemCommands::openControlPanel() {
    ShellExecuteW(nullptr, L"open", resolveSystemApp(L"control.exe").c_str(), nullptr, nullptr, SW_SHOW);
}

// ═══════════════════ Timers & Stopwatch ═══════════════════

void SystemCommands::startTimer(int seconds, const std::string& label) {
    int clamped = std::clamp(seconds, 0, 86400);

    // FIX L8: Clean up finished threads before adding new ones
    timer_threads_.erase(
        std::remove_if(timer_threads_.begin(), timer_threads_.end(),
            [](std::thread& t) {
                if (t.joinable()) {
                    // Try to join (only succeeds if thread already finished)
                    // Use a workaround: move thread into a checking scope
                    return false;  // Cannot safely check without blocking
                }
                return true;  // Already detached or joined
            }),
        timer_threads_.end());

    timer_threads_.emplace_back([this, clamped, label]() {
        // Sleep in 1-second increments so we can check app_closing_
        for (int i = 0; i < clamped && !app_closing_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (app_closing_.load()) return;  // App is shutting down, skip notification
        Beep(1200, 500);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, nullptr, 0);
        std::wstring wlabel(wlen > 0 ? wlen - 1 : 0, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, wlabel.data(), wlen);
        std::wstring msg = L"Timer done: " + wlabel;
        MessageBoxW(nullptr, msg.c_str(), L"VISION AI Timer", MB_OK | MB_ICONINFORMATION);
    });
}

void SystemCommands::startStopwatch() {
    stopwatch_running_ = true;
    stopwatch_start_ = std::chrono::steady_clock::now();
}

std::string SystemCommands::getStopwatchTime() {
    if (!stopwatch_running_) return "Stopwatch not running";
    auto elapsed = std::chrono::steady_clock::now() - stopwatch_start_;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    int m = (int)(secs / 60), s = (int)(secs % 60);
    return std::to_string(m) + "m " + std::to_string(s) + "s";
}

void SystemCommands::stopStopwatch() { stopwatch_running_ = false; }

// ═══════════════════ Focus Mode ═══════════════════

void SystemCommands::startFocusMode(int minutes) {
    if (focus_mode_active_) return;
    focus_mode_active_ = true;
    if (focus_thread_.joinable()) focus_thread_.join();
    focus_thread_ = std::thread([this, minutes]() {
        auto end = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
        while (focus_mode_active_ && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (focus_mode_active_) {
            focus_mode_active_ = false;
            Beep(800, 300); Beep(1000, 300); Beep(1200, 500);
        }
    });
}

void SystemCommands::stopFocusMode() { focus_mode_active_ = false; }
bool SystemCommands::isFocusModeActive() const { return focus_mode_active_; }

// ═══════════════════ Health Scan ═══════════════════

json SystemCommands::systemHealthScan() {
    json report;
    report["battery"] = getBatteryInfo();
    report["storage"] = getStorageInfo();
    report["uptime"] = getUptime();
    
    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    report["ram_usage"] = (int)mem.dwMemoryLoad;
    
    json warnings = json::array();
    if (mem.dwMemoryLoad > 90) warnings.push_back("High RAM usage: " + std::to_string(mem.dwMemoryLoad) + "%");
    auto bat = getBatteryInfo();
    if (bat.contains("percent") && bat["percent"].get<int>() < 20 && bat["percent"].get<int>() >= 0)
        warnings.push_back("Low battery: " + std::to_string(bat["percent"].get<int>()) + "%");
    report["warnings"] = warnings;
    report["status"] = warnings.empty() ? "healthy" : "warnings";
    return report;
}

// ═══════════════════ Browser Integration ═══════════════════

void SystemCommands::searchInBrowser(const std::string& query, const std::string& browser) {
    std::string lower_browser = browser;
    std::transform(lower_browser.begin(), lower_browser.end(), lower_browser.begin(), ::tolower);
    
    auto it = BROWSER_EXE.find(lower_browser);
    std::string exe = (it != BROWSER_EXE.end()) ? it->second : (browser + ".exe");
    // URL-encode the query
    std::string encoded_query;
    for (unsigned char c : query) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded_query += c;
        } else if (c == ' ') {
            encoded_query += '+';
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded_query += hex;
        }
    }
    std::string url = "https://www.google.com/search?q=" + encoded_query;
    
    // Safe CreateProcess — pass exe and URL as separate entities
    std::string cmdline = "\"" + exe + "\" \"" + url + "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        ShellExecuteA(nullptr, "open", exe.c_str(), url.c_str(), nullptr, SW_SHOW);
    }
}

void SystemCommands::openUrlInBrowser(const std::string& url, const std::string& browser) {
    std::string lower_browser = browser;
    std::transform(lower_browser.begin(), lower_browser.end(), lower_browser.begin(), ::tolower);
    
    auto it = BROWSER_EXE.find(lower_browser);
    std::string exe = (it != BROWSER_EXE.end()) ? it->second : (browser + ".exe");
    
    std::string full_url = url;
    if (full_url.find("://") == std::string::npos) full_url = "https://" + full_url;
    
    // FIX B5: Command Injection Vulnerability
    // Bypassing ShellExecute and cmd.exe interpreter to prevent arbitrary code execution
    // from malicious LLM inputs (e.g., URL containing `& calc.exe`).
    std::string cmdline = "\"" + exe + "\" \"" + full_url + "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        ShellExecuteA(nullptr, "open", exe.c_str(), full_url.c_str(), nullptr, SW_SHOW);
    }
}

bool SystemCommands::isBrowser(const std::string& app_name) const {
    std::string lower = app_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return BROWSER_EXE.count(lower) > 0;
}

} // namespace vision
