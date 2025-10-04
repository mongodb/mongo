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


#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <mach/mach_init.h>
#include <mach/mach_traps.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

ProcessInfo::ProcessInfo(ProcessId pid) : _pid(pid) {}

ProcessInfo::~ProcessInfo() {}

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

int ProcessInfo::getVirtualMemorySize() {
    task_t result;

    mach_port_t task;

    if ((result = task_for_pid(mach_task_self(), _pid.toNative(), &task)) != KERN_SUCCESS) {
        LOGV2(677702, "error getting task");
        return 0;
    }

#if !defined(__LP64__)
    task_basic_info_32 ti;
#else
    task_basic_info_64 ti;
#endif
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if ((result = task_info(task, TASK_BASIC_INFO, (task_info_t)&ti, &count)) != KERN_SUCCESS) {
        LOGV2(677703, "error getting task_info", "result"_attr = result);
        return 0;
    }
    return (int)((double)ti.virtual_size / (1024.0 * 1024));
}

int ProcessInfo::getResidentSize() {
    task_t result;

    mach_port_t task;

    if ((result = task_for_pid(mach_task_self(), _pid.toNative(), &task)) != KERN_SUCCESS) {
        LOGV2(577704, "error getting task");
        return 0;
    }


#if !defined(__LP64__)
    task_basic_info_32 ti;
#else
    task_basic_info_64 ti;
#endif
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if ((result = task_info(task, TASK_BASIC_INFO, (task_info_t)&ti, &count)) != KERN_SUCCESS) {
        LOGV2(677705, "error getting task_info", "result"_attr = result);
        return 0;
    }
    return (int)(ti.resident_size / (1024 * 1024));
}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
    struct task_events_info taskInfo;
    mach_msg_type_number_t taskInfoCount = TASK_EVENTS_INFO_COUNT;

    if (KERN_SUCCESS !=
        task_info(mach_task_self(), TASK_EVENTS_INFO, (integer_t*)&taskInfo, &taskInfoCount)) {
        LOGV2(677706, "error getting extra task_info");
        return;
    }

    info.append("page_faults", taskInfo.pageins);
}

/**
 * Get a sysctl string value by name.  Use string specialization by default.
 */
typedef long long NumberVal;
template <typename Variant>
Variant getSysctlByName(const char* sysctlName) {
    std::string value;
    size_t len;
    int status;
    // NB: sysctlbyname is called once to determine the buffer length, and once to copy
    //     the sysctl value.  Retry if the buffer length grows between calls.
    do {
        status = sysctlbyname(sysctlName, nullptr, &len, nullptr, 0);
        if (status == -1)
            break;
        value.resize(len);
        status = sysctlbyname(sysctlName, &*value.begin(), &len, nullptr, 0);
    } while (status == -1 && errno == ENOMEM);
    if (status == -1) {
        // unrecoverable error from sysctlbyname
        LOGV2(23351, "{sysctlName} unavailable", "sysctlName"_attr = sysctlName);
        return "";
    }

    // Drop any trailing NULL bytes by constructing Variant from a C string.
    return value.c_str();
}

/**
 * Get a sysctl integer value by name (specialization)
 */
template <>
long long getSysctlByName<NumberVal>(const char* sysctlName) {
    long long value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(sysctlName, &value, &len, nullptr, 0) < 0) {
        LOGV2(23352,
              "Unable to resolve sysctl {sysctlName} (number) ",
              "sysctlName"_attr = sysctlName);
    }
    if (len > 8) {
        LOGV2(23353,
              "Unable to resolve sysctl {sysctlName} as integer.  System returned {len} bytes.",
              "sysctlName"_attr = sysctlName,
              "len"_attr = len);
    }
    return value;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {
    osType = "Darwin";
    osName = "Mac OS X";
    osVersion = getSysctlByName<std::string>("kern.osrelease");
    addrSize = (getSysctlByName<NumberVal>("hw.cpu64bit_capable") ? 64 : 32);
    memSize = getSysctlByName<NumberVal>("hw.memsize");
    memLimit = memSize;
    numCores = getSysctlByName<NumberVal>("hw.ncpu");  // includes hyperthreading cores
    numPhysicalCores = getSysctlByName<NumberVal>("machdep.cpu.core_count");
    numCpuSockets = getSysctlByName<NumberVal>("hw.packages");
    pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
    cpuArch = getSysctlByName<std::string>("hw.machine");
    hasNuma = false;

    // Darwin doesn't have a sysctl field for maximum listen() backlog size.
    // `man listen` also notes:
    //
    // > BUGS
    // >      The backlog is currently limited (silently) to 128.
    //
    // `SOMAXCONN` is 128.
    defaultListenBacklog = SOMAXCONN;

    BSONObjBuilder bExtra;
    bExtra.append("versionString", getSysctlByName<std::string>("kern.version"));
    bExtra.append("alwaysFullSync",
                  static_cast<int>(getSysctlByName<NumberVal>("vfs.generic.always_do_fullfsync")));
    bExtra.append(
        "nfsAsync",
        static_cast<int>(getSysctlByName<NumberVal>("vfs.generic.nfs.client.allow_async")));
    bExtra.append("model", getSysctlByName<std::string>("hw.model"));
    bExtra.append("cpuString", getSysctlByName<std::string>("machdep.cpu.brand_string"));
    bExtra.append("pageSize", static_cast<int>(getSysctlByName<NumberVal>("hw.pagesize")));
    bExtra.append("scheduler", getSysctlByName<std::string>("kern.sched"));
    _extraStats = bExtra.obj();
}

}  // namespace mongo
