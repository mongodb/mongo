/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/views/view.h"
#include "mongo/util/uuid.h"

#include <list>
#include <memory>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class PlacementConcern {
public:
    PlacementConcern(boost::optional<DatabaseVersion> dbVersion,
                     boost::optional<ShardVersion> shardVersion)
        : _dbVersion(std::move(dbVersion)), _shardVersion(std::move(shardVersion)) {}

    // Pretends that the collection is unsharded. Acquisitions with this PlacementConcern will have
    // always have UNSHARDED description and filter, even if they are sharded. Only for use in
    // internal code paths that require it. Possible data loss if used incorrectly!
    static const PlacementConcern kPretendUnsharded;

    const boost::optional<DatabaseVersion>& getDbVersion() const {
        return _dbVersion;
    }
    const boost::optional<ShardVersion>& getShardVersion() const {
        return _shardVersion;
    }

    bool operator==(const PlacementConcern&) const = default;

private:
    PlacementConcern(boost::optional<DatabaseVersion> dbVersion,
                     boost::optional<ShardVersion> shardVersion,
                     bool pretendUnsharded)
        : _dbVersion(std::move(dbVersion)),
          _shardVersion(std::move(shardVersion)),
          _pretendUnsharded(pretendUnsharded) {}

    boost::optional<DatabaseVersion> _dbVersion;
    boost::optional<ShardVersion> _shardVersion;
    bool _pretendUnsharded = false;
};

struct AcquisitionPrerequisites {
    enum PlacementConcernPlaceholder {
        /**
         * Special PlacementConcern which mimics direct connection to a shard, causing the
         * acquisition to bypass any sharding checks and acquire just the local catalog portion. Any
         * sharding service values, such as the description or the filter are not allowed to be used
         * (will invariant).
         *
         * Note the *with potential data loss* in the name, which indicates that it allows the
         * caller to operate on a collection which is not even on the local shard, thus if used
         * incorrectly can lead to data loss.
         */
        kLocalCatalogOnlyWithPotentialDataLoss,
    };

    using PlacementConcernVariant = std::variant<PlacementConcern, PlacementConcernPlaceholder>;

    enum ViewMode { kMustBeCollection, kCanBeView };

    enum OperationType { kRead, kUnreplicatedWrite, kWrite };

    AcquisitionPrerequisites(NamespaceString nss,
                             boost::optional<UUID> uuid,
                             repl::ReadConcernArgs readConcern,
                             PlacementConcernVariant placementConcern,
                             OperationType operationType,
                             ViewMode viewMode,
                             Date_t lockAcquisitionDeadline = Date_t::max())
        : nss(std::move(nss)),
          uuid(std::move(uuid)),
          readConcern(std::move(readConcern)),
          placementConcern(std::move(placementConcern)),
          operationType(operationType),
          viewMode(viewMode),
          lockAcquisitionDeadline(lockAcquisitionDeadline) {}

    NamespaceString nss;
    boost::optional<UUID> uuid;

    repl::ReadConcernArgs readConcern;
    PlacementConcernVariant placementConcern;
    OperationType operationType;
    ViewMode viewMode;
    Date_t lockAcquisitionDeadline;

    // When true, present a catalog consistent with the read timestamp. When false, relax this by
    // presenting always the latest catalog unconditionally.
    // IMPORTANT: False has weaker consistency guarantees and must only be only used by
    // IndexBuildsCoordinator.
    // TODO SERVER-109542: Remove this.
    bool useConsistentCatalog = true;
};

namespace shard_role_details {

struct AcquisitionLocks {
    // TODO SERVER-77213: This should mostly go away once the Locker resides inside
    // TransactionResources and the underlying locks point to it instead of the opCtx.
    LockMode globalLock = MODE_NONE;
    Lock::GlobalLockOptions globalLockOptions;
    bool hasLockFreeReadsBlock = false;

    LockMode dbLock = MODE_NONE;
    Lock::DBLockSkipOptions dbLockOptions;

    LockMode collLock = MODE_NONE;
};

struct AcquiredBase {
    AcquiredBase(int acquireCollectionCallNum,
                 AcquisitionPrerequisites prerequisites,
                 std::shared_ptr<Lock::DBLock> dbLock,
                 boost::optional<Lock::CollectionLock> collectionLock,
                 std::shared_ptr<LockFreeReadsBlock> lockFreeReadsBlock,
                 std::shared_ptr<Lock::GlobalLock> globalLock,
                 AcquisitionLocks locksRequirements)
        : acquireCollectionCallNum(acquireCollectionCallNum),
          prerequisites(std::move(prerequisites)),
          dbLock(std::move(dbLock)),
          collectionLock(std::move(collectionLock)),
          lockFreeReadsBlock(std::move(lockFreeReadsBlock)),
          globalLock(std::move(globalLock)),
          locks(std::move(locksRequirements)) {}

    // The number containing at which acquireCollection call this acquisition was built. All
    // acquisitions created on the same call to acquireCollection will share the same number
    // and contain shared_ptrs to the Global/DB/Lock-free locks shared amongst them.
    int acquireCollectionCallNum;

    AcquisitionPrerequisites prerequisites;

    std::shared_ptr<Lock::DBLock> dbLock;
    boost::optional<Lock::CollectionLock> collectionLock;

    std::shared_ptr<LockFreeReadsBlock> lockFreeReadsBlock;
    std::shared_ptr<Lock::GlobalLock> globalLock;  // Only for lock-free acquisitions. Otherwise the
                                                   // global lock is held by 'dbLock'.

    AcquisitionLocks locks;

    // Maintains a reference count to how many references there are to this acquisition by the
    // CollectionAcquisition/ViewAcquisition class.
    mutable int64_t refCount = 0;
};

struct AcquiredCollection : AcquiredBase {
    AcquiredCollection(int acquireCollectionCallNum,
                       AcquisitionPrerequisites prerequisites,
                       std::shared_ptr<Lock::DBLock> dbLock,
                       boost::optional<Lock::CollectionLock> collectionLock,
                       std::shared_ptr<LockFreeReadsBlock> lockFreeReadsBlock,
                       std::shared_ptr<Lock::GlobalLock> globalLock,
                       AcquisitionLocks locksRequirements,
                       boost::optional<ScopedCollectionDescription> collectionDescription,
                       boost::optional<ScopedCollectionFilter> ownershipFilter,
                       CollectionPtr collectionPtr)
        : AcquiredBase(acquireCollectionCallNum,
                       std::move(prerequisites),
                       std::move(dbLock),
                       std::move(collectionLock),
                       std::move(lockFreeReadsBlock),
                       std::move(globalLock),
                       std::move(locksRequirements)),
          collectionDescription(std::move(collectionDescription)),
          ownershipFilter(std::move(ownershipFilter)),
          collectionPtr(std::move(collectionPtr)),
          invalidated(false) {}

    AcquiredCollection(int acquireCollectionCallNum,
                       AcquisitionPrerequisites prerequisites,
                       std::shared_ptr<Lock::DBLock> dbLock,
                       boost::optional<Lock::CollectionLock> collectionLock,
                       AcquisitionLocks locksRequirements,
                       CollectionPtr collectionPtr)
        : AcquiredCollection(acquireCollectionCallNum,
                             std::move(prerequisites),
                             std::move(dbLock),
                             std::move(collectionLock),
                             nullptr,
                             nullptr,
                             std::move(locksRequirements),
                             boost::none,
                             boost::none,
                             std::move(collectionPtr)) {};

    boost::optional<ScopedCollectionDescription> collectionDescription;
    boost::optional<ScopedCollectionFilter> ownershipFilter;

    CollectionPtr collectionPtr;

    // Indicates whether this acquisition has been invalidated after a ScopedLocalCatalogWriteFence
    // was unable to restore it on rollback.
    bool invalidated;
};

struct AcquiredView : AcquiredBase {
    std::shared_ptr<const ViewDefinition> viewDefinition;
};

/**
 * Interface for locking. Caller DOES NOT own pointer.
 */
// TODO (SERVER-77213): Move implementation to .cpp file
inline Locker* getLocker(OperationContext* opCtx) {
    return opCtx->lockState_DO_NOT_USE();
}

inline const Locker* getLocker(const OperationContext* opCtx) {
    return opCtx->lockState_DO_NOT_USE();
}

/**
 * Sets the locker for use by this OperationContext. Call during OperationContext initialization,
 * only.
 */
void makeLockerOnOperationContext(OperationContext* opCtx);

/**
 * Swaps the locker, releasing the old locker to the caller.
 * The client lock is going to be acquired by this function.
 */
std::unique_ptr<Locker> swapLocker(OperationContext* opCtx, std::unique_ptr<Locker> newLocker);
std::unique_ptr<Locker> swapLocker(OperationContext* opCtx,
                                   std::unique_ptr<Locker> newLocker,
                                   ClientLock& clientLock);

/**
 * Get the RecoveryUnit for the given opCtx. Caller DOES NOT own pointer.
 */
// TODO (SERVER-77213): Move implementation to .cpp file
inline RecoveryUnit* getRecoveryUnit(OperationContext* opCtx) {
    return opCtx->recoveryUnit_DO_NOT_USE();
}

inline const RecoveryUnit* getRecoveryUnit(const OperationContext* opCtx) {
    return opCtx->recoveryUnit_DO_NOT_USE();
}

/**
 * Returns the RecoveryUnit (same return value as recoveryUnit()) but the caller takes
 * ownership of the returned RecoveryUnit, and the OperationContext instance relinquishes
 * ownership. Sets the RecoveryUnit to NULL. Requires holding the client lock.
 */
std::unique_ptr<RecoveryUnit> releaseRecoveryUnit(OperationContext* opCtx);
std::unique_ptr<RecoveryUnit> releaseRecoveryUnit(OperationContext* opCtx, ClientLock& clientLock);

/*
 * Sets up a new, inactive RecoveryUnit in the OperationContext. Destroys any previous recovery
 * unit and executes its rollback handlers.
 */
// TODO (SERVER-77213): Move implementation to .cpp file
inline void replaceRecoveryUnit(OperationContext* opCtx) {
    ClientLock lk(opCtx->getClient());
    opCtx->replaceRecoveryUnit_DO_NOT_USE(lk);
}

/*
 * Similar to replaceRecoveryUnit(), but returns the previous recovery unit like
 * releaseRecoveryUnit(). Requires holding the client lock.
 */
std::unique_ptr<RecoveryUnit> releaseAndReplaceRecoveryUnit(OperationContext* opCtx,
                                                            ClientLock& clientLock);

/**
 * Associates the OperatingContext with a different RecoveryUnit for getMore or
 * subtransactions, see RecoveryUnitSwap. The new state is passed and the old state is
 * returned separately even though the state logically belongs to the RecoveryUnit,
 * as it is managed by the OperationContext. The client lock is going to be acquired by this
 * function.
 */
WriteUnitOfWork::RecoveryUnitState setRecoveryUnit(OperationContext* opCtx,
                                                   std::unique_ptr<RecoveryUnit> unit,
                                                   WriteUnitOfWork::RecoveryUnitState state);
WriteUnitOfWork::RecoveryUnitState setRecoveryUnit(OperationContext* opCtx,
                                                   std::unique_ptr<RecoveryUnit> unit,
                                                   WriteUnitOfWork::RecoveryUnitState state,
                                                   ClientLock& clientLock);

WriteUnitOfWork* getWriteUnitOfWork(OperationContext* opCtx);

void setWriteUnitOfWork(OperationContext* opCtx, std::unique_ptr<WriteUnitOfWork> writeUnitOfWork);

/**
 * This class is a container for all the collection resources which are currently acquired by a
 * given operation. Operations consist of one or more transactions, which "acquire" and "release"
 * collections within their lifetime.
 *
 * Transactions start either explicitly (through the construction of a WUOW) or implicitly, from the
 * moment the first collection is acquired. They last until the last collection snapshot is released
 * or the WriteUnitOfWork commits (whichever is longer).
 *
 * Because of the above definition, within a transaction, acquisitions are always 2-phase, meaning
 * that acquiring a collection and then releasing it will defer the release until the transaction
 * actually commits. The boundaries of the transaction are considered to be the WUOW. If there is no
 * WUOW, the transaction ends when the snapshot is released.
 *
 * There are three steps associated with each acquisition:
 *
 *  - Locking: Acquiring the necessary lock manager locks in order to ensure stability of the
 * snapshot for the duration of the acquisition.
 *  - Snapshotting: Taking a consistent snapshot across all the "services" associated with the
 * collection (shard filter, storage catalog, data snapshot).
 *  - Resource reservation: This is service-specific and indicates setting the necessary state so
 * that the snapshot is consistent for the duration of the acquisition. Example of resource
 * acquisition is the RangePreserver, which blocks orphan cleanups.
 *
 * Acquiring a collection performs all three steps: locking, resource reservation and snapshotting.
 *
 * Releasing a collection performs the inverse of acquisition, freeing locks, reservations and the
 * snapshot, such that a new acquire may see newer state (if the readConcern of the transaction
 * permits it).
 *
 * Yielding *all* transaction resources only frees locks and the snapshot, but it keeps the resource
 * reservations.
 *
 * Restoring *all* transaction resources only performs locking and snapshotting (in accordance with
 * the read concern of the operation).
 */
struct TransactionResources {
    TransactionResources();

    TransactionResources(TransactionResources&&) = delete;
    TransactionResources& operator=(TransactionResources&&) = delete;

    TransactionResources(TransactionResources&) = delete;
    TransactionResources& operator=(TransactionResources&) = delete;

    ~TransactionResources();

    static TransactionResources& get(OperationContext* opCtx);

    static bool isPresent(OperationContext* opCtx);

    static std::unique_ptr<TransactionResources> detachFromOpCtx(OperationContext* opCtx);
    static void attachToOpCtx(OperationContext* opCtx,
                              std::unique_ptr<TransactionResources> transactionResources);

    AcquiredCollection& addAcquiredCollection(AcquiredCollection&& acquiredCollection);
    const AcquiredView& addAcquiredView(AcquiredView&& acquiredView);

    void releaseAllResourcesOnCommitOrAbort() noexcept;

    /**
     * Asserts that this transaction context is not holding any collection acquisitions (i.e., it is
     * pristine). Used for invarianting in places where we do not expect an existing snapshot to
     * have been acquired because the caller expects to operate on latest.
     */
    void assertNoAcquiredCollections() const;

    bool isEmpty() const {
        return state == State::EMPTY;
    }

    /**
     * Transaction resources can only be in one of 4 states:
     * - EMPTY: This state is equivalent to a brand new constructed transaction resources which have
     *   never received an acquisition.
     * - ACTIVE: There is at least one acquisition in use and the resources have not been yielded.
     * - YIELDED: The resources are either yielded or in the process of reacquisition after a yield.
     * - STASHED: The resources have been stashed for subsequent getMore operations.
     * - FAILED: The reacquisition after a yield failed, we cannot perform any new acquisitions and
     *   the operation must release all acquisitions. The operation must effectively cancel the
     *   current operation.
     *
     * The set of valid transitions are:
     * - EMPTY <-> ACTIVE <-> YIELDED
     * - EMPTY <-> ACTIVE <-> STASHED
     * - STASHED <-> FAILED -> EMPTY
     * - YIELDED <-> FAILED -> EMPTY
     */
    enum class State { EMPTY, ACTIVE, STASHED, YIELDED, FAILED };

    State state{State::EMPTY};

    ////////////////////////////////////////////////////////////////////////////////////////
    // Global resources (cover all collections for the operation)

    // The read concern with which the transaction runs. All acquisitions must match that read
    // concern.
    boost::optional<repl::ReadConcernArgs> readConcern;

    // Set of locks acquired by the operation or nullptr if yielded.
    std::unique_ptr<Locker> locker;

    ////////////////////////////////////////////////////////////////////////////////////////
    // Per-collection resources

    using AcquiredCollections = std::list<AcquiredCollection>;
    using AcquiredViews = std::list<AcquiredView>;

    // Set of all collections which are currently acquired
    AcquiredCollections acquiredCollections;
    AcquiredViews acquiredViews;

    // Reference counters used for controlling how many references there are to the
    // TransactionResources object.
    int64_t collectionAcquisitionReferences = 0;
    int64_t viewAcquisitionReferences = 0;

    ////////////////////////////////////////////////////////////////////////////////////////
    // Yield/restore logic

    // If this value is set, indicates that yield has been performed on the owning
    // TransactionResources resources and the yielded state is contained in the structure below.
    struct YieldedStateHolder {
        Locker::LockSnapshot yieldedLocker;
    };
    boost::optional<YieldedStateHolder> yielded;

    // The number of times we have called acquireCollection* on these TransactionResources. The
    // number is used to identify acquisitions that share the same global/db locks.
    int currentAcquireCallCount = 0;

    // The catalog epoch when the resources were first acquired.
    boost::optional<int64_t> catalogEpoch;

    int increaseAcquireCollectionCallCount() {
        return currentAcquireCallCount++;
    }
};

}  // namespace shard_role_details
}  // namespace mongo
