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
#include <vm/vm_param.h>

#include "mongo/util/scopeguard.h"
#include "processinfo.h"

namespace mongo {

    ProcessInfo::ProcessInfo(pid_t pid) : _pid( pid ) {
    }

    ProcessInfo::~ProcessInfo() {
    }

    static const uintptr_t pageSizeBytes = sysconf(_SC_PAGESIZE);

    /**
     * Get a sysctl string value by name.  Use string specialization by default.
     */
    template <typename T>
    int getSysctlByNameWithDefault(const char* sysctlName,
                                   const T& defaultValue,
                                   T* result);

    template <>
    int getSysctlByNameWithDefault<uintptr_t>(const char* sysctlName,
                                              const uintptr_t& defaultValue,
                                              uintptr_t* result) {
        uintptr_t value;
        size_t len = sizeof(value);
        if (sysctlbyname(sysctlName, &value, &len, NULL, 0) == -1) {
            *result = defaultValue;
            return errno;
        }
        if (len != sizeof(value)) {
            *result = defaultValue;
            return EINVAL;
        }

        *result = value;
        return 0;
    }

    template <>
    int getSysctlByNameWithDefault<string>(const char* sysctlName,
                                           const string& defaultValue,
                                           string* result) {
        char value[256];
        size_t len = sizeof(value);
        if (sysctlbyname(sysctlName, &value, &len, NULL, 0) == -1) {
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
        kvm_t *kd;
        int cnt;
        char err[_POSIX2_LINE_MAX];
        if ((kd = kvm_open(NULL, "/dev/null", "/dev/null", O_RDONLY, err)) == NULL)
            return -1;
        kinfo_proc * task = kvm_getprocs(kd, KERN_PROC_PID, _pid, &cnt);
        kvm_close(kd);
        return task->ki_size / 1024 / 1024; // convert from bytes to MB
    }

    int ProcessInfo::getResidentSize() {
        kvm_t *kd;
        int cnt;
        char err[_POSIX2_LINE_MAX];
        if ((kd = kvm_open(NULL, "/dev/null", "/dev/null", O_RDONLY, err)) == NULL)
            return -1;
        kinfo_proc * task = kvm_getprocs(kd, KERN_PROC_PID, _pid, &cnt);
        kvm_close(kd);
        return task->ki_rssize * sysconf( _SC_PAGESIZE ) / 1024 / 1024; // convert from pages to MB
    }

    void ProcessInfo::SystemInfo::collectSystemInfo() {
        osType = "BSD";
        osName = "FreeBSD";

        int status = getSysctlByNameWithDefault("kern.version", string("unknown"), &osVersion);
        if (status != 0)
            log() << "Unable to collect OS Version. (errno: " 
                  << errno << " msg: " << strerror(errno) << ")" << endl;

        status = getSysctlByNameWithDefault("hw.machine_arch", string("unknown"), &cpuArch);
        if (status != 0)
            log() << "Unable to collect Machine Architecture. (errno: "
                  << errno << " msg: " << strerror(errno) << ")" << endl;
        addrSize = cpuArch.find("64") != std::string::npos ? 64 : 32;

        uintptr_t numBuffer;
        uintptr_t defaultNum = 1;
        status = getSysctlByNameWithDefault("hw.physmem", defaultNum, &numBuffer);
        memSize = numBuffer;
        if (status != 0)
            log() << "Unable to collect Physical Memory. (errno: "
                  << errno << " msg: " << strerror(errno) << ")" << endl;

        status = getSysctlByNameWithDefault("hw.ncpu", defaultNum, &numBuffer);
        numCores = numBuffer;
        if (status != 0)
            log() << "Unable to collect Number of CPUs. (errno: "
                  << errno << " msg: " << strerror(errno) << ")" << endl;

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

    inline char* getPageAddress(char* addr) {
        return reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(addr) & ~(pageSizeBytes - 1));
    }

    bool ProcessInfo::blockInMemory( char * start ) {
         start = getPageAddress(start);

         char x = 0;
         if ( mincore( start , 128 , &x ) ) {
             log() << "mincore failed: " << errnoWithDescription() << endl;
             return 1;
         }
         return x & 0x1;
    }

}
