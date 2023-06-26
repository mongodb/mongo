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


#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <kvm.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/vmmeter.h>
#include <vm/vm_param.h>
#endif

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

ProcessInfo::ProcessInfo(ProcessId pid) : _pid(pid) {}

ProcessInfo::~ProcessInfo() {}

/**
 * Get a sysctl string value by name.  Use string specialization by default.
 */
template <typename T>
int getSysctlByNameWithDefault(const char* sysctlName, const T& defaultValue, T* result);

template <>
int getSysctlByNameWithDefault<uintptr_t>(const char* sysctlName,
                                          const uintptr_t& defaultValue,
                                          uintptr_t* result) {
    uintptr_t value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(sysctlName, &value, &len, NULL, 0) == -1) {
        *result = defaultValue;
        return errno;
    }
    if (len > sizeof(value)) {
        *result = defaultValue;
        return EINVAL;
    }

    *result = value;
    return 0;
}

template <>
int getSysctlByNameWithDefault<std::string>(const char* sysctlName,
                                            const std::string& defaultValue,
                                            std::string* result) {
    char value[256] = {0};
    size_t len = sizeof(value);
    if (sysctlbyname(sysctlName, &value, &len, NULL, 0) == -1) {
        *result = defaultValue;
        return errno;
    }
    *result = value;
    return 0;
}

int ProcessInfo::getVirtualMemorySize() {
    kvm_t* kd = NULL;
    int cnt = 0;
    char err[_POSIX2_LINE_MAX] = {0};
    if ((kd = kvm_open(NULL, "/dev/null", "/dev/null", O_RDONLY, err)) == NULL)
        return -1;
    kinfo_proc* task = kvm_getprocs(kd, KERN_PROC_PID, _pid.toNative(), &cnt);
    int vss = task->ki_size / 1024 / 1024;  // convert from bytes to MB
    kvm_close(kd);
    return vss;
}

int ProcessInfo::getResidentSize() {
    kvm_t* kd = NULL;
    int cnt = 0;
    char err[_POSIX2_LINE_MAX] = {0};
    if ((kd = kvm_open(NULL, "/dev/null", "/dev/null", O_RDONLY, err)) == NULL)
        return -1;
    kinfo_proc* task = kvm_getprocs(kd, KERN_PROC_PID, _pid.toNative(), &cnt);
    int rss = task->ki_rssize * sysconf(_SC_PAGESIZE) / 1024 / 1024;  // convert from pages to MB
    kvm_close(kd);
    return rss;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {
    osType = "BSD";
    osName = "FreeBSD";

    int status = getSysctlByNameWithDefault("kern.version", std::string("unknown"), &osVersion);
    if (status != 0)
        LOGV2(23332,
              "Unable to collect OS Version. (errno: {errno} msg: {msg})",
              "Unable to collect OS Version.",
              "errno"_attr = status,
              "msg"_attr = strerror(status));

    status = getSysctlByNameWithDefault("hw.machine_arch", std::string("unknown"), &cpuArch);
    if (status != 0)
        LOGV2(23333,
              "Unable to collect Machine Architecture. (errno: {errno} msg: {msg})",
              "Unable to collect Machine Architecture.",
              "errno"_attr = status,
              "msg"_attr = strerror(status));
    addrSize = cpuArch.find("64") != std::string::npos ? 64 : 32;

    uintptr_t numBuffer;
    uintptr_t defaultNum = 1;
    status = getSysctlByNameWithDefault("hw.physmem", defaultNum, &numBuffer);
    memSize = numBuffer;
    memLimit = memSize;
    if (status != 0)
        LOGV2(23334,
              "Unable to collect Physical Memory. (errno: {errno} msg: {msg})",
              "Unable to collect Physical Memory.",
              "errno"_attr = status,
              "msg"_attr = strerror(status));

    status = getSysctlByNameWithDefault("hw.ncpu", defaultNum, &numBuffer);
    numCores = numBuffer;
    if (status != 0)
        LOGV2(23335,
              "Unable to collect Number of CPUs. (errno: {errno} msg: {msg})",
              "Unable to collect Number of CPUs.",
              "errno"_attr = status,
              "msg"_attr = strerror(status));

    pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));

    hasNuma = false;
}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {}

bool ProcessInfo::supported() {
    return true;
}

// get the number of CPUs available to the scheduler
boost::optional<unsigned long> ProcessInfo::getNumCoresForProcess() {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs)
        return nprocs;
    return boost::none;
}
}  // namespace mongo
