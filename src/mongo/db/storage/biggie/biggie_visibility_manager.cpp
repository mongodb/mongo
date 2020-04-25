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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/db/storage/biggie/biggie_record_store.h"
#include "mongo/db/storage/biggie/biggie_visibility_manager.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {
namespace biggie {

/**
 * Used by the visibility manager to register changes when the RecoveryUnit either commits or
 * rolls back changes.
 */
class VisibilityManagerChange : public RecoveryUnit::Change {
public:
    VisibilityManagerChange(VisibilityManager* visibilityManager, RecordStore* rs, RecordId rid)
        : _visibilityManager(visibilityManager), _rs(rs), _rid(rid) {}
    ~VisibilityManagerChange() = default;

    virtual void commit(boost::optional<Timestamp>) {
        _visibilityManager->dealtWithRecord(_rid);
    }

    virtual void rollback() {
        _visibilityManager->dealtWithRecord(_rid);
        stdx::lock_guard<Latch> lk(_rs->_cappedCallbackMutex);
        if (_rs->_cappedCallback)
            _rs->_cappedCallback->notifyCappedWaitersIfNeeded();
    }

private:
    VisibilityManager* _visibilityManager;
    const RecordStore* const _rs;
    const RecordId _rid;
};

void VisibilityManager::dealtWithRecord(RecordId rid) {
    stdx::lock_guard<Latch> lock(_stateLock);
    _uncommittedRecords.erase(rid);
    _opsBecameVisibleCV.notify_all();
}

void VisibilityManager::addUncommittedRecord(OperationContext* opCtx,
                                             RecordStore* rs,
                                             RecordId rid) {
    stdx::lock_guard<Latch> lock(_stateLock);
    _uncommittedRecords.insert(rid);
    opCtx->recoveryUnit()->registerChange(std::make_unique<VisibilityManagerChange>(this, rs, rid));

    if (rid > _highestSeen)
        _highestSeen = rid;
}

RecordId VisibilityManager::getAllCommittedRecord() {
    stdx::lock_guard<Latch> lock(_stateLock);
    return _uncommittedRecords.empty() ? _highestSeen
                                       : RecordId(_uncommittedRecords.begin()->repr() - 1);
}

bool VisibilityManager::isFirstHidden(RecordId rid) {
    stdx::lock_guard<Latch> lock(_stateLock);
    if (_uncommittedRecords.empty())
        return false;
    return *_uncommittedRecords.begin() == rid;
}

void VisibilityManager::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    stdx::unique_lock<Latch> lock(_stateLock);
    const RecordId waitFor = _highestSeen;
    opCtx->waitForConditionOrInterrupt(_opsBecameVisibleCV, lock, [&] {
        return _uncommittedRecords.empty() || *_uncommittedRecords.begin() > waitFor;
    });
}

}  // namespace biggie
}  // namespace mongo
