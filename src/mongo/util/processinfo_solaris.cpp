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


#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

#ifndef _WIN32
#include <malloc.h>
#include <procfs.h>

#include <sys/lgrp_user.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/systeminfo.h>
#include <sys/utsname.h>
#endif

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/file.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

/**
 * Read the first line from a file; return empty string on failure
 */
static std::string readLineFromFile(const char* fname) {
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
        if (!f) {
            auto ec = lastSystemError();
            msgasserted(16846,
                        str::stream()
                            << "couldn't open \"/proc/self/psinfo\": " << errorMessage(ec));
        }
        size_t num = fread(&psinfo, sizeof(psinfo), 1, f);
        auto ec = lastSystemError();
        fclose(f);
        massert(16847,
                str::stream() << "couldn't read from \"/proc/self/psinfo\": " << errorMessage(ec),
                num == 1);
    }
    psinfo_t psinfo;
};

struct ProcUsage {
    ProcUsage() {
        FILE* f = fopen("/proc/self/usage", "r");
        if (!f) {
            auto ec = lastSystemError();
            msgasserted(
                16848, str::stream() << "couldn't open \"/proc/self/usage\": " << errorMessage(ec));
        }
        size_t num = fread(&prusage, sizeof(prusage), 1, f);
        auto ec = lastSystemError();
        fclose(f);
        massert(16849,
                str::stream() << "couldn't read from \"/proc/self/usage\": " << errorMessage(ec),
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
boost::optional<unsigned long> ProcessInfo::getNumCoresForProcess() {
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

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
    ProcUsage p;
    info.appendNumber("page_faults", static_cast<long long>(p.prusage.pr_majf));
}

bool checkNumaEnabled() {
    lgrp_cookie_t cookie = lgrp_init(LGRP_VIEW_OS);

    if (cookie == LGRP_COOKIE_NONE) {
        auto ec = lastSystemError();
        LOGV2_WARNING(23362,
                      "lgrp_init failed: {errnoWithDescription}",
                      "errnoWithDescription"_attr = errorMessage(ec));
        return false;
    }

    ON_BLOCK_EXIT([&] { lgrp_fini(cookie); });

    int groups = lgrp_nlgrps(cookie);

    if (groups == -1) {
        auto ec = lastSystemError();
        LOGV2_WARNING(23363,
                      "lgrp_nlgrps failed: {errnoWithDescription}",
                      "errnoWithDescription"_attr = errorMessage(ec));
        return false;
    }

    // NUMA machines have more then 1 locality group
    return groups > 1;
}

/**
 * Save a BSON obj representing the host system's details
 */
void ProcessInfo::SystemInfo::collectSystemInfo() {
    struct utsname unameData;
    if (uname(&unameData) == -1) {
        LOGV2(23356,
              "Unable to collect detailed system information: {strerror_errno}",
              "strerror_errno"_attr = strerror(errno));
    }

    char buf_64[32];
    char buf_native[32];
    if (sysinfo(SI_ARCHITECTURE_64, buf_64, sizeof(buf_64)) != -1 &&
        sysinfo(SI_ARCHITECTURE_NATIVE, buf_native, sizeof(buf_native)) != -1) {
        addrSize = str::equals(buf_64, buf_native) ? 64 : 32;
    } else {
        LOGV2(23357,
              "Unable to determine system architecture: {strerror_errno}",
              "strerror_errno"_attr = strerror(errno));
    }

    osType = unameData.sysname;
    osName = str::ltrim(readLineFromFile("/etc/release"));
    osVersion = unameData.version;
    pageSize = static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
    memSize = pageSize * static_cast<unsigned long long>(sysconf(_SC_PHYS_PAGES));
    memLimit = memSize;
    numCores = static_cast<unsigned>(sysconf(_SC_NPROCESSORS_CONF));
    cpuArch = unameData.machine;
    hasNuma = checkNumaEnabled();

    // We prefer FSync over msync, when:
    // 1. Pre-Oracle Solaris 11.2 releases
    // 2. Illumos kernel releases (which is all non Oracle Solaris releases)
    preferMsyncOverFSync = false;

    // The proper way to set `defaultListenBacklog` is to use `libkstat` to
    // look up the value of the [tcp_conn_req_max_q][1] tuning parameter.
    // That would require users of `ProcessInfo` to link `libkstat`.
    // Instead, use the compile-time constant `SOMAXCONN`.
    // [1]: https://docs.oracle.com/cd/E19159-01/819-3681/abeir/index.html
    defaultListenBacklog = SOMAXCONN;

    if (str::startsWith(osName, "Oracle Solaris")) {
        std::vector<std::string> versionComponents;
        str::splitStringDelim(osVersion, &versionComponents, '.');

        if (versionComponents.size() > 1) {
            unsigned majorInt, minorInt;
            Status majorStatus = NumberParser{}(versionComponents[0], &majorInt);

            Status minorStatus = NumberParser{}(versionComponents[1], &minorInt);

            if (!majorStatus.isOK() || !minorStatus.isOK()) {
                LOGV2_WARNING(23360,
                              "Could not parse OS version numbers from uname: {osVersion}",
                              "osVersion"_attr = osVersion);
            } else if ((majorInt == 11 && minorInt >= 2) || majorInt > 11) {
                preferMsyncOverFSync = true;
            }
        } else {
            LOGV2_WARNING(23361,
                          "Could not parse OS version string from uname: {osVersion}",
                          "osVersion"_attr = osVersion);
        }
    }

    BSONObjBuilder bExtra;
    bExtra.append("kernelVersion", unameData.release);
    bExtra.append("pageSize", static_cast<long long>(pageSize));
    bExtra.append("numPages", static_cast<int>(sysconf(_SC_PHYS_PAGES)));
    bExtra.append("maxOpenFiles", static_cast<int>(sysconf(_SC_OPEN_MAX)));
    _extraStats = bExtra.obj();
}

}  // namespace mongo
