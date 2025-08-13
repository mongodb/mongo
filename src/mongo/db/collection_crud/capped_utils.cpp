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

#include "mongo/db/collection_crud/capped_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/local_oplog_info.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/rename_collection.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/unique_collection_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

void cloneCollectionAsCapped(OperationContext* opCtx,
                             Database* db,
                             const CollectionAcquisition& fromCollection,
                             CollectionAcquisition& toCollection,
                             long long size,
                             bool temp,
                             const boost::optional<UUID>& targetUUID) {
    const auto& fromNss = fromCollection.nss();
    const auto& toNss = toCollection.nss();

    if (!fromCollection.exists()) {
        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "source collection " << fromNss.toStringForErrorMsg()
                                << " does not exist");
    }

    uassert(6367302,
            "Cannot convert an encrypted collection to a capped collection",
            !fromCollection.getCollectionPtr()->getCollectionOptions().encryptedFieldConfig);

    uassert(ErrorCodes::IllegalOperation,
            "Cannot convert a timeseries collection to a capped collection",
            !fromCollection.getCollectionPtr()->isTimeseriesCollection());

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "cloneCollectionAsCapped failed - destination collection "
                          << toNss.toStringForErrorMsg() << " already exists. source collection: "
                          << fromNss.toStringForErrorMsg(),
            !toCollection.exists());

    // create new collection
    {
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx, &toCollection);
        auto options = fromCollection.getCollectionPtr()->getCollectionOptions();
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

    tassert(10769702,
            "Expected toCollection to exist now",
            toCollection.exists());  // we created above

    // how much data to ignore because it won't fit anyway
    // datasize and extentSize can't be compared exactly, so add some padding to 'size'

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    long long allocatedSpaceGuess =
        std::max(static_cast<long long>(size * 2),
                 static_cast<long long>(
                     toCollection.getCollectionPtr()->getRecordStore()->storageSize(ru) * 2));

    long long excessSize = fromCollection.getCollectionPtr()->dataSize(opCtx) - allocatedSpaceGuess;

    auto exec =
        InternalPlanner::collectionScan(opCtx,
                                        fromCollection,
                                        PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY,
                                        InternalPlanner::FORWARD);

    BSONObj objToClone;
    RecordId loc;

    DisableDocumentValidation validationDisabler(opCtx);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isOplogDisabledForCappedCollection = replCoord->isOplogDisabledFor(opCtx, toNss);

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
                if (!fromCollection.getCollectionPtr()->findDoc(opCtx, loc, &snapshottedObj)) {
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
                if (toCollection.getCollectionPtr()->needsCappedLock()) {
                    Lock::ResourceLock heldUntilEndOfWUOW{
                        opCtx, ResourceId(RESOURCE_METADATA, toNss), MODE_X};
                }
                auto oplogInfo = LocalOplogInfo::get(opCtx);
                auto oplogSlots = oplogInfo->getNextOpTimes(opCtx, /*batchSize=*/1);
                insertStmt.oplogSlot = oplogSlots.front();
            }

            uassertStatusOK(collection_internal::insertDocument(opCtx,
                                                                toCollection.getCollectionPtr(),
                                                                std::move(insertStmt),
                                                                nullptr /* OpDebug */,
                                                                true /*fromMigrate*/));
            wunit.commit();

            // Go to the next document
            retries = 0;
        } catch (const StorageUnavailableException& e) {
            retries++;  // logAndBackoff expects this to be 1 on first call.
            logAndRecordWriteConflictAndBackoff(opCtx,
                                                retries,
                                                "cloneCollectionAsCapped",
                                                e.reason(),
                                                NamespaceStringOrUUID(fromNss));

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            exec->restoreState(nullptr);  // Handles any WCEs internally.
        }
    }

    MONGO_UNREACHABLE;
}

namespace {
struct ConvertToCappedAcquisitions {
    CollectionAcquisition sourceAcquisition;
    CollectionAcquisition tmpAcquisition;
    boost::optional<CollectionAcquisition> targetUUIDAcquisition;
};

ConvertToCappedAcquisitions acquireLocksForConvertToCapped(
    OperationContext* opCtx,
    const NamespaceString& fromNs,
    const boost::optional<UUID>& targetUUID) {
    const auto& dbName = fromNs.dbName();
    const auto modelName = fmt::format("tmp%%%%%.convertToCapped.{}", fromNs.coll());

    // If given a targetUUID we must also acquire whatever collection with it since it's a
    // temporary leftover collection from a failed execution and will be dropped.s
    bool shouldFetchWithTargetUUID = targetUUID.has_value();
    while (true) {
        // We attempt to acquire all target collections. This may be retried if the temporary
        // collection already exists since we want a unique collection name.
        auto tmpName = uassertStatusOKWithContext(
            generateRandomCollectionName(opCtx, dbName, modelName),
            str::stream() << "Cannot generate temporary collection namespace to convert "
                          << fromNs.toStringForErrorMsg() << " to a capped collection");
        CollectionAcquisitionRequests requests = {
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, fromNs, AcquisitionPrerequisites::OperationType::kWrite),
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, tmpName, AcquisitionPrerequisites::OperationType::kWrite),
        };
        if (shouldFetchWithTargetUUID) {
            // We also attempt to acquire any leftover collection with the specified targetUUID.
            // That collection will be dropped as part of pre-conversion cleanup.
            requests.push_back(CollectionAcquisitionRequest::fromOpCtx(
                opCtx,
                NamespaceStringOrUUID{fromNs.dbName(), *targetUUID},
                AcquisitionPrerequisites::OperationType::kWrite));
        }
        try {
            auto acquisitions = makeAcquisitionMap(acquireCollections(opCtx, requests, MODE_X));
            tassert(10769703,
                    "Expected tmp collection to have been acquired",
                    acquisitions.contains(tmpName));
            bool tmpNameExists = acquisitions.at(tmpName).exists();
            if (tmpNameExists) {
                // Retry the acquisitions as the tmpName already exists.
                continue;
            }
            boost::optional<CollectionAcquisition> leftoverAcq;
            if (shouldFetchWithTargetUUID) {
                // We have to iterate the map since we don't know the actual name of the leftover
                // collection.
                const auto& tempColl =
                    std::find_if(acquisitions.begin(), acquisitions.end(), [&](const auto& elem) {
                        return elem.second.exists() && elem.second.uuid() == *targetUUID;
                    })->second;
                // The leftover might be the same acquisition as the source, so make a copy here.
                leftoverAcq.emplace(tempColl);
            }
            return ConvertToCappedAcquisitions{
                .sourceAcquisition = std::move(acquisitions.at(fromNs)),
                .tmpAcquisition = std::move(acquisitions.at(tmpName)),
                .targetUUIDAcquisition = std::move(leftoverAcq)};
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // There is no leftover collection with the targetUUID, no cleanup is necessary. As
            // all acquisitions have been released we retry the acquisition but now without
            // acquiring the UUID.
            shouldFetchWithTargetUUID = false;
            continue;
        }
    }
}
}  // namespace

void convertToCapped(OperationContext* opCtx,
                     const NamespaceString& ns,
                     long long size,
                     bool fromMigrate,
                     const boost::optional<UUID>& targetUUID) {
    auto [sourceAcq, tmpAcq, leftoverAcq] = acquireLocksForConvertToCapped(opCtx, ns, targetUUID);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns);

    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while converting " << ns.toStringForErrorMsg()
                          << " to a capped collection",
            !userInitiatedWritesAndNotPrimary);

    const auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, ns.dbName());
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "database " << ns.dbName().toStringForErrorMsg() << " not found",
            db);

    if (sourceAcq.exists()) {
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(sourceAcq.uuid());
    }

    if (targetUUID) {
        // Return if the collection is already capped with the given UUID.
        if (sourceAcq.exists() && (sourceAcq.uuid() == targetUUID) &&
            sourceAcq.getCollectionPtr()->isCapped()) {
            auto actualSize = sourceAcq.getCollectionPtr()->getCappedMaxSize();
            tassert(10050600,
                    fmt::format("Found a capped collection at namespace {} with UUID {}, "
                                "but it has size {} instead of {}",
                                ns.toStringForErrorMsg(),
                                targetUUID->toString(),
                                size,
                                actualSize),
                    actualSize == size);
            return;
        }

        if (leftoverAcq) {
            // A previous execution left an existing temporary collection with the given targetUUID.
            // In that case let's drop the temporary collection to be able to proceed with the
            // creation of a new one.
            invariant(leftoverAcq->exists());
            tassert(10050601,
                    fmt::format("Expected leftover capped collection with UUID {} to be temporary",
                                targetUUID->toString()),
                    leftoverAcq->getCollectionPtr()->isTemporary());
            DropReply unused;
            uassertStatusOK(
                dropCollection(opCtx,
                               leftoverAcq->nss(),
                               &unused,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops,
                               fromMigrate));
        }
    }

    cloneCollectionAsCapped(opCtx, db, sourceAcq, tmpAcq, size, true /* temp */, targetUUID);

    RenameCollectionOptions options;
    options.dropTarget = true;
    options.stayTemp = false;
    options.markFromMigrate = fromMigrate;
    uassertStatusOK(renameCollection(opCtx, tmpAcq.nss(), ns, options));
}

}  // namespace mongo
