#include "doctor.h"
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;

namespace vision {

DiagnosticResult Doctor::checkMemoryUsage() {
    DiagnosticResult r;
    r.check_name = "System Memory (RAM)";
    r.passed = true;

#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);

    DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
    DWORDLONG physMemUsed = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
    double percentUsed = (double)physMemUsed / (double)totalPhysMem * 100.0;

    std::ostringstream oss;
    oss << "Total: " << (totalPhysMem / (1024 * 1024 * 1024)) << " GB, "
        << "Used: " << percentUsed << "%";
    r.message = oss.str();

    if (percentUsed > 90.0) {
        r.passed = false;
        r.message += " (CRITICAL)";
        r.suggestion = "System is running very low on RAM. Close background applications.";
    } else if (percentUsed > 80.0) {
        r.message += " (WARNING)";
    }
#else
    r.message = "Memory check not implemented for this OS.";
#endif

    return r;
}

DiagnosticResult Doctor::checkCPU() {
    DiagnosticResult r;
    r.check_name = "CPU Cores";
    r.passed = true;

#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    
    std::ostringstream oss;
    oss << "Logical Processors: " << sysInfo.dwNumberOfProcessors;
    r.message = oss.str();

    if (sysInfo.dwNumberOfProcessors < 4) {
        r.passed = false;
        r.suggestion = "Less than 4 logical processors detected. Performance may be degraded.";
    }
#else
    r.message = "CPU check not implemented for this OS.";
#endif
    return r;
}

DiagnosticResult Doctor::checkFileSystem() {
    DiagnosticResult r;
    r.check_name = "File System";
    r.passed = true;
    
    try {
        auto space = fs::space(".");
        double gbAvailable = (double)space.available / (1024 * 1024 * 1024);
        
        std::ostringstream oss;
        oss << gbAvailable << " GB available in working directory.";
        r.message = oss.str();
        
        if (gbAvailable < 5.0) {
            r.passed = false;
            r.suggestion = "Less than 5GB of disk space available. Free up space.";
        }
    } catch (const std::exception& e) {
        r.passed = false;
        r.message = "Failed to query filesystem.";
        r.suggestion = e.what();
    }
    
    return r;
}

std::string Doctor::runFullDiagnostic() {
    std::vector<DiagnosticResult> results;
    
    results.push_back(checkMemoryUsage());
    results.push_back(checkCPU());
    results.push_back(checkFileSystem());

    return formatReport(results);
}

std::string Doctor::formatReport(const std::vector<DiagnosticResult>& results) {
    std::ostringstream oss;
    oss << "=== SYSTEM DIAGNOSTIC REPORT ===\n\n";

    int passed_count = 0;
    for (const auto& r : results) {
        oss << "[" << (r.passed ? "PASS" : "FAIL") << "] " << r.check_name << "\n";
        oss << "    Info: " << r.message << "\n";
        if (!r.passed && !r.suggestion.empty()) {
            oss << "    Fix:  " << r.suggestion << "\n";
        }
        oss << "\n";
        
        if (r.passed) passed_count++;
    }

    oss << "================================\n";
    oss << "Summary: " << passed_count << "/" << results.size() << " checks passed.\n";
    return oss.str();
}

} // namespace vision
