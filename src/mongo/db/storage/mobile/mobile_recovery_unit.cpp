/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <string>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/log.h"

#define RECOVERY_UNIT_TRACE() LOG(MOBILE_TRACE_LEVEL) << "MobileSE: RecoveryUnit ID:" << _id << " "

namespace mongo {

AtomicInt64 MobileRecoveryUnit::_nextID(0);

MobileRecoveryUnit::MobileRecoveryUnit(MobileSessionPool* sessionPool)
    : _inUnitOfWork(false), _active(false), _isReadOnly(true), _sessionPool(sessionPool) {
    // Increment the global instance count and assign this instance an id.
    _id = _nextID.addAndFetch(1);

    RECOVERY_UNIT_TRACE() << " Created.";
}

MobileRecoveryUnit::~MobileRecoveryUnit() {
    invariant(!_inUnitOfWork);
    _abort();
    RECOVERY_UNIT_TRACE() << " Destroyed.";
}

void MobileRecoveryUnit::_commit() {
    if (_session && _active) {
        _txnClose(true);
    }

    for (auto& change : _changes) {
        try {
            change->commit(boost::none);
        } catch (...) {
            std::terminate();
        }
    }
    _changes.clear();
}

void MobileRecoveryUnit::_abort() {
    if (_session && _active) {
        _txnClose(false);
    }
    for (auto it = _changes.rbegin(); it != _changes.rend(); ++it) {
        try {
            (*it)->rollback();
        } catch (...) {
            std::terminate();
        }
    }
    _changes.clear();
    invariant(!_active);
}

void MobileRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_areWriteUnitOfWorksBanned);
    invariant(!_inUnitOfWork);

    RECOVERY_UNIT_TRACE() << " Unit of work Active.";

    if (_active) {
        // Confirm a write transaction is not running
        invariant(_isReadOnly);

        // Rollback read transaction running outside wuow
        _txnClose(false);
    }
    _txnOpen(opCtx, false);
    _inUnitOfWork = true;
}

void MobileRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork);

    RECOVERY_UNIT_TRACE() << " Unit of work commited, marked inactive.";

    _inUnitOfWork = false;
    _commit();
}

void MobileRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork);

    RECOVERY_UNIT_TRACE() << " Unit of work aborted, marked inactive.";

    _inUnitOfWork = false;
    _abort();
}

void MobileRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork);
    if (_active) {
        // We can't be in a WriteUnitOfWork, so it is safe to rollback.
        _txnClose(false);
    }
    _areWriteUnitOfWorksBanned = false;
}

void MobileRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork);
    _changes.push_back(std::unique_ptr<Change>{change});
}

MobileSession* MobileRecoveryUnit::getSession(OperationContext* opCtx, bool readOnly) {
    RECOVERY_UNIT_TRACE() << " getSession called with readOnly:" << (readOnly ? "TRUE" : "FALSE");

    invariant(_inUnitOfWork || readOnly);
    if (!_active) {
        _txnOpen(opCtx, readOnly);
    }

    return _session.get();
}

MobileSession* MobileRecoveryUnit::getSessionNoTxn(OperationContext* opCtx) {
    _ensureSession(opCtx);
    return _session.get();
}

void MobileRecoveryUnit::assertInActiveTxn() const {
    fassert(37050, _active);
}

void MobileRecoveryUnit::_ensureSession(OperationContext* opCtx) {
    RECOVERY_UNIT_TRACE() << " Creating new session:" << (_session ? "NO" : "YES");
    if (!_session) {
        _session = _sessionPool->getSession(opCtx);
    }
}

void MobileRecoveryUnit::_txnOpen(OperationContext* opCtx, bool readOnly) {
    invariant(!_active);
    RECOVERY_UNIT_TRACE() << " _txnOpen called with readOnly:" << (readOnly ? "TRUE" : "FALSE");
    _ensureSession(opCtx);

    /*
     * Starting a transaction with the "BEGIN" statement doesn't take an immediate lock.
     * SQLite defers taking any locks until the database is first accessed. This creates the
     * possibility of having multiple transactions opened in parallel. All sessions except the
     * first to request the access get a database locked error.
     * However, "BEGIN IMMEDIATE" forces SQLite to take a lock immediately. If another session
     * tries to create a transaction in parallel, it receives a busy error and then retries.
     * Reads outside these explicit transactions proceed unaffected.
     */

    // Check for correct locking at higher levels
    if (readOnly) {
        // Confirm that this reader has taken a shared lock
        if (!opCtx->lockState()->isLockHeldForMode(
                ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL), MODE_S)) {
            opCtx->lockState()->dump();
            invariant(!"Reading without a shared lock");
        }
        SqliteStatement::execQuery(_session.get(), "BEGIN");
    } else {
        // Single writer allowed at a time, confirm a global write lock has been taken
        if (!opCtx->lockState()->isLockHeldForMode(
                ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL), MODE_X)) {
            opCtx->lockState()->dump();
            invariant(!"Writing without an exclusive lock");
        }
        SqliteStatement::execQuery(_session.get(), "BEGIN EXCLUSIVE");
    }

    _isReadOnly = readOnly;
    _active = true;
}

void MobileRecoveryUnit::_txnClose(bool commit) {
    invariant(_active);
    RECOVERY_UNIT_TRACE() << " _txnClose called with " << (commit ? "commit " : "rollback ");

    if (commit) {
        SqliteStatement::execQuery(_session.get(), "COMMIT");
    } else {
        SqliteStatement::execQuery(_session.get(), "ROLLBACK");
    }

    _active = false;
    _isReadOnly = true;  // I don't suppose we need this, but no harm in doing so
}

void MobileRecoveryUnit::enqueueFailedDrop(std::string& dropQuery) {
    _sessionPool->failedDropsQueue.enqueueOp(dropQuery);
}
}
