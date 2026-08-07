// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/progress_meter.h"

#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/*
 * Thread-safe variant of ProgressMeterHolder. Workers call hit() lock-free via an atomic escrow
 * counter; the counter is flushed to the underlying ProgressMeter (under the parent client lock)
 * only when the accumulated hits cross a configurable threshold. finished() flushes any remaining
 * escrowed hits and marks the meter inactive.
 *
 * set() and get() carry the same client-lock requirements as ProgressMeterHolder.
 * finished() acquires the parent client lock internally and must not be called while holding it;
 * callers must ensure no threads are still calling hit().
 * hit() may be called concurrently from any thread without holding any lock.
 */
class ConcurrentProgressMeterHolder {
public:
    static constexpr int64_t kDefaultFlushThreshold = 100;

    ConcurrentProgressMeterHolder() = default;

    // Cannot copy
    ConcurrentProgressMeterHolder(const ConcurrentProgressMeterHolder&) = delete;
    ConcurrentProgressMeterHolder& operator=(const ConcurrentProgressMeterHolder&) = delete;

    ~ConcurrentProgressMeterHolder() {
        if (_pm) {
            finished();
        }
    }

    void set(WithLock,
             ProgressMeter& pm,
             OperationContext* opCtx,
             int64_t flushThreshold = kDefaultFlushThreshold);

    ProgressMeter* get(WithLock) {
        return _pm;
    }

    // Thread-safe. Accumulates n hits atomically; acquires the parent client lock and flushes
    // to the underlying ProgressMeter only when the escrow threshold is crossed.
    void hit(int64_t n = 1);

    // Must be called from the parent thread. Flushes remaining escrowed hits and marks finished.
    void finished();

private:
    void _flush();

    ProgressMeter* _pm{nullptr};
    OperationContext* _parentOpCtx{nullptr};
    Atomic<int> _pendingHits{0};
    int64_t _flushThreshold{kDefaultFlushThreshold};
};

}  // namespace mongo
