// processinfo_darwin.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "../pch.h"
#include "processinfo.h"
#include "log.h"
#include <db/jsobj.h>

#include <mach/vm_statistics.h>
#include <mach/task_info.h>
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <mach/mach_traps.h>
#include <mach/task.h>
#include <mach/vm_map.h>
#include <mach/shared_region.h>
#include <iostream>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

using namespace std;

namespace mongo {

    ProcessInfo::ProcessInfo( ProcessId pid ) : _pid( pid ) {
    }

    ProcessInfo::~ProcessInfo() {
    }

    bool ProcessInfo::supported() {
        return true;
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
        mach_msg_type_number_t  count = TASK_BASIC_INFO_COUNT;
        if ( ( result = task_info( task , TASK_BASIC_INFO , (task_info_t)&ti, &count ) )  != KERN_SUCCESS ) {
            cout << "error getting task_info: " << result << endl;
            return 0;
        }
        return (int)((double)ti.virtual_size / (1024.0 * 1024 ) );
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
        mach_msg_type_number_t  count = TASK_BASIC_INFO_COUNT;
        if ( ( result = task_info( task , TASK_BASIC_INFO , (task_info_t)&ti, &count ) )  != KERN_SUCCESS ) {
            cout << "error getting task_info: " << result << endl;
            return 0;
        }
        return (int)( ti.resident_size / (1024 * 1024 ) );
    }

    void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
        struct task_events_info taskInfo;
        mach_msg_type_number_t taskInfoCount = TASK_EVENTS_INFO_COUNT;

        if ( KERN_SUCCESS != task_info(mach_task_self(), TASK_EVENTS_INFO, 
                                       (integer_t*)&taskInfo, &taskInfoCount) ) {
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
    Variant getSysctlByName( const char * sysctlName ) {
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
        value.resize(len);
        return value;
    }

    /**
     * Get a sysctl integer value by name (specialization)
     */
    template <>
    long long getSysctlByName< NumberVal > ( const char * sysctlName ) {
        long long value = 0;
        size_t len = sizeof(value);
        if ( sysctlbyname(sysctlName, &value, &len, NULL, 0) < 0 ) {
            log() << "Unable to resolve sysctl " << sysctlName << " (number) " << endl;
        }
        if (len > 8) {
            log() << "Unable to resolve sysctl " << sysctlName << " as integer.  System returned " << len << " bytes." << endl;
        }
        return value;
    }

    void ProcessInfo::SystemInfo::collectSystemInfo() {
        osType = "Darwin";
        osName = "Mac OS X";
        osVersion = getSysctlByName< string >( "kern.osrelease");
        addrSize = (getSysctlByName< NumberVal >( "hw.cpu64bit_capable" ) ? 64 : 32);
        memSize = getSysctlByName< NumberVal >( "hw.memsize" );
        numCores = getSysctlByName< NumberVal >( "hw.ncpu" ); // includes hyperthreading cores
        pageSize = static_cast<unsigned long long>(sysconf( _SC_PAGESIZE ));
        cpuArch = getSysctlByName< string >( "hw.machine" );
        hasNuma = checkNumaEnabled();
        
        BSONObjBuilder bExtra;
        bExtra.append( "versionString", getSysctlByName< string >( "kern.version" ) );
        bExtra.append( "alwaysFullSync", static_cast< int >( getSysctlByName< NumberVal >( "vfs.generic.always_do_fullfsync" ) ) );
        bExtra.append( "nfsAsync", static_cast< int >( getSysctlByName< NumberVal >( "vfs.generic.nfs.client.allow_async" ) ) );
        bExtra.append( "model", getSysctlByName< string >( "hw.model" ) );
        bExtra.append( "physicalCores", static_cast< int >( getSysctlByName< NumberVal >( "machdep.cpu.core_count" ) ) );
        bExtra.append( "cpuFrequencyMHz", static_cast< int >( (getSysctlByName< NumberVal >( "hw.cpufrequency" ) / (1000 * 1000)) ) );
        bExtra.append( "cpuString", getSysctlByName< string >( "machdep.cpu.brand_string" ) );
        bExtra.append( "cpuFeatures", getSysctlByName< string >( "machdep.cpu.features" ) + string(" ") + 
                                      getSysctlByName< string >( "machdep.cpu.extfeatures" ) );
        bExtra.append( "pageSize", static_cast< int >( getSysctlByName< NumberVal >( "hw.pagesize" ) ) );
        bExtra.append( "scheduler", getSysctlByName< string >( "kern.sched" ) );
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
