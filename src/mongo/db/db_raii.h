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
#include "mongo/stdx/variant.h"
#include "mongo/util/overloaded_visitor.h"
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
     * Describes which diagnostics to update during the lifetime of this object.
     */
    enum class LogMode {
        kUpdateTop,    // Increments the Top counter for this operation type and this namespace
                       // upon destruction.
        kUpdateCurOp,  // Adjusts the state on the CurOp object associated with the
                       // OperationContext. Updates the namespace to be 'nss', starts a timer
                       // for the operation (if it hasn't already started), and figures out and
                       // records the profiling level of the operation.
        kUpdateTopAndCurOp,  // Performs the operations of both the LogModes specified above.
    };

    /**
     * If 'logMode' is 'kUpdateCurOp' or 'kUpdateTopAndCurOp', sets up and records state on the
     * CurOp object attached to 'opCtx', as described above.
     */
    AutoStatsTracker(OperationContext* opCtx,
                     const NamespaceString& nss,
                     Top::LockType lockType,
                     LogMode logMode,
                     int dbProfilingLevel,
                     Date_t deadline = Date_t::max(),
                     const std::vector<NamespaceStringOrUUID>& secondaryNssVector = {});

    /**
     * Records stats about the current operation via Top, if 'logMode' is 'kUpdateTop' or
     * 'kUpdateTopAndCurOp'.
     */
    ~AutoStatsTracker();

private:
    OperationContext* _opCtx;
    Top::LockType _lockType;
    const LogMode _logMode;
    std::set<NamespaceString> _nssSet;
};

/**
 * Locked version of AutoGetCollectionForRead for setting up an operation for read that ensured that
 * the read will be performed against an appropriately committed snapshot if the operation is using
 * a readConcern of 'majority'.
 *
 * Use this when you want to read the contents of a collection, but you are not at the top-level of
 * some command. This will ensure your reads obey any requested readConcern, but will not update the
 * status of CurrentOp, or add a Top entry.
 *
 * Additional collection and/or database locks will be acquired for 'secondaryNssOrUUIDs'
 * namespaces.
 */
class AutoGetCollectionForRead {
public:
    AutoGetCollectionForRead(OperationContext* opCtx,
                             const NamespaceStringOrUUID& nsOrUUID,
                             AutoGetCollection::Options = {});

    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }

    const Collection* operator->() const {
        return getCollection().get();
    }

    const CollectionPtr& operator*() const {
        return getCollection();
    }

    const CollectionPtr& getCollection() const;
    const ViewDefinition* getView() const;
    const NamespaceString& getNss() const;

    bool isAnySecondaryNamespaceAViewOrSharded() const {
        return _secondaryNssIsAViewOrSharded;
    }

private:
    // The caller was expecting to conflict with batch application before entering this
    // function.
    // i.e. the caller does not currently have a ShouldNotConflict... block in scope.
    bool _callerWasConflicting;
    // If this field is set, the reader will not take the ParallelBatchWriterMode lock and conflict
    // with secondary batch application. This stays in scope with the _autoColl so that locks are
    // taken and released in the right order.
    boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock>
        _shouldNotConflictWithSecondaryBatchApplicationBlock;

    // Ordering matters, the _collLocks should destruct before the _autoGetDb releases the
    // rstl/global/database locks.
    AutoGetDb _autoDb;
    std::vector<CollectionNamespaceOrUUIDLock> _collLocks;

    CollectionPtr _coll;
    std::shared_ptr<const ViewDefinition> _view;

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string
    NamespaceString _resolvedNss;

    // Tracks whether any secondary collection namespaces is a view or sharded.
    bool _secondaryNssIsAViewOrSharded = false;
};

/**
 * Same as AutoGetCollectionForRead above except does not take collection, database or rstl locks.
 * Takes the global lock and may take the PBWM, same as AutoGetCollectionForRead. Ensures a
 * consistent in-memory and on-disk view of the storage catalog.
 *
 * This implementation uses the point-in-time (PIT) catalog.
 */
class AutoGetCollectionForReadLockFree final {
public:
    AutoGetCollectionForReadLockFree(OperationContext* opCtx,
                                     NamespaceStringOrUUID nsOrUUID,
                                     AutoGetCollection::Options options = {});

    const CollectionPtr& getCollection() const {
        return _collectionPtr;
    }

    const ViewDefinition* getView() const {
        return _view.get();
    }

    const NamespaceString& getNss() const {
        return _resolvedNss;
    }

    bool isAnySecondaryNamespaceAViewOrSharded() const {
        return _secondaryNssIsAViewOrSharded;
    }

private:
    /**
     * Creates the std::function object used by CollectionPtrs to restore state for this
     * AutoGetCollectionForReadLockFree object after having yielded.
     */
    CollectionPtr::RestoreFn _makeRestoreFromYieldFn(
        const AutoGetCollection::Options& options,
        bool callerExpectedToConflictWithSecondaryBatchApplication,
        const DatabaseName& dbName);

    // Used so that we can reset the read source back to the original read source when this instance
    // of AutoGetCollectionForReadLockFree is destroyed.
    RecoveryUnit::ReadSource _originalReadSource;

    // Whether or not this AutoGetCollectionForReadLockFree is being constructed while
    // there's already a lock-free read in progress.
    bool _isLockFreeReadSubOperation;

    // Whether or not the calling context expects to conflict with secondary batch application. This
    // is just used for invariant checking.
    bool _callerExpectedToConflictWithSecondaryBatchApplication;

    // If this field is set, the reader will not take the ParallelBatchWriterMode lock and conflict
    // with secondary batch application.
    boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock>
        _shouldNotConflictWithSecondaryBatchApplicationBlock;

    // Increments a counter on the OperationContext for the number of lock-free reads when
    // constructed, and decrements on destruction.
    //
    // Doesn't change after construction, but only set if it's a collection and not a view.
    boost::optional<LockFreeReadsBlock> _lockFreeReadsBlock;

    // This must be constructed after LockFreeReadsBlock since LockFreeReadsBlock sets a flag that
    // GlobalLock uses in its constructor.
    Lock::GlobalLock _globalLock;

    // Holds the collection that was acquired from the catalog and logic for refetching the
    // collection state from the catalog when restoring from yield.
    //
    // Doesn't change after construction.
    CollectionPtr _collectionPtr;

    // Tracks whether any secondary collection namespace is a view or is sharded.
    //
    // Doesn't change after construction, see comment in "EmplaceHelper".  Should NOT invariant that
    // this is true when restoring from yield, because changing to be sharded is allowed, but
    // changing to a view is not.
    bool _secondaryNssIsAViewOrSharded{false};

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string.
    //
    // May change after construction, when restoring from yield.
    NamespaceString _resolvedNss;

    // Only set if _collectionPtr does not contain a nullptr and if the requested namesapce is a
    // view.
    //
    // May change after construction, when restoring from yield.
    std::shared_ptr<const ViewDefinition> _view;
};

/**
 * Creates either an AutoGetCollectionForRead or AutoGetCollectionForReadLockFree depending on
 * whether a lock-free read is supported.
 */
class AutoGetCollectionForReadMaybeLockFree {
public:
    AutoGetCollectionForReadMaybeLockFree(OperationContext* opCtx,
                                          const NamespaceStringOrUUID& nsOrUUID,
                                          AutoGetCollection::Options options = {});

    /**
     * Passthrough functions to either _autoGet or _autoGetLockFree.
     */
    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }
    const Collection* operator->() const {
        return getCollection().get();
    }
    const CollectionPtr& operator*() const {
        return getCollection();
    }
    const CollectionPtr& getCollection() const;
    const ViewDefinition* getView() const;
    const NamespaceString& getNss() const;
    bool isAnySecondaryNamespaceAViewOrSharded() const;

private:
    boost::optional<AutoGetCollectionForRead> _autoGet;
    boost::optional<AutoGetCollectionForReadLockFree> _autoGetLockFree;
};

/**
 * Logic common to both AutoGetCollectionForReadCommand and AutoGetCollectionForReadCommandLockFree.
 * Not intended for direct use.
 */
template <typename AutoGetCollectionForReadType>
class AutoGetCollectionForReadCommandBase {
    AutoGetCollectionForReadCommandBase(const AutoGetCollectionForReadCommandBase&) = delete;
    AutoGetCollectionForReadCommandBase& operator=(const AutoGetCollectionForReadCommandBase&) =
        delete;

public:
    AutoGetCollectionForReadCommandBase(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::Options options = {},
        AutoStatsTracker::LogMode logMode = AutoStatsTracker::LogMode::kUpdateTopAndCurOp);

    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }

    const Collection* operator->() const {
        return getCollection().get();
    }

    const CollectionPtr& operator*() const {
        return getCollection();
    }

    const CollectionPtr& getCollection() const {
        return _autoCollForRead.getCollection();
    }

    const ViewDefinition* getView() const {
        return _autoCollForRead.getView();
    }

    const NamespaceString& getNss() const {
        return _autoCollForRead.getNss();
    }

    bool isAnySecondaryNamespaceAViewOrSharded() const {
        return _autoCollForRead.isAnySecondaryNamespaceAViewOrSharded();
    }

protected:
    AutoGetCollectionForReadType _autoCollForRead;
    AutoStatsTracker _statsTracker;
};

/**
 * Same as AutoGetCollectionForRead, but in addition will add a Top entry upon destruction and
 * ensure the CurrentOp object has the right namespace and has started its timer.
 */
class AutoGetCollectionForReadCommand
    : public AutoGetCollectionForReadCommandBase<AutoGetCollectionForRead> {
public:
    AutoGetCollectionForReadCommand(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::Options options = {},
        AutoStatsTracker::LogMode logMode = AutoStatsTracker::LogMode::kUpdateTopAndCurOp)
        : AutoGetCollectionForReadCommandBase(opCtx, nsOrUUID, std::move(options), logMode) {}
};

/**
 * Same as AutoGetCollectionForReadCommand except no collection, database or RSTL lock is taken.
 */
class AutoGetCollectionForReadCommandLockFree {
public:
    AutoGetCollectionForReadCommandLockFree(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::Options options = {},
        AutoStatsTracker::LogMode logMode = AutoStatsTracker::LogMode::kUpdateTopAndCurOp);

    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }

    const Collection* operator->() const {
        return getCollection().get();
    }

    const CollectionPtr& operator*() const {
        return getCollection();
    }

    const CollectionPtr& getCollection() const {
        return _autoCollForReadCommandBase->getCollection();
    }

    const ViewDefinition* getView() const {
        return _autoCollForReadCommandBase->getView();
    }

    const NamespaceString& getNss() const {
        return _autoCollForReadCommandBase->getNss();
    }

    bool isAnySecondaryNamespaceAViewOrSharded() const {
        return _autoCollForReadCommandBase->isAnySecondaryNamespaceAViewOrSharded();
    }

private:
    boost::optional<AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadLockFree>>
        _autoCollForReadCommandBase;
};

/**
 * Creates either an AutoGetCollectionForReadCommand or AutoGetCollectionForReadCommandLockFree
 * depending on whether a lock-free read is supported in the situation per the results of
 * supportsLockFreeRead().
 */
class AutoGetCollectionForReadCommandMaybeLockFree {
public:
    AutoGetCollectionForReadCommandMaybeLockFree(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::Options options = {},
        AutoStatsTracker::LogMode logMode = AutoStatsTracker::LogMode::kUpdateTopAndCurOp);

    /**
     * Passthrough function to either _autoGet or _autoGetLockFree.
     */
    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }
    const Collection* operator->() const {
        return getCollection().get();
    }
    const CollectionPtr& operator*() const {
        return getCollection();
    }
    const CollectionPtr& getCollection() const;
    const ViewDefinition* getView() const;
    const NamespaceString& getNss() const;
    bool isAnySecondaryNamespaceAViewOrSharded() const;

private:
    boost::optional<AutoGetCollectionForReadCommand> _autoGet;
    boost::optional<AutoGetCollectionForReadCommandLockFree> _autoGetLockFree;
};

/**
 * Acquires the global MODE_IS lock and establishes a consistent CollectionCatalog and storage
 * snapshot.
 */
class AutoReadLockFree {
public:
    AutoReadLockFree(OperationContext* opCtx, Date_t deadline = Date_t::max());

private:
    // Sets a flag on the opCtx to inform subsequent code that the operation is running lock-free.
    LockFreeReadsBlock _lockFreeReadsBlock;

    Lock::GlobalLock _globalLock;
};

/**
 * Establishes a consistent CollectionCatalog with a storage snapshot. Also verifies Database
 * sharding state for the provided Db. Takes MODE_IS global lock.
 *
 * Similar to AutoGetCollectionForReadLockFree but does not take readConcern into account. Any
 * Collection returned by the stashed catalog will not refresh the storage snapshot on yield.
 *
 * Should only be used to read catalog metadata for a particular Db and not for reading from
 * Collection(s).
 */
class AutoGetDbForReadLockFree {
public:
    AutoGetDbForReadLockFree(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             Date_t deadline = Date_t::max());

private:
    // Sets a flag on the opCtx to inform subsequent code that the operation is running lock-free.
    LockFreeReadsBlock _lockFreeReadsBlock;

    Lock::GlobalLock _globalLock;
};

/**
 * Creates either an AutoGetDb or AutoGetDbForReadLockFree depending on whether a lock-free read is
 * supported in the situation per the results of supportsLockFreeRead().
 */
class AutoGetDbForReadMaybeLockFree {
public:
    AutoGetDbForReadMaybeLockFree(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  Date_t deadline = Date_t::max());

private:
    boost::optional<AutoGetDb> _autoGet;
    boost::optional<AutoGetDbForReadLockFree> _autoGetLockFree;
};

/**
 * Opens the database that we want to use and sets the appropriate namespace on the
 * current operation.
 */
class OldClientContext {
    OldClientContext(const OldClientContext&) = delete;
    OldClientContext& operator=(const OldClientContext&) = delete;

public:
    OldClientContext(OperationContext* opCtx, const NamespaceString& nss, bool doVersion = true);
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

/**
 * TODO (SERVER-69813): Get rid of this when ShardServerCatalogCacheLoader will be removed.
 * RAII type for letting secondary reads to block behind the PBW lock.
 * Note: Do not add additional usage. This is only temporary for ease of backport.
 */
struct BlockSecondaryReadsDuringBatchApplication_DONT_USE {
public:
    BlockSecondaryReadsDuringBatchApplication_DONT_USE(OperationContext* opCtx);
    ~BlockSecondaryReadsDuringBatchApplication_DONT_USE();

private:
    OperationContext* _opCtx{nullptr};
    boost::optional<bool> _originalSettings;
};

}  // namespace mongo
