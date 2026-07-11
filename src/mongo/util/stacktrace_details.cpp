// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/stacktrace_details.h"

#ifdef __linux__

#include "mongo/base/parse_number.h"

#include <unistd.h>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

namespace mongo::stacktrace_details {
namespace {

boost::filesystem::path taskDir() {
    return boost::filesystem::path("/proc/self/task");
}

}  // namespace

int getThreadId() {
    return ::syscall(SYS_gettid);
}

int terminateThread(int pid, int tid, int sig) {
    return syscall(SYS_tgkill, pid, tid, sig);
}

void iterateTids(const std::function<void(int)>& f) {
    int selfTid = getThreadId();
    auto iter = boost::filesystem::directory_iterator{taskDir()};
    for (const auto& entry : iter) {
        int tid;
        if (!NumberParser{}(entry.path().filename().string(), &tid).isOK())
            continue;  // Ignore non-integer names (e.g. "." or "..").
        if (tid == selfTid)
            continue;  // skip the current thread
        f(tid);
    }
}

bool tidExists(int tid) {
    return exists(taskDir() / std::to_string(tid));
}

std::string readThreadName(int tid) {
    std::string threadName;
    try {
        boost::filesystem::ifstream in(taskDir() / std::to_string(tid) / "comm");
        std::getline(in, threadName);
    } catch (...) {
    }
    return threadName;
}

}  // namespace mongo::stacktrace_details

#endif  // __linux__
