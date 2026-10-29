// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/rwmutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * WithLock is an attestation to pass as an argument to functions that must be called only while
 * holding a lock, as a rigorous alternative to an unchecked naming convention and/or stern
 * comments.  It helps prevent a common usage error.
 *
 * It may be used to modernize code from (something like) this
 *
 *     // Member _mutex MUST be held when calling this:
 *     void _clobber_inlock(OperationContext* opCtx) {
 *         _stuff = makeStuff(opCtx);
 *     }
 *
 * into
 *
 *     void _clobber(WithLock, OperationContext* opCtx) {
 *         _stuff = makeStuff(opCtx);
 *     }
 *
 * A call to such a function looks like this:
 *
 *     std::lock_guard<std::mutex> lk(_mutex);
 *     _clobber(lk, opCtx);  // instead of _clobber_inlock(opCtx)
 *
 * Note that the formal argument need not (and should not) be named unless it is needed to pass
 * the attestation along to another function:
 *
 *     void _clobber(WithLock lock, OperationContext* opCtx) {
 *         _really_clobber(lock, opCtx);
 *     }
 *
 */
struct WithLock {
    template <typename LatchT>
    explicit(false) WithLock(const std::lock_guard<LatchT>&) {}

    template <typename LatchT>
    explicit(false) WithLock(const std::unique_lock<LatchT>& lock) {
        invariant(lock.owns_lock());
    }

    template <bool lockExclusively>
    explicit(false) WithLock(const WriteRarelyRWMutex::ScopedLock<lockExclusively>& lock) {
        invariant(lock.owns_lock());
    }

    WithLock(const WithLock&) = default;
    WithLock(WithLock&&) = default;

    void operator=(const WithLock&) = delete;

    /*
     * Produces a WithLock without benefit of any actual lock, for use in cases where a lock is not
     * really needed, such as in many (but not all!) constructors.
     */
    static WithLock withoutLock() {
        return {};
    }

private:
    WithLock() = default;
};

}  // namespace mongo
