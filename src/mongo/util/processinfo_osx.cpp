// processinfo_darwin.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>

#include <iostream>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_traps.h>
#include <mach/shared_region.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <mach/vm_map.h>
#include <mach/vm_statistics.h>

#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"

using namespace std;

namespace mongo {

ProcessInfo::ProcessInfo(ProcessId pid) : _pid(pid) {}

ProcessInfo::~ProcessInfo() {}

bool ProcessInfo::supported() {
    return true;
}

// get the number of CPUs available to the scheduler
boost::optional<unsigned long> ProcessInfo::getNumAvailableCores() {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs)
        return nprocs;
    return boost::none;
}

int ProcessInfo::getVirtualMemorySize() {
    task_t result;

    mach_port_t task;

    if ((result = task_for_pid(mach_task_self(), _pid.toNative(), &task)) != KERN_SUCCESS) {
        cout << "error getting task\n";
        return 0;
    }

#if !defined(__LP64__)
    task_basic_info_32 ti;
#else
    task_basic_info_64 ti;
#endif
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if ((result = task_info(task, TASK_BASIC_INFO, (task_info_t)&ti, &count)) != KERN_SUCCESS) {
        cout << "error getting task_info: " << result << endl;
        return 0;
    }
    return (int)((double)ti.virtual_size / (1024.0 * 1024));
}

int ProcessInfo::getResidentSize() {
    task_t result;

    mach_port_t task;

    if ((result = task_for_pid(mach_task_self(), _pid.toNative(), &task)) != KERN_SUCCESS) {
        cout << "error getting task\n";
        return 0;
    }


#if !defined(__LP64__)
    task_basic_info_32 ti;
#else
    task_basic_info_64 ti;
#endif
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if ((result = task_info(task, TASK_BASIC_INFO, (task_info_t)&ti, &count)) != KERN_SUCCESS) {
        cout << "error getting task_info: " << result << endl;
        return 0;
    }
    return (int)(ti.resident_size / (1024 * 1024));
}

double ProcessInfo::getSystemMemoryPressurePercentage() {
    return 0.0;
}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
    struct task_events_info taskInfo;
    mach_msg_type_number_t taskInfoCount = TASK_EVENTS_INFO_COUNT;

    if (KERN_SUCCESS !=
        task_info(mach_task_self(), TASK_EVENTS_INFO, (integer_t*)&taskInfo, &taskInfoCount)) {
        cout << "error getting extra task_info" << endl;
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
    string value;
    size_t len;
    int status;
    // NB: sysctlbyname is called once to determine the buffer length, and once to copy
    //     the sysctl value.  Retry if the buffer length grows between calls.
    do {
        status = sysctlbyname(sysctlName, NULL, &len, NULL, 0);
        if (status == -1)
            break;
        value.resize(len);
        status = sysctlbyname(sysctlName, &*value.begin(), &len, NULL, 0);
    } while (status == -1 && errno == ENOMEM);
    if (status == -1) {
        // unrecoverable error from sysctlbyname
        log() << sysctlName << " unavailable" << endl;
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
    if (sysctlbyname(sysctlName, &value, &len, NULL, 0) < 0) {
        log() << "Unable to resolve sysctl " << sysctlName << " (number) " << endl;
    }
    if (len > 8) {
        log() << "Unable to resolve sysctl " << sysctlName << " as integer.  System returned "
              << len << " bytes." << endl;
    }
    return value;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {
    osType = "Darwin";
    osName = "Mac OS X";
    osVersion = getSysctlByName<string>("kern.osrelease");
    addrSize = (getSysctlByName<NumberVal>("hw.cpu64bit_capable") ? 64 : 32);
    memSize = getSysctlByName<NumberVal>("hw.memsize");
    numCores = getSysctlByName<NumberVal>("hw.ncpu");  // includes hyperthreading cores
    pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
    cpuArch = getSysctlByName<string>("hw.machine");
    hasNuma = checkNumaEnabled();

    BSONObjBuilder bExtra;
    bExtra.append("versionString", getSysctlByName<string>("kern.version"));
    bExtra.append("alwaysFullSync",
                  static_cast<int>(getSysctlByName<NumberVal>("vfs.generic.always_do_fullfsync")));
    bExtra.append(
        "nfsAsync",
        static_cast<int>(getSysctlByName<NumberVal>("vfs.generic.nfs.client.allow_async")));
    bExtra.append("model", getSysctlByName<string>("hw.model"));
    bExtra.append("physicalCores",
                  static_cast<int>(getSysctlByName<NumberVal>("machdep.cpu.core_count")));
    bExtra.append(
        "cpuFrequencyMHz",
        static_cast<int>((getSysctlByName<NumberVal>("hw.cpufrequency") / (1000 * 1000))));
    bExtra.append("cpuString", getSysctlByName<string>("machdep.cpu.brand_string"));
    bExtra.append("cpuFeatures",
                  getSysctlByName<string>("machdep.cpu.features") + string(" ") +
                      getSysctlByName<string>("machdep.cpu.extfeatures"));
    bExtra.append("pageSize", static_cast<int>(getSysctlByName<NumberVal>("hw.pagesize")));
    bExtra.append("scheduler", getSysctlByName<string>("kern.sched"));
    _extraStats = bExtra.obj();
}

bool ProcessInfo::checkNumaEnabled() {
    return false;
}

bool ProcessInfo::blockCheckSupported() {
    return true;
}

bool ProcessInfo::blockInMemory(const void* start) {
    char x = 0;
    if (mincore(alignToStartOfPage(start), getPageSize(), &x)) {
        log() << "mincore failed: " << errnoWithDescription() << endl;
        return 1;
    }
    return x & 0x1;
}

bool ProcessInfo::pagesInMemory(const void* start, size_t numPages, vector<char>* out) {
    out->resize(numPages);
    if (mincore(alignToStartOfPage(start), numPages * getPageSize(), &out->front())) {
        log() << "mincore failed: " << errnoWithDescription() << endl;
        return false;
    }
    for (size_t i = 0; i < numPages; ++i) {
        (*out)[i] &= 0x1;
    }
    return true;
}
}
