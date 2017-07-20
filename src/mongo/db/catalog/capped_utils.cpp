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

#include "mongo/db/catalog/capped_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view.h"
#include "mongo/util/scopeguard.h"

mongo::Status mongo::emptyCapped(OperationContext* opCtx, const NamespaceString& collectionName) {
    AutoGetDb autoDb(opCtx, collectionName.db(), MODE_X);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, collectionName);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while truncating collection: "
                                    << collectionName.ns());
    }

    Database* db = autoDb.getDb();
    massert(13429, "no such database", db);

    Collection* collection = db->getCollection(opCtx, collectionName);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "emptycapped not supported on view: " << collectionName.ns(),
            collection || !db->getViewCatalog()->lookup(opCtx, collectionName.ns()));
    massert(28584, "no such collection", collection);

    if (collectionName.isSystem() && !collectionName.isSystemDotProfile()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot truncate a system collection: "
                                    << collectionName.ns());
    }

    if (NamespaceString::virtualized(collectionName.ns())) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot truncate a virtual collection: "
                                    << collectionName.ns());
    }

    if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
         repl::ReplicationCoordinator::modeNone) &&
        collectionName.isOplog()) {
        return Status(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Cannot truncate a live oplog while replicating: "
                                    << collectionName.ns());
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

mongo::Status mongo::cloneCollectionAsCapped(OperationContext* opCtx,
                                             Database* db,
                                             const std::string& shortFrom,
                                             const std::string& shortTo,
                                             double size,
                                             bool temp) {
    NamespaceString fromNss(db->name(), shortFrom);
    NamespaceString toNss(db->name(), shortTo);

    Collection* fromCollection = db->getCollection(opCtx, fromNss);
    if (!fromCollection) {
        if (db->getViewCatalog()->lookup(opCtx, fromNss.ns())) {
            return Status(ErrorCodes::CommandNotSupportedOnView,
                          str::stream() << "cloneCollectionAsCapped not supported for views: "
                                        << fromNss.ns());
        }
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "source collection " << fromNss.ns() << " does not exist");
    }

    if (db->getCollection(opCtx, toNss)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "cloneCollectionAsCapped failed - destination collection "
                                    << toNss.ns()
                                    << " already exists. source collection: "
                                    << fromNss.ns());
    }

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
        Status status = createCollection(opCtx, toNss.db().toString(), cmd.done());
        if (!status.isOK())
            return status;
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
                return Status::OK();
            case PlanExecutor::ADVANCED: {
                if (excessSize > 0) {
                    // 4x is for padding, power of 2, etc...
                    excessSize -= (4 * objToClone.value().objsize());
                    continue;
                }
                break;
            }
            default:
                // Unreachable as:
                // 1) We require a read lock (at a minimum) on the "from" collection
                //    and won't yield, preventing collection drop and PlanExecutor::DEAD
                // 2) PlanExecutor::FAILURE is only returned on PlanStage::FAILURE. The
                //    CollectionScan PlanStage does not have a FAILURE scenario.
                // 3) All other PlanExecutor states are handled above
                invariant(false);
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
            toCollection
                ->insertDocument(opCtx,
                                 InsertStatement(objToClone.value()),
                                 nullOpDebug,
                                 true,
                                 opCtx->writesAreReplicated())
                .transitional_ignore();
            wunit.commit();

            // Go to the next document
            retries = 0;
        } catch (const WriteConflictException& wce) {
            CurOp::get(opCtx)->debug().writeConflicts++;
            retries++;  // logAndBackoff expects this to be 1 on first call.
            wce.logAndBackoff(retries, "cloneCollectionAsCapped", fromNss.ns());

            // Can't use writeConflictRetry since we need to save/restore exec around call to
            // abandonSnapshot.
            exec->saveState();
            opCtx->recoveryUnit()->abandonSnapshot();
            exec->restoreState();  // Handles any WCEs internally.
        }
    }

    invariant(false);  // unreachable
}

mongo::Status mongo::convertToCapped(OperationContext* opCtx,
                                     const NamespaceString& collectionName,
                                     double size) {
    StringData dbname = collectionName.db();
    StringData shortSource = collectionName.coll();
    const std::string shortTmpName = str::stream() << "tmp.convertToCapped." << shortSource;
    const NamespaceString longTmpName(dbname, shortTmpName);

    AutoGetDb autoDb(opCtx, collectionName.db(), MODE_X);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, collectionName);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while converting " << collectionName.ns()
                                    << " to a capped collection");
    }

    Database* const db = autoDb.getDb();
    if (!db) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "database " << dbname << " not found");
    }

    BackgroundOperation::assertNoBgOpInProgForDb(dbname);

    // If the temporary collection already exists due to an earlier aborted attempt, delete it.
    if (db->getCollection(opCtx, longTmpName)) {
        BSONObjBuilder unusedResult;
        Status status =
            dropCollection(opCtx,
                           longTmpName,
                           unusedResult,
                           repl::OpTime(),
                           DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
        if (!status.isOK())
            return status;
    }

    {
        Status status =
            cloneCollectionAsCapped(opCtx, db, shortSource.toString(), shortTmpName, size, true);

        if (!status.isOK())
            return status;
    }

    return renameCollection(
        opCtx, longTmpName, collectionName, /*dropTarget*/ true, /*stayTemp*/ false);
}
