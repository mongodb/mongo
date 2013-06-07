/*    Copyright 2013 10gen Inc.
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

#include <boost/filesystem.hpp>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <procfs.h>
#include <stdio.h>
#include <string>
#include <sys/mman.h>
#include <sys/systeminfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include "mongo/util/file.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"

namespace mongo {

    /**
     * Read the first line from a file; return empty string on failure
     */
    static string readLineFromFile(const char* fname) {
        std::string fstr;
        std::ifstream f(fname);
        if (f.is_open()) {
            std::getline(f, fstr);
        }
        return fstr;
    }

    struct ProcPsinfo {
        ProcPsinfo() {
            FILE* f = fopen("/proc/self/psinfo", "r");
            massert(16846,
                    mongoutils::str::stream() << "couldn't open \"/proc/self/psinfo\": "
                                              << errnoWithDescription(), 
                    f);
            size_t num = fread(&psinfo, sizeof(psinfo), 1, f);
            int err = errno;
            fclose(f);
            massert(16847,
                    mongoutils::str::stream() << "couldn't read from \"/proc/self/psinfo\": "
                                              << errnoWithDescription(err), 
                    num == 1);
        }
       psinfo_t psinfo;
    };

    struct ProcUsage {
        ProcUsage() {
            FILE* f = fopen("/proc/self/usage", "r");
            massert(16848,
                    mongoutils::str::stream() << "couldn't open \"/proc/self/usage\": "
                                              << errnoWithDescription(), 
                    f);
            size_t num = fread(&prusage, sizeof(prusage), 1, f);
            int err = errno;
            fclose(f);
            massert(16849,
                    mongoutils::str::stream() << "couldn't read from \"/proc/self/usage\": "
                                              << errnoWithDescription(err), 
                    num == 1);
        }
       prusage_t prusage;
    };

    ProcessInfo::ProcessInfo(ProcessId pid) : _pid(pid) { }
    ProcessInfo::~ProcessInfo() { }

    bool ProcessInfo::supported() {
        return true;
    }

    int ProcessInfo::getVirtualMemorySize() {
        ProcPsinfo p;
        return static_cast<int>(p.psinfo.pr_size / 1024);
    }

    int ProcessInfo::getResidentSize() {
        ProcPsinfo p;
        return static_cast<int>(p.psinfo.pr_rssize / 1024);
    }

    void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
        ProcUsage p;
        info.appendNumber("page_faults", static_cast<long long>(p.prusage.pr_majf));
    }

    /**
     * Save a BSON obj representing the host system's details
     */
    void ProcessInfo::SystemInfo::collectSystemInfo() {
        struct utsname unameData;
        if (uname(&unameData) == -1) {
            log() << "Unable to collect detailed system information: " << strerror(errno) << endl;
        }

        char buf_64[32];
        char buf_native[32];
        if (sysinfo(SI_ARCHITECTURE_64, buf_64, sizeof(buf_64)) != -1 &&
            sysinfo(SI_ARCHITECTURE_NATIVE, buf_native, sizeof(buf_native)) != -1) {
            addrSize = mongoutils::str::equals(buf_64, buf_native) ? 64 : 32;
        }
        else {
            log() << "Unable to determine system architecture: " << strerror(errno) << endl;
        }

        osType = unameData.sysname;
        osName = mongoutils::str::ltrim(readLineFromFile("/etc/release"));
        osVersion = unameData.version;
        pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
        memSize = pageSize * static_cast<unsigned long long>(sysconf(_SC_PHYS_PAGES));
        numCores = static_cast<unsigned>(sysconf(_SC_NPROCESSORS_CONF));
        cpuArch = unameData.machine;
        hasNuma = checkNumaEnabled();

        BSONObjBuilder bExtra;
        bExtra.append("kernelVersion", unameData.release);
        bExtra.append("pageSize", static_cast<long long>(pageSize));
        bExtra.append("numPages", static_cast<int>(sysconf(_SC_PHYS_PAGES)));
        bExtra.append("maxOpenFiles", static_cast<int>(sysconf(_SC_OPEN_MAX)));
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
        if (mincore(static_cast<char*>(const_cast<void*>(alignToStartOfPage(start))),
                    getPageSize(),
                    &x)) {
            log() << "mincore failed: " << errnoWithDescription() << endl;
            return 1;
        }
        return x & 0x1;
    }

    bool ProcessInfo::pagesInMemory(const void* start, size_t numPages, std::vector<char>* out) {
        out->resize(numPages);
        if (mincore(static_cast<char*>(const_cast<void*>(alignToStartOfPage(start))),
                    numPages * getPageSize(),
                    &out->front())) {
            log() << "mincore failed: " << errnoWithDescription() << endl;
            return false;
        }
        for (size_t i = 0; i < numPages; ++i) {
            (*out)[i] &= 0x1;
        }
        return true;
    }

}
