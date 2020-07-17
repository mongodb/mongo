/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <mutex>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/oplog_hack.h"

namespace mongo {
namespace ephemeral_for_test {

RecoveryUnit::RecoveryUnit(KVEngine* parentKVEngine, std::function<void()> cb)
    : _waitUntilDurableCallback(cb), _KVEngine(parentKVEngine) {}

RecoveryUnit::~RecoveryUnit() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _abort();
}

void RecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _setState(State::kInactiveInUnitOfWork);
}

void RecoveryUnit::doCommitUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));

    if (_dirty) {
        invariant(_forked);
        while (true) {
            auto masterInfo = _KVEngine->getMasterInfo();
            try {
                invariant(_mergeBase);
                _workingCopy.merge3(*_mergeBase, *masterInfo.second);
            } catch (const merge_conflict_exception&) {
                throw WriteConflictException();
            }

            if (_KVEngine->trySwapMaster(_workingCopy, masterInfo.first)) {
                // Merged successfully
                break;
            } else {
                // Retry the merge, but update the mergeBase since some progress was made merging.
                _mergeBase = masterInfo.second;
            }
        }
        _forked = false;
        _dirty = false;
    } else if (_forked) {
        if (kDebugBuild)
            invariant(*_mergeBase == _workingCopy);
    }

    _setState(State::kCommitting);
    commitRegisteredChanges(boost::none);
    _setState(State::kInactive);
}

void RecoveryUnit::doAbortUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));
    _abort();
}

bool RecoveryUnit::waitUntilDurable(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    invariant(!opCtx->lockState()->isLocked() || storageGlobalParams.repair);
    return true;  // This is an in-memory storage engine.
}

Status RecoveryUnit::obtainMajorityCommittedSnapshot() {
    return Status::OK();
}

void RecoveryUnit::prepareUnitOfWork() {
    invariant(_inUnitOfWork());
}

void RecoveryUnit::doAbandonSnapshot() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _forked = false;
    _dirty = false;
    _setMergeNull();
}

bool RecoveryUnit::forkIfNeeded() {
    if (_forked)
        return false;

    // Update the copies of the trees when not in a WUOW so cursors can retrieve the latest data.
    auto masterInfo = _KVEngine->getMasterInfo();
    _mergeBase = masterInfo.second;
    _workingCopy = *masterInfo.second;
    invariant(_mergeBase);

    // Call cleanHistory in case _mergeBase was holding a shared_ptr to an older tree.
    _KVEngine->cleanHistory();
    _forked = true;
    return true;
}

Status RecoveryUnit::setTimestamp(Timestamp timestamp) {
    auto key = oploghack::keyForOptime(timestamp);
    if (!key.isOK())
        return key.getStatus();

    _KVEngine->visibilityManager()->reserveRecord(this, key.getValue());
    return Status::OK();
}
void RecoveryUnit::setOrderedCommit(bool orderedCommit) {}

void RecoveryUnit::_abort() {
    _forked = false;
    _dirty = false;
    _setMergeNull();
    _setState(State::kAborting);
    abortRegisteredChanges();
    _setState(State::kInactive);
}

void RecoveryUnit::_setMergeNull() {
    _mergeBase = nullptr;
    if (!KVEngine::instanceExists()) {
        _KVEngine->cleanHistory();
    }
}

RecoveryUnit* RecoveryUnit::get(OperationContext* opCtx) {
    return checked_cast<ephemeral_for_test::RecoveryUnit*>(opCtx->recoveryUnit());
}
}  // namespace ephemeral_for_test
}  // namespace mongo
