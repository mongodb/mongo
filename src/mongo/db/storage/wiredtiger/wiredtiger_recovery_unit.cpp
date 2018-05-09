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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Always notifies prepare conflict waiters when a transaction commits or aborts, even when the
// transaction is not prepared. This should always be enabled if WTPrepareConflictForReads is
// used, which fails randomly. If this is not enabled, no prepare conflicts will be resolved,
// because the recovery unit may not ever actually be in a prepared state.
MONGO_FP_DECLARE(WTAlwaysNotifyPrepareConflictWaiters);

// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicUInt64 nextSnapshotId{1};

logger::LogSeverity kSlowTransactionSeverity = logger::LogSeverity::Debug(1);
}  // namespace

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc)
    : WiredTigerRecoveryUnit(sc, sc->getKVEngine()->getOplogManager()) {}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc,
                                               WiredTigerOplogManager* oplogManager)
    : _sessionCache(sc),
      _oplogManager(oplogManager),
      _inUnitOfWork(false),
      _active(false),
      _mySnapshotId(nextSnapshotId.fetchAndAdd(1)) {}

WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
    invariant(!_inUnitOfWork);
    _abort();
}

void WiredTigerRecoveryUnit::_commit() {
    // Since we cannot have both a _lastTimestampSet and a _commitTimestamp, we set the
    // commit time as whichever is non-empty. If both are empty, then _lastTimestampSet will
    // be boost::none and we'll set the commit time to that.
    auto commitTime = _commitTimestamp.isNull() ? _lastTimestampSet : _commitTimestamp;

    try {
        bool notifyDone = !_prepareTimestamp.isNull();
        if (_session && _active) {
            _txnClose(true);
        }

        if (MONGO_FAIL_POINT(WTAlwaysNotifyPrepareConflictWaiters)) {
            notifyDone = true;
        }

        if (notifyDone) {
            _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
        }

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit(commitTime);
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void WiredTigerRecoveryUnit::_abort() {
    try {
        bool notifyDone = !_prepareTimestamp.isNull();
        if (_session && _active) {
            _txnClose(false);
        }

        if (MONGO_FAIL_POINT(WTAlwaysNotifyPrepareConflictWaiters)) {
            notifyDone = true;
        }

        if (notifyDone) {
            _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
        }

        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = it->get();
            LOG(2) << "CUSTOM ROLLBACK " << redact(demangleName(typeid(*change)));
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
}

void WiredTigerRecoveryUnit::prepareUnitOfWork() {
    invariant(!_areWriteUnitOfWorksBanned);
    invariant(_inUnitOfWork);
    invariant(!_prepareTimestamp.isNull());

    auto session = getSession();
    WT_SESSION* s = session->getSession();

    LOG(1) << "preparing transaction at time: " << _prepareTimestamp;

    const std::string conf = "prepare_timestamp=" + integerToHex(_prepareTimestamp.asULL());
    // Prepare the transaction.
    invariantWTOK(s->prepare_transaction(s, conf.c_str()));
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
    const bool forceCheckpoint = false;
    const bool stableCheckpoint = false;
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

bool WiredTigerRecoveryUnit::waitUntilUnjournaledWritesDurable() {
    invariant(!_inUnitOfWork);
    const bool forceCheckpoint = true;
    const bool stableCheckpoint = true;
    // Calling `waitUntilDurable` with `forceCheckpoint` set to false only performs a log
    // (journal) flush, and thus has no effect on unjournaled writes. Setting `forceCheckpoint` to
    // true will lock in stable writes to unjournaled tables.
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

void WiredTigerRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork);
    _changes.push_back(std::unique_ptr<Change>{change});
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    fassert(28575, _active);
}

WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
    if (!_active) {
        _txnOpen();
    }
    return _session.get();
}

WiredTigerSession* WiredTigerRecoveryUnit::getSessionNoTxn() {
    _ensureSession();
    WiredTigerSession* session = _session.get();

    // Handling queued drops can be slow, which is not desired for internal operations like FTDC
    // sampling. Disable handling of queued drops for such sessions.
    session->dropQueuedIdentsAtSessionEndAllowed(false);
    return session;
}

void WiredTigerRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork);
    if (_active) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _areWriteUnitOfWorksBanned = false;
}

void WiredTigerRecoveryUnit::preallocateSnapshot() {
    // Begin a new transaction, if one is not already started.
    getSession();
}

void* WiredTigerRecoveryUnit::writingPtr(void* data, size_t len) {
    // This API should not be used for anything other than the MMAP V1 storage engine
    MONGO_UNREACHABLE;
}

void WiredTigerRecoveryUnit::_txnClose(bool commit) {
    invariant(_active);
    WT_SESSION* s = _session->getSession();
    if (_timer) {
        const int transactionTime = _timer->millis();
        if (transactionTime >= serverGlobalParams.slowMS) {
            LOG(kSlowTransactionSeverity) << "Slow WT transaction. Lifetime of SnapshotId "
                                          << _mySnapshotId << " was " << transactionTime << "ms";
        }
    }

    int wtRet;
    if (commit) {
        if (!_commitTimestamp.isNull()) {
            const std::string conf = "commit_timestamp=" + integerToHex(_commitTimestamp.asULL());
            invariantWTOK(s->timestamp_transaction(s, conf.c_str()));
            _isTimestamped = true;
        }

        wtRet = s->commit_transaction(s, nullptr);
        LOG(3) << "WT commit_transaction for snapshot id " << _mySnapshotId;
    } else {
        wtRet = s->rollback_transaction(s, nullptr);
        invariant(!wtRet);
        LOG(3) << "WT rollback_transaction for snapshot id " << _mySnapshotId;
    }

    if (_isTimestamped) {
        if (!_orderedCommit) {
            // We only need to update oplog visibility where commits can be out-of-order with
            // respect to their assigned optime and such commits might otherwise be visible.
            // This should happen only on primary nodes.
            _oplogManager->triggerJournalFlush();
        }
        _isTimestamped = false;
    }
    invariantWTOK(wtRet);

    invariant(!_lastTimestampSet || _commitTimestamp.isNull(),
              str::stream() << "Cannot have both a _lastTimestampSet and a "
                               "_commitTimestamp. _lastTimestampSet: "
                            << _lastTimestampSet->toString()
                            << ". _commitTimestamp: "
                            << _commitTimestamp.toString());

    // We reset the _lastTimestampSet between transactions. Since it is legal for one
    // transaction on a RecoveryUnit to call setTimestamp() and another to call
    // setCommitTimestamp().
    _lastTimestampSet = boost::none;

    _active = false;
    _prepareTimestamp = Timestamp();
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
    _isOplogReader = false;
    _orderedCommit = true;  // Default value is true; we assume all writes are ordered.
}

SnapshotId WiredTigerRecoveryUnit::getSnapshotId() const {
    // TODO: use actual wiredtiger txn id
    return SnapshotId(_mySnapshotId);
}

Status WiredTigerRecoveryUnit::obtainMajorityCommittedSnapshot() {
    invariant(_isReadingFromPointInTime());
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }
    _majorityCommittedSnapshot = *snapshotName;
    return Status::OK();
}

boost::optional<Timestamp> WiredTigerRecoveryUnit::getPointInTimeReadTimestamp() const {
    if (!_isReadingFromPointInTime())
        return boost::none;

    if (getReadConcernLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
        !_readAtTimestamp.isNull()) {
        return _readAtTimestamp;
    }

    invariant(!_majorityCommittedSnapshot.isNull());
    return _majorityCommittedSnapshot;
}

void WiredTigerRecoveryUnit::_txnOpen() {
    invariant(!_active);
    _ensureSession();

    // Only start a timer for transaction's lifetime if we're going to log it.
    if (shouldLog(kSlowTransactionSeverity)) {
        _timer.reset(new Timer());
    }
    WT_SESSION* session = _session->getSession();

    auto ignorePrepare = _readConcernLevel == repl::ReadConcernLevel::kAvailableReadConcern;
    // '_readAtTimestamp' is available outside of a check for readConcern level 'snapshot' to
    // accommodate unit testing. Note that the order of this if/else chain below is important for
    // correctness.
    if (_readAtTimestamp != Timestamp::min()) {
        invariantWTOK(session->begin_transaction(session, NULL));
        auto rollbacker =
            MakeGuard([&] { invariant(session->rollback_transaction(session, nullptr) == 0); });
        auto status =
            _sessionCache->snapshotManager().setTransactionReadTimestamp(_readAtTimestamp, session);

        if (!status.isOK() && status.code() == ErrorCodes::BadValue) {
            uasserted(ErrorCodes::SnapshotTooOld,
                      str::stream() << "Read timestamp " << _readAtTimestamp.toString()
                                    << " is older than the oldest available timestamp.");
        }
        uassertStatusOK(status);
        rollbacker.Dismiss();
    } else if (_isReadingFromPointInTime() && !_shouldReadAtLastAppliedTimestamp) {
        // We reset _majorityCommittedSnapshot to the actual read timestamp used when the
        // transaction was started.
        _majorityCommittedSnapshot =
            _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(session);
    } else if (_shouldReadAtLastAppliedTimestamp &&
               _sessionCache->snapshotManager().getLocalSnapshot()) {
        // Read from the last applied timestamp (tracked globally by the SnapshotManager), which is
        // the timestamp of the most recent completed replication batch operation. This should
        // be true for local or available readConcern on secondaries, or for speculative snapshot
        // reads in multi-document transactions.
        auto localSnapshot = _sessionCache->snapshotManager().beginTransactionOnLocalSnapshot(
            session, ignorePrepare);
        // Record the local timestamp actually used.
        if (_isReadingFromPointInTime()) {
            _readAtTimestamp = localSnapshot;
        }
    } else if (_isOplogReader) {
        _sessionCache->snapshotManager().beginTransactionOnOplog(
            _sessionCache->getKVEngine()->getOplogManager(), session);
    } else {
        uassert(ErrorCodes::SnapshotUnavailable,
                "No local snapshot available for snapshot read.",
                !_isReadingFromPointInTime());
        invariantWTOK(
            session->begin_transaction(session, ignorePrepare ? "ignore_prepare=true" : nullptr));
    }

    LOG(3) << "WT begin_transaction for snapshot id " << _mySnapshotId;
    _active = true;
}


Status WiredTigerRecoveryUnit::setTimestamp(Timestamp timestamp) {
    _ensureSession();
    LOG(3) << "WT set timestamp of future write operations to " << timestamp;
    WT_SESSION* session = _session->getSession();
    invariant(_inUnitOfWork);
    invariant(_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set WUOW timestamp to "
                            << timestamp.toString());

    _lastTimestampSet = timestamp;

    // Starts the WT transaction associated with this session.
    getSession();

    const std::string conf = "commit_timestamp=" + integerToHex(timestamp.asULL());
    auto rc = session->timestamp_transaction(session, conf.c_str());
    if (rc == 0) {
        _isTimestamped = true;
    }
    return wtRCToStatus(rc, "timestamp_transaction");
}

void WiredTigerRecoveryUnit::setCommitTimestamp(Timestamp timestamp) {
    invariant(!_inUnitOfWork);
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set it to "
                            << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set commit timestamp to "
                            << timestamp.toString());
    invariant(!_isTimestamped);

    _commitTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getCommitTimestamp() {
    return _commitTimestamp;
}

void WiredTigerRecoveryUnit::clearCommitTimestamp() {
    invariant(!_inUnitOfWork);
    invariant(!_commitTimestamp.isNull());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to clear commit timestamp.");
    invariant(!_isTimestamped);

    _commitTimestamp = Timestamp();
}

void WiredTigerRecoveryUnit::setPrepareTimestamp(Timestamp timestamp) {
    invariant(_inUnitOfWork);
    invariant(_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull());

    _prepareTimestamp = timestamp;
}

Status WiredTigerRecoveryUnit::setPointInTimeReadTimestamp(Timestamp timestamp) {
    _readAtTimestamp = timestamp;
    return Status::OK();
}

void WiredTigerRecoveryUnit::setIsOplogReader() {
    // Note: it would be nice to assert !active here, but OplogStones currently opens a cursor on
    // the oplog while the recovery unit is already active.
    _isOplogReader = true;
}

void WiredTigerRecoveryUnit::beginIdle() {
    // Close all cursors, we don't want to keep any old cached cursors around.
    if (_session) {
        _session->closeAllCursors("");
    }
}

// ---------------------

WiredTigerCursor::WiredTigerCursor(const std::string& uri,
                                   uint64_t tableId,
                                   bool forRecordStore,
                                   OperationContext* opCtx) {
    _tableID = tableId;
    _ru = WiredTigerRecoveryUnit::get(opCtx);
    _session = _ru->getSession();
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
}
