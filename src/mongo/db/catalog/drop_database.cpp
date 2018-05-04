/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_database.h"

#include <algorithm>

#include "mongo/db/background.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FP_DECLARE(dropDatabaseHangAfterLastCollectionDrop);

namespace {

// This is used to wait for the collection drops to replicate to a majority of the replica set.
// Note: Even though we're setting UNSET here, kMajority implies JOURNAL if journaling is supported
// by mongod and writeConcernMajorityJournalDefault is set to true in the ReplSetConfig.
const WriteConcernOptions kDropDatabaseWriteConcern(WriteConcernOptions::kMajority,
                                                    WriteConcernOptions::SyncMode::UNSET,
                                                    Minutes(10));

/**
 * Removes database from catalog and writes dropDatabase entry to oplog.
 */
Status _finishDropDatabase(OperationContext* opCtx, const std::string& dbName, Database* db) {
    // If Database::dropDatabase() fails, we should reset the drop-pending state on Database.
    auto dropPendingGuard = MakeGuard([db, opCtx] { db->setDropPending(opCtx, false); });

    Database::dropDatabase(opCtx, db);
    dropPendingGuard.Dismiss();

    log() << "dropDatabase " << dbName << " - finished";

    if (MONGO_FAIL_POINT(dropDatabaseHangAfterLastCollectionDrop)) {
        log() << "dropDatabase - fail point dropDatabaseHangAfterLastCollectionDrop enabled. "
                 "Blocking until fail point is disabled. ";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(dropDatabaseHangAfterLastCollectionDrop);
    }

    WriteUnitOfWork wunit(opCtx);
    getGlobalServiceContext()->getOpObserver()->onDropDatabase(opCtx, dbName);
    wunit.commit();

    return Status::OK();
}

}  // namespace

Status dropDatabase(OperationContext* opCtx, const std::string& dbName) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot drop a database in read-only mode",
            !storageGlobalParams.readOnly);

    // As of SERVER-32205, dropping the admin database is prohibited.
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Dropping the '" << dbName << "' database is prohibited.",
            dbName != NamespaceString::kAdminDb);

    // TODO (Kal): OldClientContext legacy, needs to be removed
    {
        CurOp::get(opCtx)->ensureStarted();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(dbName);
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    std::size_t numCollectionsToDrop = 0;

    // We have to wait for the last drop-pending collection to be removed if there are no
    // collections to drop.
    repl::OpTime latestDropPendingOpTime;

    using Result = boost::optional<Status>;
    // Get an optional result--if it's there, early return; otherwise, wait for collections to drop.
    auto result = writeConflictRetry(opCtx, "dropDatabase_collection", dbName, [&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        Database* const db = autoDB.getDb();
        if (!db) {
            return Result(Status(ErrorCodes::NamespaceNotFound,
                                 str::stream() << "Could not drop database " << dbName
                                               << " because it does not exist"));
        }

        bool userInitiatedWritesAndNotPrimary =
            opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

        if (userInitiatedWritesAndNotPrimary) {
            return Result(
                Status(ErrorCodes::NotMaster,
                       str::stream() << "Not primary while dropping database " << dbName));
        }

        log() << "dropDatabase " << dbName << " - starting";
        db->setDropPending(opCtx, true);

        // If Database::dropCollectionEventIfSystem() fails, we should reset the drop-pending state
        // on Database.
        auto dropPendingGuard = MakeGuard([&db, opCtx] { db->setDropPending(opCtx, false); });

        std::vector<NamespaceString> collectionsToDrop;
        for (Collection* collection : *db) {
            const auto& nss = collection->ns();
            if (nss.isDropPendingNamespace() && replCoord->isReplEnabled() &&
                opCtx->writesAreReplicated()) {
                log() << "dropDatabase " << dbName << " - found drop-pending collection: " << nss;
                latestDropPendingOpTime = std::max(
                    latestDropPendingOpTime, uassertStatusOK(nss.getDropPendingNamespaceOpTime()));
                continue;
            }
            if (replCoord->isOplogDisabledFor(opCtx, nss) || nss.isSystemDotIndexes()) {
                continue;
            }
            collectionsToDrop.push_back(nss);
        }
        numCollectionsToDrop = collectionsToDrop.size();

        log() << "dropDatabase " << dbName << " - dropping " << numCollectionsToDrop
              << " collections";
        for (auto nss : collectionsToDrop) {
            log() << "dropDatabase " << dbName << " - dropping collection: " << nss;
            if (!opCtx->writesAreReplicated()) {
                // Dropping a database on a primary replicates individual collection drops
                // followed by a database drop oplog entry. When a secondary observes the database
                // drop oplog entry, all of the replicated collections that were dropped must have
                // been processed. Only non-replicated collections like `system.profile` should be
                // left to remove. Collections with the `tmp.mr` namespace may or may not be
                // getting replicated; be conservative and assume they are not.
                invariant(!nss.isReplicated() || nss.coll().startsWith("tmp.mr"));
            }

            WriteUnitOfWork wunit(opCtx);
            // A primary processing this will assign a timestamp when the operation is written to
            // the oplog. As stated above, a secondary processing must only observe non-replicated
            // collections, thus this should not be timestamped.
            fassert(40476, db->dropCollectionEvenIfSystem(opCtx, nss));
            wunit.commit();
        }
        dropPendingGuard.Dismiss();

        // If there are no collection drops to wait for, we complete the drop database operation.
        if (numCollectionsToDrop == 0U && latestDropPendingOpTime.isNull()) {
            return Result(_finishDropDatabase(opCtx, dbName, db));
        }

        return Result(boost::none);
    });

    if (result) {
        return *result;
    }

    // If waitForWriteConcern() returns an error or throws an exception, we should reset the
    // drop-pending state on Database.
    auto dropPendingGuardWhileAwaitingReplication = MakeGuard([dbName, opCtx] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        if (auto db = autoDB.getDb()) {
            db->setDropPending(opCtx, false);
        }
    });

    {
        // Holding of any locks is disallowed while awaiting replication because this can
        // potentially block for long time while doing network activity.
        //
        // Even though dropDatabase() does not explicitly acquire any locks before awaiting
        // replication, it is possible that the caller of this function may already have acquired
        // a lock. The applyOps command is an example of a dropDatabase() caller that does this.
        // Therefore, we have to release any locks using a TempRelease RAII object.
        //
        // TODO: Remove the use of this TempRelease object when SERVER-29802 is completed.
        // The work in SERVER-29802 will adjust the locking rules around applyOps operations and
        // dropDatabase is expected to be one of the operations where we expect to no longer acquire
        // the global lock.
        Lock::TempRelease release(opCtx->lockState());

        auto awaitOpTime = [&]() {
            if (numCollectionsToDrop > 0U) {
                const auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
                return clientInfo.getLastOp();
            }
            invariant(!latestDropPendingOpTime.isNull());
            return latestDropPendingOpTime;
        }();

        log() << "dropDatabase " << dbName << " waiting for " << awaitOpTime
              << " to be replicated at " << kDropDatabaseWriteConcern.toBSON() << ". Dropping "
              << numCollectionsToDrop << " collections, with last collection drop at "
              << latestDropPendingOpTime;
        auto result = replCoord->awaitReplication(opCtx, awaitOpTime, kDropDatabaseWriteConcern);
        const auto& status = result.status;
        if (!status.isOK()) {
            return status.withContext(
                str::stream() << "dropDatabase " << dbName << " failed waiting for "
                              << numCollectionsToDrop
                              << " collection drops (most recent drop optime: "
                              << awaitOpTime.toString()
                              << ") to replicate.");
        }

        log() << "dropDatabase " << dbName << " - successfully dropped " << numCollectionsToDrop
              << " collections (most recent drop optime: " << awaitOpTime << ") after "
              << result.duration << ". dropping database";
    }

    dropPendingGuardWhileAwaitingReplication.Dismiss();

    return writeConflictRetry(opCtx, "dropDatabase_database", dbName, [&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        auto db = autoDB.getDb();
        if (!db) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "Could not drop database " << dbName
                                        << " because it does not exist after dropping "
                                        << numCollectionsToDrop
                                        << " collection(s).");
        }

        // If we fail to complete the database drop, we should reset the drop-pending state on
        // Database.
        auto dropPendingGuard = MakeGuard([&db, opCtx] { db->setDropPending(opCtx, false); });

        bool userInitiatedWritesAndNotPrimary =
            opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::PrimarySteppedDown,
                          str::stream() << "Could not drop database " << dbName
                                        << " because we transitioned from PRIMARY to "
                                        << replCoord->getMemberState().toString()
                                        << " while waiting for "
                                        << numCollectionsToDrop
                                        << " pending collection drop(s).");
        }

        dropPendingGuard.Dismiss();
        return _finishDropDatabase(opCtx, dbName, db);
    });
}

}  // namespace mongo
