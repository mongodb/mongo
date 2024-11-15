/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/thread_util.h"

#ifdef __linux__

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <unistd.h>

#include "mongo/base/parse_number.h"

namespace mongo {
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

}  // namespace mongo

#endif  // __linux__
