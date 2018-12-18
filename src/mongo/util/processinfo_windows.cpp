// processinfo_win32.cpp


/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <bitset>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <iostream>
#include <psapi.h>

#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

using namespace std;
using std::unique_ptr;

namespace mongo {

// dynamically link to psapi.dll (in case this version of Windows
// does not support what we need)
struct PsApiInit {
    bool supported;
    typedef BOOL(WINAPI* pQueryWorkingSetEx)(HANDLE hProcess, PVOID pv, DWORD cb);
    pQueryWorkingSetEx QueryWSEx;

    PsApiInit() {
        HINSTANCE psapiLib = LoadLibrary(TEXT("psapi.dll"));
        if (psapiLib) {
            QueryWSEx =
                reinterpret_cast<pQueryWorkingSetEx>(GetProcAddress(psapiLib, "QueryWorkingSetEx"));
            if (QueryWSEx) {
                supported = true;
                return;
            }
        }
        supported = false;
    }
};

static PsApiInit* psapiGlobal = NULL;

int _wconvertmtos(SIZE_T s) {
    return (int)(s / (1024 * 1024));
}

ProcessInfo::ProcessInfo(ProcessId pid) {}

ProcessInfo::~ProcessInfo() {}

// get the number of CPUs available to the current process
boost::optional<unsigned long> ProcessInfo::getNumCoresForProcess() {
    DWORD_PTR process_mask, system_mask;

    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        std::bitset<sizeof(process_mask) * 8> mask(process_mask);
        if (mask.count() > 0)
            return mask.count();
    }
    return boost::none;
}

bool ProcessInfo::supported() {
    return true;
}

int ProcessInfo::getVirtualMemorySize() {
    MEMORYSTATUSEX mse;
    mse.dwLength = sizeof(mse);
    BOOL status = GlobalMemoryStatusEx(&mse);
    if (!status) {
        DWORD gle = GetLastError();
        error() << "GlobalMemoryStatusEx failed with " << errnoWithDescription(gle);
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
        DWORD gle = GetLastError();
        error() << "GetProcessMemoryInfo failed with " << errnoWithDescription(gle);
        fassert(28622, status);
    }

    return _wconvertmtos(pmc.WorkingSetSize);
}

double ProcessInfo::getSystemMemoryPressurePercentage() {
    MEMORYSTATUSEX mse;
    mse.dwLength = sizeof(mse);
    BOOL status = GlobalMemoryStatusEx(&mse);
    if (!status) {
        DWORD gle = GetLastError();
        error() << "GlobalMemoryStatusEx failed with " << errnoWithDescription(gle);
        fassert(28623, status);
    }

    DWORDLONG totalPageFile = mse.ullTotalPageFile;
    if (totalPageFile == 0) {
        return false;
    }

    // If the page file is >= 50%, say we are low on system memory
    // If the page file is >= 75%, we are running very low on system memory
    //
    DWORDLONG highWatermark = totalPageFile / 2;
    DWORDLONG veryHighWatermark = 3 * (totalPageFile / 4);

    DWORDLONG usedPageFile = mse.ullTotalPageFile - mse.ullAvailPageFile;

    // Below the watermark, we are fine
    // Also check we will not do a divide by zero below
    if (usedPageFile < highWatermark || veryHighWatermark <= highWatermark) {
        return 0.0;
    }

    // Above the high watermark, we tell MMapV1 how much to remap
    // < 1.0, we have some pressure, but not much so do not be very aggressive
    // 1.0 = we are at very high watermark, remap everything
    // > 1.0, the user may run out of memory, remap everything
    // i.e., Example (N - 50) / (75 - 50)
    return static_cast<double>(usedPageFile - highWatermark) / (veryHighWatermark - highWatermark);
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
        DWORD gle = GetLastError();
        warning() << "GetFileVersionInfoSizeA on " << filePath << " failed with "
                  << errnoWithDescription(gle);
        return false;
    }

    std::unique_ptr<char[]> verData(new char[verSize]);
    if (GetFileVersionInfoA(filePath, NULL, verSize, verData.get()) == 0) {
        DWORD gle = GetLastError();
        warning() << "GetFileVersionInfoSizeA on " << filePath << " failed with "
                  << errnoWithDescription(gle);
        return false;
    }

    UINT size;
    VS_FIXEDFILEINFO* verInfo;
    if (VerQueryValueA(verData.get(), "\\", (LPVOID*)&verInfo, &size) == 0) {
        DWORD gle = GetLastError();
        warning() << "VerQueryValueA on " << filePath << " failed with "
                  << errnoWithDescription(gle);
        return false;
    }

    if (size != sizeof(VS_FIXEDFILEINFO)) {
        warning() << "VerQueryValueA on " << filePath << " returned structure with unexpected size";
        return false;
    }

    fileVersionMS = verInfo->dwFileVersionMS;
    fileVersionLS = verInfo->dwFileVersionLS;
    return true;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {
    BSONObjBuilder bExtra;
    stringstream verstr;
    OSVERSIONINFOEX osvi;   // os version
    MEMORYSTATUSEX mse;     // memory stats
    SYSTEM_INFO ntsysinfo;  // system stats

    // get basic processor properties
    GetNativeSystemInfo(&ntsysinfo);
    addrSize = (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 64 : 32);
    numCores = ntsysinfo.dwNumberOfProcessors;
    pageSize = static_cast<unsigned long long>(ntsysinfo.dwPageSize);
    bExtra.append("pageSize", static_cast<long long>(pageSize));

    // get memory info
    mse.dwLength = sizeof(mse);
    if (GlobalMemoryStatusEx(&mse)) {
        memSize = mse.ullTotalPhys;
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

        osName = "Microsoft ";
        switch (osvi.dwMajorVersion) {
            case 10:
                if (osvi.wProductType == VER_NT_WORKSTATION)
                    osName += "Windows 10";
                else
                    osName += "Windows Server 2016";
                break;
            case 6:
                switch (osvi.dwMinorVersion) {
                    case 3:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows 8.1";
                        else
                            osName += "Windows Server 2012 R2";
                        break;
                    case 2:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows 8";
                        else
                            osName += "Windows Server 2012";
                        break;
                    case 1:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows 7";
                        else
                            osName += "Windows Server 2008 R2";
                        break;
                    case 0:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows Vista";
                        else
                            osName += "Windows Server 2008";
                        break;
                    default:
                        osName += "Windows NT version ";
                        osName += verstr.str();
                        break;
                }
                break;
            default:
                osName += "Windows";
                break;
        }
    } else {
        // unable to get any version data
        osName += "Windows NT";
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
    hasNuma = checkNumaEnabled();
    _extraStats = bExtra.obj();
    if (psapiGlobal == NULL) {
        psapiGlobal = new PsApiInit();
    }
}

bool ProcessInfo::checkNumaEnabled() {
    typedef BOOL(WINAPI * LPFN_GLPI)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

    DWORD returnLength = 0;
    DWORD numaNodeCount = 0;
    unique_ptr<SYSTEM_LOGICAL_PROCESSOR_INFORMATION[]> buffer;

    LPFN_GLPI glpi(reinterpret_cast<LPFN_GLPI>(
        GetProcAddress(GetModuleHandleW(L"kernel32"), "GetLogicalProcessorInformation")));
    if (glpi == NULL) {
        return false;
    }

    DWORD returnCode = 0;
    do {
        returnCode = glpi(buffer.get(), &returnLength);

        if (returnCode == FALSE) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buffer.reset(reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(
                    new BYTE[returnLength]));
            } else {
                DWORD gle = GetLastError();
                warning() << "GetLogicalProcessorInformation failed with "
                          << errnoWithDescription(gle);
                return false;
            }
        }
    } while (returnCode == FALSE);

    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = buffer.get();

    unsigned int byteOffset = 0;
    while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {
        if (ptr->Relationship == RelationNumaNode) {
            // Non-NUMA systems report a single record of this type.
            numaNodeCount++;
        }

        byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        ptr++;
    }

    // For non-NUMA machines, the count is 1
    return numaNodeCount > 1;
}

bool ProcessInfo::blockCheckSupported() {
    sysInfo();  // Initialize SystemInfo, which calls collectSystemInfo(), which creates
                // psapiGlobal.
    return psapiGlobal->supported;
}

bool ProcessInfo::blockInMemory(const void* start) {
#if 0
        // code for printing out page fault addresses and pc's --
        // this could be useful for targetting heavy pagefault locations in the code
        static BOOL bstat = InitializeProcessForWsWatch( GetCurrentProcess() );
        PSAPI_WS_WATCH_INFORMATION_EX wiex[30];
        DWORD bufsize =  sizeof(wiex);
        bstat = GetWsChangesEx( GetCurrentProcess(), &wiex[0], &bufsize );
        if (bstat) {
            for (int i=0; i<30; i++) {
                if (wiex[i].BasicInfo.FaultingPc == 0) break;
                cout << "faulting pc = " << wiex[i].BasicInfo.FaultingPc <<
                    " address = " << wiex[i].BasicInfo.FaultingVa <<
                    " thread id = " << wiex[i].FaultingThreadId << endl;
            }
        }
#endif
    PSAPI_WORKING_SET_EX_INFORMATION wsinfo;
    wsinfo.VirtualAddress = const_cast<void*>(start);
    BOOL result = psapiGlobal->QueryWSEx(GetCurrentProcess(), &wsinfo, sizeof(wsinfo));
    if (result)
        if (wsinfo.VirtualAttributes.Valid)
            return true;
    return false;
}

bool ProcessInfo::pagesInMemory(const void* start, size_t numPages, vector<char>* out) {
    out->resize(numPages);
    unique_ptr<PSAPI_WORKING_SET_EX_INFORMATION[]> wsinfo(
        new PSAPI_WORKING_SET_EX_INFORMATION[numPages]);

    const void* startOfFirstPage = alignToStartOfPage(start);
    for (size_t i = 0; i < numPages; i++) {
        wsinfo[i].VirtualAddress = reinterpret_cast<void*>(
            reinterpret_cast<unsigned long long>(startOfFirstPage) + i * getPageSize());
    }

    BOOL result = psapiGlobal->QueryWSEx(
        GetCurrentProcess(), wsinfo.get(), sizeof(PSAPI_WORKING_SET_EX_INFORMATION) * numPages);

    if (!result)
        return false;
    for (size_t i = 0; i < numPages; ++i) {
        (*out)[i] = wsinfo[i].VirtualAttributes.Valid ? 1 : 0;
    }
    return true;
}
}
