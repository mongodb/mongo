/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#ifdef __linux__
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"

#include "mongo/util/sysprofile.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo::sysprofile {
void runProfiler(StringData profile_name, PerfMode mode, StringData parentPid) {
    const std::string perfBinary = "/usr/bin/perf";
    const std::string perfName = perfBinary.substr(perfBinary.find_last_of('/') + 1);
    // Clear the signal mask set from mongod so that perf can handle SIGINT properly.
    clearSignalMask();
    // Redirect stdout/stderr to /dev/null of child process.
    int console = open("/dev/null", O_RDWR);
    tassert(8387203, "Error in opening /dev/null", console > 0);
    tassert(8387204, "Can't redirect perf stdout", dup2(console, STDOUT_FILENO) != -1);
    tassert(8387205, "Can't redirect perf stderr", dup2(console, STDERR_FILENO) != -1);
    switch (mode) {
        case PerfMode::record: {
            quickExit(execl(perfBinary.c_str(),
                            perfName.c_str(),
                            "record",
                            "-g",
                            "-o",
                            profile_name.rawData(),
                            "-p",
                            parentPid.rawData(),
                            nullptr));
        } break;
        case PerfMode::counters: {
            quickExit(execl(perfBinary.c_str(),
                            perfName.c_str(),
                            "stat",
                            "-e",
                            "cycles,cache-misses,cache-references,L1-dcache-loads,L1-"
                            "dcache-load-misses,"
                            "context-switches,cpu-migrations,page-faults,branch-"
                            "instructions,branch-misses,"
                            "dTLB-load-misses,dTLB-loads",
                            "-o",
                            profile_name.rawData(),
                            "-p",
                            parentPid.rawData(),
                            nullptr));
        } break;
    }
}

pid_t spawn(StringData filename, PerfMode mode) {
    std::stringstream pidStream;
    pidStream << getpid();
    auto pidString = pidStream.str();

    auto profile_name = filename + ((mode == PerfMode::record) ? ".data" : ".txt");
    pid_t pid = fork();
    switch (pid) {
        case -1:
            LOGV2_ERROR(8387201, "Failed to fork.", "errno"_attr = errno);
            break;
        case 0:
            // Child process.
            runProfiler(profile_name, mode, pidString.c_str());
            break;
        default:
            LOGV2(8387202, "A child process is forked for perf.", "pid"_attr = pid);
            break;
    }
    return pid;
}

bool stop(pid_t pid) {
    LOGV2(8387206, "Sending a SIGINT signal to child process.", "pid"_attr = pid);
    return kill(pid, SIGINT) == 0 && waitpid(pid, nullptr, 0) == pid;
}
}  // namespace mongo::sysprofile
#endif
