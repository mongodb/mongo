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

#include "mongo/db/local_catalog/drop_indexes.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterAbortingIndexes);

// Field name in dropIndexes command for indexes to drop.
constexpr auto kIndexFieldName = "index"_sd;

Status checkCollExists(const NamespaceString& nss, const CollectionAcquisition& collAcq) {
    if (!collAcq.exists()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "ns not found " << nss.toStringForErrorMsg());
    }
    return Status::OK();
}

Status checkReplState(OperationContext* opCtx, const CollectionPtr& collPtr) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto canAcceptWrites = replCoord->canAcceptWritesFor(opCtx, collPtr->ns());
    bool writesAreReplicatedAndNotPrimary = opCtx->writesAreReplicated() && !canAcceptWrites;

    if (writesAreReplicatedAndNotPrimary) {
        return Status(
            ErrorCodes::NotWritablePrimary,
            fmt::format("Not primary while dropping indexes for collection '{}' with UUID {}",
                        collPtr->ns().toStringForErrorMsg(),
                        collPtr->uuid().toString()));
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
    indexCatalog->findIndexesByKeyPattern(
        opCtx, keyPattern, IndexCatalog::InclusionPolicy::kAll, &indexes);
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
    return visit(OverloadedVisitor{[&](const std::string& indexName) -> bool {
                                       // While the clusteredIndex's name is optional during user
                                       // creation, it should always be filled in by default on the
                                       // collection object.
                                       auto clusteredIndexName = clusteredIndexSpec.getName();
                                       invariant(clusteredIndexName.has_value());

                                       return clusteredIndexName.value() == indexName;
                                   },
                                   [&](const std::vector<std::string>& indexNames) -> bool {
                                       // While the clusteredIndex's name is optional during user
                                       // creation, it should always be filled in by default on the
                                       // collection object.
                                       auto clusteredIndexName = clusteredIndexSpec.getName();
                                       invariant(clusteredIndexName.has_value());

                                       return std::find(indexNames.begin(),
                                                        indexNames.end(),
                                                        clusteredIndexName.value()) !=
                                           indexNames.end();
                                   },
                                   [&](const BSONObj& indexKey) -> bool {
                                       return clusteredIndexSpec.getKey().woCompare(indexKey) == 0;
                                   }},
                 index);
}

/**
 * Returns a list of index names that the caller requested to abort/drop. Requires a collection lock
 * to be held to look up the index name from the key pattern. Returns an empty vector if an index
 * key pattern is not found (instead of IndexNotFound error).
 */
StatusWith<std::vector<std::string>> getIndexNames(OperationContext* opCtx,
                                                   const CollectionPtr& collection,
                                                   const IndexArgument& index) {
    invariant(
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(collection->ns(), MODE_IX));

    return visit(
        OverloadedVisitor{
            [](const std::string& arg) -> StatusWith<std::vector<std::string>> { return {{arg}}; },
            [](const std::vector<std::string>& arg) -> StatusWith<std::vector<std::string>> {
                return arg;
            },
            [&](const BSONObj& arg) -> StatusWith<std::vector<std::string>> {
                auto swDescriptor =
                    getDescriptorByKeyPattern(opCtx, collection->getIndexCatalog(), arg);
                if (swDescriptor.getStatus() == ErrorCodes::IndexNotFound) {
                    return std::vector<std::string>{};
                }
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
                             IndexCatalogEntry* entry) {
    if (entry->descriptor()->isIdIndex()) {
        return Status(ErrorCodes::InvalidOptions, "cannot drop _id index");
    }

    // Support dropping unfinished indexes, but only if the index is 'frozen'. These indexes only
    // exist in standalone mode.
    if (entry->isFrozen()) {
        invariant(!entry->isReady());
        invariant(getReplSetMemberInStandaloneMode(opCtx->getServiceContext()));
        // Return here. No need to fall through to op observer on standalone.
        return indexCatalog->dropUnfinishedIndex(opCtx, collection, entry);
    }

    // Do not allow dropping unfinished indexes that are not frozen.
    if (!entry->isReady()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "can't drop unfinished index with name: "
                                    << entry->descriptor()->indexName());
    }

    // Log the operation first, which reserves an optime in the oplog and sets the timestamp for
    // future writes. This guarantees the durable catalog's metadata change to share the same
    // timestamp when dropping the index below.
    opCtx->getServiceContext()->getOpObserver()->onDropIndex(opCtx,
                                                             collection->ns(),
                                                             collection->uuid(),
                                                             entry->descriptor()->indexName(),
                                                             entry->descriptor()->infoObj());

    auto s = indexCatalog->dropIndexEntry(opCtx, collection, entry);
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
    invariant(
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(collection->ns(), MODE_X));

    if (indexNames.empty()) {
        return;
    }

    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    auto collDescription =
        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, collection->ns())
            ->getCollectionDescription(opCtx);

    if (indexNames.front() == "*") {
        if (collDescription.isSharded() && !forceDropShardKeyIndex) {
            indexCatalog->dropIndexes(
                opCtx,
                collection,
                [&](const IndexDescriptor* desc) {
                    if (desc->isIdIndex()) {
                        return false;
                    }
                    // Allow users to drop the hashed index for any index that is compatible with
                    // the shard key. Note skipDroppingHashedShardKeyIndex is used in some tests to
                    // prevent dropIndexes from dropping the hashed shard key index so we can
                    // continue to test chunk migration with hashed sharding. Otherwise, dropIndexes
                    // with '*' would drop the index and prevent chunk migration from running.
                    const auto& shardKey = collDescription.getShardKeyPattern();
                    const bool skipDropIndex =
                        skipDroppingHashedShardKeyIndex || !shardKey.isHashedPattern();
                    if (isCompatibleWithShardKey(opCtx,
                                                 CollectionPtr(collection),
                                                 desc->getEntry(),
                                                 shardKey.toBSON(),
                                                 false /* requiresSingleKey */) &&
                        skipDropIndex) {
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
                !isLastNonHiddenRangedShardKeyIndex(
                    opCtx, CollectionPtr(collection), indexName, collDescription.getKeyPattern()));
        }

        auto writableEntry = indexCatalog->getWritableEntryByName(
            opCtx, indexName, IndexCatalog::InclusionPolicy::kAll);
        if (!writableEntry) {
            LOGV2_WARNING(9015201,
                          "Index not found during dropIndexes, skipping",
                          "indexName"_attr = indexName,
                          "namespace"_attr = collection->ns());
            continue;
        }
        uassertStatusOK(dropIndexByDescriptor(opCtx, collection, indexCatalog, writableEntry));
    }
}

void assertNoMovePrimaryInProgress(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        bool isMovePrimaryInProgress =
            DatabaseShardingState::assertDbLockedAndAcquire(opCtx, nss.dbName())
                ->isMovePrimaryInProgress();
        auto scopedCss = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss);

        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        collDesc.throwIfReshardingInProgress(nss);

        // Only collections that are not registered in the sharding catalog are affected by
        // movePrimary
        if (!collDesc.hasRoutingTable()) {
            if (isMovePrimaryInProgress) {
                LOGV2(4976500, "assertNoMovePrimaryInProgress", logAttrs(nss));

                uasserted(ErrorCodes::MovePrimaryInProgress,
                          "movePrimary is in progress for namespace " + nss.toStringForErrorMsg());
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
/**
 * Translate the index spec according to the underelying collection metadata.
 *
 * TODO SERVER-102344 remove the forceRawDataMode once 9.0 becomes last LTS
 */
BSONObj translateIndexSpec(OperationContext* opCtx,
                           const CollectionAcquisition& collAcq,
                           const BSONObj& origIndexSpec,
                           const bool forceRawDataMode) {
    // If the request namespace refers to a time-series collection translate the index spec for
    if (!forceRawDataMode && !isRawDataOperation(opCtx) &&
        collAcq.getCollectionPtr()->isTimeseriesCollection()) {

        auto swBucketsIndexSpec = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
            collAcq.getCollectionPtr()->getTimeseriesOptions().get(), origIndexSpec);

        uassert(ErrorCodes::IndexNotFound,
                fmt::format("{}. Failed to translate index spec for timeseries collection '{}'",
                            swBucketsIndexSpec.getStatus().toString(),
                            collAcq.nss().toStringForErrorMsg()),
                swBucketsIndexSpec.isOK());

        return std::move(swBucketsIndexSpec.getValue());
    }
    return origIndexSpec;
}

}  // namespace

/**
 * Precondition checks for dropIndexes operation
 */
Status validateDropIndexes(OperationContext* opCtx,
                           const NamespaceString& origNss,
                           const boost::optional<UUID>& expectedUUID,
                           const boost::optional<BSONObj>& shardKeyPattern,
                           const CollectionAcquisition& collAcq,
                           const IndexArgument& index,
                           const std::vector<std::string>& indexNames) {

    uassertStatusOK(checkReplState(opCtx, collAcq.getCollectionPtr()));

    if (collAcq.getCollectionPtr()->isClustered() &&
        containsClusteredIndex(collAcq.getCollectionPtr(), index)) {
        uasserted(5979800, "It is illegal to drop the clusteredIndex");
    }

    const bool isWildcard = holds_alternative<std::string>(index) && get<std::string>(index) == "*";

    if (isWildcard) {
        return Status::OK();
    }

    const IndexCatalog* indexCatalog = collAcq.getCollectionPtr()->getIndexCatalog();
    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto hasActiveIndexBuild =
        indexBuildsCoord->hasIndexBuilder(opCtx, collAcq.uuid(), {indexNames});

    for (const auto& indexName : indexNames) {
        auto indexDescriptor =
            indexCatalog->findIndexByName(opCtx, indexName, IndexCatalog::InclusionPolicy::kAll);

        if (!indexDescriptor) {
            continue;  // Skip non-existent indexes
        }

        auto entry = indexCatalog->getEntry(indexDescriptor);
        if (entry->descriptor()->isIdIndex()) {
            return Status(ErrorCodes::InvalidOptions, "cannot drop _id index");
        }

        if (!entry->isReady() && !entry->isFrozen() && !hasActiveIndexBuild) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream() << "can't drop unfinished index with name: "
                                        << entry->descriptor()->indexName());
        }

        if (shardKeyPattern) {
            if (isLastNonHiddenRangedShardKeyIndex(
                    opCtx, collAcq.getCollectionPtr(), indexName, *shardKeyPattern)) {
                return Status(ErrorCodes::CannotDropShardKeyIndex,
                              "Cannot drop the only compatible index for this collection's "
                              "shard key");
            }
        }
    }

    return Status::OK();
}

DropIndexesReply dropIndexes(OperationContext* opCtx,
                             const NamespaceString& origNss,
                             const boost::optional<UUID>& expectedUUID,
                             const IndexArgument& origIndexArgument,
                             const bool forceRawDataMode) {
    // We only need to hold an intent lock to send abort signals to the active index builder(s)
    // we intend to abort.
    auto collAcq = boost::make_optional<CollectionAcquisition>(
        timeseries::acquireCollectionWithBucketsLookup(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, origNss, AcquisitionPrerequisites::OperationType::kRead, expectedUUID),
            LockMode::MODE_IX)
            .first);

    uassertStatusOK(checkCollExists(origNss, *collAcq));

    const auto index = [&]() -> IndexArgument {
        if (auto origIndexSpec = std::get_if<BSONObj>(&origIndexArgument)) {
            return translateIndexSpec(opCtx, *collAcq, *origIndexSpec, forceRawDataMode);
        }
        return origIndexArgument;
    }();

    auto indexNames = uassertStatusOK(getIndexNames(opCtx, collAcq->getCollectionPtr(), index));

    auto shardKeyPattern = std::invoke([&]() -> boost::optional<BSONObj> {
        const auto& collDesc = collAcq->getShardingDescription();
        if (collDesc.isSharded()) {
            return collDesc.getKeyPattern();
        }
        return boost::none;
    });

    uassertStatusOK(validateDropIndexes(
        opCtx, origNss, expectedUUID, shardKeyPattern, *collAcq, index, indexNames));

    const UUID collectionUUID = collAcq->uuid();
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(51806,
              "CMD: dropIndexes",
              logAttrs(collAcq->nss()),
              "uuid"_attr = collectionUUID,
              "indexes"_attr = visit(OverloadedVisitor{[](const std::string& arg) { return arg; },
                                                       [](const std::vector<std::string>& arg) {
                                                           return boost::algorithm::join(arg, ",");
                                                       },
                                                       [](const BSONObj& arg) {
                                                           return arg.toString();
                                                       }},
                                     index));
    }

    DropIndexesReply reply;
    reply.setNIndexesWas(collAcq->getCollectionPtr()->getIndexCatalog()->numIndexesTotal());

    const bool isWildcard = holds_alternative<std::string>(index) && get<std::string>(index) == "*";

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

    // When releasing the collection lock to send the abort signal to the index builders, it's
    // possible for new index builds to start. Keep aborting in-progress index builds if they
    // satisfy the caller's input.
    NamespaceString collNs = collAcq->nss();
    // Release locks before aborting index builds. The helper will acquire locks on our behalf.
    collAcq.reset();

    std::vector<UUID> abortedIndexBuilders;
    boost::optional<AutoGetCollection> collection;
    while (true) {
        // Send the abort signal to any index builders that match the users request. Waits until
        // all aborted builders complete.
        auto justAborted = abortActiveIndexBuilders(opCtx, collNs, collectionUUID, indexNames);

        abortedIndexBuilders.insert(
            abortedIndexBuilders.end(), justAborted.begin(), justAborted.end());

        if (MONGO_unlikely(hangAfterAbortingIndexes.shouldFail())) {
            LOGV2(4731900, "Hanging on hangAfterAbortingIndexes fail point");
            hangAfterAbortingIndexes.pauseWhileSet();
        }

        // Abandon the snapshot as the index catalog will compare the in-memory state to the
        // disk state, which may have changed when we released the lock temporarily.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

        // Take an exclusive lock on the collection now to be able to perform index catalog
        // writes when removing ready indexes from disk.
        //
        // This time we acquire by UUID because we want to be sure to re-acquire the same
        // collection even if it has been concurrently renamed while the lock was released.
        //
        // We don't use acquisition API here because they do not allow to acquire by UUID when the
        // request has been sent with a ShardVersion.
        collection.emplace(opCtx, NamespaceStringOrUUID(collNs.dbName(), collectionUUID), MODE_X);

        // If the UUID of the collection changed we fail with NamespcaeNotFound.
        // The UUID of the collection can change because we released and re-acquired the
        // collection locks.
        uassert(ErrorCodes::NamespaceNotFound,
                fmt::format("UUID for collection '{}' changed during execution of drop index "
                            "operation. Original UUID {}.",
                            collNs.toStringForErrorMsg(),
                            collectionUUID.toString()),
                *collection);

        const auto& collPtr = collection->getCollection();
        uassertStatusOK(checkReplState(opCtx, collPtr));

        // Check to see if a new index build was started that the caller requested to be aborted.
        bool abortAgain = false;
        if (isWildcard) {
            abortAgain = indexBuildsCoord->inProgForCollection(collectionUUID);
        } else {
            abortAgain = indexBuildsCoord->hasIndexBuilder(opCtx, collectionUUID, indexNames);
        }

        if (!abortAgain) {
            assertNoMovePrimaryInProgress(opCtx, collPtr->ns());
            break;
        }

        // Before releasing the lock again refresh the variables needed by this loop.
        indexNames = uassertStatusOK(getIndexNames(opCtx, collPtr, index));
        // The collection could have been renamed when we dropped locks.
        collNs = collPtr->ns();
        // Release the lock and loop again.
        collection.reset();
    }

    // Drop any ready indexes that were created while we yielded our locks while aborting using
    // similar index specs.
    if (!isWildcard && !abortedIndexBuilders.empty()) {
        // The index catalog requires that no active index builders are running when dropping ready
        // indexes.
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(collectionUUID);
        writeConflictRetry(opCtx, "dropIndexes", (*collection)->ns(), [&] {
            WriteUnitOfWork wuow(opCtx);
            AutoStatsTracker statsTracker(
                opCtx,
                (*collection)->ns(),
                Top::LockType::WriteLocked,
                AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileLevel((*collection)->ns().dbName()));

            // Iterate through all the aborted indexes and drop any indexes that are ready in
            // the index catalog. This would indicate that while we yielded our locks during the
            // abort phase, a new identical index was created.
            CollectionWriter collWriter{opCtx, *collection};
            auto writableColl = collWriter.getWritableCollection(opCtx);
            auto indexCatalog = writableColl->getIndexCatalog();
            for (const auto& indexName : indexNames) {
                auto collDesc = CollectionShardingState::assertCollectionLockedAndAcquire(
                                    opCtx, collWriter->ns())
                                    ->getCollectionDescription(opCtx);
                if (collDesc.isSharded()) {
                    uassert(ErrorCodes::CannotDropShardKeyIndex,
                            "Cannot drop the only compatible index for this collection's shard key",
                            !isLastNonHiddenRangedShardKeyIndex(
                                opCtx, collWriter.get(), indexName, collDesc.getKeyPattern()));
                }

                auto writableEntry = indexCatalog->getWritableEntryByName(
                    opCtx, indexName, IndexCatalog::InclusionPolicy::kAll);
                if (!writableEntry) {
                    // A similar index wasn't created while we yielded the locks during abort.
                    continue;
                }

                uassertStatusOK(
                    dropIndexByDescriptor(opCtx, writableColl, indexCatalog, writableEntry));
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
        invariant((*collection)->getIndexCatalog()->numIndexesInProgress() == 0);
    }

    writeConflictRetry(
        opCtx, "dropIndexes", (*collection)->ns(), [opCtx, &collection, &indexNames, &reply] {
            WriteUnitOfWork wunit(opCtx);
            AutoStatsTracker statsTracker(
                opCtx,
                (*collection)->ns(),
                Top::LockType::WriteLocked,
                AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileLevel((*collection)->ns().dbName()));

            CollectionWriter writer{opCtx, *collection};
            dropReadyIndexes(opCtx, writer.getWritableCollection(opCtx), indexNames, &reply, false);
            wunit.commit();
        });

    return reply;
}

DropIndexesReply dropIndexesDryRun(OperationContext* opCtx,
                                   const NamespaceString& origNss,
                                   const boost::optional<UUID>& expectedUUID,
                                   const IndexArgument& origIndexArgument,
                                   const boost::optional<BSONObj>& shardKeyPattern,
                                   const bool forceRawDataMode) {
    // We only need to hold an intent lock to send abort signals to the active index builder(s)
    // we intend to abort.
    auto collAcq =
        timeseries::acquireCollectionWithBucketsLookup(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, origNss, AcquisitionPrerequisites::OperationType::kRead, expectedUUID),
            LockMode::MODE_IX)
            .first;


    uassertStatusOK(checkCollExists(origNss, collAcq));

    const auto index = [&]() -> IndexArgument {
        if (auto origIndexSpec = std::get_if<BSONObj>(&origIndexArgument)) {
            return translateIndexSpec(opCtx, collAcq, *origIndexSpec, forceRawDataMode);
        }
        return origIndexArgument;
    }();

    auto indexNames = uassertStatusOK(getIndexNames(opCtx, collAcq.getCollectionPtr(), index));

    uassertStatusOK(validateDropIndexes(
        opCtx, origNss, expectedUUID, shardKeyPattern, collAcq, index, indexNames));

    DropIndexesReply reply;
    reply.setNIndexesWas(collAcq.getCollectionPtr()->getIndexCatalog()->numIndexesTotal());

    const bool isWildcard = holds_alternative<std::string>(index) && get<std::string>(index) == "*";

    if (isWildcard) {
        if (shardKeyPattern) {
            reply.setMsg(
                "non-_id indexes and non-shard key indexes would be dropped for collection"_sd);
        } else {
            reply.setMsg("non-_id indexes would be dropped for collection"_sd);
        }
    } else {
        if (indexNames.size() == 1) {
            reply.setMsg("index '" + indexNames[0] + "' would be dropped");
        } else {
            reply.setMsg(std::to_string(indexNames.size()) + " indexes would be dropped");
        }
    }

    return reply;
}

Status dropIndexesForApplyOps(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& cmdObj) try {
    BSONObjBuilder bob(cmdObj);
    bob.append("$db", nss.dbName().serializeWithoutTenantPrefix_UNSAFE());
    auto cmdObjWithDb = bob.obj();
    auto parsed = DropIndexes::parse(cmdObjWithDb,
                                     IDLParserContext{"dropIndexes",
                                                      auth::ValidatedTenancyScope::get(opCtx),
                                                      nss.tenantId(),
                                                      SerializationContext::stateStorageRequest()});

    return writeConflictRetry(opCtx, "dropIndexes", nss, [opCtx, &nss, &cmdObj, &parsed] {
        auto collAcq =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, nss, AcquisitionPrerequisites::OperationType::kWrite),
                              LockMode::MODE_X);

        // If db/collection does not exist, short circuit and return.
        Status status = checkCollExists(nss, collAcq);
        if (!status.isOK()) {
            return status;
        }

        if (!serverGlobalParams.quiet.load()) {
            LOGV2(20344,
                  "CMD: dropIndexes",
                  logAttrs(nss),
                  "indexes"_attr = cmdObj[kIndexFieldName].toString(false));
        }

        auto swIndexNames = getIndexNames(opCtx, collAcq.getCollectionPtr(), parsed.getIndex());
        if (!swIndexNames.isOK()) {
            return swIndexNames.getStatus();
        }

        WriteUnitOfWork wunit(opCtx);
        AutoStatsTracker statsTracker(opCtx,
                                      collAcq.nss(),
                                      Top::LockType::WriteLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(collAcq.nss().dbName()));
        CollectionWriter writer{opCtx, &collAcq};

        DropIndexesReply ignoredReply;
        dropReadyIndexes(opCtx,
                         writer.getWritableCollection(opCtx),
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
