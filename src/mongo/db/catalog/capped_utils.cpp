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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/capped_utils.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {
    std::vector<BSONObj> stopIndexBuildsEmptyCapped(OperationContext* opCtx,
                                         Database* db, 
                                         const NamespaceString& ns) {
        IndexCatalog::IndexKillCriteria criteria;
        criteria.ns = ns;
        return IndexBuilder::killMatchingIndexBuilds(db->getCollection(ns), criteria);
    }

    std::vector<BSONObj> stopIndexBuildsConvertToCapped(OperationContext* opCtx,
                                                        Database* db,
                                                        const NamespaceString& ns) {
        IndexCatalog::IndexKillCriteria criteria;
        criteria.ns = ns;
        Collection* coll = db->getCollection(ns);
        if (coll) {
            return IndexBuilder::killMatchingIndexBuilds(coll, criteria);
        }
        return std::vector<BSONObj>();
    }

} // namespace

    Status emptyCapped(OperationContext* txn,
                       const NamespaceString& collectionName) {
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetDb autoDb(txn, collectionName.db(), MODE_X);

        bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
            !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                    collectionName.db());

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while truncating collection "
                                        << collectionName.ns());
        }

        Database* db = autoDb.getDb();
        massert(13429, "no such database", db);

        Collection* collection = db->getCollection(collectionName);
        massert(28584, "no such collection", collection);

        std::vector<BSONObj> indexes = stopIndexBuildsEmptyCapped(txn, db, collectionName);

        WriteUnitOfWork wuow(txn);

        Status status = collection->truncate(txn);
        if (!status.isOK()) {
            return status;
        }

        IndexBuilder::restoreIndexes(txn, indexes);

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
                          str::stream() << "source collection " << fromNs <<  " does not exist");

        if (db->getCollection(toNs))
            return Status(ErrorCodes::NamespaceExists, "to collection already exists");

        // create new collection
        {
            OldClientContext ctx(txn,  toNs);
            BSONObjBuilder spec;
            spec.appendBool("capped", true);
            spec.append("size", size);
            if (temp)
                spec.appendBool("temp", true);

            WriteUnitOfWork wunit(txn);
            Status status = userCreateNS(txn, ctx.db(), toNs, spec.done());
            if (!status.isOK())
                return status;
            wunit.commit();
        }

        Collection* toCollection = db->getCollection(toNs);
        invariant(toCollection); // we created above

        // how much data to ignore because it won't fit anyway
        // datasize and extentSize can't be compared exactly, so add some padding to 'size'

        long long allocatedSpaceGuess =
            std::max(static_cast<long long>(size * 2),
                     static_cast<long long>(toCollection->getRecordStore()->storageSize(txn) * 2));

        long long excessSize = fromCollection->dataSize(txn) - allocatedSpaceGuess;

        boost::scoped_ptr<PlanExecutor> exec(InternalPlanner::collectionScan(
                    txn,
                    fromNs,
                    fromCollection,
                    InternalPlanner::FORWARD));


        while (true) {
            BSONObj obj;
            PlanExecutor::ExecState state = exec->getNext(&obj, NULL);

            switch(state) {
            case PlanExecutor::IS_EOF:
                return Status::OK();
            case PlanExecutor::DEAD:
                db->dropCollection(txn, toNs);
                return Status(ErrorCodes::InternalError, "executor turned dead while iterating");
            case PlanExecutor::FAILURE:
                return Status(ErrorCodes::InternalError, "executor error while iterating");
            case PlanExecutor::ADVANCED:
                if (excessSize > 0) {
                    excessSize -= (4 * obj.objsize()); // 4x is for padding, power of 2, etc...
                    continue;
                }

                WriteUnitOfWork wunit(txn);
                toCollection->insertDocument(txn, obj, true, txn->writesAreReplicated());
                wunit.commit();
            }
        }

        invariant(false); // unreachable
    }

    Status convertToCapped(OperationContext* txn,
                           const NamespaceString& collectionName,
                           double size) {

        StringData dbname = collectionName.db();
        StringData shortSource = collectionName.coll();

        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetDb autoDb(txn, collectionName.db(), MODE_X);

        bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
            !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while converting "
                                        << collectionName.ns() << " to a capped collection");
        }

        Database* const db = autoDb.getDb();
        if (!db) {
            return Status(ErrorCodes::DatabaseNotFound,
                          str::stream() << "database " << dbname << " not found");
        }

        stopIndexBuildsConvertToCapped(txn, db, collectionName);
        BackgroundOperation::assertNoBgOpInProgForDb(dbname);

        std::string shortTmpName = str::stream() << "tmp.convertToCapped." << shortSource;
        std::string longTmpName = str::stream() << dbname << "." << shortTmpName;

        WriteUnitOfWork wunit(txn);
        if (db->getCollection(longTmpName)) {
            Status status = db->dropCollection(txn, longTmpName);
            if (!status.isOK())
                return status;
        }


        bool shouldReplicateWrites = txn->writesAreReplicated();
        txn->setReplicatedWrites(false);
        Status status = cloneCollectionAsCapped(txn,
                                                db,
                                                shortSource.toString(),
                                                shortTmpName,
                                                size,
                                                true);

        if (!status.isOK()) {
            txn->setReplicatedWrites(shouldReplicateWrites);
            return status;
        }

        verify(db->getCollection(longTmpName));

        status = db->dropCollection(txn, collectionName.ns());
        txn->setReplicatedWrites(shouldReplicateWrites);
        if (!status.isOK())
            return status;

        status = db->renameCollection(txn, longTmpName, collectionName.ns(), false);
        if (!status.isOK())
            return status;

        getGlobalServiceContext()->getOpObserver()->onConvertToCapped(
                txn,
                NamespaceString(collectionName),
                size);

        wunit.commit();
        return Status::OK();
    }

} // namespace mongo
