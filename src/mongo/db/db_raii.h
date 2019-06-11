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

#include <string>

#include "mongo/db/catalog_raii.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/timer.h"

namespace mongo {

/**
 * RAII-style class which can update the diagnostic state on the operation's CurOp object and record
 * the operation via Top upon destruction. Can be configured to only update the Top counters if
 * desired.
 */
class AutoStatsTracker {
    AutoStatsTracker(const AutoStatsTracker&) = delete;
    AutoStatsTracker& operator=(const AutoStatsTracker&) = delete;

public:
    /**
     *Describes which diagnostics to update during the lifetime of this object.
     */
    enum class LogMode {
        kUpdateTop,  // Increments the Top counter for this operation type and this namespace upon
                     // destruction.
        kUpdateTopAndCurop,  // In addition to incrementing the Top counter, adjusts state on the
                             // CurOp object associated with the OperationContext. Updates the
                             // namespace to be 'nss', starts a timer for the operation (if it
                             // hasn't started already), and figures out and records the profiling
                             // level of the operation.
    };

    /**
     * If 'logMode' is 'kUpdateTopAndCurop', sets up and records state on the CurOp object attached
     * to 'opCtx', as described above.
     */
    AutoStatsTracker(OperationContext* opCtx,
                     const NamespaceString& nss,
                     Top::LockType lockType,
                     LogMode logMode,
                     boost::optional<int> dbProfilingLevel,
                     Date_t deadline = Date_t::max());

    /**
     * Records stats about the current operation via Top.
     */
    ~AutoStatsTracker();

private:
    OperationContext* _opCtx;
    Top::LockType _lockType;
    const NamespaceString _nss;
};

/**
 * Same as calling AutoGetCollection with MODE_IS, but in addition ensures that the read will be
 * performed against an appropriately committed snapshot if the operation is using a readConcern of
 * 'majority'.
 *
 * Use this when you want to read the contents of a collection, but you are not at the top-level of
 * some command. This will ensure your reads obey any requested readConcern, but will not update the
 * status of CurrentOp, or add a Top entry.
 *
 * NOTE: Must not be used with any locks held, because it needs to block waiting on the committed
 * snapshot to become available.
 */
class AutoGetCollectionForRead {
    AutoGetCollectionForRead(const AutoGetCollectionForRead&) = delete;
    AutoGetCollectionForRead& operator=(const AutoGetCollectionForRead&) = delete;

public:
    AutoGetCollectionForRead(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::ViewMode viewMode = AutoGetCollection::ViewMode::kViewsForbidden,
        Date_t deadline = Date_t::max());

    Database* getDb() const {
        return _autoColl->getDb();
    }

    Collection* getCollection() const {
        return _autoColl->getCollection();
    }

    ViewDefinition* getView() const {
        return _autoColl->getView();
    }

    const NamespaceString& getNss() const {
        return _autoColl->getNss();
    }

private:
    // If this field is set, the reader will not take the ParallelBatchWriterMode lock and conflict
    // with secondary batch application. This stays in scope with the _autoColl so that locks are
    // taken and released in the right order.
    boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock>
        _shouldNotConflictWithSecondaryBatchApplicationBlock;

    // This field is optional, because the code to wait for majority committed snapshot needs to
    // release locks in order to block waiting
    boost::optional<AutoGetCollection> _autoColl;

    // Returns true if we should read at the last applied timestamp instead of at "no" timestamp
    // (i.e. reading with the "latest" snapshot reflecting all writes).  Reading at the last applied
    // timestamp avoids reading in-flux data actively being written by the replication system.
    bool _shouldReadAtLastAppliedTimestamp(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           repl::ReadConcernLevel readConcernLevel) const;

    // Returns true if the minSnapshot causes conflicting catalog changes for either the provided
    // lastAppliedTimestamp or the point-in-time snapshot of the RecoveryUnit on 'opCtx'.
    bool _conflictingCatalogChanges(OperationContext* opCtx,
                                    boost::optional<Timestamp> minSnapshot,
                                    boost::optional<Timestamp> lastAppliedTimestamp) const;
};

/**
 * Same as AutoGetCollectionForRead, but in addition will add a Top entry upon destruction and
 * ensure the CurrentOp object has the right namespace and has started its timer.
 */
class AutoGetCollectionForReadCommand {
    AutoGetCollectionForReadCommand(const AutoGetCollectionForReadCommand&) = delete;
    AutoGetCollectionForReadCommand& operator=(const AutoGetCollectionForReadCommand&) = delete;

public:
    AutoGetCollectionForReadCommand(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::ViewMode viewMode = AutoGetCollection::ViewMode::kViewsForbidden,
        Date_t deadline = Date_t::max(),
        AutoStatsTracker::LogMode logMode = AutoStatsTracker::LogMode::kUpdateTopAndCurop);

    Database* getDb() const {
        return _autoCollForRead.getDb();
    }

    Collection* getCollection() const {
        return _autoCollForRead.getCollection();
    }

    ViewDefinition* getView() const {
        return _autoCollForRead.getView();
    }

    const NamespaceString& getNss() const {
        return _autoCollForRead.getNss();
    }

private:
    AutoGetCollectionForRead _autoCollForRead;
    AutoStatsTracker _statsTracker;
};

/**
 * Opens the database that we want to use and sets the appropriate namespace on the
 * current operation.
 */
class OldClientContext {
    OldClientContext(const OldClientContext&) = delete;
    OldClientContext& operator=(const OldClientContext&) = delete;

public:
    OldClientContext(OperationContext* opCtx, const std::string& ns, bool doVersion = true);
    ~OldClientContext();

    Database* db() const {
        return _db;
    }

    /** @return if the db was created by this OldClientContext */
    bool justCreated() const {
        return _justCreated;
    }

private:
    friend class CurOp;

    const Timer _timer;

    OperationContext* const _opCtx;

    Database* _db;
    bool _justCreated{false};
};

/**
 * Returns a MODE_IX LockMode if a read is performed under readConcern level snapshot, or a MODE_IS
 * lock otherwise. MODE_IX acquisition will allow a read to participate in two-phase locking.
 * Throws an exception if 'system.views' is being queried within a transaction.
 */
LockMode getLockModeForQuery(OperationContext* opCtx, const boost::optional<NamespaceString>& nss);

/**
 * When in scope, enforces prepare conflicts in the storage engine. Reads and writes in this scope
 * will block on accessing an already updated document which is in prepared state. And they will
 * unblock after the prepared transaction that performed the update commits/aborts.
 */
class EnforcePrepareConflictsBlock {
public:
    explicit EnforcePrepareConflictsBlock(OperationContext* opCtx)
        : _opCtx(opCtx), _originalValue(opCtx->recoveryUnit()->getPrepareConflictBehavior()) {
        // It is illegal to call setPrepareConflictBehavior() while any storage transaction is
        // active. setPrepareConflictBehavior() invariants that there is no active storage
        // transaction.
        _opCtx->recoveryUnit()->setPrepareConflictBehavior(PrepareConflictBehavior::kEnforce);
    }

    ~EnforcePrepareConflictsBlock() {
        // If we are still holding locks, we might still have open storage transactions. However, we
        // did not start with any active transactions when we first entered the scope. And
        // transactions started within this scope cannot be reused outside of the scope. So we need
        // to call abandonSnapshot() to close any open transactions on destruction. Any reads or
        // writes should have already completed as we are exiting the scope. Therefore, this call is
        // safe.
        if (_opCtx->lockState()->isLocked()) {
            _opCtx->recoveryUnit()->abandonSnapshot();
        }
        // It is illegal to call setPrepareConflictBehavior() while any storage transaction is
        // active. There should not be any active transaction if we are not holding locks. If locks
        // are still being held, the above abandonSnapshot() call should have already closed all
        // storage transactions.
        _opCtx->recoveryUnit()->setPrepareConflictBehavior(_originalValue);
    }

private:
    OperationContext* _opCtx;
    PrepareConflictBehavior _originalValue;
};

}  // namespace mongo
