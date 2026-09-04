// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/stacktrace_details.h"

#ifdef __linux__

#include "mongo/base/parse_number.h"
#include "mongo/logv2/log.h"

#include <unistd.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/optional.hpp>
#include <sys/syscall.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

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
    boost::filesystem::directory_iterator iter{taskDir()};
    for (const auto& entry : iter) {
        int tid;
        if (!NumberParser{}(entry.path().filename().string(), &tid).isOK()) {
            LOGV2_WARNING(13424300,
                          "Failed to parse thread id from procfs entry, skipping",
                          "entry"_attr = entry.path().string());
            continue;
        }
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
