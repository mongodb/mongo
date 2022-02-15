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
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace ephemeral_for_test {
namespace {
MONGO_FAIL_POINT_DEFINE(EFTAlwaysThrowWCEOnWrite);
MONGO_FAIL_POINT_DEFINE(EFTThrowWCEOnMerge);
}  // namespace

RecoveryUnit::RecoveryUnit(KVEngine* parentKVEngine, std::function<void()> cb)
    : _waitUntilDurableCallback(cb), _KVEngine(parentKVEngine) {}

RecoveryUnit::~RecoveryUnit() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _abort();
}

void RecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _setState(_isActive() ? State::kActive : State::kInactiveInUnitOfWork);
}

void RecoveryUnit::doCommitUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_getState()));

    if (_dirty) {
        invariant(_forked);
        if (MONGO_unlikely(EFTThrowWCEOnMerge.shouldFail())) {
            throw WriteConflictException();
        }

        while (true) {
            auto masterInfo = _KVEngine->getMasterInfo(_readAtTimestamp);
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
        _isTimestamped = false;
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

Status RecoveryUnit::majorityCommittedSnapshotAvailable() const {
    return Status::OK();
}

void RecoveryUnit::prepareUnitOfWork() {
    invariant(_inUnitOfWork());
}

void RecoveryUnit::doAbandonSnapshot() {
    invariant(!_inUnitOfWork(), toString(_getState()));
    _forked = false;
    _dirty = false;
    _isTimestamped = false;
    _setMergeNull();
    _setState(State::kInactive);
}

void RecoveryUnit::makeDirty() {
    if (MONGO_unlikely(EFTAlwaysThrowWCEOnWrite.shouldFail())) {
        throw WriteConflictException();
    }
    _dirty = true;
    if (!_isActive()) {
        _setState(_inUnitOfWork() ? State::kActive : State::kActiveNotInUnitOfWork);
    }
}

bool RecoveryUnit::forkIfNeeded() {
    if (_forked)
        return false;

    boost::optional<Timestamp> readFrom = boost::none;
    switch (_timestampReadSource) {
        case ReadSource::kNoTimestamp:
        case ReadSource::kMajorityCommitted:
        case ReadSource::kNoOverlap:
        case ReadSource::kLastApplied:
            break;
        case ReadSource::kProvided:
            readFrom = _readAtTimestamp;
            break;
        case ReadSource::kAllDurableSnapshot:
            readFrom = _KVEngine->getAllDurableTimestamp();
            break;
    }
    // Update the copies of the trees when not in a WUOW so cursors can retrieve the latest data.
    auto masterInfo = _KVEngine->getMasterInfo(readFrom);
    _mergeBase = masterInfo.second;
    _workingCopy = *masterInfo.second;
    invariant(_mergeBase);

    // Call cleanHistory in case _mergeBase was holding a shared_ptr to an older tree.
    _KVEngine->cleanHistory();
    _forked = true;
    return true;
}

Status RecoveryUnit::setTimestamp(Timestamp timestamp) {
    auto key = record_id_helpers::keyForOptime(timestamp);
    if (!key.isOK())
        return key.getStatus();

    _KVEngine->visibilityManager()->reserveRecord(this, key.getValue());
    _isTimestamped = true;
    return Status::OK();
}

void RecoveryUnit::setOrderedCommit(bool orderedCommit) {}

void RecoveryUnit::_abort() {
    _forked = false;
    _dirty = false;
    _isTimestamped = false;
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

void RecoveryUnit::setTimestampReadSource(ReadSource readSource,
                                          boost::optional<Timestamp> provided) {
    invariant(!provided == (readSource != ReadSource::kProvided));
    invariant(!(provided && provided->isNull()));

    _timestampReadSource = readSource;
    _readAtTimestamp = (provided) ? *provided : Timestamp();
}

RecoveryUnit::ReadSource RecoveryUnit::getTimestampReadSource() const {
    return _timestampReadSource;
}

RecoveryUnit* RecoveryUnit::get(OperationContext* opCtx) {
    return checked_cast<ephemeral_for_test::RecoveryUnit*>(opCtx->recoveryUnit());
}
}  // namespace ephemeral_for_test
}  // namespace mongo
