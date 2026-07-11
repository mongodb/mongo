// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/record_store.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/damage_vector.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

void CappedInsertNotifier::notifyAll() const {
    std::lock_guard<std::mutex> lk(_mutex);
    ++_version;
    _notifier.notify_all();
}

uint64_t CappedInsertNotifier::getVersion() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _version;
}

void CappedInsertNotifier::waitUntil(OperationContext* opCtx,
                                     uint64_t prevVersion,
                                     Date_t deadline) const {
    std::unique_lock<std::mutex> lk(_mutex);
    opCtx->waitForConditionOrInterruptUntil(_notifier, lk, deadline, [this, prevVersion]() {
        return _dead || prevVersion != _version;
    });
}

void CappedInsertNotifier::kill() {
    std::lock_guard<std::mutex> lk(_mutex);
    _dead = true;
    _notifier.notify_all();
}

bool CappedInsertNotifier::isDead() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _dead;
}

}  // namespace mongo
