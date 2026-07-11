// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"

#include <utility>

namespace mongo::resharding {

/**
 * Idempotent helpers for fulfilling a SharedPromise. Each overload no-ops when the promise has
 * already been fulfilled, so call sites that may race with other fulfillment paths (e.g.
 * step-up recovery vs. main flow) can call these without checking readiness themselves.
 *
 * The WithLock parameter documents that the caller must already hold the mutex that guards the
 * promise.
 */
inline void ensureFulfilledPromise(WithLock, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

inline void ensureFulfilledPromise(WithLock, SharedPromise<void>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(std::move(error));
    }
}

template <typename T>
void ensureFulfilledPromise(WithLock, SharedPromise<T>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(std::move(error));
    }
}

template <class T>
void ensureFulfilledPromise(WithLock, SharedPromise<T>& sp, T value) {
    auto future = sp.getFuture();
    if (!future.isReady()) {
        sp.emplaceValue(std::move(value));
    } else {
        tassert(12559800,
                "Attempted to fulfill an already-fulfilled SharedPromise with a different value",
                future.get() == value);
    }
}

}  // namespace mongo::resharding
