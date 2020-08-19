/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/initial_sync_shared_data.h"

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
