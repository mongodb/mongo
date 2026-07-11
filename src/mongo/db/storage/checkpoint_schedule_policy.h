// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_clock_source.h"

#include <functional>
#include <memory>
#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Abstract interface governing when the Checkpointer thread wakes up to take a checkpoint.
 * The Checkpointer calls waitUntilReady() each loop iteration and then takes a checkpoint.
 */
class [[MONGO_MOD_OPEN]] CheckpointSchedulePolicy {
public:
    virtual ~CheckpointSchedulePolicy() = default;

    /**
     * Blocks until the next checkpoint should be taken. shouldWake() returns true when the
     * Checkpointer has been asked to shut down or take an immediate checkpoint.
     */
    virtual void waitUntilReady(std::unique_lock<std::mutex>& lock,
                                stdx::condition_variable& cv,
                                std::function<bool()> shouldWake) = 0;

    /**
     * Called by Checkpointer::notifyOplogWrite() on every oplog write. Returns true the first time
     * accumulated bytes cross the volume threshold in a given checkpoint cycle, signalling that the
     * Checkpointer should wake the checkpoint thread via notify_one(). Returns false on all
     * subsequent calls in the same cycle.
     *
     * Threading: may be called concurrently from multiple oplog writer threads. Implementations
     * must be lock-free or otherwise thread-safe. No Checkpointer locks are held at call time.
     */
    virtual bool accumulateOplogBytes(int64_t bytes) = 0;
};

/**
 * Returns a CheckpointSchedulePolicy that wakes on the fixed interval governed by
 * storageGlobalParams.syncdelay.
 */
std::unique_ptr<CheckpointSchedulePolicy> createFixedIntervalPolicy(
    ClockSource* clock = SystemClockSource::get());

}  // namespace mongo
