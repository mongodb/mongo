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

#include "mongo/base/string_data.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_type.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/container/flat_set.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
                     boost::optional<std::vector<NamespaceStringOrUUID>::const_iterator>
                         secondaryNssVectorBegin = boost::none,
                     boost::optional<std::vector<NamespaceStringOrUUID>::const_iterator>
                         secondaryNssVectorEnd = boost::none);

    /**
     * Records stats about the current operation via Top, if 'logMode' is 'kUpdateTop' or
     * 'kUpdateTopAndCurOp'.
     */
    ~AutoStatsTracker();

private:
    OperationContext* _opCtx;
    Top::LockType _lockType;
    const LogMode _logMode;
    boost::container::flat_set<NamespaceString,
                               std::less<NamespaceString>,
                               absl::InlinedVector<NamespaceString, 1>>
        _nssSet;
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
                             const AutoGetCollection::Options& = {});

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

    bool isAnySecondaryNamespaceAView() const {
        return _isAnySecondaryNamespaceAView;
    }

private:
    // Ordering matters, the _collLocks should destruct before the _autoGetDb releases the
    // rstl/global/database locks.
    AutoGetDb _autoDb;
    std::vector<CollectionNamespaceOrUUIDLock> _collLocks;

    CollectionPtr _coll;
    std::shared_ptr<const ViewDefinition> _view;

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string
    NamespaceString _resolvedNss;

    // Tracks whether any secondary collection namespaces is a view.
    bool _isAnySecondaryNamespaceAView = false;
};

/**
 * Same as AutoGetCollectionForRead above except does not take collection, database or rstl locks.
 * Takes the global lock, same as AutoGetCollectionForRead. Ensures a consistent in-memory and
 * on-disk view of the storage catalog.
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

    bool isAnySecondaryNamespaceAView() const {
        return _isAnySecondaryNamespaceAView;
    }

private:
    ConsistentCollection _restoreFromYield(OperationContext* opCtx,
                                           boost::optional<UUID> optUuid,
                                           bool hasSecondaryNamespaces);

    // Whether or not this AutoGetCollectionForReadLockFree is being constructed while
    // there's already a lock-free read in progress.
    bool _isLockFreeReadSubOperation;

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

    // Tracks whether any secondary collection namespace is a view. Note that this should not change
    // after construction because even during a yield, it is not possible for a regular collection
    // to become a view.
    bool _isAnySecondaryNamespaceAView{false};

    // If the object was instantiated with a UUID, contains the resolved namespace, otherwise it is
    // the same as the input namespace string.
    //
    // May change after construction, when restoring from yield.
    NamespaceString _resolvedNss;

    // Holds a copy of '_resolvedNss.dbName()'. Unlike '_resolvedNss', this field does _not_ change
    // after construction.
    DatabaseName _resolvedDbName;

    // Only set if _collectionPtr does not contain a nullptr and if the requested namespace is a
    // view.
    //
    // May change after construction, when restoring from yield.
    std::shared_ptr<const ViewDefinition> _view;

    AutoGetCollection::Options _options;
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
    bool isAnySecondaryNamespaceAView() const;

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
        const AutoGetCollection::Options& options = {},
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

    bool isAnySecondaryNamespaceAView() const {
        return _autoCollForRead.isAnySecondaryNamespaceAView();
    }

protected:
    boost::optional<AutoStatsTracker> _statsTracker;
    AutoGetCollectionForReadType _autoCollForRead;
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
class AutoGetCollectionForReadCommandLockFree
    : public AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadLockFree> {
public:
    AutoGetCollectionForReadCommandLockFree(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollection::Options options = {},
        AutoStatsTracker::LogMode logMode = AutoStatsTracker::LogMode::kUpdateTopAndCurOp)
        : AutoGetCollectionForReadCommandBase(opCtx, nsOrUUID, options, logMode) {}

    explicit operator bool() const {
        return static_cast<bool>(getCollection());
    }

    const Collection* operator->() const {
        return getCollection().get();
    }

    const CollectionPtr& operator*() const {
        return getCollection();
    }
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
    query_shape::CollectionType getCollectionType() const;
    const ViewDefinition* getView() const;
    const NamespaceString& getNss() const;
    bool isAnySecondaryNamespaceAView() const;

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
 * Returns a MODE_IX LockMode if a read is performed under readConcern level snapshot, or a MODE_IS
 * lock otherwise. MODE_IX acquisition will allow a read to participate in two-phase locking.
 * Throws an exception if 'system.views' is being queried within a transaction.
 */
LockMode getLockModeForQuery(OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID);

/**
 * When in scope, enforces prepare conflicts in the storage engine. Reads and writes in this scope
 * will block on accessing an already updated document which is in prepared state. And they will
 * unblock after the prepared transaction that performed the update commits/aborts.
 */
class EnforcePrepareConflictsBlock {
public:
    explicit EnforcePrepareConflictsBlock(OperationContext* opCtx)
        : _opCtx(opCtx),
          _originalValue(shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior()) {
        // It is illegal to call setPrepareConflictBehavior() while any storage transaction is
        // active. setPrepareConflictBehavior() invariants that there is no active storage
        // transaction.
        shard_role_details::getRecoveryUnit(_opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kEnforce);
    }

    ~EnforcePrepareConflictsBlock() {
        // If we are still holding locks, we might still have open storage transactions. However, we
        // did not start with any active transactions when we first entered the scope. And
        // transactions started within this scope cannot be reused outside of the scope. So we need
        // to call abandonSnapshot() to close any open transactions on destruction. Any reads or
        // writes should have already completed as we are exiting the scope. Therefore, this call is
        // safe.
        if (shard_role_details::getLocker(_opCtx)->isLocked()) {
            shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        }
        // It is illegal to call setPrepareConflictBehavior() while any storage transaction is
        // active. There should not be any active transaction if we are not holding locks. If locks
        // are still being held, the above abandonSnapshot() call should have already closed all
        // storage transactions.
        shard_role_details::getRecoveryUnit(_opCtx)->setPrepareConflictBehavior(_originalValue);
    }

private:
    OperationContext* _opCtx;
    PrepareConflictBehavior _originalValue;
};

// Asserts whether the read concern is supported for the given collection with the specified read
// source.
void assertReadConcernSupported(const CollectionPtr& coll,
                                const repl::ReadConcernArgs& readConcernArgs,
                                const RecoveryUnit::ReadSource& readSource);
}  // namespace mongo
