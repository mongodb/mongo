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

#pragma once

#include <boost/optional.hpp>
#include <cstdint>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

class ProcessInfo {
public:
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
    static unsigned getNumCores() {
        return sysInfo().numCores;
    }

    /**
     * Get the number of physical CPUs
     */
    static unsigned getNumPhysicalCores() {
        return sysInfo().numPhysicalCores;
    }

    /**
     * Get the number of cores available. Make a best effort to get the cores for this process.
     * If that information is not available, get the total number of CPUs.
     */
    static unsigned long getNumAvailableCores() {
        return ProcessInfo::getNumCoresForProcess().value_or(ProcessInfo::getNumCores());
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
     * Determine if we need to workaround slow msync performance on Illumos/Solaris
     */
    static bool preferMsyncOverFSync() {
        return sysInfo().preferMsyncOverFSync;
    }

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
        unsigned long long pageSize;
        std::string cpuArch;
        bool hasNuma;
        BSONObj _extraStats;

        // On non-Solaris (ie, Linux, Darwin, *BSD) kernels, prefer msync.
        // Illumos kernels do O(N) scans in memory of the page table during msync which
        // causes high CPU, Oracle Solaris 11.2 and later modified ZFS to workaround mongodb
        // Oracle Solaris Bug:
        //  18658199 Speed up msync() on ZFS by 90000x with this one weird trick
        bool preferMsyncOverFSync;

        SystemInfo()
            : addrSize(0),
              memSize(0),
              memLimit(0),
              numCores(0),
              numPhysicalCores(0),
              pageSize(0),
              hasNuma(false),
              preferMsyncOverFSync(true) {
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

    static bool checkNumaEnabled();

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
    static boost::optional<unsigned long> getNumCoresForProcess();
};

bool writePidFile(const std::string& path);
}  // namespace mongo
