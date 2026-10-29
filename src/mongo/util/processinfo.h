// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"

#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] ProcessInfo {
public:
    static auto constexpr kTranparentHugepageDirectory = "/sys/kernel/mm/transparent_hugepage";
    static auto constexpr kGlibcTunableEnvVar = "GLIBC_TUNABLES";
    static auto constexpr kRseqKey = "glibc.pthread.rseq";

    ProcessInfo(ProcessId pid = ProcessId::getCurrent());
    ~ProcessInfo();

    /**
     * @return mbytes
     */
    int getVirtualMemorySize();

    /**
     * @return mbytes
     */
    int getResidentSize();

    /**
     * Get the type of os (e.g. Windows, Linux, Mac OS)
     */
    static const std::string& getOsType() {
        return sysInfo().osType;
    }

    /**
     * Get the os Name (e.g. Ubuntu, Gentoo, Windows Server 2008)
     */
    static const std::string& getOsName() {
        return sysInfo().osName;
    }

    /**
     * Get the os version (e.g. 10.04, 11.3.0, 6.1 (build 7600))
     */
    static const std::string& getOsVersion() {
        return sysInfo().osVersion;
    }

    /**
     * Get the cpu address size (e.g. 32, 36, 64)
     */
    static unsigned getAddrSize() {
        return sysInfo().addrSize;
    }

    /**
     * Get the size of total memory available to the process in bytes
     */
    static unsigned long long getMemSizeBytes() {
        return sysInfo().memLimit;
    }

    /**
     * Get the size of total memory available to the process in MB
     */
    static unsigned long long getMemSizeMB() {
        return sysInfo().memLimit / (1024 * 1024);
    }

    /**
     * Get the total memory available on the machine in MB
     */
    static unsigned long long getSystemMemSizeMB() {
        return sysInfo().memSize / (1024 * 1024);
    }

    /**
     * Get the number of (logical) CPUs
     */
    static unsigned getNumLogicalCores() {
        return sysInfo().numCores;
    }

    /**
     * Get the number of physical CPUs
     */
    static unsigned getNumPhysicalCores() {
        return sysInfo().numPhysicalCores;
    }

    /**
     * Get the number of CPU sockets
     */
    static unsigned getNumCpuSockets() {
        return sysInfo().numCpuSockets;
    }

    /**
     * Get the number of cores available. Make a best effort to get the cores for this process.
     * If that information is not available, get the total number of CPUs.
     */
    static uint64_t getNumAvailableCores() {
        return ProcessInfo::getNumCoresForProcess().value_or(ProcessInfo::getNumLogicalCores());
    }

    /**
     * Get the number of cores available for process or return the errorValue.
     */
    static int64_t getNumCoresAvailableToProcess(int64_t errorValue = -1) {
        const auto cores = ProcessInfo::getNumCoresForProcess();
        return cores ? static_cast<int64_t>(cores.value()) : errorValue;
    }

    /**
     * Get the system page size in bytes.
     */
    static unsigned long long getPageSize() {
        return sysInfo().pageSize;
    }

    /**
     * Get the CPU architecture (e.g. x86, x86_64)
     */
    static const std::string& getArch() {
        return sysInfo().cpuArch;
    }

    /**
     * Determine if NUMA is enabled (interleaved) for this process
     */
    static bool hasNumaEnabled() {
        return sysInfo().hasNuma;
    }

    /**
     * Get the number of NUMA nodes if NUMA is enabled, or 1 otherwise.
     */
    static uint64_t getNumNumaNodes() {
        if (sysInfo().hasNuma) {
            return sysInfo().numNumaNodes;
        }
        return 1;
    }

    /**
     * Determine if we need to workaround slow msync performance on Illumos/Solaris
     */
    static bool preferMsyncOverFSync() {
        return sysInfo().preferMsyncOverFSync;
    }

    /**
     * Transparent hugepage files display settings like so, with the selected setting in brackets:
     *      always defer [defer+madvise] madvise never
     *
     * This function parses out the selected setting from this file format.
     */
    static StatusWith<std::string> readTransparentHugePagesParameter(
        std::string_view parameter, std::string_view directory = kTranparentHugepageDirectory);

    /**
     * Check whether the environment variable GLIBC_TUNABLES=glibc.pthread.rseq=0 is correctly set.
     */
    static bool checkGlibcRseqTunable();

    /**
     * Get extra system stats
     */
    void appendSystemDetails(BSONObjBuilder& details) const {
        details.appendElements(sysInfo()._extraStats);
    }

    /**
     * Append platform-specific data to obj
     */
    void getExtraInfo(BSONObjBuilder& info);

    bool supported();

    static const std::string& getProcessName() {
        return appInfo().getProcessName();
    }

    static int getDefaultListenBacklog() {
        return sysInfo().defaultListenBacklog;
    }

private:
    /**
     * Host and operating system info.  Does not change over time.
     */
    class SystemInfo {
    public:
        std::string osType;
        std::string osName;
        std::string osVersion;
        unsigned addrSize;
        unsigned long long memSize;
        unsigned long long memLimit;
        unsigned numCores;
        unsigned numPhysicalCores;
        unsigned numCpuSockets;
        unsigned long long pageSize;
        std::string cpuArch;
        bool hasNuma;
        unsigned numNumaNodes;
        BSONObj _extraStats;

        // On non-Solaris (ie, Linux, Darwin, *BSD) kernels, prefer msync.
        // Illumos kernels do O(N) scans in memory of the page table during msync which
        // causes high CPU, Oracle Solaris 11.2 and later modified ZFS to workaround mongodb
        // Oracle Solaris Bug:
        //  18658199 Speed up msync() on ZFS by 90000x with this one weird trick
        bool preferMsyncOverFSync;

        int defaultListenBacklog;

        SystemInfo()
            : addrSize(0),
              memSize(0),
              memLimit(0),
              numCores(0),
              numPhysicalCores(0),
              numCpuSockets(0),
              pageSize(0),
              hasNuma(false),
              numNumaNodes(0),
              preferMsyncOverFSync(true),
              defaultListenBacklog(0) {
            // populate SystemInfo during construction
            collectSystemInfo();
        }

    private:
        /** Collect host system info */
        void collectSystemInfo();
    };

    class ApplicationInfo {
    public:
        void init(const std::vector<std::string>& argv) {
            invariant(!_isInitialized);
            _isInitialized = true;
            if (!argv.empty()) {
                _processName = argv[0];
            }
        }
        const std::string& getProcessName() const {
            return _processName;
        }

    private:
        bool _isInitialized = false;
        std::string _processName;
    };

    ProcessId _pid;

    static const SystemInfo& sysInfo() {
        static ProcessInfo::SystemInfo systemInfo;
        return systemInfo;
    }

public:
    static ApplicationInfo& appInfo() {
        static StaticImmortal<ApplicationInfo> applicationInfo{};
        return applicationInfo.value();
    }

private:
    /**
     * Get the number of available CPUs. Depending on the OS, the number can be the
     * number of available CPUs to the current process or scheduler.
     */
    static boost::optional<uint64_t> getNumCoresForProcess();
};

bool writePidFile(const std::string& path);
}  // namespace mongo
