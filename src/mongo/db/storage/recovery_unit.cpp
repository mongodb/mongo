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

#include "mongo/db/storage/recovery_unit.h"

#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/scopeguard.h"

#include <exception>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicWord<unsigned long long> nextSnapshotId{1};

SnapshotId getNextSnapshotId() {
    return SnapshotId(nextSnapshotId.fetchAndAdd(1));
}
}  // namespace

const RecoveryUnit::OpenSnapshotOptions RecoveryUnit::kDefaultOpenSnapshotOptions =
    RecoveryUnit::OpenSnapshotOptions();

void RecoveryUnit::ensureSnapshot() {
    if (!_snapshot) {
        _snapshot.emplace(getNextSnapshotId());
    }
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
    validateInUnitOfWork();
    _changes.push_back(std::move(change));
}

void RecoveryUnit::registerChangeForTwoPhaseDrop(std::unique_ptr<Change> change) {
    validateInUnitOfWork();
    _changesForTwoPhaseDrop.push_back(std::move(change));
}

void RecoveryUnit::registerChangeForCatalogVisibility(std::unique_ptr<Change> change) {
    validateInUnitOfWork();
    _changesForCatalogVisibility.push_back(std::move(change));
}

void RecoveryUnit::commitRegisteredChanges(boost::optional<Timestamp> commitTimestamp) {
    // Getting to this method implies `runPreCommitHooks` completed successfully, resulting in
    // having its contents cleared.
    invariant(_preCommitHooks.empty());
    _executeCommitHandlers(commitTimestamp);
}

void RecoveryUnit::beginUnitOfWork(bool readOnly) {
    _readOnly = readOnly;
    if (!_readOnly) {
        doBeginUnitOfWork();
    }
}

void RecoveryUnit::commitUnitOfWork() {
    if (_readOnly) {
        _readOnly = false;
        commitRegisteredChanges(boost::none);
        return;
    }

    runPreCommitHooks(_opCtx);

    doCommitUnitOfWork();
    resetSnapshot();
}

void RecoveryUnit::abortUnitOfWork() {
    if (_readOnly) {
        _readOnly = false;
        abortRegisteredChanges();
        return;
    }

    doAbortUnitOfWork();
    resetSnapshot();
}

void RecoveryUnit::abandonSnapshot() {
    doAbandonSnapshot();
    resetSnapshot();
}

void RecoveryUnit::setOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
}

void RecoveryUnit::_executeCommitHandlers(boost::optional<Timestamp> commitTimestamp) {
    invariant(_opCtx ||
              (_changes.empty() && _changesForCatalogVisibility.empty() &&
               _changesForTwoPhaseDrop.empty()));
    bool debugLoggingThreeEnabled =
        logv2::shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(3));
    for (auto& change : _changesForTwoPhaseDrop) {
        // Log at higher level because commits occur far more frequently than rollbacks.
        if (debugLoggingThreeEnabled) {
            LOGV2_DEBUG(7789501,
                        3,
                        "Custom commit",
                        "changeName"_attr = redact(demangleName(typeid(*change))));
        }
        change->commit(_opCtx, commitTimestamp);
    }
    for (auto& change : _changesForCatalogVisibility) {
        // Log at higher level because commits occur far more frequently than rollbacks.
        if (debugLoggingThreeEnabled) {
            LOGV2_DEBUG(5255701,
                        3,
                        "Custom commit.",
                        "changeName"_attr = redact(demangleName(typeid(*change))));
        }
        change->commit(_opCtx, commitTimestamp);
    }
    for (auto& change : _changes) {
        // Log at higher level because commits occur far more frequently than rollbacks.
        if (debugLoggingThreeEnabled) {
            LOGV2_DEBUG(22244,
                        3,
                        "Custom commit.",
                        "changeName"_attr = redact(demangleName(typeid(*change))));
        }
        change->commit(_opCtx, commitTimestamp);
    }
    _changes.clear();
    _changesForCatalogVisibility.clear();
    _changesForTwoPhaseDrop.clear();
}

void RecoveryUnit::abortRegisteredChanges() {
    _preCommitHooks.clear();
    _executeRollbackHandlers();
}
void RecoveryUnit::_executeRollbackHandlers() {
    // Make sure we have an OperationContext when executing rollback handlers. Unless we have no
    // handlers to run, which might be the case in unit tests.
    invariant(_opCtx ||
              (_changes.empty() && _changesForCatalogVisibility.empty() &&
               _changesForTwoPhaseDrop.empty()));
    bool debugLoggingTwoEnabled =
        logv2::shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(2));
    for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend(); it != end;
         ++it) {
        Change* change = it->get();
        if (debugLoggingTwoEnabled) {
            LOGV2_DEBUG(22245,
                        2,
                        "Custom rollback",
                        "changeName"_attr = redact(demangleName(typeid(*change))));
        }
        change->rollback(_opCtx);
    }

    for (Changes::const_reverse_iterator it = _changesForTwoPhaseDrop.rbegin(),
                                         end = _changesForTwoPhaseDrop.rend();
         it != end;
         ++it) {
        Change* change = it->get();
        if (debugLoggingTwoEnabled) {
            LOGV2_DEBUG(7789502,
                        2,
                        "Custom rollback",
                        "changeName"_attr = redact(demangleName(typeid(*change))));
        }
        change->rollback(_opCtx);
    }

    for (Changes::const_reverse_iterator it = _changesForCatalogVisibility.rbegin(),
                                         end = _changesForCatalogVisibility.rend();
         it != end;
         ++it) {
        Change* change = it->get();
        if (debugLoggingTwoEnabled) {
            LOGV2_DEBUG(5255702,
                        2,
                        "Custom rollback",
                        "changeName"_attr = redact(demangleName(typeid(*change))));
        }
        change->rollback(_opCtx);
    }

    _changesForTwoPhaseDrop.clear();
    _changesForCatalogVisibility.clear();
    _changes.clear();
}

void RecoveryUnit::_setState(State newState) {
    _state = newState;
}

void RecoveryUnit::validateInUnitOfWork() const {
    invariant(_inUnitOfWork() || _readOnly,
              fmt::format("state: {}, readOnly: {}", toString(_getState()), _readOnly));
}

void RecoveryUnit::setIsolation(Isolation isolation) {
    if (isolation == _isolation) {
        return;
    }

    tassert(10775301, "Cannot change isolation level on an active recovery unit", !isActive());
    _setIsolation(isolation);
    _isolation = isolation;
}

StorageWriteTransaction::StorageWriteTransaction(RecoveryUnit& ru, bool readOnly) : _ru(ru) {
    // Cannot nest
    invariant(!_ru.inUnitOfWork());
    _ru.beginUnitOfWork(readOnly);
}

StorageWriteTransaction::~StorageWriteTransaction() {
    if (!_committed && !_aborted) {
        abort();
    }
}

void StorageWriteTransaction::prepare() {
    invariant(!_aborted);
    invariant(!_committed);
    invariant(!_prepared);

    _ru.prepareUnitOfWork();
    _prepared = true;
}

void StorageWriteTransaction::commit() {
    invariant(!_aborted);
    invariant(!_committed);

    _ru.commitUnitOfWork();
    _committed = true;
}

void StorageWriteTransaction::abort() {
    invariant(!_aborted);
    invariant(!_committed);

    _ru.abortUnitOfWork();
    _aborted = true;
}

}  // namespace mongo
