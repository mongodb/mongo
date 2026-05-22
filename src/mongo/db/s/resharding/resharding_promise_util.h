/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
