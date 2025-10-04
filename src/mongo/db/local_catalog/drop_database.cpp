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


#include "mongo/db/local_catalog/drop_database.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

MONGO_FAIL_POINT_DEFINE(dropDatabaseHangAfterAllCollectionsDrop);
MONGO_FAIL_POINT_DEFINE(dropDatabaseHangBeforeInMemoryDrop);
MONGO_FAIL_POINT_DEFINE(dropDatabaseHangAfterWaitingForIndexBuilds);
MONGO_FAIL_POINT_DEFINE(dropDatabaseHangHoldingLock);
MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionDuringDropDatabase);

namespace {

Status _checkNssAndReplState(OperationContext* opCtx, Database* db, const DatabaseName& dbName) {
    if (!db) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Could not drop database " << dbName.toStringForErrorMsg()
                                    << " because it does not exist");
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool userInitiatedWritesAndNotPrimary =
        opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping database "
                                    << dbName.toStringForErrorMsg());
    }

    return Status::OK();
}

/**
 * Removes database from catalog and writes dropDatabase entry to oplog.
 *
 * Throws on errors.
 */
void _finishDropDatabase(OperationContext* opCtx,
                         const DatabaseName& dbName,
                         Database* db,
                         std::size_t numCollections,
                         bool abortIndexBuilds,
                         bool fromMigrate) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_X));

    if (!abortIndexBuilds) {
        IndexBuildsCoordinator::get(opCtx)->assertNoBgOpInProgForDb(dbName);
    }

    // Testing depends on this failpoint stopping execution before the dropDatabase oplog entry is
    // written, as well as before the in-memory state is cleared.
    if (MONGO_unlikely(dropDatabaseHangBeforeInMemoryDrop.shouldFail())) {
        LOGV2(20334, "dropDatabase - fail point dropDatabaseHangBeforeInMemoryDrop enabled");
        dropDatabaseHangBeforeInMemoryDrop.pauseWhileSet(opCtx);
    }

    writeConflictRetry(opCtx, "dropDatabase_database", NamespaceString(dbName), [&] {
        // We need to replicate the dropDatabase oplog entry and clear the collection catalog in the
        // same transaction. This is to prevent stepdown from interrupting between these two
        // operations and leaving this node in an inconsistent state.
        WriteUnitOfWork wunit(opCtx);
        opCtx->getServiceContext()->getOpObserver()->onDropDatabase(opCtx, dbName, fromMigrate);

        auto databaseHolder = DatabaseHolder::get(opCtx);
        databaseHolder->dropDb(opCtx, db);

        if (MONGO_unlikely(throwWriteConflictExceptionDuringDropDatabase.shouldFail())) {
            throwWriteConflictException(
                "Write conflict due to throwWriteConflictExceptionDuringDropDatabase fail point");
        }

        wunit.commit();
    });

    LOGV2(20336, "dropDatabase", logAttrs(dbName), "numCollectionsDropped"_attr = numCollections);
}

Status _dropDatabase(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     bool abortIndexBuilds,
                     bool markFromMigrate) {
    // As this code can potentially require replication we disallow holding locks entirely. Holding
    // of any locks is disallowed while awaiting replication because this can potentially block for
    // long time while doing network activity.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    uassert(ErrorCodes::IllegalOperation,
            "Cannot drop a database in read-only mode",
            !opCtx->readOnly());

    // As of SERVER-32205, dropping the admin database is prohibited.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Dropping the '" << dbName.toStringForErrorMsg()
                          << "' database is prohibited.",
            !dbName.isAdminDB());

    {
        CurOp::get(opCtx)->ensureStarted();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS(lk, dbName);
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    std::size_t numCollectionsToDrop = 0;
    std::size_t numCollections = 0;

    const auto tenantLockMode{
        boost::make_optional(dbName.tenantId() && dbName.isConfigDB(), MODE_X)};
    {
        boost::optional<AutoGetDb> autoDB;
        autoDB.emplace(opCtx, dbName, MODE_X /* database lock mode*/, tenantLockMode);

        Database* db = autoDB->getDb();
        Status status = _checkNssAndReplState(opCtx, db, dbName);
        if (!status.isOK()) {
            return status;
        }

        if (CollectionCatalog::get(opCtx)->isDropPending(dbName)) {
            return Status(ErrorCodes::DatabaseDropPending,
                          str::stream() << "The database is currently being dropped. Database: "
                                        << dbName.toStringForErrorMsg());
        }

        if (MONGO_unlikely(dropDatabaseHangHoldingLock.shouldFail())) {
            LOGV2(7490900,
                  "dropDatabase - fail point dropDatabaseHangHoldingLock "
                  "enabled");
            dropDatabaseHangHoldingLock.pauseWhileSet();
        }

        LOGV2(20337, "dropDatabase - starting", logAttrs(dbName));
        CollectionCatalog::write(
            opCtx, [&dbName](CollectionCatalog& catalog) { catalog.addDropPending(dbName); });
        ScopeGuard dropPendingGuard([opCtx, &dbName] {
            CollectionCatalog::write(opCtx, [&dbName](CollectionCatalog& catalog) {
                catalog.removeDropPending(dbName);
            });
        });
        auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

        if (abortIndexBuilds) {
            // We need to keep aborting all the active index builders for this database until there
            // are none left when we retrieve the exclusive database lock again.
            while (indexBuildsCoord->inProgForDb(dbName)) {
                // Drop locks. The drop helper will acquire locks on our behalf.
                autoDB = boost::none;

                // Sends the abort signal to all the active index builders for this database. Waits
                // for aborted index builds to complete.
                indexBuildsCoord->abortDatabaseIndexBuilds(opCtx, dbName, "dropDatabase command");

                if (MONGO_unlikely(dropDatabaseHangAfterWaitingForIndexBuilds.shouldFail())) {
                    LOGV2(4612300,
                          "dropDatabase - fail point dropDatabaseHangAfterWaitingForIndexBuilds "
                          "enabled");
                    dropDatabaseHangAfterWaitingForIndexBuilds.pauseWhileSet();
                }

                autoDB.emplace(opCtx, dbName, MODE_X /* database lock mode*/, tenantLockMode);
                db = autoDB->getDb();

                // Abandon the snapshot as the index catalog will compare the in-memory state to the
                // disk state, which may have changed when we released the collection lock
                // temporarily.
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

                status = _checkNssAndReplState(opCtx, db, dbName);
                if (!status.isOK()) {
                    return status;
                }
            }
        }

        auto catalog = CollectionCatalog::get(opCtx);

        // Drop the database views collection first, to ensure that time-series view namespaces are
        // removed before their underlying buckets collections. This ensures oplog order, such that
        // a time-series view may be missing while the buckets collection exists, but a time-series
        // view is never present without its corresponding buckets collection.
        auto viewCollPtr = catalog->lookupCollectionByNamespace(
            opCtx, NamespaceString::makeSystemDotViewsNamespace(dbName));
        if (viewCollPtr) {
            ++numCollections;
            const auto& nss = viewCollPtr->ns();
            LOGV2(7193700,
                  "dropDatabase - dropping collection",
                  logAttrs(dbName),
                  "namespace"_attr = nss);

            writeConflictRetry(opCtx, "dropDatabase_views_collection", nss, [&] {
                WriteUnitOfWork wunit(opCtx);
                fassert(7193701, db->dropCollectionEvenIfSystem(opCtx, nss, {}, markFromMigrate));
                wunit.commit();
            });
        }

        // The system.profile collection is created using an untimestamped write to the catalog when
        // enabling profiling on a database. So we drop it untimestamped as well to avoid mixed-mode
        // timestamp usage.
        auto systemProfilePtr = catalog->lookupCollectionByNamespace(
            opCtx, NamespaceString::makeSystemDotProfileNamespace(dbName));
        if (systemProfilePtr) {
            const Timestamp commitTs =
                shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp();
            if (!commitTs.isNull()) {
                shard_role_details::getRecoveryUnit(opCtx)->clearCommitTimestamp();
            }

            // Ensure this block exits with the same commit timestamp state that it was called with.
            ScopeGuard addCommitTimestamp([&opCtx, commitTs] {
                if (!commitTs.isNull()) {
                    shard_role_details::getRecoveryUnit(opCtx)->setCommitTimestamp(commitTs);
                }
            });

            const auto& nss = systemProfilePtr->ns();
            LOGV2(7574000,
                  "dropDatabase - dropping collection",
                  logAttrs(dbName),
                  "namespace"_attr = nss);

            invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
            writeConflictRetry(opCtx, "dropDatabase_system.profile_collection", nss, [&] {
                WriteUnitOfWork wunit(opCtx);
                fassert(7574001, db->dropCollectionEvenIfSystem(opCtx, nss, {}, markFromMigrate));
                wunit.commit();
            });
        }

        // Refresh the catalog so the views and profile collections aren't present.
        catalog = CollectionCatalog::get(opCtx);

        std::vector<NamespaceString> collectionsToDrop;
        for (auto&& collection : catalog->range(db->name())) {
            if (!collection) {
                break;
            }

            const auto& nss = collection->ns();
            numCollections++;

            LOGV2(20338,
                  "dropDatabase - dropping collection",
                  logAttrs(dbName),
                  "namespace"_attr = nss);

            if (replCoord->isOplogDisabledFor(opCtx, nss)) {
                continue;
            }
            collectionsToDrop.push_back(nss);
        }
        numCollectionsToDrop = collectionsToDrop.size();

        for (const auto& nss : collectionsToDrop) {
            if (!opCtx->writesAreReplicated()) {
                // Dropping a database on a primary replicates individual collection drops followed
                // by a database drop oplog entry. When a secondary observes the database drop oplog
                // entry, all of the replicated collections that were dropped must have been
                // processed. Only non-replicated collections should be left to remove. Collections
                // with the `tmp.mr` namespace may or may not be getting replicated; be conservative
                // and assume they are not.
                invariant(!nss.isReplicated() || nss.coll().starts_with("tmp.mr"));
            }

            if (!abortIndexBuilds) {
                IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
                    catalog->lookupCollectionByNamespace(opCtx, nss)->uuid());
            }

            writeConflictRetry(opCtx, "dropDatabase_collection", nss, [&] {
                WriteUnitOfWork wunit(opCtx);
                // A primary processing this will assign a timestamp when the operation is written
                // to the oplog. As stated above, a secondary processing must only observe
                // non-replicated collections, thus this should not be timestamped.
                fassert(40476, db->dropCollectionEvenIfSystem(opCtx, nss, {}, markFromMigrate));
                wunit.commit();
            });
        }

        // If there are no collection drops to wait for, we complete the drop database operation.
        if (numCollectionsToDrop == 0U) {
            _finishDropDatabase(
                opCtx, dbName, db, numCollections, abortIndexBuilds, markFromMigrate);
            return Status::OK();
        }

        dropPendingGuard.dismiss();
    }

    ScopeGuard dropPendingGuard([opCtx, &dbName] {
        CollectionCatalog::write(
            opCtx, [&dbName](CollectionCatalog& catalog) { catalog.removeDropPending(dbName); });
    });

    // Verify again that we haven't obtained any other locks before replication.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    auto awaitOpTime = [&]() {
        invariant(numCollectionsToDrop > 0U);
        const auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
        return clientInfo.getLastOp();
    }();

    // The user-supplied wTimeout should be used when waiting for majority write concern.
    const auto& userWriteConcern = opCtx->getWriteConcern();
    const auto wTimeout = !userWriteConcern.isImplicitDefaultWriteConcern()
        ? userWriteConcern.wTimeout
        : WriteConcernOptions::Timeout(duration_cast<Milliseconds>(Minutes(10)));

    // This is used to wait for the collection drops to replicate to a majority of the replica
    // set. Note: Even though we're setting UNSET here, kMajority implies JOURNAL if journaling
    // is supported by mongod and writeConcernMajorityJournalDefault is set to true in the
    // ReplSetConfig.
    const WriteConcernOptions dropDatabaseWriteConcern(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, wTimeout);

    LOGV2(20340,
          "dropDatabase waiting for replication and dropping collections",
          logAttrs(dbName),
          "awaitOpTime"_attr = awaitOpTime,
          "dropDatabaseWriteConcern"_attr = dropDatabaseWriteConcern.toBSON(),
          "numCollectionsToDrop"_attr = numCollectionsToDrop);

    auto result = replCoord->awaitReplication(opCtx, awaitOpTime, dropDatabaseWriteConcern);

    // If the user-provided write concern is weaker than majority, this is effectively a no-op.
    if (result.status.isOK() && !userWriteConcern.usedDefaultConstructedWC) {
        LOGV2(20341,
              "dropDatabase waiting for replication",
              logAttrs(dbName),
              "awaitOpTime"_attr = awaitOpTime,
              "writeConcern"_attr = userWriteConcern.toBSON());
        result = replCoord->awaitReplication(opCtx, awaitOpTime, userWriteConcern);
    }

    if (!result.status.isOK()) {
        return result.status.withContext(str::stream()
                                         << "dropDatabase " << dbName.toStringForErrorMsg()
                                         << " failed waiting for " << numCollectionsToDrop
                                         << " collection drop(s) (most recent drop optime: "
                                         << awaitOpTime.toString() << ") to replicate.");
    }

    LOGV2(20342,
          "dropDatabase - successfully dropped collections",
          logAttrs(dbName),
          "numCollectionsDropped"_attr = numCollectionsToDrop,
          "mostRecentDropOpTime"_attr = awaitOpTime,
          "duration"_attr = result.duration);


    if (MONGO_unlikely(dropDatabaseHangAfterAllCollectionsDrop.shouldFail())) {
        LOGV2(20343,
              "dropDatabase - fail point dropDatabaseHangAfterAllCollectionsDrop enabled. "
              "Blocking until fail point is disabled");
        dropDatabaseHangAfterAllCollectionsDrop.pauseWhileSet();
    }

    AutoGetDb autoDB(opCtx, dbName, MODE_X /* database lock mode*/, tenantLockMode);
    auto db = autoDB.getDb();
    if (!db) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Could not drop database " << dbName.toStringForErrorMsg()
                                    << " because it does not exist after dropping "
                                    << numCollectionsToDrop << " collection(s).");
    }

    bool userInitiatedWritesAndNotPrimary =
        opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream()
                          << "Could not drop database " << dbName.toStringForErrorMsg()
                          << " because we transitioned from PRIMARY to "
                          << replCoord->getMemberState().toString() << " while waiting for "
                          << numCollectionsToDrop << " pending collection drop(s).");
    }

    _finishDropDatabase(opCtx, dbName, db, numCollections, abortIndexBuilds, markFromMigrate);

    return Status::OK();
}

}  // namespace

Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName, bool markFromMigrate) {
    const bool abortIndexBuilds = true;
    return _dropDatabase(opCtx, dbName, abortIndexBuilds, markFromMigrate);
}

Status dropDatabaseForApplyOps(OperationContext* opCtx,
                               const DatabaseName& dbName,
                               bool markFromMigrate) {
    const bool abortIndexBuilds = false;
    return _dropDatabase(opCtx, dbName, abortIndexBuilds, markFromMigrate);
}

}  // namespace mongo
