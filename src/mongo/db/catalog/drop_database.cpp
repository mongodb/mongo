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
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

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

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        Database* const db = autoDB.getDb();
        if (!db) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "Could not drop database " << dbName
                                        << " because it does not exist");
        }

        bool userInitiatedWritesAndNotPrimary =
            opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while dropping database " << dbName);
        }

        log() << "dropDatabase " << dbName << " - starting";
        db->setDropPending(opCtx, true);

        // If Database::dropCollectionEventIfSystem() fails, we should reset the drop-pending state
        // on Database.
        auto dropPendingGuard = MakeGuard([&db, opCtx] { db->setDropPending(opCtx, false); });

        for (auto collection : *db) {
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
            log() << "dropDatabase " << dbName << " - dropping collection: " << nss;
            WriteUnitOfWork wunit(opCtx);
            fassertStatusOK(40476, db->dropCollectionEvenIfSystem(opCtx, nss));
            wunit.commit();
            numCollectionsToDrop++;
        }
        dropPendingGuard.Dismiss();

        // If there are no collection drops to wait for, we complete the drop database operation.
        if (numCollectionsToDrop == 0U && latestDropPendingOpTime.isNull()) {
            return _finishDropDatabase(opCtx, dbName, db);
        }
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "dropDatabase_collection", dbName);

    // If waitForWriteConcern() returns an error or throws an exception, we should reset the
    // drop-pending state on Database.
    auto dropPendingGuardWhileAwaitingReplication = MakeGuard([dbName, opCtx] {
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        if (auto db = autoDB.getDb()) {
            db->setDropPending(opCtx, false);
        }
    });

    if (numCollectionsToDrop > 0U) {
        auto status =
            replCoord->awaitReplicationOfLastOpForClient(opCtx, kDropDatabaseWriteConcern).status;
        if (!status.isOK()) {
            return Status(status.code(),
                          str::stream() << "dropDatabase " << dbName << " failed waiting for "
                                        << numCollectionsToDrop
                                        << " collection drops to replicate: "
                                        << status.reason());
        }

        log() << "dropDatabase " << dbName << " - successfully dropped " << numCollectionsToDrop
              << " collections. dropping database";
    } else {
        invariant(!latestDropPendingOpTime.isNull());
        auto status =
            replCoord->awaitReplication(opCtx, latestDropPendingOpTime, kDropDatabaseWriteConcern)
                .status;
        if (!status.isOK()) {
            return Status(
                status.code(),
                str::stream()
                    << "dropDatabase "
                    << dbName
                    << " failed waiting for pending collection drops (most recent drop optime: "
                    << latestDropPendingOpTime.toString()
                    << ") to replicate: "
                    << status.reason());
        }

        log() << "dropDatabase " << dbName
              << " - pending collection drops completed. dropping database";
    }
    dropPendingGuardWhileAwaitingReplication.Dismiss();

    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb autoDB(opCtx, dbName, MODE_X);
        if (auto db = autoDB.getDb()) {
            return _finishDropDatabase(opCtx, dbName, db);
        }

        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Could not drop database " << dbName
                                    << " because it does not exist after dropping "
                                    << numCollectionsToDrop
                                    << " collection(s).");
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "dropDatabase_database", dbName);

    MONGO_UNREACHABLE;
}

}  // namespace mongo
