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

#include "mongo/db/catalog/drop_indexes.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

// Field name in dropIndexes command for indexes to drop.
constexpr auto kIndexFieldName = "index"_sd;

Status checkView(OperationContext* opCtx,
                 const NamespaceString& nss,
                 Database* db,
                 Collection* collection) {
    if (!collection) {
        if (db && ViewCatalog::get(db)->lookup(opCtx, nss.ns())) {
            return Status(ErrorCodes::CommandNotSupportedOnView,
                          str::stream() << "Cannot drop indexes on view " << nss);
        }
        return Status(ErrorCodes::NamespaceNotFound, str::stream() << "ns not found " << nss);
    }
    return Status::OK();
}

Status checkReplState(OperationContext* opCtx, NamespaceStringOrUUID dbAndUUID) {
    bool writesAreReplicatedAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, dbAndUUID);

    if (writesAreReplicatedAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while dropping indexes on database "
                                    << dbAndUUID.db() << " with collection " << dbAndUUID.uuid());
    }
    return Status::OK();
}

/**
 * Validates the key pattern passed through the command.
 */
StatusWith<const IndexDescriptor*> getDescriptorByKeyPattern(OperationContext* opCtx,
                                                             IndexCatalog* indexCatalog,
                                                             const BSONElement& keyPattern) {
    const bool includeUnfinished = true;
    std::vector<const IndexDescriptor*> indexes;
    indexCatalog->findIndexesByKeyPattern(
        opCtx, keyPattern.embeddedObject(), includeUnfinished, &indexes);
    if (indexes.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "can't find index with key: " << keyPattern.embeddedObject());
    } else if (indexes.size() > 1) {
        return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                      str::stream() << indexes.size() << " indexes found for key: "
                                    << keyPattern.embeddedObject() << ", identify by name instead."
                                    << " Conflicting indexes: " << indexes[0]->infoObj() << ", "
                                    << indexes[1]->infoObj());
    }

    const IndexDescriptor* desc = indexes[0];
    if (desc->isIdIndex()) {
        return Status(ErrorCodes::InvalidOptions, "cannot drop _id index");
    }

    if (desc->indexName() == "*") {
        // Dropping an index named '*' results in an drop-index oplog entry with a name of '*',
        // which in 3.6 and later is interpreted by replication as meaning "drop all indexes on
        // this collection".
        return Status(ErrorCodes::InvalidOptions,
                      "cannot drop an index named '*' by key pattern.  You must drop the "
                      "entire collection, drop all indexes on the collection by using an index "
                      "name of '*', or downgrade to 3.4 to drop only this index.");
    }

    return desc;
}

/**
 * Returns a list of index names that the caller requested to abort/drop. Requires a collection lock
 * to be held to look up the index name from the key pattern.
 */
StatusWith<std::vector<std::string>> getIndexNames(OperationContext* opCtx,
                                                   Collection* collection,
                                                   const BSONElement& indexElem) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IX));

    std::vector<std::string> indexNames;
    if (indexElem.type() == String) {
        std::string indexToAbort = indexElem.valuestr();
        indexNames.push_back(indexToAbort);
    } else if (indexElem.type() == Object) {
        auto swDescriptor =
            getDescriptorByKeyPattern(opCtx, collection->getIndexCatalog(), indexElem);
        if (!swDescriptor.isOK()) {
            return swDescriptor.getStatus();
        }
        indexNames.push_back(swDescriptor.getValue()->indexName());
    } else if (indexElem.type() == Array) {
        for (auto indexNameElem : indexElem.Array()) {
            invariant(indexNameElem.type() == String);
            indexNames.push_back(indexNameElem.valuestr());
        }
    }

    return indexNames;
}

/**
 * Attempts to abort a single index builder that is responsible for all the index names passed in.
 */
std::vector<UUID> abortIndexBuildByIndexNamesNoWait(OperationContext* opCtx,
                                                    Collection* collection,
                                                    std::vector<std::string> indexNames) {

    boost::optional<UUID> buildUUID =
        IndexBuildsCoordinator::get(opCtx)->abortIndexBuildByIndexNamesNoWait(
            opCtx, collection->uuid(), indexNames, std::string("dropIndexes command"));
    if (buildUUID) {
        return {*buildUUID};
    }
    return {};
}

/**
 * Drops single index given a descriptor.
 */
Status dropIndexByDescriptor(OperationContext* opCtx,
                             Collection* collection,
                             IndexCatalog* indexCatalog,
                             const IndexDescriptor* desc) {
    if (desc->isIdIndex()) {
        return Status(ErrorCodes::InvalidOptions, "cannot drop _id index");
    }

    // Support dropping unfinished indexes, but only if the index is 'frozen'. These indexes only
    // exist in standalone mode.
    auto entry = indexCatalog->getEntry(desc);
    if (entry->isFrozen()) {
        invariant(!entry->isReady(opCtx));
        invariant(getReplSetMemberInStandaloneMode(opCtx->getServiceContext()));
        // Return here. No need to fall through to op observer on standalone.
        return indexCatalog->dropUnfinishedIndex(opCtx, desc);
    }

    // Do not allow dropping unfinished indexes that are not frozen.
    if (!entry->isReady(opCtx)) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "can't drop unfinished index with name: " << desc->indexName());
    }

    auto s = indexCatalog->dropIndex(opCtx, desc);
    if (!s.isOK()) {
        return s;
    }

    opCtx->getServiceContext()->getOpObserver()->onDropIndex(
        opCtx, collection->ns(), collection->uuid(), desc->indexName(), desc->infoObj());

    return Status::OK();
}

/**
 * Aborts all the index builders on the collection if the first element in 'indexesToAbort' is "*",
 * otherwise this attempts to abort a single index builder building the given index names.
 */
std::vector<UUID> abortActiveIndexBuilders(OperationContext* opCtx,
                                           Collection* collection,
                                           const std::vector<std::string>& indexNames) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IX));

    if (indexNames.empty()) {
        return {};
    }

    if (indexNames.front() == "*") {
        return IndexBuildsCoordinator::get(opCtx)->abortCollectionIndexBuildsNoWait(
            opCtx, collection->uuid(), "dropIndexes command");
    }

    return abortIndexBuildByIndexNamesNoWait(opCtx, collection, indexNames);
}

Status dropReadyIndexes(OperationContext* opCtx,
                        Collection* collection,
                        const std::vector<std::string>& indexNames,
                        BSONObjBuilder* anObjBuilder) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));

    if (indexNames.empty()) {
        return Status::OK();
    }

    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    if (indexNames.front() == "*") {
        indexCatalog->dropAllIndexes(
            opCtx, false, [opCtx, collection](const IndexDescriptor* desc) {
                opCtx->getServiceContext()->getOpObserver()->onDropIndex(opCtx,
                                                                         collection->ns(),
                                                                         collection->uuid(),
                                                                         desc->indexName(),
                                                                         desc->infoObj());
            });

        anObjBuilder->append("msg", "non-_id indexes dropped for collection");
        return Status::OK();
    }

    bool includeUnfinished = true;
    for (const auto& indexName : indexNames) {
        auto desc = indexCatalog->findIndexByName(opCtx, indexName, includeUnfinished);
        if (!desc) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream() << "index not found with name [" << indexName << "]");
        }
        Status status = dropIndexByDescriptor(opCtx, collection, indexCatalog, desc);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

}  // namespace

Status dropIndexes(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const BSONObj& cmdObj,
                   BSONObjBuilder* result) {
    // We only need to hold an intent lock to send abort signals to the active index builder(s) we
    // intend to abort.
    boost::optional<AutoGetCollection> autoColl;
    autoColl.emplace(opCtx, nss, MODE_IX);

    Database* db = autoColl->getDb();
    Collection* collection = autoColl->getCollection();
    Status status = checkView(opCtx, nss, db, collection);
    if (!status.isOK()) {
        return status;
    }

    const UUID collectionUUID = collection->uuid();
    const NamespaceStringOrUUID dbAndUUID = {nss.db().toString(), collectionUUID};

    status = checkReplState(opCtx, dbAndUUID);
    if (!status.isOK()) {
        return status;
    }

    if (!serverGlobalParams.quiet.load()) {
        LOGV2(51806,
              "CMD: dropIndexes {nss}: {cmdObj_kIndexFieldName_false}",
              "nss"_attr = nss,
              "cmdObj_kIndexFieldName_false"_attr = cmdObj[kIndexFieldName].toString(false));
    }

    result->appendNumber("nIndexesWas", collection->getIndexCatalog()->numIndexesTotal(opCtx));

    // Validate basic user input.
    BSONElement indexElem = cmdObj.getField(kIndexFieldName);
    const bool isWildcard = indexElem.type() == String && indexElem.String() == "*";

    // If an Array was passed in, verify that all the elements are of type String.
    if (indexElem.type() == Array) {
        for (auto indexNameElem : indexElem.Array()) {
            if (indexNameElem.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "dropIndexes " << collection->ns() << " (" << collectionUUID
                                  << ") failed to drop multiple indexes "
                                  << indexElem.toString(false) << ": index name must be a string");
            }
        }
    }

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

    // When releasing the collection lock to send the abort signal to the index builders, it's
    // possible for new index builds to start. Keep aborting in-progress index builds if they
    // satisfy the caller's input.
    std::vector<UUID> abortedIndexBuilders;
    std::vector<std::string> indexNames;
    while (true) {
        auto swIndexNames = getIndexNames(opCtx, collection, indexElem);
        if (!swIndexNames.isOK()) {
            return swIndexNames.getStatus();
        }

        indexNames = swIndexNames.getValue();

        // Send the abort signal to any index builders that match the users request.
        abortedIndexBuilders = abortActiveIndexBuilders(opCtx, collection, indexNames);

        // Now that the abort signals were sent to the intended index builders, release our lock
        // temporarily to allow the index builders to process the abort signal. Holding a lock here
        // will cause the index builders to block indefinitely.
        autoColl = boost::none;
        if (abortedIndexBuilders.size() == 1) {
            indexBuildsCoord->awaitIndexBuildFinished(collectionUUID, abortedIndexBuilders.front());
        } else if (abortedIndexBuilders.size() > 1) {
            // Only the "*" wildcard can abort multiple index builders.
            invariant(isWildcard);
            indexBuildsCoord->awaitNoIndexBuildInProgressForCollection(collectionUUID);
        }

        // Take an exclusive lock on the collection now to be able to perform index catalog writes
        // when removing ready indexes from disk.
        autoColl.emplace(opCtx, dbAndUUID, MODE_X);

        // Abandon the snapshot as the index catalog will compare the in-memory state to the disk
        // state, which may have changed when we released the lock temporarily.
        opCtx->recoveryUnit()->abandonSnapshot();

        db = autoColl->getDb();
        collection = autoColl->getCollection();
        if (!collection) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "ns not found on database " << dbAndUUID.db()
                                        << " with collection " << dbAndUUID.uuid());
        }

        status = checkReplState(opCtx, dbAndUUID);
        if (!status.isOK()) {
            return status;
        }

        // Check to see if a new index build was started that the caller requested to be aborted.
        bool abortAgain = false;
        if (isWildcard) {
            abortAgain = indexBuildsCoord->inProgForCollection(collectionUUID);
        } else {
            abortAgain = indexBuildsCoord->hasIndexBuilder(opCtx, collectionUUID, indexNames);
        }

        if (!abortAgain) {
            break;
        }

        // We only need to hold an intent lock to send abort signals to the active index
        // builder(s) we intend to abort.
        autoColl = boost::none;
        autoColl.emplace(opCtx, dbAndUUID, MODE_IX);

        // Abandon the snapshot as the index catalog will compare the in-memory state to the
        // disk state, which may have changed when we released the lock temporarily.
        opCtx->recoveryUnit()->abandonSnapshot();

        db = autoColl->getDb();
        collection = autoColl->getCollection();
        if (!collection) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "ns not found on database " << dbAndUUID.db()
                                        << " with collection " << dbAndUUID.uuid());
        }

        status = checkReplState(opCtx, dbAndUUID);
        if (!status.isOK()) {
            return status;
        }
    }

    // If the "*" wildcard was not specified, verify that all the index names belonging to the
    // index builder were aborted.
    if (!isWildcard && !abortedIndexBuilders.empty()) {
        invariant(abortedIndexBuilders.size() == 1);

        // This is necessary to check shard version.
        OldClientContext ctx(opCtx, collection->ns().ns());

        // Iterate through all aborted indexes and verify none of them are ready. This would
        // indicate a flaw with the abort logic that allows indexes to complete despite the
        // dropIndexes command reporting they were aborted.
        auto indexCatalog = collection->getIndexCatalog();
        const bool includeUnfinished = false;
        const bool noneReady = std::none_of(indexNames.begin(), indexNames.end(), [&](auto name) {
            return indexCatalog->findIndexByName(opCtx, name, includeUnfinished);
        });

        invariant(noneReady,
                  str::stream() << "Found completed indexes despite aborting index build: "
                                << abortedIndexBuilders.front());
        return Status::OK();
    }

    if (!abortedIndexBuilders.empty()) {
        // All the index builders were sent the abort signal, remove all the remaining indexes in
        // the index catalog.
        invariant(isWildcard);
        invariant(indexNames.size() == 1);
        invariant(indexNames.front() == "*");
        invariant(collection->getIndexCatalog()->numIndexesInProgress(opCtx) == 0);
    } else {
        // The index catalog requires that no active index builders are running when dropping
        // indexes.
        BackgroundOperation::assertNoBgOpInProgForNs(collection->ns());
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(collectionUUID);
    }

    return writeConflictRetry(
        opCtx, "dropIndexes", dbAndUUID.toString(), [opCtx, &collection, &indexNames, result] {
            WriteUnitOfWork wunit(opCtx);

            // This is necessary to check shard version.
            OldClientContext ctx(opCtx, collection->ns().ns());

            // Use an empty BSONObjBuilder to avoid duplicate appends to result on retry loops.
            BSONObjBuilder tempObjBuilder;
            Status status = dropReadyIndexes(opCtx, collection, indexNames, &tempObjBuilder);
            if (!status.isOK()) {
                return status;
            }

            wunit.commit();

            result->appendElementsUnique(
                tempObjBuilder.done());  // This append will only happen once.
            return Status::OK();
        });
}

Status dropIndexesForApplyOps(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& cmdObj,
                              BSONObjBuilder* result) {
    return writeConflictRetry(opCtx, "dropIndexes", nss.db(), [opCtx, &nss, &cmdObj, result] {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);

        // If db/collection does not exist, short circuit and return.
        Database* db = autoColl.getDb();
        Collection* collection = autoColl.getCollection();
        Status status = checkView(opCtx, nss, db, collection);
        if (!status.isOK()) {
            return status;
        }

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20344,
                  "CMD: dropIndexes {nss}: {cmdObj_kIndexFieldName_false}",
                  "nss"_attr = nss,
                  "cmdObj_kIndexFieldName_false"_attr = cmdObj[kIndexFieldName].toString(false));
        }

        BackgroundOperation::assertNoBgOpInProgForNs(nss);
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            collection->uuid());

        BSONElement indexElem = cmdObj.getField(kIndexFieldName);
        auto swIndexNames = getIndexNames(opCtx, collection, indexElem);
        if (!swIndexNames.isOK()) {
            return swIndexNames.getStatus();
        }

        WriteUnitOfWork wunit(opCtx);

        // This is necessary to check shard version.
        OldClientContext ctx(opCtx, nss.ns());

        // Use an empty BSONObjBuilder to avoid duplicate appends to result on retry loops.
        BSONObjBuilder tempObjBuilder;
        status = dropReadyIndexes(opCtx, collection, swIndexNames.getValue(), &tempObjBuilder);
        if (!status.isOK()) {
            return status;
        }

        wunit.commit();

        result->appendElementsUnique(tempObjBuilder.done());  // This append will only happen once.
        return Status::OK();
    });
}

}  // namespace mongo
