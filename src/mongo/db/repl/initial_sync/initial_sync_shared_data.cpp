// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"

#include "mongo/util/assert_util.h"

#include <ratio>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
int InitialSyncSharedData::incrementRetryingOperations(WithLock lk) {
    if (_retryingOperationsCount++ == 0) {
        _syncSourceUnreachableSince = getClock()->now();
    }
    return _retryingOperationsCount;
}

int InitialSyncSharedData::decrementRetryingOperations(WithLock lk) {
    invariant(_retryingOperationsCount > 0);
    if (--_retryingOperationsCount == 0) {
        _totalTimeUnreachable += (getClock()->now() - _syncSourceUnreachableSince);
        _syncSourceUnreachableSince = Date_t();
    }
    return _retryingOperationsCount;
}

bool InitialSyncSharedData::shouldRetryOperation(WithLock lk, RetryableOperation* retryableOp) {
    if (!*retryableOp) {
        retryableOp->emplace(this);
        incrementRetryingOperations(lk);
    }
    invariant((**retryableOp).getSharedData() == this);
    auto outageDuration = getCurrentOutageDuration(lk);
    if (outageDuration <= getAllowedOutageDuration(lk)) {
        incrementTotalRetries(lk);
        return true;
    } else {
        (**retryableOp).release(lk);
        *retryableOp = boost::none;
        return false;
    }
}
Milliseconds InitialSyncSharedData::getTotalTimeUnreachable(WithLock lk) {
    return _totalTimeUnreachable +
        ((_retryingOperationsCount > 0) ? getClock()->now() - _syncSourceUnreachableSince
                                        : Milliseconds::zero());
}

Milliseconds InitialSyncSharedData::getCurrentOutageDuration(WithLock lk) {
    return ((_retryingOperationsCount > 0) ? getClock()->now() - _syncSourceUnreachableSince
                                           : Milliseconds::min());
}

}  // namespace repl
}  // namespace mongo
