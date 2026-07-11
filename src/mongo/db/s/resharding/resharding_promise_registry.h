// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"

#include <vector>

namespace mongo {

/**
 * Tracks all ReshardingPromise instances belonging to a single resharding participant
 * (coordinator, donor, or recipient).
 *
 * Document is the participant's state document type. recover() accepts a const Document&
 * so that each promise's recovery function can inspect the durably-recorded state and set its value
 * accordingly.
 *
 * Lifecycle:
 *   1. At construction time, each ReshardingPromise calls registerPromise().
 *   2. After the participant reconstructs after a failover, recover() is called once
 *      with the reloaded state document to fulfill promises whose milestones were
 *      already reached in the previous term.
 *   3. On any terminal error, setError() broadcasts the error to all remaining
 *      unfulfilled promises.
 *
 * Thread safety: registerPromise() must complete before any concurrent callers
 * invoke setError() or recover(). Declare the registry before the promises it
 * tracks so that C++ member initialization order guarantees this.
 */
template <typename Document>
class ReshardingPromiseRegistry {
public:
    using RecoveryFn = std::function<void(WithLock, const Document&)>;
    using ErrorFn = std::function<void(WithLock, Status)>;

    /**
     * Registers a promise's recovery and error callbacks. Called automatically by
     * ReshardingPromise's constructor — not intended for direct use.
     */
    void registerPromise(RecoveryFn recoveryFn, ErrorFn errorFn) {
        _entries.push_back({std::move(recoveryFn), std::move(errorFn)});
    }

    /**
     * Calls each registered recovery function with the provided document under the
     * caller's lock. Promises whose milestones have already been durably recorded
     * will be fulfilled immediately; unfulfilled promises are left pending.
     */
    void recover(WithLock lk, const Document& doc) {
        for (auto& entry : _entries) {
            entry.recoveryFn(lk, doc);
        }
    }

    /**
     * Propagates a terminal error to every registered promise that has not yet been
     * fulfilled.
     */
    void setError(WithLock lk, Status status) {
        for (auto& entry : _entries) {
            entry.errorFn(lk, status);
        }
    }

private:
    struct Entry {
        RecoveryFn recoveryFn;
        ErrorFn errorFn;
    };

    std::vector<Entry> _entries;
};

}  // namespace mongo
