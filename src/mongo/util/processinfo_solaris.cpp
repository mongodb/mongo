/*    Copyright 2013 10gen Inc.
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

#include <boost/filesystem.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <procfs.h>
#include <stdio.h>
#include <string>
#include <sys/lgrp_user.h>
#include <sys/mman.h>
#include <sys/systeminfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stringutils.h"

using namespace std;

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
    ProcPsinfo p;
    return static_cast<int>(p.psinfo.pr_size / 1024);
}

int ProcessInfo::getResidentSize() {
    ProcPsinfo p;
    return static_cast<int>(p.psinfo.pr_rssize / 1024);
}

double ProcessInfo::getSystemMemoryPressurePercentage() {
    return 0.0;
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
    } else {
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

    // We prefer FSync over msync, when:
    // 1. Pre-Oracle Solaris 11.2 releases
    // 2. Illumos kernel releases (which is all non Oracle Solaris releases)
    preferMsyncOverFSync = false;

    if (mongoutils::str::startsWith(osName, "Oracle Solaris")) {
        std::vector<std::string> versionComponents;
        splitStringDelim(osVersion, &versionComponents, '.');

        if (versionComponents.size() > 1) {
            unsigned majorInt, minorInt;
            Status majorStatus = parseNumberFromString<unsigned>(versionComponents[0], &majorInt);

            Status minorStatus = parseNumberFromString<unsigned>(versionComponents[1], &minorInt);

            if (!majorStatus.isOK() || !minorStatus.isOK()) {
                warning() << "Could not parse OS version numbers from uname: " << osVersion;
            } else if ((majorInt == 11 && minorInt >= 2) || majorInt > 11) {
                preferMsyncOverFSync = true;
            }
        } else {
            warning() << "Could not parse OS version string from uname: " << osVersion;
        }
    }

    BSONObjBuilder bExtra;
    bExtra.append("kernelVersion", unameData.release);
    bExtra.append("pageSize", static_cast<long long>(pageSize));
    bExtra.append("numPages", static_cast<int>(sysconf(_SC_PHYS_PAGES)));
    bExtra.append("maxOpenFiles", static_cast<int>(sysconf(_SC_OPEN_MAX)));
    _extraStats = bExtra.obj();
}

bool ProcessInfo::checkNumaEnabled() {
    lgrp_cookie_t cookie = lgrp_init(LGRP_VIEW_OS);

    if (cookie == LGRP_COOKIE_NONE) {
        warning() << "lgrp_init failed: " << errnoWithDescription();
        return false;
    }

    ON_BLOCK_EXIT(lgrp_fini, cookie);

    int groups = lgrp_nlgrps(cookie);

    if (groups == -1) {
        warning() << "lgrp_nlgrps failed: " << errnoWithDescription();
        return false;
    }

    // NUMA machines have more then 1 locality group
    return groups > 1;
}

bool ProcessInfo::blockCheckSupported() {
    return true;
}

bool ProcessInfo::blockInMemory(const void* start) {
    char x = 0;
    if (mincore(
            static_cast<char*>(const_cast<void*>(alignToStartOfPage(start))), getPageSize(), &x)) {
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
