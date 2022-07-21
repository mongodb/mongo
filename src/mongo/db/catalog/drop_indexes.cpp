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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_indexes.h"

#include <boost/algorithm/string/join.hpp>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/overloaded_visitor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterAbortingIndexes);

// Field name in dropIndexes command for indexes to drop.
constexpr auto kIndexFieldName = "index"_sd;

Status checkView(OperationContext* opCtx,
                 const NamespaceString& nss,
                 const CollectionPtr& collection) {
    if (!collection) {
        if (CollectionCatalog::get(opCtx)->lookupView(opCtx, nss)) {
            return Status(ErrorCodes::CommandNotSupportedOnView,
                          str::stream() << "Cannot drop indexes on view " << nss);
        }
        return Status(ErrorCodes::NamespaceNotFound, str::stream() << "ns not found " << nss);
    }
    return Status::OK();
}

Status checkReplState(OperationContext* opCtx,
                      NamespaceStringOrUUID dbAndUUID,
                      const CollectionPtr& collection) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto canAcceptWrites = replCoord->canAcceptWritesFor(opCtx, dbAndUUID);
    bool writesAreReplicatedAndNotPrimary = opCtx->writesAreReplicated() && !canAcceptWrites;

    if (writesAreReplicatedAndNotPrimary) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping indexes on database "
                                    << dbAndUUID.db() << " with collection " << dbAndUUID.uuid());
    }

    // Disallow index drops on drop-pending namespaces (system.drop.*) if we are primary.
    auto isPrimary = replCoord->getSettings().usingReplSets() && canAcceptWrites;
    const auto& nss = collection->ns();
    if (isPrimary && nss.isDropPendingNamespace()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Cannot drop indexes on drop-pending namespace " << nss
                                    << " in database " << dbAndUUID.db() << " with uuid "
                                    << dbAndUUID.uuid());
    }

    return Status::OK();
}

/**
 * Validates the key pattern passed through the command.
 */
StatusWith<const IndexDescriptor*> getDescriptorByKeyPattern(OperationContext* opCtx,
                                                             const IndexCatalog* indexCatalog,
                                                             const BSONObj& keyPattern) {
    std::vector<const IndexDescriptor*> indexes;
    indexCatalog->findIndexesByKeyPattern(opCtx,
                                          keyPattern,
                                          IndexCatalog::InclusionPolicy::kReady |
                                              IndexCatalog::InclusionPolicy::kUnfinished |
                                              IndexCatalog::InclusionPolicy::kFrozen,
                                          &indexes);
    if (indexes.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "can't find index with key: " << keyPattern);
    } else if (indexes.size() > 1) {
        return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                      str::stream() << indexes.size() << " indexes found for key: " << keyPattern
                                    << ", identify by name instead."
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
 * It is illegal to drop a collection's clusteredIndex.
 *
 * Returns true if 'index' is or contains the clusteredIndex.
 */
bool containsClusteredIndex(const CollectionPtr& collection, const IndexArgument& index) {
    invariant(collection && collection->isClustered());

    auto clusteredIndexSpec = collection->getClusteredInfo()->getIndexSpec();
    return stdx::visit(
        OverloadedVisitor{[&](const std::string& indexName) -> bool {
                              // While the clusteredIndex's name is optional during user
                              // creation, it should always be filled in by default on the
                              // collection object.
                              auto clusteredIndexName = clusteredIndexSpec.getName();
                              invariant(clusteredIndexName.is_initialized());

                              return clusteredIndexName.get() == indexName;
                          },
                          [&](const std::vector<std::string>& indexNames) -> bool {
                              // While the clusteredIndex's name is optional during user
                              // creation, it should always be filled in by default on the
                              // collection object.
                              auto clusteredIndexName = clusteredIndexSpec.getName();
                              invariant(clusteredIndexName.is_initialized());

                              return std::find(indexNames.begin(),
                                               indexNames.end(),
                                               clusteredIndexName.get()) != indexNames.end();
                          },
                          [&](const BSONObj& indexKey) -> bool {
                              return clusteredIndexSpec.getKey().woCompare(indexKey) == 0;
                          }},
        index);
}

/**
 * Returns a list of index names that the caller requested to abort/drop. Requires a collection lock
 * to be held to look up the index name from the key pattern.
 */
StatusWith<std::vector<std::string>> getIndexNames(OperationContext* opCtx,
                                                   const CollectionPtr& collection,
                                                   const IndexArgument& index) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_IX));

    return stdx::visit(
        OverloadedVisitor{
            [](const std::string& arg) -> StatusWith<std::vector<std::string>> { return {{arg}}; },
            [](const std::vector<std::string>& arg) -> StatusWith<std::vector<std::string>> {
                return arg;
            },
            [&](const BSONObj& arg) -> StatusWith<std::vector<std::string>> {
                auto swDescriptor =
                    getDescriptorByKeyPattern(opCtx, collection->getIndexCatalog(), arg);
                if (!swDescriptor.isOK()) {
                    return swDescriptor.getStatus();
                }
                return {{swDescriptor.getValue()->indexName()}};
            }},
        index);
}

/**
 * Attempts to abort a single index builder that is responsible for all the index names passed in.
 */
std::vector<UUID> abortIndexBuildByIndexNames(OperationContext* opCtx,
                                              UUID collectionUUID,
                                              std::vector<std::string> indexNames) {

    boost::optional<UUID> buildUUID =
        IndexBuildsCoordinator::get(opCtx)->abortIndexBuildByIndexNames(
            opCtx, collectionUUID, indexNames, std::string("dropIndexes command"));
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
        return indexCatalog->dropUnfinishedIndex(opCtx, collection, desc);
    }

    // Do not allow dropping unfinished indexes that are not frozen.
    if (!entry->isReady(opCtx)) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "can't drop unfinished index with name: " << desc->indexName());
    }

    // Log the operation first, which reserves an optime in the oplog and sets the timestamp for
    // future writes. This guarantees the durable catalog's metadata change to share the same
    // timestamp when dropping the index below.
    opCtx->getServiceContext()->getOpObserver()->onDropIndex(
        opCtx, collection->ns(), collection->uuid(), desc->indexName(), desc->infoObj());

    auto s = indexCatalog->dropIndex(opCtx, collection, desc);
    if (!s.isOK()) {
        return s;
    }

    return Status::OK();
}

/**
 * Aborts all the index builders on the collection if the first element in 'indexesToAbort' is "*",
 * otherwise this attempts to abort a single index builder building the given index names.
 */
std::vector<UUID> abortActiveIndexBuilders(OperationContext* opCtx,
                                           const NamespaceString& collectionNs,
                                           const UUID& collectionUUID,
                                           const std::vector<std::string>& indexNames) {
    if (indexNames.empty()) {
        return {};
    }

    if (indexNames.front() == "*") {
        return IndexBuildsCoordinator::get(opCtx)->abortCollectionIndexBuilds(
            opCtx, collectionNs, collectionUUID, "dropIndexes command");
    }

    return abortIndexBuildByIndexNames(opCtx, collectionUUID, indexNames);
}

void dropReadyIndexes(OperationContext* opCtx,
                      Collection* collection,
                      const std::vector<std::string>& indexNames,
                      DropIndexesReply* reply,
                      bool forceDropShardKeyIndex) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));

    if (indexNames.empty()) {
        return;
    }

    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    auto collDescription =
        CollectionShardingState::get(opCtx, collection->ns())->getCollectionDescription(opCtx);

    if (indexNames.front() == "*") {
        if (collDescription.isSharded() && !forceDropShardKeyIndex) {
            indexCatalog->dropIndexes(
                opCtx,
                collection,
                [&](const IndexDescriptor* desc) {
                    if (desc->isIdIndex()) {
                        return false;
                    }

                    if (isCompatibleWithShardKey(opCtx,
                                                 CollectionPtr(collection),
                                                 desc->getEntry(),
                                                 collDescription.getKeyPattern(),
                                                 false /* requiresSingleKey */)) {
                        return false;
                    }

                    return true;
                },
                [opCtx, collection](const IndexDescriptor* desc) {
                    opCtx->getServiceContext()->getOpObserver()->onDropIndex(opCtx,
                                                                             collection->ns(),
                                                                             collection->uuid(),
                                                                             desc->indexName(),
                                                                             desc->infoObj());
                });

            reply->setMsg("non-_id indexes and non-shard key indexes dropped for collection"_sd);
        } else {
            indexCatalog->dropAllIndexes(
                opCtx, collection, false, [opCtx, collection](const IndexDescriptor* desc) {
                    opCtx->getServiceContext()->getOpObserver()->onDropIndex(opCtx,
                                                                             collection->ns(),
                                                                             collection->uuid(),
                                                                             desc->indexName(),
                                                                             desc->infoObj());
                });

            reply->setMsg("non-_id indexes dropped for collection"_sd);
        }
        return;
    }

    for (const auto& indexName : indexNames) {
        if (collDescription.isSharded()) {
            uassert(
                ErrorCodes::CannotDropShardKeyIndex,
                "Cannot drop the only compatible index for this collection's shard key",
                !isLastShardKeyIndex(
                    opCtx, collection, indexCatalog, indexName, collDescription.getKeyPattern()));
        }

        auto desc = indexCatalog->findIndexByName(opCtx,
                                                  indexName,
                                                  IndexCatalog::InclusionPolicy::kReady |
                                                      IndexCatalog::InclusionPolicy::kUnfinished |
                                                      IndexCatalog::InclusionPolicy::kFrozen);
        if (!desc) {
            uasserted(ErrorCodes::IndexNotFound,
                      str::stream() << "index not found with name [" << indexName << "]");
        }
        uassertStatusOK(dropIndexByDescriptor(opCtx, collection, indexCatalog, desc));
    }
}

void assertMovePrimaryInProgress(OperationContext* opCtx, const NamespaceString& ns) {
    auto dss = DatabaseShardingState::get(opCtx, ns.db());
    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);

    try {
        const auto collDesc =
            CollectionShardingState::get(opCtx, ns)->getCollectionDescription(opCtx);
        if (!collDesc.isSharded()) {
            auto mpsm = dss->getMovePrimarySourceManager(dssLock);

            if (mpsm) {
                LOGV2(4976500, "assertMovePrimaryInProgress", "namespace"_attr = ns.toString());

                uasserted(ErrorCodes::MovePrimaryInProgress,
                          "movePrimary is in progress for namespace " + ns.toString());
            }
        }
    } catch (const DBException& ex) {
        if (ex.toStatus() != ErrorCodes::MovePrimaryInProgress) {
            LOGV2(4976501, "Error when getting collection description", "what"_attr = ex.what());
            return;
        }
        throw;
    }
}

}  // namespace

DropIndexesReply dropIndexes(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& expectedUUID,
                             const IndexArgument& index) {
    // We only need to hold an intent lock to send abort signals to the active index builder(s) we
    // intend to abort.
    boost::optional<AutoGetCollection> collection;
    collection.emplace(opCtx, nss, MODE_IX);

    checkCollectionUUIDMismatch(opCtx, nss, collection->getCollection(), expectedUUID);
    uassertStatusOK(checkView(opCtx, nss, collection->getCollection()));

    const UUID collectionUUID = (*collection)->uuid();
    const NamespaceStringOrUUID dbAndUUID = {nss.db().toString(), collectionUUID};
    uassertStatusOK(checkReplState(opCtx, dbAndUUID, collection->getCollection()));
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(51806,
              "CMD: dropIndexes",
              "namespace"_attr = nss,
              "uuid"_attr = collectionUUID,
              "indexes"_attr =
                  stdx::visit(OverloadedVisitor{[](const std::string& arg) { return arg; },
                                                [](const std::vector<std::string>& arg) {
                                                    return boost::algorithm::join(arg, ",");
                                                },
                                                [](const BSONObj& arg) { return arg.toString(); }},
                              index));
    }

    if ((*collection)->isClustered() &&
        containsClusteredIndex(collection->getCollection(), index)) {
        uasserted(5979800, "It is illegal to drop the clusteredIndex");
    }

    DropIndexesReply reply;
    reply.setNIndexesWas((*collection)->getIndexCatalog()->numIndexesTotal(opCtx));

    const bool isWildcard =
        stdx::holds_alternative<std::string>(index) && stdx::get<std::string>(index) == "*";

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

    // When releasing the collection lock to send the abort signal to the index builders, it's
    // possible for new index builds to start. Keep aborting in-progress index builds if they
    // satisfy the caller's input.
    std::vector<UUID> abortedIndexBuilders;
    std::vector<std::string> indexNames;
    while (true) {
        indexNames = uassertStatusOK(getIndexNames(opCtx, collection->getCollection(), index));

        // Copy the namespace and UUID before dropping locks.
        auto collUUID = (*collection)->uuid();
        auto collNs = (*collection)->ns();

        // Release locks before aborting index builds. The helper will acquire locks on our
        // behalf.
        collection = boost::none;

        // Send the abort signal to any index builders that match the users request. Waits until
        // all aborted builders complete.
        auto justAborted = abortActiveIndexBuilders(opCtx, collNs, collUUID, indexNames);
        abortedIndexBuilders.insert(
            abortedIndexBuilders.end(), justAborted.begin(), justAborted.end());

        if (MONGO_unlikely(hangAfterAbortingIndexes.shouldFail())) {
            LOGV2(4731900, "Hanging on hangAfterAbortingIndexes fail point");
            hangAfterAbortingIndexes.pauseWhileSet();
        }

        // Abandon the snapshot as the index catalog will compare the in-memory state to the
        // disk state, which may have changed when we released the lock temporarily.
        opCtx->recoveryUnit()->abandonSnapshot();

        // Take an exclusive lock on the collection now to be able to perform index catalog
        // writes when removing ready indexes from disk.
        collection.emplace(opCtx, dbAndUUID, MODE_X);

        if (!*collection) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection '" << nss << "' with UUID " << dbAndUUID.uuid()
                                    << " in database " << dbAndUUID.db() << " does not exist.");
        }

        // The collection could have been renamed when we dropped locks.
        collNs = (*collection)->ns();

        uassertStatusOK(checkReplState(opCtx, dbAndUUID, collection->getCollection()));

        // Check to see if a new index build was started that the caller requested to be
        // aborted.
        bool abortAgain = false;
        if (isWildcard) {
            abortAgain = indexBuildsCoord->inProgForCollection(collectionUUID);
        } else {
            abortAgain = indexBuildsCoord->hasIndexBuilder(opCtx, collectionUUID, indexNames);
        }

        if (!abortAgain) {
            assertMovePrimaryInProgress(opCtx, collNs);
            CollectionShardingState::get(opCtx, collNs)
                ->getCollectionDescription(opCtx)
                .throwIfReshardingInProgress(collNs);
            break;
        }
    }

    // Drop any ready indexes that were created while we yielded our locks while aborting using
    // similar index specs.
    if (!isWildcard && !abortedIndexBuilders.empty()) {
        // The index catalog requires that no active index builders are running when dropping ready
        // indexes.
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(collectionUUID);
        writeConflictRetry(opCtx, "dropIndexes", dbAndUUID.toString(), [&] {
            WriteUnitOfWork wuow(opCtx);

            // This is necessary to check shard version.
            OldClientContext ctx(opCtx, (*collection)->ns());

            // Iterate through all the aborted indexes and drop any indexes that are ready in
            // the index catalog. This would indicate that while we yielded our locks during the
            // abort phase, a new identical index was created.
            auto indexCatalog = collection->getWritableCollection(opCtx)->getIndexCatalog();
            for (const auto& indexName : indexNames) {
                auto collDescription =
                    CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx);

                if (collDescription.isSharded()) {
                    uassert(ErrorCodes::CannotDropShardKeyIndex,
                            "Cannot drop the only compatible index for this collection's shard key",
                            !isLastShardKeyIndex(opCtx,
                                                 collection->getCollection(),
                                                 indexCatalog,
                                                 indexName,
                                                 collDescription.getKeyPattern()));
                }

                auto desc =
                    indexCatalog->findIndexByName(opCtx,
                                                  indexName,
                                                  IndexCatalog::InclusionPolicy::kReady |
                                                      IndexCatalog::InclusionPolicy::kUnfinished |
                                                      IndexCatalog::InclusionPolicy::kFrozen);
                if (!desc) {
                    // A similar index wasn't created while we yielded the locks during abort.
                    continue;
                }

                uassertStatusOK(dropIndexByDescriptor(
                    opCtx, collection->getWritableCollection(opCtx), indexCatalog, desc));
            }

            wuow.commit();
        });

        return reply;
    }

    if (!abortedIndexBuilders.empty()) {
        // All the index builders were sent the abort signal, remove all the remaining indexes
        // in the index catalog.
        invariant(isWildcard);
        invariant(indexNames.size() == 1);
        invariant(indexNames.front() == "*");
        invariant((*collection)->getIndexCatalog()->numIndexesInProgress(opCtx) == 0);
    }

    writeConflictRetry(
        opCtx, "dropIndexes", dbAndUUID.toString(), [opCtx, &collection, &indexNames, &reply] {
            WriteUnitOfWork wunit(opCtx);

            // This is necessary to check shard version.
            OldClientContext ctx(opCtx, (*collection)->ns());
            dropReadyIndexes(
                opCtx, collection->getWritableCollection(opCtx), indexNames, &reply, false);
            wunit.commit();
        });

    return reply;
}

Status dropIndexesForApplyOps(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& cmdObj) try {
    BSONObjBuilder bob(cmdObj);
    bob.append("$db", nss.db());
    auto cmdObjWithDb = bob.obj();
    auto parsed = DropIndexes::parse({"dropIndexes"}, cmdObjWithDb);

    return writeConflictRetry(opCtx, "dropIndexes", nss.db(), [opCtx, &nss, &cmdObj, &parsed] {
        AutoGetCollection collection(opCtx, nss, MODE_X);

        // If db/collection does not exist, short circuit and return.
        Status status = checkView(opCtx, nss, collection.getCollection());
        if (!status.isOK()) {
            return status;
        }

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20344,
                  "CMD: dropIndexes",
                  "namespace"_attr = nss,
                  "indexes"_attr = cmdObj[kIndexFieldName].toString(false));
        }

        auto swIndexNames = getIndexNames(opCtx, collection.getCollection(), parsed.getIndex());
        if (!swIndexNames.isOK()) {
            return swIndexNames.getStatus();
        }

        WriteUnitOfWork wunit(opCtx);

        // This is necessary to check shard version.
        OldClientContext ctx(opCtx, nss);

        DropIndexesReply ignoredReply;
        dropReadyIndexes(opCtx,
                         collection.getWritableCollection(opCtx),
                         swIndexNames.getValue(),
                         &ignoredReply,
                         true);

        wunit.commit();
        return Status::OK();
    });
} catch (const DBException& exc) {
    return exc.toStatus();
}

}  // namespace mongo
