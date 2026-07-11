// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/resharding_promise_registry.h"
#include "mongo/util/future.h"

#include <utility>

namespace mongo {

/**
 * A SharedPromise wrapper that self-registers with a ReshardingPromiseRegistry.
 *
 * On construction, registers:
 *   - An auto-generated ErrorFn that forwards a terminal error to the inner
 *     SharedPromise when registry.setError() is called.
 *   - A caller-supplied RecoveryFn, invoked during registry.recover() after a
 *     failover. The RecoveryFn receives the participant's reloaded state document
 *     and is responsible for fulfilling this promise if its milestone was already
 *     durably recorded in the previous term.
 *
 * Double-fulfillment (calling emplaceValue or setError after an earlier fulfillment)
 * is silently ignored so that registry.setError() can safely broadcast to all
 * registered promises without checking each one's individual state.
 *
 * The WithLock parameter on emplaceValue and setError is expected to guard fulfillment:
 * callers must hold the associated lock when fulfilling, ensuring that the readiness
 * check and fulfillment are performed atomically with respect to other lock-holders.
 *
 * Non-copyable and non-movable: the auto-generated ErrorFn captures 'this', so
 * instances must reside at a stable address (direct class member, or unique_ptr).
 */
template <typename T>
class ReshardingPromise {
public:
    template <typename Document>
    ReshardingPromise(ReshardingPromiseRegistry<Document>& registry,
                      typename ReshardingPromiseRegistry<Document>::RecoveryFn recoveryFn);

    ReshardingPromise(const ReshardingPromise&) = delete;
    ReshardingPromise& operator=(const ReshardingPromise&) = delete;
    ReshardingPromise(ReshardingPromise&&) = delete;
    ReshardingPromise& operator=(ReshardingPromise&&) = delete;

    SharedSemiFuture<T> getFuture() const;

    template <typename... Args>
    void emplaceValue(WithLock, Args&&... args);

    void setError(WithLock, Status status);

private:
    SharedPromise<T> _promise;
};

template <typename T>
template <typename Document>
ReshardingPromise<T>::ReshardingPromise(
    ReshardingPromiseRegistry<Document>& registry,
    typename ReshardingPromiseRegistry<Document>::RecoveryFn recoveryFn) {
    registry.registerPromise(std::move(recoveryFn),
                             [this](WithLock lk, Status status) { setError(lk, status); });
}

template <typename T>
SharedSemiFuture<T> ReshardingPromise<T>::getFuture() const {
    return _promise.getFuture();
}

template <typename T>
template <typename... Args>
void ReshardingPromise<T>::emplaceValue(WithLock, Args&&... args) {
    if (!_promise.getFuture().isReady()) {
        _promise.emplaceValue(std::forward<Args>(args)...);
    }
}

template <typename T>
void ReshardingPromise<T>::setError(WithLock, Status status) {
    if (!_promise.getFuture().isReady()) {
        _promise.setError(std::move(status));
    }
}

}  // namespace mongo
