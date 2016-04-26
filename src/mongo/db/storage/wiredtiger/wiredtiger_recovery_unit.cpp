// wiredtiger_recovery_unit.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace {
// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicUInt64 nextSnapshotId{1};
}  // namespace

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc)
    : _sessionCache(sc),
      _inUnitOfWork(false),
      _active(false),
      _mySnapshotId(nextSnapshotId.fetchAndAdd(1)),
      _everStartedWrite(false) {}

WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
    invariant(!_inUnitOfWork);
    _abort();
}

void WiredTigerRecoveryUnit::reportState(BSONObjBuilder* b) const {
    b->append("wt_inUnitOfWork", _inUnitOfWork);
    b->append("wt_active", _active);
    b->append("wt_everStartedWrite", _everStartedWrite);
    b->appendNumber("wt_mySnapshotId", static_cast<long long>(_mySnapshotId));
    if (_active)
        b->append("wt_millisSinceCommit", _timer.millis());
}

void WiredTigerRecoveryUnit::prepareForCreateSnapshot(OperationContext* opCtx) {
    invariant(!_active);  // Can't already be in a WT transaction.
    invariant(!_inUnitOfWork);
    invariant(!_readFromMajorityCommittedSnapshot);

    // Starts the WT transaction that will be the basis for creating a named snapshot.
    getSession(opCtx);
    _areWriteUnitOfWorksBanned = true;
}

void WiredTigerRecoveryUnit::_commit() {
    try {
        if (_session && _active) {
            _txnClose(true);
        }

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void WiredTigerRecoveryUnit::_abort() {
    try {
        if (_session && _active) {
            _txnClose(false);
        }

        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = *it;
            LOG(2) << "CUSTOM ROLLBACK " << demangleName(typeid(*change));
            change->rollback();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void WiredTigerRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_areWriteUnitOfWorksBanned);
    invariant(!_inUnitOfWork);
    _inUnitOfWork = true;
    _everStartedWrite = true;
}

void WiredTigerRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _commit();
}

void WiredTigerRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _abort();
}

void WiredTigerRecoveryUnit::_ensureSession() {
    if (!_session) {
        _session = _sessionCache->getSession();
    }
}

bool WiredTigerRecoveryUnit::waitUntilDurable() {
    invariant(!_inUnitOfWork);
    // For inMemory storage engines, the data is "as durable as it's going to get".
    // That is, a restart is equivalent to a complete node failure.
    if (_sessionCache->isEphemeral()) {
        return true;
    }
    // _session may be nullptr. We cannot _ensureSession() here as that needs shutdown protection.
    _sessionCache->waitUntilDurable(false);
    return true;
}

void WiredTigerRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork);
    _changes.push_back(change);
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    fassert(28575, _active);
}

WiredTigerSession* WiredTigerRecoveryUnit::getSession(OperationContext* opCtx) {
    if (!_active) {
        _txnOpen(opCtx);
    }
    return _session.get();
}

WiredTigerSession* WiredTigerRecoveryUnit::getSessionNoTxn(OperationContext* opCtx) {
    _ensureSession();
    return _session.get();
}

void WiredTigerRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork);
    if (_active) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _areWriteUnitOfWorksBanned = false;
}

void* WiredTigerRecoveryUnit::writingPtr(void* data, size_t len) {
    // This API should not be used for anything other than the MMAP V1 storage engine
    MONGO_UNREACHABLE;
}

void WiredTigerRecoveryUnit::setOplogReadTill(const RecordId& id) {
    _oplogReadTill = id;
}

void WiredTigerRecoveryUnit::_txnClose(bool commit) {
    invariant(_active);
    WT_SESSION* s = _session->getSession();
    if (commit) {
        invariantWTOK(s->commit_transaction(s, NULL));
        LOG(3) << "WT commit_transaction";
    } else {
        invariantWTOK(s->rollback_transaction(s, NULL));
        LOG(3) << "WT rollback_transaction";
    }
    _active = false;
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
}

SnapshotId WiredTigerRecoveryUnit::getSnapshotId() const {
    // TODO: use actual wiredtiger txn id
    return SnapshotId(_mySnapshotId);
}

Status WiredTigerRecoveryUnit::setReadFromMajorityCommittedSnapshot() {
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }

    _majorityCommittedSnapshot = *snapshotName;
    _readFromMajorityCommittedSnapshot = true;
    return Status::OK();
}

boost::optional<SnapshotName> WiredTigerRecoveryUnit::getMajorityCommittedSnapshot() const {
    if (!_readFromMajorityCommittedSnapshot)
        return {};
    return _majorityCommittedSnapshot;
}

void WiredTigerRecoveryUnit::_txnOpen(OperationContext* opCtx) {
    invariant(!_active);
    _ensureSession();

    WT_SESSION* s = _session->getSession();

    if (_readFromMajorityCommittedSnapshot) {
        _majorityCommittedSnapshot =
            _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(s);
    } else {
        invariantWTOK(s->begin_transaction(s, NULL));
    }

    LOG(3) << "WT begin_transaction";
    _timer.reset();
    _active = true;
}

// ---------------------

WiredTigerCursor::WiredTigerCursor(const std::string& uri,
                                   uint64_t tableId,
                                   bool forRecordStore,
                                   OperationContext* txn) {
    _tableID = tableId;
    _ru = WiredTigerRecoveryUnit::get(txn);
    _session = _ru->getSession(txn);
    _cursor = _session->getCursor(uri, tableId, forRecordStore);
    if (!_cursor) {
        error() << "no cursor for uri: " << uri;
    }
}

WiredTigerCursor::~WiredTigerCursor() {
    _session->releaseCursor(_tableID, _cursor);
    _cursor = NULL;
}

void WiredTigerCursor::reset() {
    invariantWTOK(_cursor->reset(_cursor));
}

WT_SESSION* WiredTigerCursor::getWTSession() {
    return _session->getSession();
}
}
