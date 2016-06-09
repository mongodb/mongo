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

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
Status emptyCapped(OperationContext* txn, const NamespaceString& collectionName) {
    ScopedTransaction scopedXact(txn, MODE_IX);
    AutoGetDb autoDb(txn, collectionName.db(), MODE_X);

    bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(collectionName);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while truncating collection "
                                    << collectionName.ns());
    }

    Database* db = autoDb.getDb();
    massert(13429, "no such database", db);

    Collection* collection = db->getCollection(collectionName);
    massert(28584, "no such collection", collection);

    BackgroundOperation::assertNoBgOpInProgForNs(collectionName.ns());

    WriteUnitOfWork wuow(txn);

    Status status = collection->truncate(txn);
    if (!status.isOK()) {
        return status;
    }

    getGlobalServiceContext()->getOpObserver()->onEmptyCapped(txn, collection->ns());

    wuow.commit();

    return Status::OK();
}

Status cloneCollectionAsCapped(OperationContext* txn,
                               Database* db,
                               const std::string& shortFrom,
                               const std::string& shortTo,
                               double size,
                               bool temp) {
    std::string fromNs = db->name() + "." + shortFrom;
    std::string toNs = db->name() + "." + shortTo;

    Collection* fromCollection = db->getCollection(fromNs);
    if (!fromCollection)
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "source collection " << fromNs << " does not exist");

    if (db->getCollection(toNs))
        return Status(ErrorCodes::NamespaceExists, "to collection already exists");

    // create new collection
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        const auto fromOptions =
            fromCollection->getCatalogEntry()->getCollectionOptions(txn).toBSON();
        OldClientContext ctx(txn, toNs);
        BSONObjBuilder spec;
        spec.appendBool("capped", true);
        spec.append("size", size);
        if (temp)
            spec.appendBool("temp", true);
        spec.appendElementsUnique(fromOptions);

        WriteUnitOfWork wunit(txn);
        Status status = userCreateNS(txn, ctx.db(), toNs, spec.done());
        if (!status.isOK())
            return status;
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "cloneCollectionAsCapped", fromNs);

    Collection* toCollection = db->getCollection(toNs);
    invariant(toCollection);  // we created above

    // how much data to ignore because it won't fit anyway
    // datasize and extentSize can't be compared exactly, so add some padding to 'size'

    long long allocatedSpaceGuess =
        std::max(static_cast<long long>(size * 2),
                 static_cast<long long>(toCollection->getRecordStore()->storageSize(txn) * 2));

    long long excessSize = fromCollection->dataSize(txn) - allocatedSpaceGuess;

    std::unique_ptr<PlanExecutor> exec(InternalPlanner::collectionScan(
        txn, fromNs, fromCollection, PlanExecutor::YIELD_MANUAL, InternalPlanner::FORWARD));

    exec->setYieldPolicy(PlanExecutor::WRITE_CONFLICT_RETRY_ONLY, fromCollection);

    Snapshotted<BSONObj> objToClone;
    RecordId loc;
    PlanExecutor::ExecState state = PlanExecutor::FAILURE;  // suppress uninitialized warnings

    DisableDocumentValidation validationDisabler(txn);

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
            if (objToClone.snapshotId() != txn->recoveryUnit()->getSnapshotId() &&
                !fromCollection->findDoc(txn, loc, &objToClone)) {
                // doc was deleted so don't clone it.
                retries = 0;
                continue;
            }

            WriteUnitOfWork wunit(txn);
            OpDebug* const nullOpDebug = nullptr;
            toCollection->insertDocument(
                txn, objToClone.value(), nullOpDebug, true, txn->writesAreReplicated());
            wunit.commit();

            // Go to the next document
            retries = 0;
        } catch (const WriteConflictException& wce) {
            CurOp::get(txn)->debug().writeConflicts++;
            retries++;  // logAndBackoff expects this to be 1 on first call.
            wce.logAndBackoff(retries, "cloneCollectionAsCapped", fromNs);

            // Can't use WRITE_CONFLICT_RETRY_LOOP macros since we need to save/restore exec
            // around call to abandonSnapshot.
            exec->saveState();
            txn->recoveryUnit()->abandonSnapshot();
            exec->restoreState();  // Handles any WCEs internally.
        }
    }

    invariant(false);  // unreachable
}

Status convertToCapped(OperationContext* txn, const NamespaceString& collectionName, double size) {
    StringData dbname = collectionName.db();
    StringData shortSource = collectionName.coll();

    ScopedTransaction transaction(txn, MODE_IX);
    AutoGetDb autoDb(txn, collectionName.db(), MODE_X);

    bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(collectionName);

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

    std::string shortTmpName = str::stream() << "tmp.convertToCapped." << shortSource;
    std::string longTmpName = str::stream() << dbname << "." << shortTmpName;

    if (db->getCollection(longTmpName)) {
        WriteUnitOfWork wunit(txn);
        Status status = db->dropCollection(txn, longTmpName);
        if (!status.isOK())
            return status;
    }


    const bool shouldReplicateWrites = txn->writesAreReplicated();
    txn->setReplicatedWrites(false);
    ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, txn, shouldReplicateWrites);
    Status status =
        cloneCollectionAsCapped(txn, db, shortSource.toString(), shortTmpName, size, true);

    if (!status.isOK()) {
        return status;
    }

    verify(db->getCollection(longTmpName));

    {
        WriteUnitOfWork wunit(txn);
        status = db->dropCollection(txn, collectionName.ns());
        txn->setReplicatedWrites(shouldReplicateWrites);
        if (!status.isOK())
            return status;

        status = db->renameCollection(txn, longTmpName, collectionName.ns(), false);
        if (!status.isOK())
            return status;

        getGlobalServiceContext()->getOpObserver()->onConvertToCapped(
            txn, NamespaceString(collectionName), size);

        wunit.commit();
    }
    return Status::OK();
}

}  // namespace mongo
