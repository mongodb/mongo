// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/repl_sync_shared_data.h"

namespace mongo {
namespace repl {

void ReplSyncSharedData::lock() {
    _mutex.lock();
}

void ReplSyncSharedData::unlock() {
    _mutex.unlock();
}

Status ReplSyncSharedData::getStatus(WithLock lk) {
    return _status;
}

void ReplSyncSharedData::setStatusIfOK(WithLock lk, Status newStatus) {
    if (_status.isOK())
        _status = newStatus;
}

}  // namespace repl
}  // namespace mongo
