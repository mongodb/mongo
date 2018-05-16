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

#include <boost/optional.hpp>
#include <cstdint>
#include <memory>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/util/timer.h"

namespace mongo {

class BSONObjBuilder;

class WiredTigerRecoveryUnit final : public RecoveryUnit {
public:
    WiredTigerRecoveryUnit(WiredTigerSessionCache* sc);

    /**
     * It's expected a consumer would want to call the constructor that simply takes a
     * `WiredTigerSessionCache`. That constructor accesses the `WiredTigerKVEngine` to find the
     * `WiredTigerOplogManager`. However, unit tests construct `WiredTigerRecoveryUnits` with a
     * `WiredTigerSessionCache` that do not have a valid `WiredTigerKVEngine`. This constructor is
     * expected to only be useful in those cases.
     */
    WiredTigerRecoveryUnit(WiredTigerSessionCache* sc, WiredTigerOplogManager* oplogManager);
    ~WiredTigerRecoveryUnit();

    void beginUnitOfWork(OperationContext* opCtx) override;
    void prepareUnitOfWork() override;
    void commitUnitOfWork() override;
    void abortUnitOfWork() override;

    bool waitUntilDurable() override;

    bool waitUntilUnjournaledWritesDurable() override;

    void registerChange(Change* change) override;

    void abandonSnapshot() override;
    void preallocateSnapshot() override;

    Status obtainMajorityCommittedSnapshot() override;

    boost::optional<Timestamp> getPointInTimeReadTimestamp() const override;

    SnapshotId getSnapshotId() const override;

    Status setTimestamp(Timestamp timestamp) override;

    void setCommitTimestamp(Timestamp timestamp) override;

    void clearCommitTimestamp() override;

    Timestamp getCommitTimestamp() override;

    void setPrepareTimestamp(Timestamp timestamp) override;

    void setIgnorePrepared(bool ignore) override;

    void setTimestampReadSource(ReadSource source,
                                boost::optional<Timestamp> provided = boost::none) override;

    ReadSource getTimestampReadSource() const override;

    void* writingPtr(void* data, size_t len) override;

    void setRollbackWritesDisabled() override {}

    virtual void setOrderedCommit(bool orderedCommit) override {
        _orderedCommit = orderedCommit;
    }

    // ---- WT STUFF

    WiredTigerSession* getSession();
    void setIsOplogReader() {
        _isOplogReader = true;
    }

    /**
     * Enter a period of wait or computation during which there are no WT calls.
     * Any non-relevant cached handles can be closed.
     */
    void beginIdle();

    /**
     * Returns a session without starting a new WT txn on the session. Will not close any already
     * running session.
     */

    WiredTigerSession* getSessionNoTxn();

    WiredTigerSessionCache* getSessionCache() {
        return _sessionCache;
    }
    bool inActiveTxn() const {
        return _active;
    }
    void assertInActiveTxn() const;

    static WiredTigerRecoveryUnit* get(OperationContext* opCtx) {
        return checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());
    }

    static void appendGlobalStats(BSONObjBuilder& b);

private:
    void _abort();
    void _commit();

    void _ensureSession();
    void _txnClose(bool commit);
    void _txnOpen();

    WiredTigerSessionCache* _sessionCache;  // not owned
    WiredTigerOplogManager* _oplogManager;  // not owned
    UniqueWiredTigerSession _session;
    bool _areWriteUnitOfWorksBanned = false;
    bool _inUnitOfWork;
    bool _active;
    bool _isTimestamped = false;

    // Specifies which external source to use when setting read timestamps on transactions.
    ReadSource _timestampReadSource = ReadSource::kNone;

    // Commits are assumed ordered.  Unordered commits are assumed to always need to reserve a
    // new optime, and thus always call oplogDiskLocRegister() on the record store.
    bool _orderedCommit = true;

    // Ignoring prepared transactions will not return prepare conflicts and allow seeing prepared,
    // but uncommitted data.
    WiredTigerBeginTxnBlock::IgnorePrepared _ignorePrepared{
        WiredTigerBeginTxnBlock::IgnorePrepared::kNoIgnore};
    Timestamp _commitTimestamp;
    Timestamp _prepareTimestamp;
    boost::optional<Timestamp> _lastTimestampSet;
    uint64_t _mySnapshotId;
    Timestamp _majorityCommittedSnapshot;
    Timestamp _readAtTimestamp;
    std::unique_ptr<Timer> _timer;
    bool _isOplogReader = false;
    typedef std::vector<std::unique_ptr<Change>> Changes;
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
                     OperationContext* opCtx);

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
