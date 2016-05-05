// wiredtiger_recovery_unit.h

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

#pragma once

#include <wiredtiger.h>

#include <memory.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_name.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/timer.h"

namespace mongo {

class BSONObjBuilder;

class WiredTigerRecoveryUnit final : public RecoveryUnit {
public:
    WiredTigerRecoveryUnit(WiredTigerSessionCache* sc);

    virtual ~WiredTigerRecoveryUnit();

    virtual void reportState(BSONObjBuilder* b) const;

    void beginUnitOfWork(OperationContext* opCtx) final;
    void commitUnitOfWork() final;
    void abortUnitOfWork() final;

    virtual bool waitUntilDurable();

    virtual void registerChange(Change* change);

    virtual void abandonSnapshot();

    virtual void* writingPtr(void* data, size_t len);

    virtual void setRollbackWritesDisabled() {}

    virtual SnapshotId getSnapshotId() const;

    Status setReadFromMajorityCommittedSnapshot() final;
    bool isReadingFromMajorityCommittedSnapshot() const final {
        return _readFromMajorityCommittedSnapshot;
    }

    boost::optional<SnapshotName> getMajorityCommittedSnapshot() const final;

    // ---- WT STUFF

    WiredTigerSession* getSession(OperationContext* opCtx);

    /**
     * Returns a session without starting a new WT txn on the session. Will not close any already
     * running session.
     */

    WiredTigerSession* getSessionNoTxn(OperationContext* opCtx);

    WiredTigerSessionCache* getSessionCache() {
        return _sessionCache;
    }
    bool inActiveTxn() const {
        return _active;
    }
    void assertInActiveTxn() const;

    bool everStartedWrite() const {
        return _everStartedWrite;
    }

    void setOplogReadTill(const RecordId& id);
    RecordId getOplogReadTill() const {
        return _oplogReadTill;
    }

    static WiredTigerRecoveryUnit* get(OperationContext* txn) {
        return checked_cast<WiredTigerRecoveryUnit*>(txn->recoveryUnit());
    }

    static void appendGlobalStats(BSONObjBuilder& b);

    /**
     * Prepares this RU to be the basis for a named snapshot.
     *
     * Begins a WT transaction, and invariants if we are already in one.
     * Bans being in a WriteUnitOfWork until the next call to abandonSnapshot().
     */
    void prepareForCreateSnapshot(OperationContext* opCtx);

private:
    void _abort();
    void _commit();

    void _ensureSession();
    void _txnClose(bool commit);
    void _txnOpen(OperationContext* opCtx);

    WiredTigerSessionCache* _sessionCache;  // not owned
    UniqueWiredTigerSession _session;
    bool _areWriteUnitOfWorksBanned = false;
    bool _inUnitOfWork;
    bool _active;
    uint64_t _mySnapshotId;
    bool _everStartedWrite;
    Timer _timer;
    RecordId _oplogReadTill;
    bool _readFromMajorityCommittedSnapshot = false;
    SnapshotName _majorityCommittedSnapshot = SnapshotName::min();

    typedef OwnedPointerVector<Change> Changes;
    Changes _changes;
};

/**
 * This is a smart pointer that wraps a WT_CURSOR and knows how to obtain and get from pool.
 */
class WiredTigerCursor {
public:
    WiredTigerCursor(const std::string& uri,
                     uint64_t tableID,
                     bool forRecordStore,
                     OperationContext* txn);

    ~WiredTigerCursor();


    WT_CURSOR* get() const {
        // TODO(SERVER-16816): assertInActiveTxn();
        return _cursor;
    }

    WT_CURSOR* operator->() const {
        return get();
    }

    WiredTigerSession* getSession() {
        return _session;
    }
    WT_SESSION* getWTSession();

    void reset();

    void assertInActiveTxn() const {
        _ru->assertInActiveTxn();
    }

private:
    uint64_t _tableID;
    WiredTigerRecoveryUnit* _ru;  // not owned
    WiredTigerSession* _session;
    WT_CURSOR* _cursor;  // owned, but pulled
};
}
