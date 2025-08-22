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

#include "mongo/db/local_catalog/catalog_raii.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/catalog_helper.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/collection_yield_restore.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/shard_role_api/direct_connection_util.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/repl/collection_utils.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <functional>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

/**
 * Performs some sanity checks on the collection and database.
 */
void verifyDbAndCollection(OperationContext* opCtx,
                           LockMode modeColl,
                           const NamespaceStringOrUUID& nsOrUUID,
                           const NamespaceString& resolvedNss,
                           const Collection* coll,
                           Database* db,
                           bool verifyWriteEligible) {
    invariant(!nsOrUUID.isUUID() || coll,
              str::stream() << "Collection for " << resolvedNss.toStringForErrorMsg()
                            << " disappeared after successfully resolving "
                            << nsOrUUID.toStringForErrorMsg());

    invariant(!nsOrUUID.isUUID() || db,
              str::stream() << "Database for " << resolvedNss.toStringForErrorMsg()
                            << " disappeared after successfully resolving "
                            << nsOrUUID.toStringForErrorMsg());

    // In most cases we expect modifications for system.views to upgrade MODE_IX to MODE_X before
    // taking the lock. One exception is a query by UUID of system.views in a transaction. Usual
    // queries of system.views (by name, not UUID) within a transaction are rejected. However, if
    // the query is by UUID we can't determine whether the namespace is actually system.views until
    // we take the lock here. So we have this one last assertion.
    uassert(51070,
            "Modifications to system.views must take an exclusive lock",
            !resolvedNss.isSystemDotViews() || modeColl != MODE_IX);

    if (!db || !coll) {
        return;
    }

    // Verify that we are using the latest instance if we intend to perform writes.
    if (verifyWriteEligible) {
        auto latest = CollectionCatalog::latest(opCtx);
        if (!latest->isLatestCollection(opCtx, coll)) {
            throwWriteConflictException(str::stream() << "Unable to write to collection '"
                                                      << coll->ns().toStringForErrorMsg()
                                                      << "' due to catalog changes; please "
                                                         "retry the operation");
        }
        if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
            const auto mySnapshot =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
            if (mySnapshot && *mySnapshot < coll->getMinimumValidSnapshot()) {
                throwWriteConflictException(str::stream()
                                            << "Unable to write to collection '"
                                            << coll->ns().toStringForErrorMsg()
                                            << "' due to snapshot timestamp " << *mySnapshot
                                            << " being older than collection minimum "
                                            << *coll->getMinimumValidSnapshot()
                                            << "; please retry the operation");
            }
        }
    }
}

}  // namespace

AutoGetDb::AutoGetDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     boost::optional<LockMode> tenantLockMode,
                     Date_t deadline)
    : AutoGetDb(opCtx, dbName, mode, tenantLockMode, deadline, [] {
          Lock::GlobalLockOptions options;
          return options;
      }()) {}

AutoGetDb::AutoGetDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     boost::optional<LockMode> tenantLockMode,
                     Date_t deadline,
                     Lock::DBLockSkipOptions options)
    : _dbName(dbName), _dbLock(opCtx, dbName, mode, deadline, options, tenantLockMode), _db([&] {
          auto databaseHolder = DatabaseHolder::get(opCtx);
          return databaseHolder->getDb(opCtx, dbName);
      }()) {
    // Check if this operation is a direct connection and if it is authorized to be one after
    // acquiring the lock.
    if (!options.skipDirectConnectionChecks) {
        direct_connection_util::checkDirectShardOperationAllowed(opCtx, NamespaceString(dbName));
    }

    // The 'primary' database must be version checked for sharding.
    DatabaseShardingState::acquire(opCtx, _dbName)->checkDbVersionOrThrow(opCtx);
}

bool AutoGetDb::canSkipRSTLLock(const NamespaceStringOrUUID& nsOrUUID) {
    if (nsOrUUID.isNamespaceString()) {
        return repl::canCollectionSkipRSTLLockAcquisition(nsOrUUID.nss());
    }
    return false;
}

bool AutoGetDb::canSkipFlowControlTicket(const NamespaceStringOrUUID& nsOrUUID) {
    if (nsOrUUID.isNamespaceString()) {
        const auto& nss = nsOrUUID.nss();
        bool notReplicated = !nss.isReplicated();
        // TODO: Improve comment
        //
        // If the 'opCtx' is in a multi document transaction, pure reads on the
        // transaction session collections would acquire the global lock in the IX
        // mode and acquire a flow control ticket.
        bool isTransactionCollection = nss == NamespaceString::kSessionTransactionsTableNamespace ||
            nss == NamespaceString::kTransactionCoordinatorsNamespace;
        return notReplicated || isTransactionCollection;
    }
    return false;
}

AutoGetDb AutoGetDb::createForAutoGetCollection(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    LockMode modeColl,
    const auto_get_collection::OptionsWithSecondaryCollections& options) {
    auto& deadline = options._deadline;

    invariant(!opCtx->isLockFreeReadsOp());

    // Acquire the global/RSTL and all the database locks (may or may not be multiple
    // databases).
    Lock::DBLockSkipOptions dbLockOptions;
    if (options._globalLockOptions) {
        dbLockOptions = *options._globalLockOptions;
    } else {
        dbLockOptions.skipRSTLLock = canSkipRSTLLock(nsOrUUID);
        // Do not use write intent for non-replicated collections.
        if (nsOrUUID.isNamespaceString() && !nsOrUUID.nss().isReplicated()) {
            dbLockOptions.explicitIntent = isSharedLockMode(modeColl)
                ? rss::consensus::IntentRegistry::Intent::Read
                : rss::consensus::IntentRegistry::Intent::LocalWrite;
        }
        dbLockOptions.skipFlowControlTicket = canSkipFlowControlTicket(nsOrUUID);
    }
    // Skip checking direct connections for the database and only do so in AutoGetCollection
    dbLockOptions.skipDirectConnectionChecks = true;

    return AutoGetDb(opCtx,
                     nsOrUUID.dbName(),
                     isSharedLockMode(modeColl) ? MODE_IS : MODE_IX,
                     boost::none /* tenantLockMode */,
                     deadline,
                     std::move(dbLockOptions));
}

AutoGetDb::AutoGetDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline)
    : AutoGetDb(opCtx, dbName, mode, boost::none, deadline) {}

Database* AutoGetDb::ensureDbExists(OperationContext* opCtx) {
    if (_db) {
        return _db;
    }

    auto databaseHolder = DatabaseHolder::get(opCtx);
    _db = databaseHolder->openDb(opCtx, _dbName, nullptr);

    DatabaseShardingState::acquire(opCtx, _dbName)->checkDbVersionOrThrow(opCtx);

    return _db;
}

Database* AutoGetDb::refreshDbReferenceIfNull(OperationContext* opCtx) {
    if (!_db) {
        auto databaseHolder = DatabaseHolder::get(opCtx);
        _db = databaseHolder->getDb(opCtx, _dbName);

        DatabaseShardingState::acquire(opCtx, _dbName)->checkDbVersionOrThrow(opCtx);
    }
    return _db;
}


CollectionNamespaceOrUUIDLock::CollectionNamespaceOrUUIDLock(OperationContext* opCtx,
                                                             const NamespaceStringOrUUID& nsOrUUID,
                                                             LockMode mode,
                                                             Date_t deadline)
    : _lock(resolveAndLockCollectionByNssOrUUID(opCtx, nsOrUUID, mode, deadline)) {}

Lock::CollectionLock CollectionNamespaceOrUUIDLock::resolveAndLockCollectionByNssOrUUID(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    LockMode mode,
    Date_t deadline) {
    if (nsOrUUID.isNamespaceString()) {
        return Lock::CollectionLock{opCtx, nsOrUUID.nss(), mode, deadline};
    }

    auto resolveNs = [opCtx, &nsOrUUID] {
        return CollectionCatalog::get(opCtx)
            ->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(opCtx, nsOrUUID);
    };

    // We cannot be sure that the namespace we lock matches the UUID given because we resolve
    // the namespace from the UUID without the safety of a lock. Therefore, we will continue
    // to re-lock until the namespace we resolve from the UUID before and after taking the
    // lock is the same.
    while (true) {
        auto ns = resolveNs();
        Lock::CollectionLock lock{opCtx, ns, mode, deadline};
        if (ns == resolveNs()) {
            return lock;
        }
    }
}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     const Options& options)
    : AutoGetCollection(opCtx,
                        nsOrUUID,
                        modeColl,
                        options,
                        /*verifyWriteEligible=*/modeColl != MODE_IS) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     const Options& options,
                                     ForReadTag reader)
    : AutoGetCollection(opCtx, nsOrUUID, modeColl, options, /*verifyWriteEligible=*/false) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     const Options& options,
                                     bool verifyWriteEligible)
    : _autoDb(AutoGetDb::createForAutoGetCollection(opCtx, nsOrUUID, modeColl, options)) {

    auto& viewMode = options._viewMode;
    auto& deadline = options._deadline;
    auto& secondaryNssOrUUIDsBegin = options._secondaryNssOrUUIDsBegin;
    auto& secondaryNssOrUUIDsEnd = options._secondaryNssOrUUIDsEnd;

    // Out of an abundance of caution, force operations to acquire new snapshots after
    // acquiring exclusive collection locks. Operations that hold MODE_X locks make an
    // assumption that all writes are visible in their snapshot and no new writes will commit.
    // This may not be the case if an operation already has a snapshot open before acquiring an
    // exclusive lock.
    if (modeColl == MODE_X) {
        invariant(!shard_role_details::getRecoveryUnit(opCtx)->isActive(),
                  str::stream() << "Snapshot opened before acquiring X lock for "
                                << toStringForLogging(nsOrUUID));
    }

    // Acquire the collection locks, this will also ensure that the CollectionCatalog has been
    // correctly mapped to the given collections.
    uassert(
        ErrorCodes::InvalidNamespace,
        fmt::format("Namespace {} is not a valid collection name", nsOrUUID.toStringForErrorMsg()),
        nsOrUUID.isUUID() || (nsOrUUID.isNamespaceString() && nsOrUUID.nss().isValid()));

    catalog_helper::acquireCollectionLocksInResourceIdOrder(opCtx,
                                                            nsOrUUID,
                                                            modeColl,
                                                            deadline,
                                                            secondaryNssOrUUIDsBegin,
                                                            secondaryNssOrUUIDsEnd,
                                                            &_collLocks);

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    auto catalog = CollectionCatalog::get(opCtx);
    auto databaseHolder = DatabaseHolder::get(opCtx);

    // Check that the collections are all safe to use.
    //
    // Note that unlike the other AutoGet methods we do not look at commit pending entries here.
    // This is correct because we do not establish a consistent collection and must anyway get the
    // fully committed entry. As a result, if the UUID->NSS mappping was incorrect we'd detect it as
    // part of this NSS resolution.
    _resolvedNss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    _coll = CollectionPtr::CollectionPtr_UNSAFE(
        catalog->lookupCollectionByNamespace(opCtx, _resolvedNss));
    _coll.makeYieldable(opCtx, LockedCollectionYieldRestore{opCtx, _coll});

    if (_coll) {
        // It is possible for an operation to have created the database and collection after this
        // AutoGetCollection initialized its AutoGetDb, but before it has performed the collection
        // lookup. Thus, it is possible for AutoGetDb to hold nullptr while _coll is a valid
        // pointer. This would be unexpected, as for a collection to exist the database must exist.
        // We ensure the database reference is valid by refreshing it.
        _autoDb.refreshDbReferenceIfNull(opCtx);
    }

    // Recheck if this operation is a direct connection and if it is authorized to be one after
    // acquiring the collection locks.
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, _resolvedNss);

    verifyDbAndCollection(
        opCtx, modeColl, nsOrUUID, _resolvedNss, _coll.get(), _autoDb.getDb(), verifyWriteEligible);
    for (auto iter = secondaryNssOrUUIDsBegin; iter != secondaryNssOrUUIDsEnd; ++iter) {
        const auto& secondaryNssOrUUID = *iter;
        auto secondaryResolvedNss =
            catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID);
        auto secondaryColl = catalog->lookupCollectionByNamespace(opCtx, secondaryResolvedNss);
        auto secondaryDbName = secondaryNssOrUUID.dbName();
        verifyDbAndCollection(opCtx,
                              MODE_IS,
                              secondaryNssOrUUID,
                              secondaryResolvedNss,
                              secondaryColl,
                              databaseHolder->getDb(opCtx, secondaryDbName),
                              verifyWriteEligible);
    }

    const auto receivedShardVersion{
        OperationShardingState::get(opCtx).getShardVersion(_resolvedNss)};

    if (_coll) {
        // Fetch and store the sharding collection description data needed for use during the
        // operation. The shardVersion will be checked later if the shard filtering metadata is
        // fetched, ensuring both that the collection description info used here and the routing
        // table are consistent with the read request's shardVersion.
        //
        // Note: sharding versioning for an operation has no concept of multiple collections.
        auto scopedCss = CollectionShardingState::acquire(opCtx, _resolvedNss);
        scopedCss->checkShardVersionOrThrow(opCtx);

        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        // TODO SERVER-79296 remove call to isSharded
        if (collDesc.isSharded()) {
            _coll.setShardKeyPattern(collDesc.getKeyPattern());
        }

        checkCollectionUUIDMismatch(opCtx, *catalog, _resolvedNss, _coll, options._expectedUUID);

        if (receivedShardVersion && *receivedShardVersion == ShardVersion::UNSHARDED()) {
            shard_role_details::checkLocalCatalogIsValidForUnshardedShardVersion(
                opCtx, *catalog, _coll, _resolvedNss);
        }

        if (receivedShardVersion) {
            shard_role_details::checkShardingAndLocalCatalogCollectionUUIDMatch(
                opCtx, _resolvedNss, *receivedShardVersion, collDesc, _coll);
        }

        return;
    }

    if (receivedShardVersion && *receivedShardVersion == ShardVersion::UNSHARDED()) {
        shard_role_details::checkLocalCatalogIsValidForUnshardedShardVersion(
            opCtx, *catalog, _coll, _resolvedNss);
    }

    if (!options._expectedUUID) {
        // We only need to look up a view if an expected collection UUID was not provided. If this
        // namespace were a view, the collection UUID mismatch check would have failed above.
        if ((_view = catalog->lookupView(opCtx, _resolvedNss))) {
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Taking " << _resolvedNss.toStringForErrorMsg()
                                  << " lock for timeseries is not allowed",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                        !_view->timeseries());

            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Namespace " << _resolvedNss.toStringForErrorMsg()
                                  << " is a view, not a collection",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted);

            uassert(StaleConfigInfo(_resolvedNss,
                                    *receivedShardVersion,
                                    ShardVersion::UNSHARDED() /* wantedVersion */,
                                    ShardingState::get(opCtx)->shardId()),
                    str::stream() << "Namespace " << _resolvedNss.toStringForErrorMsg()
                                  << " is a view therefore the shard "
                                  << "version attached to the request must be unset or UNSHARDED",
                    !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED());
            return;
        }
    }

    // There is neither a collection nor a view for the namespace, so if we reached to this point
    // there are the following possibilities depending on the received shard version:
    //   1. ShardVersion::UNSHARDED: The request comes from a router and the operation entails the
    //      implicit creation of an unsharded collection. We can continue.
    //   2. ChunkVersion::IGNORED: The request comes from a router that broadcasted the same to all
    //      shards, but this shard doesn't own any chunks for the collection. We can continue.
    //   3. boost::none: The request comes from client directly connected to the shard. We can
    //      continue.
    //   4. Any other value: The request comes from a stale router on a collection or a view which
    //      was deleted time ago (or the user manually deleted it from from underneath of sharding).
    //      We return a stale config error so that the router recovers.

    uassert(StaleConfigInfo(_resolvedNss,
                            *receivedShardVersion,
                            boost::none /* wantedVersion */,
                            ShardingState::get(opCtx)->shardId()),
            str::stream() << "No metadata for namespace " << _resolvedNss.toStringForErrorMsg()
                          << " therefore the shard "
                          << "version attached to the request must be unset, UNSHARDED or IGNORED",
            !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED() ||
                ShardVersion::isPlacementVersionIgnored(*receivedShardVersion));

    checkCollectionUUIDMismatch(opCtx, *catalog, _resolvedNss, _coll, options._expectedUUID);
}

struct CollectionWriter::SharedImpl {
    SharedImpl(CollectionWriter* parent) : _parent(parent) {}

    CollectionWriter* _parent;
    std::function<Collection*()> _writableCollectionInitializer;
};

CollectionWriter::CollectionWriter(OperationContext* opCtx, CollectionAcquisition* acquisition)
    : _acquisition(acquisition), _managed(true), _sharedImpl(std::make_shared<SharedImpl>(this)) {

    _storedCollection = CollectionPtr(CollectionCatalog::get(opCtx)->establishConsistentCollection(
        opCtx, _acquisition->nss(), boost::none /*readTimestamp*/));
    _storedCollection.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, _storedCollection));

    _sharedImpl->_writableCollectionInitializer = [this, opCtx]() mutable {
        if (!_fence) {
            _fence = std::make_unique<ScopedLocalCatalogWriteFence>(opCtx, _acquisition);
        }

        return CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForMetadataWrite(
            opCtx, _acquisition->nss());
    };
}

CollectionWriter::CollectionWriter(OperationContext* opCtx, const UUID& uuid)
    : _managed(true), _sharedImpl(std::make_shared<SharedImpl>(this)) {

    // The CollectionWriter is guaranteed to hold a MODE_X lock. The Collection* returned by the
    // lookup can't disappear. The initialization here is therefore safe.
    _storedCollection = CollectionPtr::CollectionPtr_UNSAFE(
        CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid));
    _storedCollection.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, _storedCollection));

    _sharedImpl->_writableCollectionInitializer = [opCtx, uuid]() {
        return CollectionCatalog::get(opCtx)->lookupCollectionByUUIDForMetadataWrite(opCtx, uuid);
    };
}

CollectionWriter::CollectionWriter(OperationContext* opCtx, const NamespaceString& nss)
    : _managed(true), _sharedImpl(std::make_shared<SharedImpl>(this)) {

    _storedCollection = CollectionPtr(CollectionCatalog::get(opCtx)->establishConsistentCollection(
        opCtx, nss, boost::none /*readTimestamp*/));
    _storedCollection.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, _storedCollection));

    _sharedImpl->_writableCollectionInitializer = [opCtx, nss]() {
        return CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForMetadataWrite(opCtx,
                                                                                          nss);
    };
}

CollectionWriter::CollectionWriter(OperationContext* opCtx, AutoGetCollection& autoCollection)
    : _managed(true), _sharedImpl(std::make_shared<SharedImpl>(this)) {

    // The CollectionWriter is guaranteed to hold a MODE_X lock. The Collection* within the
    // AutoGetCollection can't disappear. The initialization here is therefore safe.
    _storedCollection = CollectionPtr::CollectionPtr_UNSAFE(autoCollection._coll.get());
    _storedCollection.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, _storedCollection));

    _sharedImpl->_writableCollectionInitializer = [originalColl = _storedCollection.get(),
                                                   opCtx,
                                                   &autoCollection]() {
        auto catalog = CollectionCatalog::get(opCtx);
        auto writableColl =
            catalog->lookupCollectionByNamespaceForMetadataWrite(opCtx, autoCollection.getNss());
        // Makes the internal CollectionPtr Yieldable and resets the writable Collection when
        // the write unit of work finishes so we re-fetches and re-clones the Collection if a
        // new write unit of work is opened.
        shard_role_details::getRecoveryUnit(opCtx)->registerChange(
            [&](OperationContext* opCtx, boost::optional<Timestamp> commitTime) {
                auto& nss = autoCollection.getNss();
                // This lookup happens after commit while the  CollectionWriter is guaranteed to
                // hold a MODE_X lock. The Collection* can't disappear and we are guranteed to see
                // the lastest change from the in-memory catalog. The initialization here is
                // therefore safe.
                auto collection =
                    CollectionCatalog::latest(opCtx)->lookupCollectionByNamespace(opCtx, nss);
                autoCollection._coll = CollectionPtr::CollectionPtr_UNSAFE(collection);
                invariant(autoCollection._coll->ns() == nss);
                // Make yieldable again
                autoCollection._coll.makeYieldable(
                    opCtx, LockedCollectionYieldRestore(opCtx, autoCollection._coll));
            },
            [&autoCollection, originalColl](OperationContext* opCtx) {
                // The CollectionWriter is guaranteed to hold a MODE_X lock. The Collection* within
                // the AutoGetCollection can't disappear. The initialization here is therefore safe.
                autoCollection._coll = CollectionPtr::CollectionPtr_UNSAFE(originalColl);
                autoCollection._coll.makeYieldable(
                    opCtx, LockedCollectionYieldRestore(opCtx, autoCollection._coll));
            });

        // Invalidate the collection pointer during modifications. This matches the behavior in
        // the acquisition case
        autoCollection._coll.reset();
        return writableColl;
    };
}

CollectionWriter::CollectionWriter(Collection* writableCollection)
    : _storedCollection(CollectionPtr(writableCollection)),
      _writableCollection(writableCollection),
      _managed(false) {}

CollectionWriter::~CollectionWriter() {
    // Notify shared state that this instance is destroyed
    if (_sharedImpl) {
        _sharedImpl->_parent = nullptr;
    }
}

Collection* CollectionWriter::getWritableCollection(OperationContext* opCtx) {
    // Acquire writable instance lazily if not already available
    if (!_writableCollection) {
        _writableCollection = _sharedImpl->_writableCollectionInitializer();

        // If we are using our stored Collection then we are not managed by an AutoGetCollection
        // and we need to manage lifetime here.
        if (_managed) {
            auto rollbackCollection = std::move(_storedCollection);

            // Resets the writable Collection when the write unit of work finishes so we re-fetch
            // and re-clone the Collection if a new write unit of work is opened. Holds the back
            // pointer to the CollectionWriter explicitly so we can detect if the instance is
            // already destroyed.
            shard_role_details::getRecoveryUnit(opCtx)->registerChange(
                [shared = _sharedImpl](OperationContext* opCtx, boost::optional<Timestamp>) {
                    if (shared->_parent) {
                        shared->_parent->_writableCollection = nullptr;

                        // Make the stored collection yieldable again as we now operate with the
                        // same instance as is in the catalog.
                        CollectionPtr& coll = shared->_parent->_storedCollection;
                        coll.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, coll));
                    }
                },
                [shared = _sharedImpl, rollbackCollection = std::move(rollbackCollection)](
                    OperationContext* opCtx) mutable {
                    if (shared->_parent) {
                        shared->_parent->_writableCollection = nullptr;

                        // Restore stored collection to its previous state. The rollback
                        // instance is already yieldable.
                        shared->_parent->_storedCollection = std::move(rollbackCollection);
                    }
                });

            _storedCollection = CollectionPtr(_writableCollection);
        }
    }
    return _writableCollection;
}

LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode) {
    return nss.isSystemDotViews() ? MODE_X : mode;
}

ReadSourceScope::ReadSourceScope(OperationContext* opCtx,
                                 RecoveryUnit::ReadSource readSource,
                                 boost::optional<Timestamp> provided,
                                 bool waitForOplog)
    : _opCtx(opCtx),
      _originalReadSource(shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource()) {
    // Abandoning the snapshot is unsafe when the snapshot is managed by a lock free read
    // helper.
    invariant(!_opCtx->isLockFreeReadsOp());

    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        _originalReadTimestamp =
            *shard_role_details::getRecoveryUnit(_opCtx)->getPointInTimeReadTimestamp();
    }

    shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();

    // Wait for oplog visibility if the caller requested it.
    if (waitForOplog) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        LocalOplogInfo* oplogInfo = LocalOplogInfo::get(opCtx);
        tassert(9478700, "Should have oplog avaiable at this point", oplogInfo);
        storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx, oplogInfo->getRecordStore());
    }
    shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(readSource, provided);
}

ReadSourceScope::~ReadSourceScope() {
    // Abandoning the snapshot is unsafe when the snapshot is managed by a lock free read
    // helper.
    invariant(!_opCtx->isLockFreeReadsOp());

    shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalReadSource,
                                                                            _originalReadTimestamp);
    } else {
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(_originalReadSource);
    }
}

AutoGetOplogFastPath::AutoGetOplogFastPath(OperationContext* opCtx,
                                           OplogAccessMode mode,
                                           Date_t deadline,
                                           const AutoGetOplogFastPathOptions& options) {
    auto lockMode = (mode == OplogAccessMode::kRead) ? MODE_IS : MODE_IX;
    if (mode == OplogAccessMode::kLogOp) {
        // Invariant that global lock is already held for kLogOp mode.
        invariant(shard_role_details::getLocker(opCtx)->isWriteLocked());
    } else {
        auto globalLkOptions = Lock::GlobalLockOptions{.skipRSTLLock = options.skipRSTLLock,
                                                       .explicitIntent = options.explicitIntent};
        _globalLock.emplace(
            opCtx, lockMode, deadline, Lock::InterruptBehavior::kThrow, globalLkOptions);
    }

    _stashedCatalog = CollectionCatalog::get(opCtx);
    _oplogInfo = LocalOplogInfo::get(opCtx);
    // The oplog is a special collection that is always present in the catalog and can't be dropped,
    // so it will be found by the lookup.
    // We also stash the catalog, so the Collection* returned by the lookup will not be invalidated
    // by any catalog changes.
    // The initialization here is therefore safe.
    _oplog = CollectionPtr::CollectionPtr_UNSAFE(
        _stashedCatalog->lookupCollectionByNamespace(opCtx, NamespaceString::kRsOplogNamespace));
}

AutoGetChangeCollection::AutoGetChangeCollection(OperationContext* opCtx,
                                                 AutoGetChangeCollection::AccessMode mode,
                                                 const TenantId& tenantId,
                                                 Date_t deadline) {
    const auto changeCollectionNamespaceString = NamespaceString::makeChangeCollectionNSS(tenantId);
    if (AccessMode::kRead == mode || AccessMode::kWrite == mode ||
        AccessMode::kUnreplicatedWrite == mode) {
        auto options = AutoGetCollection::Options{}.deadline(deadline);
        if (AccessMode::kUnreplicatedWrite == mode) {
            options.globalLockOptions(Lock::GlobalLockOptions{
                .explicitIntent = rss::consensus::IntentRegistry::Intent::LocalWrite});
        }
        // Treat this as a regular AutoGetCollection.
        _coll.emplace(opCtx,
                      changeCollectionNamespaceString,
                      mode == AccessMode::kRead ? MODE_IS : MODE_IX,
                      options);
        return;
    }
    tassert(6671506, "Invalid lock mode", AccessMode::kWriteInOplogContext == mode);

    // When writing to the change collection as part of normal operation, we avoid taking any new
    // locks. The caller must already have the tenant lock that protects the tenant specific change
    // stream collection from being dropped. That's sufficient for acquiring a raw collection
    // pointer.
    tassert(6671500,
            str::stream() << "Lock not held in IX mode for the tenant " << tenantId,
            shard_role_details::getLocker(opCtx)->isLockHeldForMode(
                ResourceId(ResourceType::RESOURCE_TENANT, tenantId), LockMode::MODE_IX));
    auto changeCollectionPtr = CollectionCatalog::get(opCtx)->establishConsistentCollection(
        opCtx, changeCollectionNamespaceString, boost::none /*readTimestamp=*/);
    _changeCollection = CollectionPtr(changeCollectionPtr);
    _changeCollection.makeYieldable(opCtx, LockedCollectionYieldRestore(opCtx, _changeCollection));
}

const Collection* AutoGetChangeCollection::operator->() const {
    return (**this).get();
}

const CollectionPtr& AutoGetChangeCollection::operator*() const {
    return (_coll) ? *(*_coll) : _changeCollection;
}

AutoGetChangeCollection::operator bool() const {
    return static_cast<bool>(**this);
}

}  // namespace mongo
