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

#include "mongo/db/catalog/capped_utils.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog/unique_collection_name.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

void cloneCollectionAsCapped(OperationContext* opCtx,
                             Database* db,
                             const NamespaceString& fromNss,
                             const NamespaceString& toNss,
                             long long size,
                             bool temp,
                             const boost::optional<UUID>& targetUUID) {
    CollectionPtr fromCollection(
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, fromNss));
    if (!fromCollection) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "cloneCollectionAsCapped not supported for views: "
                              << fromNss.toStringForErrorMsg(),
                !CollectionCatalog::get(opCtx)->lookupView(opCtx, fromNss));

        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "source collection " << fromNss.toStringForErrorMsg()
                                << " does not exist");
    }

    uassert(6367302,
            "Cannot convert an encrypted collection to a capped collection",
            !fromCollection->getCollectionOptions().encryptedFieldConfig);

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "cloneCollectionAsCapped failed - destination collection "
                          << toNss.toStringForErrorMsg() << " already exists. source collection: "
                          << fromNss.toStringForErrorMsg(),
            !CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss));

    // create new collection
    {
        auto options = fromCollection->getCollectionOptions();
        // The capped collection will get its own new unique id, as the conversion isn't reversible,
        // so it can't be rolled back.
        options.uuid = targetUUID;
        options.capped = true;
        options.cappedSize = size;
        if (temp)
            options.temp = true;
        // Capped collections cannot use recordIdsReplicated:true.
        options.recordIdsReplicated = false;

        uassertStatusOK(createCollection(opCtx, toNss, options, BSONObj()));
    }

    CollectionPtr toCollection(
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss));
    invariant(toCollection);  // we created above

    // how much data to ignore because it won't fit anyway
    // datasize and extentSize can't be compared exactly, so add some padding to 'size'

    long long allocatedSpaceGuess =
        std::max(static_cast<long long>(size * 2),
                 static_cast<long long>(toCollection->getRecordStore()->storageSize(opCtx) * 2));

    long long excessSize = fromCollection->dataSize(opCtx) - allocatedSpaceGuess;

    auto exec =
        InternalPlanner::collectionScan(opCtx,
                                        &fromCollection,
                                        PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY,
                                        InternalPlanner::FORWARD);

    BSONObj objToClone;
    RecordId loc;

    DisableDocumentValidation validationDisabler(opCtx);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isOplogDisabledForCappedCollection =
        replCoord->isOplogDisabledFor(opCtx, toCollection->ns());

    int retries = 0;  // non-zero when retrying our last document.
    while (true) {
        auto beforeGetNextSnapshotId = shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId();
        PlanExecutor::ExecState state = PlanExecutor::IS_EOF;
        if (!retries) {
            state = exec->getNext(&objToClone, &loc);
        }

        switch (state) {
            case PlanExecutor::IS_EOF:
                return;
            case PlanExecutor::ADVANCED: {
                if (excessSize > 0) {
                    // 4x is for padding, power of 2, etc...
                    excessSize -= (4 * objToClone.objsize());
                    continue;
                }
                break;
            }
        }

        try {
            // If the snapshot id changed while using the 'PlanExecutor' to retrieve the next
            // document from the collection scan, then it's possible that the document retrieved
            // from the scan may have since been deleted or modified in our current snapshot.
            if (beforeGetNextSnapshotId !=
                shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId()) {
                // The snapshot has changed. Fetch the document again from the collection in order
                // to check whether it has been deleted.
                Snapshotted<BSONObj> snapshottedObj;
                if (!fromCollection->findDoc(opCtx, loc, &snapshottedObj)) {
                    // Doc was deleted so don't clone it.
                    retries = 0;
                    continue;
                }
                objToClone = std::move(snapshottedObj.value());
            }

            WriteUnitOfWork wunit(opCtx);

            InsertStatement insertStmt(objToClone);

            // When converting a regular collection into a capped collection, we may start
            // performing capped deletes during the conversion process. This can occur if the
            // regular collections data exceeds the capacities set for the capped collection.
            // Because of that, we acquire an optime for the insert now to ensure that the insert
            // oplog entry gets logged before any delete oplog entries.
            if (!isOplogDisabledForCappedCollection) {
                auto oplogInfo = LocalOplogInfo::get(opCtx);
                auto oplogSlots = oplogInfo->getNextOpTimes(opCtx, /*batchSize=*/1);
                insertStmt.oplogSlot = oplogSlots.front();
            }

            uassertStatusOK(collection_internal::insertDocument(
                opCtx, toCollection, std::move(insertStmt), nullptr /* OpDebug */, true));
            wunit.commit();

            // Go to the next document
            retries = 0;
        } catch (const StorageUnavailableException& e) {
            CurOp::get(opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            retries++;  // logAndBackoff expects this to be 1 on first call.
            logWriteConflictAndBackoff(
                retries, "cloneCollectionAsCapped", e.reason(), NamespaceStringOrUUID(fromNss));

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            exec->restoreState(&fromCollection);  // Handles any WCEs internally.
        }
    }

    MONGO_UNREACHABLE;
}

void convertToCapped(OperationContext* opCtx,
                     const NamespaceString& ns,
                     long long size,
                     const boost::optional<UUID>& targetUUID) {
    auto dbname = ns.dbName();
    StringData shortSource = ns.coll();

    AutoGetCollection coll(opCtx, ns, MODE_X);
    CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, ns)->checkShardVersionOrThrow(
        opCtx);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns);

    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while converting " << ns.toStringForErrorMsg()
                          << " to a capped collection",
            !userInitiatedWritesAndNotPrimary);

    Database* const db = coll.getDb();
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "database " << dbname.toStringForErrorMsg() << " not found",
            db);

    if (coll) {
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
    }

    if (targetUUID) {
        // Return if the collection is already capped with the given UUID.
        if (coll && (coll->uuid() == *targetUUID) && coll->isCapped()) {
            invariant(coll->getCappedMaxSize() == size);
            return;
        }

        // Check if a previous execution left an existing temporary collection with the given
        // targetUUID. In that case let's drop the temporary collection to be able to proceed with
        // the creation of a new one.
        boost::optional<NamespaceString> oldTempNssToDrop;
        try {
            AutoGetCollection tempColl(
                opCtx, NamespaceStringOrUUID{ns.dbName(), *targetUUID}, MODE_S);
            invariant(!tempColl || (tempColl && tempColl->isTemporary()));

            if (tempColl && tempColl->isTemporary()) {
                oldTempNssToDrop = tempColl.getNss();
            }
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        }

        if (oldTempNssToDrop) {
            DropReply unused;
            uassertStatusOK(
                dropCollection(opCtx,
                               *oldTempNssToDrop,
                               &unused,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops,
                               false));
        }
    }

    // Generate a temporary collection name that will not collide with any existing collections.
    boost::optional<Lock::CollectionLock> collLock;
    const auto tempNs = [&] {
        while (true) {
            auto tmpName = uassertStatusOKWithContext(
                makeUniqueCollectionName(opCtx, dbname, "tmp%%%%%.convertToCapped." + shortSource),
                str::stream() << "Cannot generate temporary collection namespace to convert "
                              << ns.toStringForErrorMsg() << " to a capped collection");

            collLock.emplace(opCtx, tmpName, MODE_X);
            if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, tmpName)) {
                return tmpName;
            }

            // The temporary collection was created by someone else between the name being
            // generated and acquiring the lock on the collection, so try again with a new
            // temporary collection name.
            collLock.reset();
        }
    }();

    cloneCollectionAsCapped(opCtx, db, ns, tempNs, size, true /* temp */, targetUUID);

    RenameCollectionOptions options;
    options.dropTarget = true;
    options.stayTemp = false;
    uassertStatusOK(renameCollection(opCtx, tempNs, ns, options));
}

}  // namespace mongo
