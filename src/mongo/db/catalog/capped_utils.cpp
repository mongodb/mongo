
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

#include "mongo/db/catalog/capped_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/util/scopeguard.h"

mongo::Status mongo::emptyCapped(OperationContext* opCtx, const NamespaceString& collectionName) {
    AutoGetDb autoDb(opCtx, collectionName.db(), MODE_X);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while truncating collection: "
                                    << collectionName);
    }

    Database* db = autoDb.getDb();
    uassert(ErrorCodes::NamespaceNotFound, "no such database", db);

    Collection* collection = db->getCollection(opCtx, collectionName);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "emptycapped not supported on view: " << collectionName.ns(),
            collection || !db->getViewCatalog()->lookup(opCtx, collectionName.ns()));
    uassert(ErrorCodes::NamespaceNotFound, "no such collection", collection);

    if (collectionName.isSystem() && !collectionName.isSystemDotProfile()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot truncate a system collection: " << collectionName);
    }

    if (collectionName.isVirtualized()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot truncate a virtual collection: " << collectionName);
    }

    if ((repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
         repl::ReplicationCoordinator::modeNone) &&
        collectionName.isOplog()) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Cannot truncate a live oplog while replicating: "
                                    << collectionName);
    }

    BackgroundOperation::assertNoBgOpInProgForNs(collectionName.ns());

    WriteUnitOfWork wuow(opCtx);

    Status status = collection->truncate(opCtx);
    if (!status.isOK()) {
        return status;
    }

    getGlobalServiceContext()->getOpObserver()->onEmptyCapped(
        opCtx, collection->ns(), collection->uuid());

    wuow.commit();

    return Status::OK();
}

void mongo::cloneCollectionAsCapped(OperationContext* opCtx,
                                    Database* db,
                                    const std::string& shortFrom,
                                    const std::string& shortTo,
                                    long long size,
                                    bool temp) {
    NamespaceString fromNss(db->name(), shortFrom);
    NamespaceString toNss(db->name(), shortTo);

    Collection* fromCollection = db->getCollection(opCtx, fromNss);
    if (!fromCollection) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "cloneCollectionAsCapped not supported for views: " << fromNss,
                !db->getViewCatalog()->lookup(opCtx, fromNss.ns()));

        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "source collection " << fromNss << " does not exist");
    }

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "source collection " << fromNss
                          << " is currently in a drop-pending state.",
            !fromNss.isDropPendingNamespace());

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "cloneCollectionAsCapped failed - destination collection " << toNss
                          << " already exists. source collection: "
                          << fromNss,
            !db->getCollection(opCtx, toNss));

    // create new collection
    {
        auto options = fromCollection->getCatalogEntry()->getCollectionOptions(opCtx);
        // The capped collection will get its own new unique id, as the conversion isn't reversible,
        // so it can't be rolled back.
        options.uuid.reset();
        options.capped = true;
        options.cappedSize = size;
        if (temp)
            options.temp = true;

        BSONObjBuilder cmd;
        cmd.append("create", toNss.coll());
        cmd.appendElements(options.toBSON());
        uassertStatusOK(createCollection(opCtx, toNss.db().toString(), cmd.done()));
    }

    Collection* toCollection = db->getCollection(opCtx, toNss);
    invariant(toCollection);  // we created above

    // how much data to ignore because it won't fit anyway
    // datasize and extentSize can't be compared exactly, so add some padding to 'size'

    long long allocatedSpaceGuess =
        std::max(static_cast<long long>(size * 2),
                 static_cast<long long>(toCollection->getRecordStore()->storageSize(opCtx) * 2));

    long long excessSize = fromCollection->dataSize(opCtx) - allocatedSpaceGuess;

    auto exec = InternalPlanner::collectionScan(opCtx,
                                                fromNss.ns(),
                                                fromCollection,
                                                PlanExecutor::WRITE_CONFLICT_RETRY_ONLY,
                                                InternalPlanner::FORWARD);

    Snapshotted<BSONObj> objToClone;
    RecordId loc;
    PlanExecutor::ExecState state = PlanExecutor::FAILURE;  // suppress uninitialized warnings

    DisableDocumentValidation validationDisabler(opCtx);

    int retries = 0;  // non-zero when retrying our last document.
    while (true) {
        if (!retries) {
            state = exec->getNextSnapshotted(&objToClone, &loc);
        }

        switch (state) {
            case PlanExecutor::IS_EOF:
                return;
            case PlanExecutor::ADVANCED: {
                if (excessSize > 0) {
                    // 4x is for padding, power of 2, etc...
                    excessSize -= (4 * objToClone.value().objsize());
                    continue;
                }
                break;
            }
            default:
                // A collection scan plan which does not yield should never fail.
                MONGO_UNREACHABLE;
        }

        try {
            // Make sure we are working with the latest version of the document.
            if (objToClone.snapshotId() != opCtx->recoveryUnit()->getSnapshotId() &&
                !fromCollection->findDoc(opCtx, loc, &objToClone)) {
                // doc was deleted so don't clone it.
                retries = 0;
                continue;
            }

            WriteUnitOfWork wunit(opCtx);
            OpDebug* const nullOpDebug = nullptr;
            uassertStatusOK(toCollection->insertDocument(
                opCtx, InsertStatement(objToClone.value()), nullOpDebug, true));
            wunit.commit();

            // Go to the next document
            retries = 0;
        } catch (const WriteConflictException&) {
            CurOp::get(opCtx)->debug().additiveMetrics.incrementWriteConflicts(1);
            retries++;  // logAndBackoff expects this to be 1 on first call.
            WriteConflictException::logAndBackoff(retries, "cloneCollectionAsCapped", fromNss.ns());

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            opCtx->recoveryUnit()->abandonSnapshot();
            exec->restoreState();  // Handles any WCEs internally.
        }
    }

    MONGO_UNREACHABLE;
}

void mongo::convertToCapped(OperationContext* opCtx,
                            const NamespaceString& collectionName,
                            long long size) {
    StringData dbname = collectionName.db();
    StringData shortSource = collectionName.coll();

    AutoGetDb autoDb(opCtx, collectionName.db(), MODE_X);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName);

    uassert(ErrorCodes::NotMaster,
            str::stream() << "Not primary while converting " << collectionName
                          << " to a capped collection",
            !userInitiatedWritesAndNotPrimary);

    Database* const db = autoDb.getDb();
    uassert(
        ErrorCodes::NamespaceNotFound, str::stream() << "database " << dbname << " not found", db);

    BackgroundOperation::assertNoBgOpInProgForDb(dbname);

    // Generate a temporary collection name that will not collide with any existing collections.
    auto tmpNameResult =
        db->makeUniqueCollectionNamespace(opCtx, "tmp%%%%%.convertToCapped." + shortSource);
    uassertStatusOKWithContext(tmpNameResult,
                               str::stream()
                                   << "Cannot generate temporary collection namespace to convert "
                                   << collectionName
                                   << " to a capped collection");

    const auto& longTmpName = tmpNameResult.getValue();
    const auto shortTmpName = longTmpName.coll().toString();

    cloneCollectionAsCapped(opCtx, db, shortSource.toString(), shortTmpName, size, true);

    RenameCollectionOptions options;
    options.dropTarget = true;
    options.stayTemp = false;
    uassertStatusOK(renameCollection(opCtx, longTmpName, collectionName, options));
}
