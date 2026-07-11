// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <functional>
#include <string>

namespace mongo::stacktrace_details {

#ifdef __linux__

/** Calls `f(tid)` on each thread `tid` in this process except the calling thread. */
void iterateTids(const std::function<void(int)>& f);

/** Returns true if the given tid exists in this process. */
bool tidExists(int tid);

/** Returns the thread name for the given tid. */
std::string readThreadName(int tid);

/** Wrapper around the gettid system call. */
int getThreadId();

/** Wrapper around the tgkill system call. */
int terminateThread(int pid, int tid, int sig);
#endif  // __linux__

}  // namespace mongo::stacktrace_details
