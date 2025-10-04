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

#pragma once

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_stats.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <memory>
#include <stack>
#include <vector>

#include <wiredtiger.h>

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using RoundUpPreparedTimestamps = WiredTigerBeginTxnBlock::RoundUpPreparedTimestamps;
using RoundUpReadTimestamp = WiredTigerBeginTxnBlock::RoundUpReadTimestamp;

extern AtomicWord<std::int64_t> snapshotTooOldErrorCount;

class WiredTigerRecoveryUnit final : public RecoveryUnit {
public:
    WiredTigerRecoveryUnit(WiredTigerConnection* connection);

    /**
     * It's expected a consumer would want to call the constructor that simply takes a
     * `WiredTigerConnection`. That constructor accesses the `WiredTigerKVEngine` to find the
     * `WiredTigerOplogManager`. However, unit tests construct `WiredTigerRecoveryUnits` with a
     * `WiredTigerConnection` that do not have a valid `WiredTigerKVEngine`. This constructor is
     * expected to only be useful in those cases.
     */
    WiredTigerRecoveryUnit(WiredTigerConnection* sc, WiredTigerOplogManager* oplogManager);
    ~WiredTigerRecoveryUnit() override;

    static WiredTigerRecoveryUnit& get(RecoveryUnit& ru) {
        return checked_cast<WiredTigerRecoveryUnit&>(ru);
    }

    static WiredTigerRecoveryUnit* get(RecoveryUnit* ru) {
        return checked_cast<WiredTigerRecoveryUnit*>(ru);
    }

    void prepareUnitOfWork() override;

    void preallocateSnapshot(
        const OpenSnapshotOptions& options = kDefaultOpenSnapshotOptions) override;

    Status majorityCommittedSnapshotAvailable() const override;

    boost::optional<Timestamp> getPointInTimeReadTimestamp() override;

    Status setTimestamp(Timestamp timestamp) override;

    bool isTimestamped() const override {
        return _isTimestamped;
    }

    void setCommitTimestamp(Timestamp timestamp) override;

    void clearCommitTimestamp() override;

    Timestamp getCommitTimestamp() const override;

    void setDurableTimestamp(Timestamp timestamp) override;

    Timestamp getDurableTimestamp() const override;

    void setPrepareTimestamp(Timestamp timestamp) override;

    Timestamp getPrepareTimestamp() const override;

    void setPrepareConflictBehavior(PrepareConflictBehavior behavior) override;

    PrepareConflictBehavior getPrepareConflictBehavior() const override;

    /**
     * Set pre-fetching capabilities for this session. This allows pre-loading of a set of pages
     * into the cache and is an optional optimization.
     */
    void setPrefetching(bool enable) override;

    void allowOneUntimestampedWrite() override {
        invariant(!_isActive());
        // In the case we're already allowing all timestamp writes we do not make the assertions
        // stricter. This would otherwise break the relaxed assumptions being set at layers above.
        if (_untimestampedWriteAssertionLevel ==
            RecoveryUnit::UntimestampedWriteAssertionLevel::kSuppressAlways) {
            return;
        }
        _untimestampedWriteAssertionLevel =
            RecoveryUnit::UntimestampedWriteAssertionLevel::kSuppressOnce;
    }

    void allowAllUntimestampedWrites() override {
        invariant(!_isActive());
        _untimestampedWriteAssertionLevel =
            RecoveryUnit::UntimestampedWriteAssertionLevel::kSuppressAlways;
    }

    void setTimestampReadSource(ReadSource source,
                                boost::optional<Timestamp> provided = boost::none) override;

    ReadSource getTimestampReadSource() const override;

    void pinReadSource() override;

    void unpinReadSource() override;

    bool isReadSourcePinned() const override;

    void setOrderedCommit(bool orderedCommit) override {
        _orderedCommit = orderedCommit;
    }

    void setReadOnce(bool readOnce) override {
        // Do not allow a session to use readOnce and regular cursors at the same time.
        invariant(!_isActive() || readOnce == _readOnce || getSession()->cursorsOut() == 0);
        _readOnce = readOnce;
    };

    bool getReadOnce() const override {
        return _readOnce;
    };

    std::unique_ptr<StorageStats> computeOperationStatisticsSinceLastCall() override;

    void ignoreAllMultiTimestampConstraints() override {
        _multiTimestampConstraintTracker.ignoreAllMultiTimestampConstraints = true;
    }

    void setCacheMaxWaitTimeout(Milliseconds) override;

    size_t getCacheDirtyBytes() override;

    // ---- WT STUFF

    WiredTigerConnection* getConnection() {
        return _connection;
    }

    WiredTigerSession* getSession();

    /**
     * Returns a session without starting a new WT txn on the session. Will not close any already
     * running session.
     */
    WiredTigerSession* getSessionNoTxn();

    /**
     * Enter a period of wait or computation during which there are no WT calls.
     * Any non-relevant cached handles can be closed.
     */
    void beginIdle();

    void assertInActiveTxn() const;

    void setTxnModified() override;

    boost::optional<int64_t> getOplogVisibilityTs() override;
    void setOplogVisibilityTs(boost::optional<int64_t> oplogVisibilityTs) override;

    void setOperationContext(OperationContext* opCtx) override;

private:
    void doBeginUnitOfWork() override;
    void doCommitUnitOfWork() override;
    void doAbortUnitOfWork() override;

    void doAbandonSnapshot() override;

    void _setIsolation(Isolation) override;

    void _abort();
    void _commit();

    void _ensureSession();
    void _txnClose(bool commit);
    void _txnOpen();

    /**
     * Starts a transaction at the current all_durable timestamp.
     * Returns the timestamp the transaction was started at.
     */
    Timestamp _beginTransactionAtAllDurableTimestamp();

    /**
     * Starts a transaction at the no-overlap timestamp. Returns the timestamp the transaction
     * was started at.
     */
    Timestamp _beginTransactionAtNoOverlapTimestamp();

    /**
     * Starts a transaction at the lastApplied timestamp stored in '_readAtTimestamp'. Sets
     * '_readAtTimestamp' to the actual timestamp used by the storage engine in case rounding
     * occurred.
     */
    void _beginTransactionAtLastAppliedTimestamp();

    /**
     * Returns the timestamp at which the current transaction is reading.
     */
    Timestamp _getTransactionReadTimestamp();

    /**
     * Keeps track of constraint violations on multi timestamp transactions. If a transaction sets
     * multiple timestamps, the first timestamp must be set prior to any writes. Vice-versa, if a
     * transaction writes a document before setting a timestamp, it must not set multiple
     * timestamps.
     */
    void _updateMultiTimestampConstraint(Timestamp timestamp);

    WiredTigerConnection* _connection;      // not owned
    WiredTigerOplogManager* _oplogManager;  // not owned
    WiredTigerManagedSession _managedSession;
    WiredTigerSession* _session = nullptr;
    bool _isTimestamped = false;

    // Helpers used to keep track of multi timestamp constraint violations on the transaction.
    struct MultiTimestampConstraintTracker {
        bool isTxnModified = false;
        bool txnHasNonTimestampedWrite = false;
        bool ignoreAllMultiTimestampConstraints = false;

        // Most operations only use one timestamp.
        static constexpr auto kDefaultInit = 1;
        std::stack<Timestamp, absl::InlinedVector<Timestamp, kDefaultInit>> timestampOrder;

    } _multiTimestampConstraintTracker;

    // Specifies which external source to use when setting read timestamps on transactions.
    ReadSource _timestampReadSource = ReadSource::kNoTimestamp;

    // Commits are assumed ordered.  Unordered commits are assumed to always need to reserve a
    // new optime, and thus always call oplogDiskLocRegister() on the record store.
    bool _orderedCommit = true;

    // When 'true', data read from disk should not be kept in the storage engine cache.
    bool _readOnce = false;

    bool _readSourcePinned = false;

    // The behavior of handling prepare conflicts.
    PrepareConflictBehavior _prepareConflictBehavior{PrepareConflictBehavior::kEnforce};
    Timestamp _commitTimestamp;
    Timestamp _durableTimestamp;
    Timestamp _prepareTimestamp;
    boost::optional<Timestamp> _lastTimestampSet;
    Timestamp _readAtTimestamp;
    UntimestampedWriteAssertionLevel _untimestampedWriteAssertionLevel =
        UntimestampedWriteAssertionLevel::kEnforce;
    std::unique_ptr<Timer> _timer;
    // The guaranteed 'no holes' point in the oplog. Forward cursor oplog reads can only read up to
    // this timestamp if they want to avoid missing any entries in the oplog that may not yet have
    // committed ('holes'). @see WiredTigerOplogManager::getOplogReadTimestamp
    boost::optional<int64_t> _oplogVisibleTs = boost::none;

    WiredTigerStats _sessionStatsAfterLastOperation;

    Milliseconds _cacheMaxWaitTimeout{0};

    // Detects any attempt to reconfigure options used by an open transaction.
    OpenSnapshotOptions _optionsUsedToOpenSnapshot;
};

// Constructs a WiredTigerCursor::Params instance from the given params and returns it.
WiredTigerCursor::Params getWiredTigerCursorParams(WiredTigerRecoveryUnit& wtRu,
                                                   uint64_t tableID,
                                                   bool allowOverwrite = false,
                                                   bool random = false);

}  // namespace mongo
