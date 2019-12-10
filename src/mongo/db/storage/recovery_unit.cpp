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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicWord<unsigned long long> nextSnapshotId{1};
}  // namespace

RecoveryUnit::RecoveryUnit() {
    assignNextSnapshotId();
}

void RecoveryUnit::assignNextSnapshotId() {
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
}

void RecoveryUnit::registerPreCommitHook(std::function<void(OperationContext*)> callback) {
    _preCommitHooks.push_back(std::move(callback));
}

void RecoveryUnit::runPreCommitHooks(OperationContext* opCtx) {
    ON_BLOCK_EXIT([&] { _preCommitHooks.clear(); });
    for (auto& hook : _preCommitHooks) {
        hook(opCtx);
    }
}

void RecoveryUnit::registerChange(std::unique_ptr<Change> change) {
    invariant(_inUnitOfWork(), toString(getState()));
    _changes.push_back(std::move(change));
}

void RecoveryUnit::commitRegisteredChanges(boost::optional<Timestamp> commitTimestamp) {
    // Getting to this method implies `runPreCommitHooks` completed successfully, resulting in
    // having its contents cleared.
    invariant(_preCommitHooks.empty());
    for (auto& change : _changes) {
        try {
            // Log at higher level because commits occur far more frequently than rollbacks.
            LOG(3) << "CUSTOM COMMIT " << redact(demangleName(typeid(*change)));
            change->commit(commitTimestamp);
        } catch (...) {
            std::terminate();
        }
    }
    _changes.clear();
}

void RecoveryUnit::abortRegisteredChanges() {
    _preCommitHooks.clear();
    try {
        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = it->get();
            LOG(2) << "CUSTOM ROLLBACK " << redact(demangleName(typeid(*change)));
            change->rollback();
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }
}
}  // namespace mongo
