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

#include "mongo/db/catalog/rename_collection.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
static void dropCollection(OperationContext* txn, Database* db, StringData collName) {
    WriteUnitOfWork wunit(txn);
    if (db->dropCollection(txn, collName).isOK()) {
        // ignoring failure case
        wunit.commit();
    }
}
}  // namespace

Status renameCollection(OperationContext* txn,
                        const NamespaceString& source,
                        const NamespaceString& target,
                        bool dropTarget,
                        bool stayTemp) {
    DisableDocumentValidation validationDisabler(txn);

    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite globalWriteLock(txn->lockState());
    // We stay in source context the whole time. This is mostly to set the CurOp namespace.
    OldClientContext ctx(txn, source.ns());

    bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(source);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while renaming collection " << source.ns()
                                    << " to "
                                    << target.ns());
    }

    Database* const sourceDB = dbHolder().get(txn, source.db());
    Collection* const sourceColl = sourceDB ? sourceDB->getCollection(source.ns()) : nullptr;
    if (!sourceColl) {
        return Status(ErrorCodes::NamespaceNotFound, "source namespace does not exist");
    }

    {
        // Ensure that collection name does not exceed maximum length.
        // Ensure that index names do not push the length over the max.
        // Iterator includes unfinished indexes.
        IndexCatalog::IndexIterator sourceIndIt =
            sourceColl->getIndexCatalog()->getIndexIterator(txn, true);
        int longestIndexNameLength = 0;
        while (sourceIndIt.more()) {
            int thisLength = sourceIndIt.next()->indexName().length();
            if (thisLength > longestIndexNameLength)
                longestIndexNameLength = thisLength;
        }

        unsigned int longestAllowed =
            std::min(int(NamespaceString::MaxNsCollectionLen),
                     int(NamespaceString::MaxNsLen) - 2 /*strlen(".$")*/ - longestIndexNameLength);
        if (target.size() > longestAllowed) {
            StringBuilder sb;
            sb << "collection name length of " << target.size() << " exceeds maximum length of "
               << longestAllowed << ", allowing for index names";
            return Status(ErrorCodes::InvalidLength, sb.str());
        }
    }

    BackgroundOperation::assertNoBgOpInProgForNs(source.ns());

    Database* const targetDB = dbHolder().openDb(txn, target.db());

    {
        WriteUnitOfWork wunit(txn);

        // Check if the target namespace exists and if dropTarget is true.
        // If target exists and dropTarget is not true, return false.
        if (targetDB->getCollection(target)) {
            if (!dropTarget) {
                return Status(ErrorCodes::NamespaceExists, "target namespace exists");
            }

            Status s = targetDB->dropCollection(txn, target.ns());
            if (!s.isOK()) {
                return s;
            }
        }

        // If we are renaming in the same database, just
        // rename the namespace and we're done.
        if (sourceDB == targetDB) {
            Status s = targetDB->renameCollection(txn, source.ns(), target.ns(), stayTemp);
            if (!s.isOK()) {
                return s;
            }

            getGlobalServiceContext()->getOpObserver()->onRenameCollection(
                txn, NamespaceString(source), NamespaceString(target), dropTarget, stayTemp);

            wunit.commit();
            return Status::OK();
        }

        wunit.commit();
    }

    // If we get here, we are renaming across databases, so we must copy all the data and
    // indexes, then remove the source collection.

    // Create the target collection. It will be removed if we fail to copy the collection.
    // TODO use a temp collection and unset the temp flag on success.
    Collection* targetColl = nullptr;
    {
        CollectionOptions options = sourceColl->getCatalogEntry()->getCollectionOptions(txn);

        WriteUnitOfWork wunit(txn);

        // No logOp necessary because the entire renameCollection command is one logOp.
        bool shouldReplicateWrites = txn->writesAreReplicated();
        txn->setReplicatedWrites(false);
        targetColl = targetDB->createCollection(txn,
                                                target.ns(),
                                                options,
                                                false);  // _id index build with others later.
        txn->setReplicatedWrites(shouldReplicateWrites);
        if (!targetColl) {
            return Status(ErrorCodes::OutOfDiskSpace, "Failed to create target collection.");
        }

        wunit.commit();
    }

    // Dismissed on success
    ScopeGuard targetCollectionDropper = MakeGuard(dropCollection, txn, targetDB, target.ns());

    MultiIndexBlock indexer(txn, targetColl);
    indexer.allowInterruption();
    std::vector<MultiIndexBlock*> indexers{&indexer};

    // Copy the index descriptions from the source collection, adjusting the ns field.
    {
        std::vector<BSONObj> indexesToCopy;
        IndexCatalog::IndexIterator sourceIndIt =
            sourceColl->getIndexCatalog()->getIndexIterator(txn, true);
        while (sourceIndIt.more()) {
            const BSONObj currIndex = sourceIndIt.next()->infoObj();

            // Process the source index.
            BSONObjBuilder newIndex;
            newIndex.append("ns", target.ns());
            newIndex.appendElementsUnique(currIndex);
            indexesToCopy.push_back(newIndex.obj());
        }
        indexer.init(indexesToCopy);
    }

    {
        // Copy over all the data from source collection to target collection.
        auto cursor = sourceColl->getCursor(txn);
        while (auto record = cursor->next()) {
            txn->checkForInterrupt();

            const auto obj = record->data.releaseToBson();

            WriteUnitOfWork wunit(txn);
            // No logOp necessary because the entire renameCollection command is one logOp.
            bool shouldReplicateWrites = txn->writesAreReplicated();
            txn->setReplicatedWrites(false);
            Status status = targetColl->insertDocument(txn, obj, indexers, true);
            txn->setReplicatedWrites(shouldReplicateWrites);
            if (!status.isOK())
                return status;
            wunit.commit();
        }
    }

    Status status = indexer.doneInserting();
    if (!status.isOK())
        return status;

    {
        // Getting here means we successfully built the target copy. We now remove the
        // source collection and finalize the rename.
        WriteUnitOfWork wunit(txn);

        bool shouldReplicateWrites = txn->writesAreReplicated();
        txn->setReplicatedWrites(false);
        Status status = sourceDB->dropCollection(txn, source.ns());
        txn->setReplicatedWrites(shouldReplicateWrites);
        if (!status.isOK())
            return status;

        indexer.commit();

        getGlobalServiceContext()->getOpObserver()->onRenameCollection(
            txn, NamespaceString(source), NamespaceString(target), dropTarget, stayTemp);

        wunit.commit();
    }

    targetCollectionDropper.Dismiss();
    return Status::OK();
}

}  // namespace mongo
