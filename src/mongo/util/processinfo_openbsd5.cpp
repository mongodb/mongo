/*    Copyright 2012 10gen Inc.
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

#include <cstdlib>
#include <string>

#include <kvm.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/vmmeter.h>
#include <unistd.h>

#include "mongo/util/scopeguard.h"
#include "processinfo.h"

namespace mongo {

    ProcessInfo::ProcessInfo(ProcessId pid) : _pid( pid ) {
    }

    ProcessInfo::~ProcessInfo() {
    }

    /**
     * Get a sysctl string value by name.  Use string specialization by default.
     */
    template <typename T>
    int getSysctlByIDWithDefault(const int *sysctlID, const int idLen,
                                   const T& defaultValue,
                                   T* result);

    template <>
    int getSysctlByIDWithDefault<uintptr_t>(const int *sysctlID,
                                              const int idLen,
                                              const uintptr_t& defaultValue,
                                              uintptr_t* result) {
        uintptr_t value = 0;
        size_t len = sizeof(value);
        if (sysctl(sysctlID, idLen, &value, &len, NULL, 0) == -1) {
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
    int getSysctlByIDWithDefault<string>(const int *sysctlID,
                                         const int idLen,
                                           const string& defaultValue,
                                           string* result) {
        char value[256] = {0};
        size_t len = sizeof(value);
        if (sysctl(sysctlID, idLen, &value, &len, NULL, 0) == -1) {
            *result = defaultValue;
            return errno;
        }
        *result = value;
        return 0;
    }

    bool ProcessInfo::checkNumaEnabled() {
        return false;
    }

    int ProcessInfo::getVirtualMemorySize() {
        kvm_t *kd = NULL;
        int cnt = 0;
        char err[_POSIX2_LINE_MAX] = {0};
        if ((kd = kvm_openfiles(NULL, NULL, NULL, KVM_NO_FILES, err)) == NULL) {
            log() << "Unable to get virt mem size: " << err << endl;
            return -1;
        }

        kinfo_proc * task = kvm_getprocs(kd, KERN_PROC_PID, _pid.toNative(),
                sizeof(kinfo_proc), &cnt);
        kvm_close(kd);
        return ((task->p_vm_dsize + task->p_vm_ssize + task->p_vm_tsize) *
               sysconf( _SC_PAGESIZE )) / 1048576;
    }

    int ProcessInfo::getResidentSize() {
        kvm_t *kd = NULL;
        int cnt = 0;
        char err[_POSIX2_LINE_MAX] = {0};
        if ((kd = kvm_openfiles(NULL, NULL, NULL, KVM_NO_FILES, err)) == NULL) {
            log() << "Unable to get res mem size: " << err << endl;
            return -1;
        }
        kinfo_proc * task = kvm_getprocs(kd, KERN_PROC_PID, _pid.toNative(),
                sizeof(kinfo_proc), &cnt);
        kvm_close(kd);
        return (task->p_vm_rssize * sysconf( _SC_PAGESIZE )) / 1048576; // convert from pages to MB
    }

    void ProcessInfo::SystemInfo::collectSystemInfo() {
        osType = "BSD";
        osName = "OpenBSD";
        int mib[2];

        mib[0] = CTL_KERN;
        mib[1] = KERN_VERSION;
        int status = getSysctlByIDWithDefault(mib, 2, string("unknown"), &osVersion);
        if (status != 0)
            log() << "Unable to collect OS Version. (errno: " 
                  << status << " msg: " << strerror(status) << ")" << endl;

        mib[0] = CTL_HW;
        mib[1] = HW_MACHINE;
        status = getSysctlByIDWithDefault(mib, 2, string("unknown"), &cpuArch);
        if (status != 0)
            log() << "Unable to collect Machine Architecture. (errno: "
                  << status << " msg: " << strerror(status) << ")" << endl;
        addrSize = cpuArch.find("64") != std::string::npos ? 64 : 32;

        uintptr_t numBuffer;
        uintptr_t defaultNum = 1;
        mib[0] = CTL_HW;
        mib[1] = HW_PHYSMEM;
        status = getSysctlByIDWithDefault(mib, 2, defaultNum, &numBuffer);
        memSize = numBuffer;
        if (status != 0)
            log() << "Unable to collect Physical Memory. (errno: "
                  << status << " msg: " << strerror(status) << ")" << endl;

        mib[0] = CTL_HW;
        mib[1] = HW_NCPU;
        status = getSysctlByIDWithDefault(mib, 2, defaultNum, &numBuffer);
        numCores = numBuffer;
        if (status != 0)
            log() << "Unable to collect Number of CPUs. (errno: "
                  << status << " msg: " << strerror(status) << ")" << endl;

        pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));

        hasNuma = checkNumaEnabled();
    }

    void ProcessInfo::getExtraInfo( BSONObjBuilder& info ) {
    }

    bool ProcessInfo::supported() {
        return true;
    }

    bool ProcessInfo::blockCheckSupported() {
        return true;
    }

    bool ProcessInfo::blockInMemory(const void* start) {
         char x = 0;
         if (mincore((void*)alignToStartOfPage(start), getPageSize(), &x)) {
             log() << "mincore failed: " << errnoWithDescription() << endl;
             return 1;
         }
         return x & 0x1;
    }

    bool ProcessInfo::pagesInMemory(const void* start, size_t numPages, vector<char>* out) {
        out->resize(numPages);
        // int mincore(const void *addr, size_t len, char *vec);
        if (mincore((void*)alignToStartOfPage(start), numPages * getPageSize(),
                    &(out->front()))) {
            log() << "mincore failed: " << errnoWithDescription() << endl;
            return false;
        }
        for (size_t i = 0; i < numPages; ++i) {
            (*out)[i] = 0x1;
        }
        return true;
    }
}
