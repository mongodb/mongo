// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/thread.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo::unittest {

/**
 * A simple wrapper around stdx::thread that joins in destructor. ONLY FOR USE IN TESTS!
 *
 * TODO: add better integration with test framework so that you get useful diagnostics
 * if the thread never completes rather than just hanging. This is particularly common
 * when the test fails on the main thread prior to signalling this thread, so it will
 * need to handle an exception in flight when hitting the destructor.
 *
 * TODO: consider adding some sort of cancellation support like std::jthread.
 */
class JoinThread : public stdx::thread {
public:
    using stdx::thread::thread;

    explicit JoinThread(stdx::thread&& thread) : stdx::thread(std::move(thread)) {}

    JoinThread(JoinThread&&) = default;
    JoinThread& operator=(JoinThread&&) = default;

    ~JoinThread() {
        if (joinable())
            join();
    }
};

}  // namespace mongo::unittest
