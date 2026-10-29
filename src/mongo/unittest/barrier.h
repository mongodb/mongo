// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mongo {
namespace unittest {

/**
 * Holds internal thread counter that is set on initialization.
 * This counter is decremented every time a thread enters countDownAndWait() and blocks.
 * All threads are unblocked when the counter reaches zero and the counter is reset.
 */
class [[MONGO_MOD_PUBLIC]] Barrier {
    Barrier(const Barrier&) = delete;
    Barrier& operator=(const Barrier&) = delete;

public:
    /**
     * Initializes barrier with a default thread count.
     */
    explicit Barrier(size_t threadCount);

    /**
     * Decrements thread counter by 1. If the thread counter is not zero, the thread blocks
     * until the counter reaches zero.
     */
    void countDownAndWait();

private:
    size_t _threadCount;
    size_t _threadsWaiting;
    uint64_t _generation;
    std::mutex _mutex;
    stdx::condition_variable _condition;
};

}  // namespace unittest
}  // namespace mongo
