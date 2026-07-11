// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/text.h"

#include <bitset>
#include <iostream>

#include <psapi.h>
#include <winsock2.h>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

namespace {

using Slpi = SYSTEM_LOGICAL_PROCESSOR_INFORMATION;
using SlpiBuf = std::aligned_storage_t<sizeof(Slpi)>;

struct LpiRecords {
    const Slpi* begin() const {
        return reinterpret_cast<const Slpi*>(slpiRecords.get());
    }

    const Slpi* end() const {
        return begin() + count;
    }

    std::unique_ptr<SlpiBuf[]> slpiRecords;
    size_t count;
};

// Both the body of this getLogicalProcessorInformationRecords and the callers of
// getLogicalProcessorInformationRecords are largely modeled off of the example code at
// https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformation
LpiRecords getLogicalProcessorInformationRecords() {

    DWORD returnLength = 0;
    LpiRecords lpiRecords{};

    DWORD returnCode = 0;
    do {
        returnCode = GetLogicalProcessorInformation(
            reinterpret_cast<Slpi*>(lpiRecords.slpiRecords.get()), &returnLength);
        if (returnCode == FALSE) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                lpiRecords.slpiRecords = std::unique_ptr<SlpiBuf[]>(
                    new SlpiBuf[((returnLength - 1) / sizeof(Slpi)) + 1]);
            } else {
                auto ec = lastSystemError();
                LOGV2_WARNING(23811,
                              "GetLogicalProcessorInformation failed",
                              "error"_attr = errorMessage(ec));
                return LpiRecords{};
            }
        }
    } while (returnCode == FALSE);


    lpiRecords.count = returnLength / sizeof(Slpi);
    return lpiRecords;
}

struct ParsedProcessorInfo {
    int physicalCoreCount;
    int numaNodeCount;
    int processorPackageCount;
};

ParsedProcessorInfo getProcessorInfo() {
    ParsedProcessorInfo ppi{0, 0, 0};
    for (auto&& lpi : getLogicalProcessorInformationRecords()) {
        switch (lpi.Relationship) {
            case RelationProcessorCore:
                ppi.physicalCoreCount++;
                break;
            case RelationNumaNode:
                ppi.numaNodeCount++;
                break;
            case RelationProcessorPackage:
                ppi.processorPackageCount++;
        }
    }
    return ppi;
}

}  // namespace
int _wconvertmtos(SIZE_T s) {
    return (int)(s / (1024 * 1024));
}

ProcessInfo::ProcessInfo(ProcessId pid) {}

ProcessInfo::~ProcessInfo() {}

// get the number of CPUs available to the current process
boost::optional<uint64_t> ProcessInfo::getNumCoresForProcess() {
    DWORD_PTR process_mask, system_mask;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return boost::none;

    std::bitset<sizeof(process_mask) * 8> mask(process_mask);
    auto num = mask.count();
    if (num == 0)
        return boost::none;

    // If we are running in a Windows Container using process isolation this process is
    // associated with a job object we can query for the cpu limit
    // https://docs.microsoft.com/en-us/virtualization/windowscontainers/manage-containers/resource-controls
    // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpuInfo;
    ZeroMemory(&cpuInfo, sizeof(cpuInfo));
    if (QueryInformationJobObject(
            NULL, JobObjectCpuRateControlInformation, &cpuInfo, sizeof(cpuInfo), NULL) &&
        (cpuInfo.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) &&
        (cpuInfo.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP)) {
        // CpuRate is a percentage times 100
        num = std::ceil(num * (static_cast<double>(cpuInfo.CpuRate) / 10000));
    }

    return num;
}

bool ProcessInfo::supported() {
    return true;
}

int ProcessInfo::getVirtualMemorySize() {
    MEMORYSTATUSEX mse;
    mse.dwLength = sizeof(mse);
    BOOL status = GlobalMemoryStatusEx(&mse);
    if (!status) {
        auto ec = lastSystemError();
        LOGV2_ERROR(23812, "GlobalMemoryStatusEx failed", "error"_attr = errorMessage(ec));
        fassert(28621, status);
    }

    DWORDLONG x = (mse.ullTotalVirtual - mse.ullAvailVirtual) / (1024 * 1024);
    invariant(x <= 0x7fffffff);
    return (int)x;
}

int ProcessInfo::getResidentSize() {
    PROCESS_MEMORY_COUNTERS pmc;
    BOOL status = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    if (!status) {
        auto ec = lastSystemError();
        LOGV2_ERROR(23813, "GetProcessMemoryInfo failed", "error"_attr = errorMessage(ec));
        fassert(28622, status);
    }

    return _wconvertmtos(pmc.WorkingSetSize);
}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
    MEMORYSTATUSEX mse;
    mse.dwLength = sizeof(mse);
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        info.append("page_faults", static_cast<int>(pmc.PageFaultCount));
        info.append("usagePageFileMB", static_cast<int>(pmc.PagefileUsage / 1024 / 1024));
    }
    if (GlobalMemoryStatusEx(&mse)) {
        info.append("totalPageFileMB", static_cast<int>(mse.ullTotalPageFile / 1024 / 1024));
        info.append("availPageFileMB", static_cast<int>(mse.ullAvailPageFile / 1024 / 1024));
        info.append("ramMB", static_cast<int>(mse.ullTotalPhys / 1024 / 1024));
    }

#ifndef _WIN64
    BOOL wow64Process;
    BOOL retWow64 = IsWow64Process(GetCurrentProcess(), &wow64Process);
    info.append("wow64Process", static_cast<bool>(retWow64 && wow64Process));
#endif
}

bool getFileVersion(const char* filePath, DWORD& fileVersionMS, DWORD& fileVersionLS) {
    DWORD verSize = GetFileVersionInfoSizeA(filePath, NULL);
    if (verSize == 0) {
        auto ec = lastSystemError();
        LOGV2_WARNING(23807,
                      "GetFileVersionInfoSizeA failed",
                      "path"_attr = filePath,
                      "error"_attr = errorMessage(ec));
        return false;
    }

    std::unique_ptr<char[]> verData(new char[verSize]);
    if (GetFileVersionInfoA(filePath, NULL, verSize, verData.get()) == 0) {
        auto ec = lastSystemError();
        LOGV2_WARNING(23808,
                      "GetFileVersionInfoSizeA failed",
                      "path"_attr = filePath,
                      "error"_attr = errorMessage(ec));
        return false;
    }

    UINT size;
    VS_FIXEDFILEINFO* verInfo;
    if (VerQueryValueA(verData.get(), "\\", (LPVOID*)&verInfo, &size) == 0) {
        auto ec = lastSystemError();
        LOGV2_WARNING(23809,
                      "VerQueryValueA failed",
                      "path"_attr = filePath,
                      "error"_attr = errorMessage(ec));
        return false;
    }

    if (size != sizeof(VS_FIXEDFILEINFO)) {
        LOGV2_WARNING(23810,
                      "VerQueryValueA returned structure with unexpected size",
                      "path"_attr = filePath);
        return false;
    }

    fileVersionMS = verInfo->dwFileVersionMS;
    fileVersionLS = verInfo->dwFileVersionLS;
    return true;
}

std::string getCpuString() {
    // get descriptive CPU string from registry
    HKEY hKey;
    LPCWSTR cpuKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    LPCWSTR valueName = L"ProcessorNameString";
    std::string cpuString;

    // Open the CPU key in the Windows Registry
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, cpuKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        ScopeGuard guard([hKey] { RegCloseKey(hKey); });
        WCHAR cpuModel[128];
        DWORD bufferSize = sizeof(cpuModel);

        // Retrieve the value of ProcessorNameString
        if (RegQueryValueEx(hKey,
                            valueName,
                            nullptr,
                            nullptr,
                            reinterpret_cast<LPBYTE>(cpuModel),
                            &bufferSize) == ERROR_SUCCESS) {
            cpuString = toUtf8String(cpuModel);
        } else {
            auto ec = lastSystemError();
            LOGV2_WARNING(7663101,
                          "Failed to retrieve CPU model name from the registry",
                          "error"_attr = errorMessage(ec));
        }

        // Close the registry key
    } else {
        auto ec = lastSystemError();
        LOGV2_WARNING(
            7663102, "Failed to open CPU key in the registry", "error"_attr = errorMessage(ec));
    }
    return cpuString;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {
    BSONObjBuilder bExtra;
    std::stringstream verstr;
    OSVERSIONINFOEX osvi;   // os version
    MEMORYSTATUSEX mse;     // memory stats
    SYSTEM_INFO ntsysinfo;  // system stats

    // get basic processor properties
    GetNativeSystemInfo(&ntsysinfo);
    addrSize = (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 64 : 32);
    numCores = ntsysinfo.dwNumberOfProcessors;
    auto ppi = getProcessorInfo();
    numPhysicalCores = ppi.physicalCoreCount;
    numCpuSockets = ppi.processorPackageCount;
    hasNuma = ppi.numaNodeCount > 1;
    numNumaNodes = ppi.numaNodeCount;
    pageSize = static_cast<unsigned long long>(ntsysinfo.dwPageSize);
    bExtra.append("pageSize", static_cast<long long>(pageSize));

    if (auto cpuString = getCpuString(); !cpuString.empty())
        bExtra.append("cpuString", cpuString);

    // get memory info
    mse.dwLength = sizeof(mse);
    if (GlobalMemoryStatusEx(&mse)) {
        memSize = mse.ullTotalPhys;

        // If we are running in a Windows Container using process isolation this process is
        // associated with a job object we can query for the memory limit
        // https://docs.microsoft.com/en-us/virtualization/windowscontainers/manage-containers/resource-controls
        // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo;
        ZeroMemory(&jobInfo, sizeof(jobInfo));
        if (QueryInformationJobObject(
                NULL, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo), NULL) &&
            (jobInfo.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) &&
            jobInfo.JobMemoryLimit != 0) {
            memLimit = jobInfo.JobMemoryLimit;
        } else {
            memLimit = memSize;
        }
    }

    // get OS version info
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
#pragma warning(push)
// GetVersionEx is deprecated
#pragma warning(disable : 4996)
    if (GetVersionEx((OSVERSIONINFO*)&osvi)) {
#pragma warning(pop)

        verstr << osvi.dwMajorVersion << "." << osvi.dwMinorVersion;
        if (osvi.wServicePackMajor)
            verstr << " SP" << osvi.wServicePackMajor;
        verstr << " (build " << osvi.dwBuildNumber << ")";

        osName = fmt::format("Microsoft Windows {} (build {})",
                             osvi.wProductType == VER_NT_WORKSTATION ? "Workstation" : "Server",
                             osvi.dwBuildNumber);
    } else {
        // unable to get any version data
        osName = "Microsoft Windows (unknown build)";
    }

    if (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        cpuArch = "x86_64";
    } else if (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
        cpuArch = "x86";
    } else if (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64) {
        cpuArch = "ia64";
    } else {
        cpuArch = "unknown";
    }

    osType = "Windows";
    osVersion = verstr.str();
    _extraStats = bExtra.obj();

    // On Windows, `SOMAXCONN` is not a maximum value. Instead, it's a special
    // placeholder value (`2**31 - 1`) indicating that the runtime should
    // choose a suitable maximum value.
    // See <https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-listen>.
    defaultListenBacklog = SOMAXCONN;
}

}  // namespace mongo
