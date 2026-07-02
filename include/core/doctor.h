#pragma once
/**
 * @file doctor.h
 * @brief Self-Diagnosis System
 *
 * Runs basic system health checks (RAM, CPU, basic directory presence).
 */

#include <string>
#include <vector>

namespace vision {

struct DiagnosticResult {
    std::string check_name;
    bool passed;
    std::string message;
    std::string suggestion;  // Only set if !passed
};

class Doctor {
public:
    /// Run all diagnostics and return a formatted report
    static std::string runFullDiagnostic();

    /// Run individual checks
    static DiagnosticResult checkMemoryUsage();
    static DiagnosticResult checkCPU();
    static DiagnosticResult checkFileSystem();

private:
    static std::string formatReport(const std::vector<DiagnosticResult>& results);
};

} // namespace vision
