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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_collection.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDropCollectionBeforeLockAcquisition);
MONGO_FAIL_POINT_DEFINE(hangDuringDropCollection);

Status _dropView(OperationContext* opCtx,
                 Database* db,
                 const NamespaceString& collectionName,
                 BSONObjBuilder& result) {
    if (!db) {
        return Status(ErrorCodes::NamespaceNotFound, "ns not found");
    }
    auto view = ViewCatalog::get(db)->lookup(opCtx, collectionName.ns());
    if (!view) {
        return Status(ErrorCodes::NamespaceNotFound, "ns not found");
    }
    Lock::CollectionLock systemViewsLock(opCtx, db->getSystemViewsName(), MODE_X);
    Lock::CollectionLock collLock(opCtx, collectionName, MODE_IX);

    if (MONGO_FAIL_POINT(hangDuringDropCollection)) {
        log() << "hangDuringDropCollection fail point enabled. Blocking until fail point is "
                 "disabled.";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangDuringDropCollection);
    }

    AutoStatsTracker statsTracker(opCtx,
                                  collectionName,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                  db->getProfilingLevel());

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName)) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while dropping collection " << collectionName);
    }

    WriteUnitOfWork wunit(opCtx);
    Status status = db->dropView(opCtx, collectionName);
    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    result.append("ns", collectionName.ns());
    return Status::OK();
}

Status _dropCollection(OperationContext* opCtx,
                       Database* db,
                       const NamespaceString& collectionName,
                       const repl::OpTime& dropOpTime,
                       DropCollectionSystemCollectionMode systemCollectionMode,
                       BSONObjBuilder& result) {
    Lock::CollectionLock collLock(opCtx, collectionName, MODE_X);
    Collection* coll = db->getCollection(opCtx, collectionName);
    if (!coll) {
        return Status(ErrorCodes::NamespaceNotFound, "ns not found");
    }

    if (MONGO_FAIL_POINT(hangDuringDropCollection)) {
        log() << "hangDuringDropCollection fail point enabled. Blocking until fail point is "
                 "disabled.";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangDuringDropCollection);
    }

    AutoStatsTracker statsTracker(opCtx,
                                  collectionName,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                  db->getProfilingLevel());

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName)) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while dropping collection " << collectionName);
    }

    WriteUnitOfWork wunit(opCtx);

    int numIndexes = coll->getIndexCatalog()->numIndexesTotal(opCtx);
    BackgroundOperation::assertNoBgOpInProgForNs(collectionName.ns());
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid().get());
    Status status =
        systemCollectionMode == DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
        ? db->dropCollection(opCtx, collectionName, dropOpTime)
        : db->dropCollectionEvenIfSystem(opCtx, collectionName, dropOpTime);

    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    result.append("nIndexesWas", numIndexes);
    result.append("ns", collectionName.ns());

    return Status::OK();
}

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& collectionName,
                      BSONObjBuilder& result,
                      const repl::OpTime& dropOpTime,
                      DropCollectionSystemCollectionMode systemCollectionMode) {
    if (!serverGlobalParams.quiet.load()) {
        log() << "CMD: drop " << collectionName;
    }

    if (MONGO_FAIL_POINT(hangDropCollectionBeforeLockAcquisition)) {
        log() << "Hanging drop collection before lock acquisition while fail point is set";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangDropCollectionBeforeLockAcquisition);
    }
    return writeConflictRetry(opCtx, "drop", collectionName.ns(), [&] {
        AutoGetDb autoDb(opCtx, collectionName.db(), MODE_IX);
        Database* db = autoDb.getDb();
        if (!db) {
            return Status(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        Collection* coll = db->getCollection(opCtx, collectionName);
        if (!coll) {
            return _dropView(opCtx, db, collectionName, result);
        } else {
            return _dropCollection(
                opCtx, db, collectionName, dropOpTime, systemCollectionMode, result);
        }
    });
}

}  // namespace mongo
