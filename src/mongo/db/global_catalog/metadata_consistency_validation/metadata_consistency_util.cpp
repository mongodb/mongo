/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/scoped_read_concern.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace metadata_consistency_util {

namespace {

MONGO_FAIL_POINT_DEFINE(insertFakeInconsistencies);
MONGO_FAIL_POINT_DEFINE(simulateCatalogTopLevelMetadataInconsistency);

/*
 * This helper throws an error for the namespace which has disappeared. The error will be a tassert
 * if it is an unexpected scenario for the collection to disappear so that our testing
 * infrastructure will catch these cases. It is a uassert with ConflictingOperationInProgress if the
 * scenario is acceptable for this to happen.
 *
 * The two accepted scenarios are:
 * 1. The collection is `config.system.sessions` - this is acceptable since this collection is only
 * droppable via direct shard connection.
 * 2. This node is a config server - this can happen since transitionToDedicated drops collections
 * in the background.
 */
void throwCollectionDisappearedError(OperationContext* opCtx, const NamespaceString& nss) {
    tassert(9690601,
            str::stream() << "Collection unexpectedly disappeared while holding database DDL lock: "
                          << nss.toStringForErrorMsg(),
            nss == NamespaceString::kLogicalSessionsNamespace ||
                ShardingState::get(opCtx)->shardId() == ShardId::kConfigServerId);

    uasserted(ErrorCodes::ConflictingOperationInProgress,
              str::stream() << "Collection " << nss.toStringForErrorMsg()
                            << " was dropped during CheckMetadataConsistency.");
}

/*
 * Returns the number of documents in the local collection.
 *
 * TODO SERVER-24266: get rid of the `getNumDocs` function and simply rely on `numRecords`.
 */
long long getNumDocs(OperationContext* opCtx, const Collection* localColl) {
    // Since users are advised to delete empty misplaced collections, rely on isEmpty
    // that is safe because the implementation guards against SERVER-24266.
    AutoGetCollection ac(opCtx, localColl->ns(), MODE_IS);
    if (!ac) {
        throwCollectionDisappearedError(opCtx, localColl->ns());
    }
    if (ac->isEmpty(opCtx)) {
        return 0;
    }
    DBDirectClient client(opCtx);
    return client.count(localColl->ns());
}

/*
 * Emit a warning log containing information about the given inconsistency
 */
void logMetadataInconsistency(const MetadataInconsistencyItem& inconsistencyItem) {
    // Please do not change the error code of this log message if not strictly necessary.
    // Automated log ingestion system relies on this specific log message to monitor cluster.
    // inconsistencies
    LOGV2_WARNING(7514800,
                  "Detected sharding metadata inconsistency",
                  "inconsistency"_attr = inconsistencyItem);
}

// TODO SERVER-108424: get rid of this check once only viewless timeseries are supported
void _checkBucketCollectionInconsistencies(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionPtr& localColl,
    const bool checkView,
    std::vector<MetadataInconsistencyItem>& inconsistencies) {

    if (!nss.isTimeseriesBucketsCollection()) {
        return;
    }

    const std::string errMsgPrefix = str::stream()
        << nss.toStringForErrorMsg() << " is a bucket collection but is missing";

    // A bucket collection must always have timeseries options
    const bool hasTimeseriesOptions = localColl->isTimeseriesCollection();
    if (!hasTimeseriesOptions) {
        const std::string errMsg = str::stream() << errMsgPrefix << " the timeseries options";
        const BSONObj options = localColl->getCollectionOptions().toBSON();
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMalformedTimeseriesBucketsCollection,
                              MalformedTimeseriesBucketsCollectionDetails{
                                  nss, std::move(errMsg), std::move(options)}));
        return;
    }

    if (!checkView) {
        return;
    }

    // A bucket collection on the primary shard must always be backed by a view in the proper
    // format. Check if there is a valid view, otherwise return current view/collection options (if
    // present).
    const auto [hasValidView, invalidOptions] = [&] {
        AutoGetCollection ac(
            opCtx,
            nss.getTimeseriesViewNamespace(),
            MODE_IS,
            AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));

        if (auto view = ac.getView()) {
            if (view->viewOn() == nss && view->pipeline().size() == 1) {
                const auto expectedViewPipeline = timeseries::generateViewPipeline(
                    *localColl->getTimeseriesOptions(), false /* asArray */);
                const auto expectedInternalUnpackStage =
                    expectedViewPipeline
                        .getField(DocumentSourceInternalUnpackBucket::kStageNameInternal)
                        .Obj();
                const auto actualPipeline = view->pipeline().front();
                if (actualPipeline.hasField(
                        DocumentSourceInternalUnpackBucket::kStageNameInternal)) {
                    const auto actualInternalUnpackStage =
                        actualPipeline
                            .getField(DocumentSourceInternalUnpackBucket::kStageNameInternal)
                            .Obj()
                            // Ignore `exclude` field introduced in v5.0 and removed in v5.1
                            .removeField(DocumentSourceInternalUnpackBucket::kExclude);
                    if (actualInternalUnpackStage.woCompare(expectedInternalUnpackStage) == 0) {
                        // The view is in the expected format
                        return std::make_pair(true, BSONObj());
                    }
                }

                // The view is not in the expected format, return the current options for debugging
                BSONArrayBuilder pipelineArray;
                const auto& pipeline = view->pipeline();
                for (const auto& stage : pipeline) {
                    pipelineArray.append(stage);
                }

                const BSONObj currentViewOptions =
                    BSON("viewOn" << toStringForLogging(view->viewOn()) << "pipeline"
                                  << pipelineArray.arr());

                return std::make_pair(false, currentViewOptions);
            }
        } else if (ac.getCollection()) {
            // A collection is present rather than a view, return the current options for debugging
            return std::make_pair(false, ac->getCollectionOptions().toBSON());
        }

        return std::make_pair(false, BSONObj());
    }();

    if (!hasValidView) {
        const std::string errMsg = str::stream() << errMsgPrefix << " a valid view backing it";
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMalformedTimeseriesBucketsCollection,
                              MalformedTimeseriesBucketsCollectionDetails{
                                  nss, std::move(errMsg), std::move(invalidOptions)}));
    }
}

void _checkCollectionFilteringInformation(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const ShardId& shardId,
                                          const CollectionPtr& localColl,
                                          bool expectTracked,
                                          std::vector<MetadataInconsistencyItem>& inconsistencies) {
    constexpr StringData kShardsFieldName = "shards"_sd;
    constexpr StringData kMetadataFieldName = "metadata"_sd;
    constexpr StringData kTrackedFieldName = "tracked"_sd;
    const auto configShardId = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();

    const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
    auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();

    // Metadata can always be unknown, so this is not an inconsistency.
    if (!optCollDescr) {
        return;
    }

    // If the critical section is taken, then we might have incorrect local metadata and that is
    // okay as it should be updated before leaving the critical section.
    auto criticalSectionSignal =
        scopedCsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite);
    if (criticalSectionSignal) {
        LOGV2_DEBUG(9302301,
                    1,
                    "Skipping checking collection  since the critical section is acquired",
                    logAttrs(nss));
        return;
    }

    if ((expectTracked && !optCollDescr->hasRoutingTable()) ||
        (!expectTracked && optCollDescr->hasRoutingTable())) {
        // This shard's filtering information regarding whether or not the collection is tracked is
        // inconsistent with that of the config server.
        // The collection is tracked by the config server in the global catalog. This shard has
        // the collection locally but it is missing the filtering information.
        inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
            MetadataInconsistencyTypeEnum::kShardCatalogCacheCollectionMetadataMismatch,
            ShardCatalogCacheCollectionMetadataMismatchDetails{
                nss,
                localColl->uuid(),
                {BSON(kMetadataFieldName
                      << BSON(kTrackedFieldName << optCollDescr->hasRoutingTable())
                      << kShardsFieldName << BSON_ARRAY(shardId)),
                 BSON(kMetadataFieldName << BSON(kTrackedFieldName << expectTracked)
                                         << kShardsFieldName << BSON_ARRAY(configShardId))}}));
    }
}

void _checkShardKeyIndexInconsistencies(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const BSONObj& shardKey,
                                        const CollectionPtr& localColl,
                                        std::vector<MetadataInconsistencyItem>& inconsistencies,
                                        const bool checkRangeDeletionIndexes) {
    const auto performChecks = [&](const CollectionPtr& localColl,
                                   std::vector<MetadataInconsistencyItem>& inconsistencies) {
        // We allow users to drop hashed shard key indexes, and therefore we don't require hashed
        // shard keys to have a supporting index.
        if (!ShardKeyPattern(shardKey).isHashedPattern()) {
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingShardKeyIndex,
                MissingShardKeyIndexDetails{localColl->ns(), shardId, shardKey}));
        }
    };

    // Check that the collection has an index that supports the shard key. The
    // checkMetadataConsistency function is executed under the database DDL lock, ensuring any
    // create collection operations, which run under the collection DDL lock, are serialized with
    // this check. Manual creation of a supportive shard key index (the operation does not run under
    // the DDL lock) immediately after the check below is not considered. As a result, this scenario
    // will lead to reporting an inconsistency.
    if (findShardKeyPrefixedIndex(opCtx, localColl, shardKey, false /*requireSingleKey*/)) {
        return;
    }

    // If the checkRangeDeletionIndexes flag is set, perform an additional check to detect
    // inconsistencies in cases where a collection has an outstanding range deletion without
    // a supporting shard key index.
    if (checkRangeDeletionIndexes) {
        bool hasRangeDeletionTasks = rangedeletionutil::hasAtLeastOneRangeDeletionTaskForCollection(
            opCtx, localColl->ns(), localColl->uuid());
        if (hasRangeDeletionTasks) {
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kRangeDeletionMissingShardKeyIndex,
                RangeDeletionMissingShardKeyIndexDetails{localColl->ns(), shardId, shardKey}));
        }
    }

    std::vector<MetadataInconsistencyItem> tmpInconsistencies;

    // Shards that do not own any chunks do not participate in the creation of new indexes, so they
    // could potentially miss any indexes created after they no longer own chunks. Thus we first
    // perform a check optimistically without taking collection lock, if missing indexes are found
    // we check under the collection lock if this shard currently own any chunk and re-execute again
    // the checks under the lock to ensure stability of the ShardVersion.
    performChecks(localColl, tmpInconsistencies);

    if (!tmpInconsistencies.size()) {
        // No index inconsistencies found
        return;
    }

    // Pessimistic check under collection lock to serialize with chunk migration commit.
    AutoGetCollection ac(opCtx, nss, MODE_IS);
    if (!ac) {
        throwCollectionDisappearedError(opCtx, nss);
    }

    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
    auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
    if (!optCollDescr) {
        LOGV2_DEBUG(7531701,
                    1,
                    "Ignoring index inconsistencies because collection metadata is unknown",
                    logAttrs(nss),
                    "inconsistencies"_attr = tmpInconsistencies);
        return;
    }

    if (!optCollDescr->hasRoutingTable()) {
        LOGV2_DEBUG(9302300,
                    1,
                    "Ignoring index inconsistencies because collection metadata is incorrect",
                    logAttrs(nss),
                    logAttrs(nss),
                    "inconsistencies"_attr = tmpInconsistencies);
        return;
    }

    if (!optCollDescr->currentShardHasAnyChunks()) {
        LOGV2_DEBUG(7531703,
                    1,
                    "Ignoring index inconsistencies because shard does not own any chunk for "
                    "this collection",
                    logAttrs(nss),
                    "inconsistencies"_attr = tmpInconsistencies);
        return;
    }

    tmpInconsistencies.clear();
    performChecks(*ac, inconsistencies);
}

std::vector<MetadataInconsistencyItem> _checkInconsistenciesBetweenBothCatalogs(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const CollectionType& catalogColl,
    const CollectionPtr& localColl,
    const bool checkRangeDeletionIndexes) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    const auto& catalogUUID = catalogColl.getUuid();
    const auto& localUUID = localColl->uuid();
    if (catalogUUID != localUUID) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
            CollectionUUIDMismatchDetails{
                nss, shardId, localUUID, catalogUUID, getNumDocs(opCtx, localColl.get())}));
    }

    const auto makeOptionsMismatchInconsistencyBetweenShardAndConfig =
        [&](const NamespaceString& nss,
            const ShardId& shardId,
            const BSONObj& shardOptions,
            const BSONObj& configOptions) {
            constexpr StringData kShardsFieldName = "shards"_sd;
            constexpr StringData kOptionsFieldName = "options"_sd;
            const auto configShardId = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();

            return metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch,
                CollectionOptionsMismatchDetails{
                    nss,
                    {BSON(kOptionsFieldName << shardOptions << kShardsFieldName
                                            << BSON_ARRAY(shardId)),
                     BSON(kOptionsFieldName << configOptions << kShardsFieldName
                                            << BSON_ARRAY(configShardId))}});
        };

    // A capped collection can't be sharded.
    if (localColl->isCapped() && !catalogColl.getUnsplittable().value_or(false)) {
        inconsistencies.emplace_back(makeOptionsMismatchInconsistencyBetweenShardAndConfig(
            nss,
            shardId,
            BSON("capped" << true),
            BSON("capped" << false << CollectionType::kUnsplittableFieldName << false)));
    }

    // Verifying timeseries options are consistent between the shard and the config server.
    const auto& localTimeseriesOptions = localColl->getTimeseriesOptions();
    const auto& catalogTimeseriesOptions = [&]() -> boost::optional<TimeseriesOptions> {
        if (const auto& timeseriesFields = catalogColl.getTimeseriesFields()) {
            return timeseriesFields->getTimeseriesOptions();
        }
        return boost::none;
    }();

    if ((localTimeseriesOptions && catalogTimeseriesOptions &&
         !timeseries::optionsAreEqual(*localTimeseriesOptions, *catalogTimeseriesOptions)) ||
        catalogTimeseriesOptions.has_value() != localTimeseriesOptions.has_value()) {
        inconsistencies.emplace_back(makeOptionsMismatchInconsistencyBetweenShardAndConfig(
            nss,
            shardId,
            BSON(CollectionType::kTimeseriesFieldsFieldName
                 << (localTimeseriesOptions ? localTimeseriesOptions->toBSON() : BSONObj())),
            BSON(CollectionType::kTimeseriesFieldsFieldName
                 << (catalogTimeseriesOptions ? catalogTimeseriesOptions->toBSON() : BSONObj()))));
    }

    // Verify default collation is consistent between the shard and the config server.
    if (localColl->getCollectionOptions().collation.woCompare(catalogColl.getDefaultCollation())) {
        inconsistencies.emplace_back(makeOptionsMismatchInconsistencyBetweenShardAndConfig(
            nss,
            shardId,
            BSON(CollectionType::kDefaultCollationFieldName
                 << localColl->getCollectionOptions().collation),
            BSON(CollectionType::kDefaultCollationFieldName
                 << (catalogColl.getDefaultCollation()))));
    }

    // Check that the metadata type locally is compatible with the type of collection on the config
    // server.
    if (catalogUUID == localUUID) {
        _checkCollectionFilteringInformation(
            opCtx, nss, shardId, localColl, true /* expectTracked */, inconsistencies);
    }


    // Check shardKey index inconsistencies.
    // Skip the check in case of unsplittable collections as we don't strictly require an index on
    // the shard key for unsplittable collections.
    const bool isSharded = !catalogColl.getUnsplittable();
    if (catalogUUID == localUUID && isSharded) {
        _checkShardKeyIndexInconsistencies(opCtx,
                                           nss,
                                           shardId,
                                           catalogColl.getKeyPattern().toBSON(),
                                           localColl,
                                           inconsistencies,
                                           checkRangeDeletionIndexes);
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> _checkLocalInconsistencies(OperationContext* opCtx,
                                                                  const NamespaceString& nss,
                                                                  const ShardId& currentShard,
                                                                  const ShardId& primaryShard,
                                                                  const CollectionPtr& localColl) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    if (currentShard != primaryShard) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kMisplacedCollection,
            MisplacedCollectionDetails{
                nss, currentShard, localColl->uuid(), getNumDocs(opCtx, localColl.get())}));
    } else {
        _checkCollectionFilteringInformation(
            opCtx, nss, currentShard, localColl, false /* expectTracked */, inconsistencies);
    }

    _checkBucketCollectionInconsistencies(
        opCtx, nss, localColl, currentShard == primaryShard, inconsistencies);

    return inconsistencies;
}

bool _collectionMustExistLocallyButDoesnt(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const ShardId& currentShard,
                                          const ShardId& primaryShard) {
    // The DBPrimary shard must always have the collection created locally regardless if it owns
    // chunks or not.
    //
    // TODO (SERVER-100309): Remove exclusion for configDB once 9.0 becomes lastLTS.
    if (currentShard == primaryShard && !nss.isConfigDB()) {
        return true;
    }

    AutoGetCollection coll(opCtx, nss, MODE_IS);
    if (coll) {
        // There is no inconsistency if the collection exists locally.
        return false;
    }

    // If the collection doesn't exist, check if the current shard owns any chunk.
    // Perform the check under the collection lock (i.e. under the AutoGetCollection scope) to make
    // sure no migration happens concurrently.
    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);

    auto optCollDescr = scopedCsr->getCurrentMetadataIfKnown();
    if (!optCollDescr) {
        LOGV2_DEBUG(
            7629301,
            1,
            "Ignoring missing collection inconsistencies because collection metadata is unknown",
            logAttrs(nss));
        return false;
    }
    auto criticalSectionSignal =
        scopedCsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite);
    if (criticalSectionSignal) {
        LOGV2_DEBUG(9461000,
                    1,
                    "Ignoring missing collection inconsistencies because collection metadata is "
                    "unknown when a critical section is active",
                    logAttrs(nss));
        return false;
    }

    return optCollDescr->hasRoutingTable() && optCollDescr->currentShardHasAnyChunks();
}

std::vector<BSONObj> _runExhaustiveAggregation(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               AggregateCommandRequest& aggRequest,
                                               StringData reason) {
    const auto logMetadataInconsistency = [](const NamespaceString& nss,
                                             const DBException& exception) {
        LOGV2(8739100,
              "Failed to refresh the routing information due to a potential metadata inconsistency",
              logAttrs(nss),
              "error"_attr = redact(exception));
    };

    std::vector<BSONObj> results;

    try {
        auto cursor = [&] {
            BSONObjBuilder responseBuilder;
            auto status = ClusterAggregate::runAggregate(opCtx,
                                                         ClusterAggregate::Namespaces{nss, nss},
                                                         aggRequest,
                                                         PrivilegeVector(),
                                                         boost::none, /*verbosity*/
                                                         &responseBuilder,
                                                         reason);
            uassertStatusOKWithContext(
                status, str::stream() << "Failed to execute aggregation for: " << reason);
            return uassertStatusOK(CursorResponse::parseFromBSON(responseBuilder.obj()));
        }();

        results = cursor.releaseBatch();

        if (!cursor.getCursorId()) {
            return results;
        }

        const auto authzSession = AuthorizationSession::get(opCtx->getClient());
        AuthzCheckFn authChecker = [&authzSession](AuthzCheckFnInputType userName) -> Status {
            return authzSession->isCoauthorizedWith(userName)
                ? Status::OK()
                : Status(ErrorCodes::Unauthorized, "User not authorized to access cursor");
        };

        // Check out the cursor. If the cursor is not found, all data was retrieve in the
        // first batch.
        const auto cursorManager = Grid::get(opCtx)->getCursorManager();
        auto pinnedCursor = uassertStatusOK(
            cursorManager->checkOutCursor(cursor.getCursorId(), opCtx, authChecker));
        while (true) {
            auto next = pinnedCursor->next();
            if (!next.isOK() || next.getValue().isEOF()) {
                break;
            }

            if (auto data = next.getValue().getResult()) {
                results.emplace_back(data.get().getOwned());
            }
        }
    } catch (const ExceptionFor<ErrorCodes::ChunkMetadataInconsistency>& e) {
        // In presence on metadata inconsistency within the config catalog, the refresh of the
        // routing information cache may fail.
        // When this happens, ignore the error: the problem will still be reported to the user
        // thanks  to the consistency checks performed on the config server.
        logMetadataInconsistency(nss, e);
    } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& e) {
        // This is for backward compatibility reasons.
        logMetadataInconsistency(nss, e);
    }
    return results;
}

std::unique_ptr<DBClientCursor> _getCollectionChunksCursor(DBDirectClient* client,
                                                           const CollectionType& coll) {
    // Running the following pipeline against 'config.chunks':
    //    db.chunks.aggregate([{ $match: { 'uuid': <UUID> }},{ $sort: { 'min': 1 }}])
    return uassertStatusOK(DBClientCursor::fromAggregationRequest(
        client,
        std::invoke([&coll] {
            AggregateCommandRequest aggRequest{
                NamespaceString::kConfigsvrChunksNamespace,
                std::vector<mongo::BSONObj>{
                    BSON("$match" << BSON(ChunkType::collectionUUID() << coll.getUuid())),
                    BSON("$sort" << BSON(ChunkType::min() << 1))}};
            aggRequest.setReadConcern(
                repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern));
            return aggRequest;
        }),
        false /* secondaryOK */,
        false /* useExhaust */));
}

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistencyInShardCatalogCache(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersionInGlobalCatalog,
    const DatabaseVersion& dbVersionInShardCatalog,
    const ShardId& primaryShard) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    const auto dbVersion = [&]() {
        const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);
        return scopedDsr->getDbVersion();
    }();

    if (!dbVersion) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalogCache,
            MissingDatabaseMetadataInShardCatalogCacheDetails{
                dbName, primaryShard, dbVersionInGlobalCatalog}));
        return inconsistencies;
    }

    const auto dbVersionInCache = *dbVersion;

    if (dbVersionInGlobalCatalog != dbVersionInCache ||
        dbVersionInShardCatalog != dbVersionInCache) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalogCache,
            InconsistentDatabaseVersionInShardCatalogCacheDetails{dbName,
                                                                  primaryShard,
                                                                  dbVersionInGlobalCatalog,
                                                                  dbVersionInShardCatalog,
                                                                  dbVersionInCache}));
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistencyInShardCatalog(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersionInGlobalCatalog,
    const ShardId& primaryShard) {
    std::vector<MetadataInconsistencyItem> inconsistencies;

    DBDirectClient client(opCtx);
    FindCommandRequest findOp{NamespaceString::kConfigShardCatalogDatabasesNamespace};
    findOp.setFilter(BSON(DatabaseType::kDbNameFieldName << DatabaseNameUtil::serialize(
                              dbName, SerializationContext::stateDefault())));
    auto cursor = client.find(std::move(findOp));

    tassert(
        10078300,
        str::stream() << "Failed to retrieve cursor while reading database metadata for database: "
                      << dbName.toStringForErrorMsg(),
        cursor);

    if (!cursor->more()) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog,
                              MissingDatabaseMetadataInShardCatalogDetails{
                                  dbName, primaryShard, dbVersionInGlobalCatalog}));
        return inconsistencies;
    }

    try {
        auto dbInShardCatalog =
            DatabaseType::parse(cursor->nextSafe().getOwned(), IDLParserContext("DatabaseType"));

        auto shardInLocalCatalog = dbInShardCatalog.getPrimary();
        if (shardInLocalCatalog != primaryShard) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kMisplacedDatabaseMetadataInShardCatalog,
                MisplacedDatabaseMetadataInShardCatalogDetails{
                    dbName, primaryShard, shardInLocalCatalog}));
        }

        auto dbVersionInShardCatalog = dbInShardCatalog.getVersion();
        if (dbVersionInGlobalCatalog != dbVersionInShardCatalog) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kInconsistentDatabaseVersionInShardCatalog,
                InconsistentDatabaseVersionInShardCatalogDetails{
                    dbName, primaryShard, dbVersionInGlobalCatalog, dbVersionInShardCatalog}));
        }

        auto cacheInconsistencies = checkDatabaseMetadataConsistencyInShardCatalogCache(
            opCtx, dbName, dbVersionInGlobalCatalog, dbVersionInShardCatalog, primaryShard);

        inconsistencies.insert(inconsistencies.end(),
                               std::make_move_iterator(cacheInconsistencies.begin()),
                               std::make_move_iterator(cacheInconsistencies.end()));
    } catch (const AssertionException&) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingDatabaseMetadataInShardCatalog,
                              MissingDatabaseMetadataInShardCatalogDetails{
                                  dbName, primaryShard, dbVersionInGlobalCatalog}));
    }

    tassert(9980501,
            "Found duplicated database metadata in the shard catalog with the same _id value",
            !cursor->more());

    return inconsistencies;
}

}  // namespace


MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss) {
    if (nss.isAdminDB()) {
        return MetadataConsistencyCommandLevelEnum::kClusterLevel;
    } else if (nss.isCollectionlessCursorNamespace()) {
        return MetadataConsistencyCommandLevelEnum::kDatabaseLevel;
    } else {
        return MetadataConsistencyCommandLevelEnum::kCollectionLevel;
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeQueuedPlanExecutor(
    OperationContext* opCtx,
    std::vector<MetadataInconsistencyItem>&& inconsistencies,
    const NamespaceString& nss) {

    auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(nss).build();
    auto ws = std::make_unique<WorkingSet>();
    auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

    insertFakeInconsistencies.execute([&](const BSONObj& data) {
        const auto numInconsistencies = data["numInconsistencies"].safeNumberLong();
        for (int i = 0; i < numInconsistencies; i++) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionUUIDMismatch,
                CollectionUUIDMismatchDetails{nss, ShardId{"shard"}, UUID::gen(), UUID::gen(), 0}));
        }
    });

    for (auto&& inconsistency : inconsistencies) {
        // Every inconsistency encountered need to be logged with the same format
        // to allow log injestion systems to correctly detect them.
        logMetadataInconsistency(inconsistency);
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->keyData.clear();
        member->recordId = RecordId();
        member->resetDocument(SnapshotId(), inconsistency.toBSON().getOwned());
        member->transitionToOwnedObj();
        root->pushBack(id);
    }

    return uassertStatusOK(
        plan_executor_factory::make(expCtx,
                                    std::move(ws),
                                    std::move(root),
                                    &CollectionPtr::null,
                                    PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                    false, /* whether returned BSON must be owned */
                                    nss));
}

CursorInitialReply createInitialCursorReplyMongod(OperationContext* opCtx,
                                                  ClientCursorParams&& cursorParams,
                                                  long long batchSize) {
    auto& exec = cursorParams.exec;
    auto& nss = cursorParams.nss;

    std::vector<BSONObj> firstBatch;
    FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
    for (long long objCount = 0; objCount < batchSize; objCount++) {
        BSONObj nextDoc;
        PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
        if (state == PlanExecutor::IS_EOF) {
            break;
        }
        invariant(state == PlanExecutor::ADVANCED);

        // If we can't fit this result inside the current batch, then we stash it for
        // later.
        if (!responseSizeTracker.haveSpaceForNext(nextDoc)) {
            exec->stashResult(nextDoc);
            break;
        }

        responseSizeTracker.add(nextDoc);
        firstBatch.push_back(std::move(nextDoc));
    }

    auto&& opDebug = CurOp::get(opCtx)->debug();
    opDebug.additiveMetrics.nBatches = 1;
    opDebug.additiveMetrics.nreturned = firstBatch.size();

    if (exec->isEOF()) {
        opDebug.cursorExhausted = true;

        CursorInitialReply resp;
        InitialResponseCursor initRespCursor{std::move(firstBatch)};
        initRespCursor.setResponseCursorBase({0LL /* cursorId */, nss});
        resp.setCursor(std::move(initRespCursor));
        return resp;
    }

    exec->saveState();
    exec->detachFromOperationContext();

    auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));

    pinnedCursor->incNBatches();
    pinnedCursor->incNReturnedSoFar(firstBatch.size());

    CursorInitialReply resp;
    InitialResponseCursor initRespCursor{std::move(firstBatch)};
    const auto cursorId = pinnedCursor.getCursor()->cursorid();
    initRespCursor.setResponseCursorBase({cursorId, nss});
    resp.setCursor(std::move(initRespCursor));

    // Record the cursorID in CurOp.
    opDebug.cursorid = cursorId;

    return resp;
}

std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistency(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const std::vector<CollectionType>& shardingCatalogCollections,
    const std::vector<CollectionPtr>& localCatalogCollections,
    const bool checkRangeDeletionIndexes) {

    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto itLocalCollections = localCatalogCollections.begin();
    auto itCatalogCollections = shardingCatalogCollections.begin();
    while (itLocalCollections != localCatalogCollections.end() &&
           itCatalogCollections != shardingCatalogCollections.end()) {
        const auto& localColl = *itLocalCollections;
        const auto& localNss = localColl->ns();
        const auto& remoteNss = itCatalogCollections->getNss();

        const auto cmp = remoteNss.coll().compare(localNss.coll());
        const bool isCollectionOnlyOnShardingCatalog = cmp < 0;
        const bool isCollectionOnBothCatalogs = cmp == 0;
        if (isCollectionOnlyOnShardingCatalog) {
            // Case where we have found a collection in the sharding catalog that it is not in the
            // local catalog.
            if (_collectionMustExistLocallyButDoesnt(opCtx, remoteNss, shardId, primaryShardId)) {
                inconsistencies.emplace_back(
                    makeInconsistency(MetadataInconsistencyTypeEnum::kMissingLocalCollection,
                                      MissingLocalCollectionDetails{
                                          remoteNss, itCatalogCollections->getUuid(), shardId}));
            }
            itCatalogCollections++;
        } else if (isCollectionOnBothCatalogs) {
            // Case where we have found same collection in the catalog client than in the local
            // catalog.
            auto inconsistenciesBetweenBothCatalogs =
                _checkInconsistenciesBetweenBothCatalogs(opCtx,
                                                         localNss,
                                                         shardId,
                                                         *itCatalogCollections,
                                                         localColl,
                                                         checkRangeDeletionIndexes);
            inconsistencies.insert(
                inconsistencies.end(),
                std::make_move_iterator(inconsistenciesBetweenBothCatalogs.begin()),
                std::make_move_iterator(inconsistenciesBetweenBothCatalogs.end()));

            itLocalCollections++;
            itCatalogCollections++;

            _checkBucketCollectionInconsistencies(opCtx,
                                                  localNss,
                                                  localColl,
                                                  primaryShardId == shardId /* isPrimaryShard */,
                                                  inconsistencies);
        } else {
            // Case where we have found a local collection that is not in the sharding catalog.
            const auto& nss = localNss;

            if (!localNss.isShardLocalNamespace()) {
                auto localInconsistencies =
                    _checkLocalInconsistencies(opCtx, nss, shardId, primaryShardId, localColl);
                inconsistencies.insert(inconsistencies.end(),
                                       std::make_move_iterator(localInconsistencies.begin()),
                                       std::make_move_iterator(localInconsistencies.end()));
            }
            itLocalCollections++;
        }
    }

    while (itLocalCollections != localCatalogCollections.end()) {
        const auto& localColl = *itLocalCollections;
        const auto& localNss = localColl->ns();

        if (!localNss.isShardLocalNamespace()) {
            auto localInconsistencies =
                _checkLocalInconsistencies(opCtx, localNss, shardId, primaryShardId, localColl);
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(localInconsistencies.begin()),
                                   std::make_move_iterator(localInconsistencies.end()));
        }
        itLocalCollections++;
    }

    while (itCatalogCollections != shardingCatalogCollections.end()) {
        if (_collectionMustExistLocallyButDoesnt(
                opCtx, itCatalogCollections->getNss(), shardId, primaryShardId)) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kMissingLocalCollection,
                MissingLocalCollectionDetails{
                    itCatalogCollections->getNss(), itCatalogCollections->getUuid(), shardId}));
        }
        itCatalogCollections++;
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkIndexesConsistencyAcrossShards(
    OperationContext* opCtx, const std::vector<CollectionType>& collections) {
    static const auto rawPipelineStages = [] {
        /**
         * The following pipeline is used to check for inconsistencies in the indexes of all the
         * collections across all shards in the cluster. In particular, it checks that:
         *      1. All shards have the same set of indexes.
         *      2. All shards have the same properties for each index.
         *
         * The pipeline is structured as follows:
         *      1. Use the $indexStats stage to gather statistics about each index in all shards.
         *      2. Group all the indexes together and collect them into an array. Also, collect the
         *      names of all the shards in the cluster.
         *      3. Create a new document for each index in the array created by the previous stage.
         *      4. Group all the indexes by name.
         *      5. For each index, create two new fields:
         *          - `missingFromShards`: array of differences between all shards that are expected
         *          to have the index and the shards that actually contain the index.
         *          - `inconsistentProperties`: array of differences between the properties of each
         *          index across all shards.
         *      6. Filter out indexes that are consistent across all shards.
         *      7. Project the final result.
         */
        auto rawPipelineBSON = fromjson(R"({pipeline: [
			{$indexStats: {}},
			{$group: {
					_id: null,
					indexDoc: {$push: '$$ROOT'},
					allShards: {$addToSet: '$shard'}
			}},
			{$unwind: '$indexDoc'},
			{$group: {
					'_id': '$indexDoc.name',
					'shards': {$push: '$indexDoc.shard'},
					'specs': {$push: {$objectToArray: {$ifNull: ['$indexDoc.spec', {}]}}},
					'allShards': {$first: '$allShards'}
			}},
			{$project: {
				missingFromShards: {$setDifference: ['$allShards', '$shards']},
				inconsistentProperties: {
					$setDifference: [
						{$reduce: {
							input: '$specs',
							initialValue: {$arrayElemAt: ['$specs', 0]},
							in: {$setUnion: ['$$value', '$$this']}}},
						{$reduce: {
							input: '$specs',
							initialValue: {$arrayElemAt: ['$specs', 0]},
							in: {$setIntersection: ['$$value', '$$this']}
						}}
					]
				}
			}},
			{$match: {
				$expr: {
					$or: [
						{$gt: [{$size: '$missingFromShards'}, 0]},
						{$gt: [{$size: '$inconsistentProperties'}, 0]
						}
					]
				}
			}},
			{$project: {
				'_id': 0,
				indexName: '$$ROOT._id',
				inconsistentProperties: 1,
				missingFromShards: 1
			}}
		]})");
        return parsePipelineFromBSON(rawPipelineBSON.firstElement());
    }();

    std::vector<MetadataInconsistencyItem> indexIncons;
    for (const auto& coll : collections) {
        const auto& nss = coll.getNss();

        AggregateCommandRequest aggRequest{nss, rawPipelineStages};

        std::vector<BSONObj> results = _runExhaustiveAggregation(
            opCtx, nss, aggRequest, "Check sharded indexes consistency across shards"_sd);

        indexIncons.reserve(results.size());
        for (auto&& rawIndexIncon : results) {
            indexIncons.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kInconsistentIndex,
                InconsistentIndexDetails{nss, std::move(rawIndexIncon)}));
        }
    }
    return indexIncons;
}


std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistencyAcrossShards(
    OperationContext* opCtx, const std::vector<CollectionType>& collections) {

    const auto getRawPipelineStages = [&](const NamespaceString& nss) {
        auto catalogEntryOnPrimaryShard =
            MongoProcessInterface::create(opCtx)->getCatalogEntry(opCtx, nss);

        /**
         * The following pipeline is used to check the collection metadata consistency across shards
         * of the given collection. In particular, it checks that all shards owning chunks of a
         * collection and the DBPrimary of that collection have the same collection metadata
         * (excluding indexes, whose consistency is checked separately).
         * The DBPrimary shard must always have the collection created locally and its collection
         * metadata must be consistent with other shards regardless if the DBPrimary shard owns
         * chunks or not.
         * Note that here we aren't checking if the collection is missing on any of
         * those shards, this is already done by
         * metadata_consistency_util::checkCollectionMetadataConsistency().
         *
         * The pipeline is structured as follows:
         *      1. Use the $listCatalog stage to gather the collection metadata from all shards
         *      owning chunks.
         *      2. Since $listCatalog only targets shards owning chunks, we may skip checking the
         *      existance of the collection on the DBPrimary shard, where the collection must also
         *      exist. Therefore, in this step we are appending the catalog entry obtained from
         *      the DBPrimary shard to the list of documents returned by $listCatalog. To do so, we
         *      need to concatenate the following 4 stages: $group, $project, $unwind and
         *      $replaceWith.
         *      3. Keep just the two meaningful fields for our purpose: `md` and `shard`.
         *      4. Then, we split the pipeline in two sub-pipelines using a facet stage. The first
         *      sub-pipeline finds inconsistencies within the collection options (`md.options`),
         *      while the second sub-pipeline finds any other inconsistency in the metadata outside
         *      `md.options` and `md.indexes`. This split lets us keep backwards compatibility, and
         *      classify both kinds of inconsistencies separately.
         *      5. Within each sub-pipeline: Group by collection options/metadata in order to
         *      detect inconsistencies between shards. We will end up having one document per every
         *      different collection options/metadata found.
         *      6. Finally, within each sub-pipeline, rename the `_id` field to `options`/`md` to
         *      deliver the inconsistency to the user (if any).
         *
         *      This is an example of the results obtained if there is a collection options
         *      mismatch between shard0 and shard1,shard2:
         *          Inconsistency type: CollectionOptionsMismatch
         *          Inconsistency details: [
         *              {
         *                options: <optionsA>,
         *                shards: [shard0]
         *              },
         *              {
         *                options: <optionsB>,
         *                shards: [shard1,shard2]
         *              }
         *          ]
         *
         *      This is an example of the results obtained if there is a collection auxiliary
         *      metadata mismatch between shard0,shard1 and shard2:
         *          Inconsistency type: CollectionAuxiliaryMetadataMismatch
         *          Inconsistency details: [
         *              {
         *                collectionMetadata: <metadataA>,
         *                shards: [shard0,shard1]
         *              },
         *              {
         *                collectionMetadata: <metadataB>,
         *                shards: [shard2]
         *              }
         *          ]
         */
        std::vector<BSONObj> pipeline;
        pipeline.emplace_back(fromjson(R"(
            {$listCatalog: {}})"));
        if (catalogEntryOnPrimaryShard) {
            pipeline.emplace_back(fromjson(R"(
                {$group: {
                    _id: 0,
                    docs: { $push: "$$ROOT" }
                }})"));
            pipeline.emplace_back(
                BSON("$project" << BSON(
                         "docs" << BSON("$concatArrays" << BSON_ARRAY(
                                            "$docs" << BSON_ARRAY(BSON(
                                                "$literal" << *catalogEntryOnPrimaryShard)))))));
            pipeline.emplace_back(fromjson(R"(
                { $unwind: '$docs' })"));
            pipeline.emplace_back(fromjson(R"(
                { $replaceWith: '$docs' })"));
        }
        simulateCatalogTopLevelMetadataInconsistency.execute([&](const auto&) {
            // Generates a CollectionAuxiliaryMetadataMismatch inconsistency by simulating that
            // $listCatalog returns a top-level field which is inconsistent across shards.
            pipeline.emplace_back(fromjson(R"(
                {$addFields: {
                    'md.testOnlyInconsistentField': '$shard'
                }})"));
        });
        pipeline.emplace_back(fromjson(R"(
            {$project: {
                md: '$md',
                shard: '$shard'
            }})"));
        // Ignore inconsistencies in the legacy timeseries flags. Due to SERVER-91195, those flags
        // have been deprecated and will be removed. At the same time, they can become inconsistent
        // in various scenarios, such as movePrimary or FCV downgrades.
        // TODO (SERVER-101423): Remove tsBucketingParametersHaveChanged once 9.0 becomes last LTS.
        // TODO (SERVER-96831): Remove tsBucketsMayHaveMixedSchemaData field once it's removed.
        pipeline.emplace_back(fromjson(R"(
            {$project: {
                'md.timeseriesBucketingParametersHaveChanged': 0,
                'md.timeseriesBucketsMayHaveMixedSchemaData': 0
            }})"));
        pipeline.emplace_back(fromjson(R"(
            {$facet: {
                options: [
                    {$group: {
                        _id: '$md.options',
                        shards: {$addToSet: '$shard'}
                    }},
                    {$project: {
                        _id: 0,
                        options: "$_id",
                        shards: 1
                    }}
                ],
                auxiliaryMetadata: [
                    {$project: {
                        "md.options": 0,
                        "md.indexes": 0
                    }},
                    {$group: {
                        _id: "$md",
                        shards: { $addToSet: "$shard" }
                    }},
                    {$project: {
                        _id: 0,
                        md: "$_id",
                        shards: 1
                    }}
                ]
            }})"));
        return pipeline;
    };

    std::vector<MetadataInconsistencyItem> inconsistencies;
    for (const auto& coll : collections) {
        const auto& nss = coll.getNss();
        AggregateCommandRequest aggRequest{nss, getRawPipelineStages(nss)};

        std::vector<BSONObj> facetedResult = _runExhaustiveAggregation(
            opCtx, nss, aggRequest, "Check collection metadata consistency across shards"_sd);

        // Even though the last stage of the aggregation is a $facet, the aggregation runner will
        // return an empty vector if aggregation fails due to an inconsistency reported elsewhere.
        if (facetedResult.empty())
            continue;
        tassert(9089900,
                "Expected collection metadata consistency check aggregation to return one document",
                facetedResult.size() == 1);

        // Every element on result's vector contains a unique collection option across the cluster
        // for the given collection. Below are listed the 3 different scenarios we can face:
        //     A) `results` is empty. Which means that the collection is missing on all the shards.
        //        This inconsistency is caught under `checkCollectionMetadataConsistency()`, so we
        //        don't take any action here.
        //     B) `results` size is 1: There is only one unique collection option across the
        //        cluster. This is the expected behavior and means that the collection options are
        //        consistent for the given collection.
        //     C) `results` size is greater than 1: There are 2 or more shards differing on their
        //        collection options, therefore we will return an inconsistency.
        //
        auto optionsField = facetedResult.front().getField("options");
        tassert(9089901,
                "Expected collection metadata check document to contain an 'options' field",
                !optionsField.eoo());
        std::vector<BSONObj> optionsResults;
        for (auto elem : optionsField.Array()) {
            optionsResults.emplace_back(elem.Obj());
        }
        if (optionsResults.size() > 1) {
            // Case where two or more shards have different collection options.
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionOptionsMismatch,
                CollectionOptionsMismatchDetails{nss, std::move(optionsResults)}));
        }

        // The same reasoning applies to metadata inconsistencies outside the collection options.
        auto auxiliaryMetadataField = facetedResult.front().getField("auxiliaryMetadata");
        tassert(9089902,
                "Expected collection metadata check document to have an 'auxiliaryMetadata' field",
                !auxiliaryMetadataField.eoo());
        std::vector<BSONObj> auxiliaryMetadataResults;
        for (auto elem : auxiliaryMetadataField.Array()) {
            auxiliaryMetadataResults.emplace_back(elem.Obj());
        }
        if (auxiliaryMetadataResults.size() > 1) {
            // Case where two or more shards have different collection auxiliary metadata.
            inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
                MetadataInconsistencyTypeEnum::kCollectionAuxiliaryMetadataMismatch,
                CollectionAuxiliaryMetadataMismatchDetails{nss,
                                                           std::move(auxiliaryMetadataResults)}));
        }
    }
    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkChunksConsistency(OperationContext* opCtx,
                                                              const CollectionType& collection) {
    tassert(9996600,
            "This method must run on the 'config' server.",
            ShardingState::get(opCtx)->shardId() == ShardId::kConfigServerId);

    DBDirectClient client{opCtx};
    // We need to read at snapshot readConcern, set it in the opCtx for DBDirectClient.
    ScopedReadConcern scopedReadConcern(
        opCtx, repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern));
    const auto chunksCursor = _getCollectionChunksCursor(&client, collection);

    const auto& uuid = collection.getUuid();
    const auto& nss = collection.getNss();
    const auto shardKeyPattern = ShardKeyPattern{collection.getKeyPattern()};
    std::vector<MetadataInconsistencyItem> inconsistencies;
    size_t totalChunks = 0;
    ChunkType previousChunk, firstChunk;

    while (chunksCursor->more()) {
        const auto chunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            chunksCursor->nextSafe(), collection.getEpoch(), collection.getTimestamp()));
        totalChunks++;

        const bool chunkHistoryEmpty = chunk.getHistory().empty();
        if (chunkHistoryEmpty) {
            const std::string errMsg = str::stream()
                << "The " << ChunkType::history() << " field is empty";
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));

        } else {
            if (chunk.getHistory().front().getShard() != chunk.getShard()) {
                std::string errMsg = str::stream()
                    << "The first element in the history for this chunk must be the owning shard "
                    << chunk.getShard() << " but it is "
                    << chunk.getHistory().front().getShard().toString();
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                    CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));
            }

            const bool onCurrentShardSinceMissing = !chunk.getOnCurrentShardSince().has_value();
            if (onCurrentShardSinceMissing) {
                const std::string errMsg = str::stream()
                    << "The " << ChunkType::onCurrentShardSince() << " field is missing";
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                    CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));
            } else if (chunk.getHistory().front().getValidAfter() !=
                       *chunk.getOnCurrentShardSince()) {
                std::string errMsg = str::stream()
                    << "The " << ChunkHistoryBase::kValidAfterFieldName
                    << " for the first element in the history"
                    << " must match the value of " << ChunkType::onCurrentShardSince();
                inconsistencies.emplace_back(makeInconsistency(
                    MetadataInconsistencyTypeEnum::kCorruptedChunkHistory,
                    CorruptedChunkHistoryDetails{nss, uuid, chunk.toConfigBSON(), errMsg}));
            }
        }

        if (!shardKeyPattern.isShardKey(chunk.getMin()) ||
            !shardKeyPattern.isShardKey(chunk.getMax())) {
            inconsistencies.emplace_back(
                makeInconsistency(MetadataInconsistencyTypeEnum::kCorruptedChunkShardKey,
                                  CorruptedChunkShardKeyDetails{
                                      nss, uuid, chunk.toConfigBSON(), shardKeyPattern.toBSON()}));
        }

        // Skip the first iteration as we need to compare the current chunk with the previous one.
        if (totalChunks == 1) {
            firstChunk = chunk;
            previousChunk = chunk;
            continue;
        }

        auto cmp = previousChunk.getMax().woCompare(chunk.getMin());
        if (cmp < 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableRangeGap,
                RoutingTableRangeGapDetails{
                    nss, uuid, previousChunk.toConfigBSON(), chunk.toConfigBSON()}));
        } else if (cmp > 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap,
                RoutingTableRangeOverlapDetails{
                    nss, uuid, previousChunk.toConfigBSON(), chunk.toConfigBSON()}));
        }

        previousChunk = std::move(chunk);
    }

    const ChunkType lastChunk = previousChunk;

    if (collection.getUnsplittable() && totalChunks > 1) {
        inconsistencies.emplace_back(makeInconsistency(
            MetadataInconsistencyTypeEnum::kTrackedUnshardedCollectionHasMultipleChunks,
            TrackedUnshardedCollectionHasMultipleChunksDetails{
                nss, collection.getUuid(), int(totalChunks)}));
    }
    // Check if the first and last chunk have MinKey and MaxKey respectively
    if (!totalChunks) {
        inconsistencies.emplace_back(
            makeInconsistency(MetadataInconsistencyTypeEnum::kMissingRoutingTable,
                              MissingRoutingTableDetails{nss, uuid}));
    } else {
        const BSONObj& minKeyObj = firstChunk.getMin();
        const auto globalMin = shardKeyPattern.getKeyPattern().globalMin();
        if (minKeyObj.woCompare(shardKeyPattern.getKeyPattern().globalMin()) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey,
                RoutingTableMissingMinKeyDetails{nss, uuid, minKeyObj, globalMin}));
        }

        const BSONObj& maxKeyObj = lastChunk.getMax();
        const auto globalMax = shardKeyPattern.getKeyPattern().globalMax();
        if (maxKeyObj.woCompare(globalMax) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey,
                RoutingTableMissingMaxKeyDetails{nss, uuid, maxKeyObj, globalMax}));
        }
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkZonesConsistency(OperationContext* opCtx,
                                                             const CollectionType& collection,
                                                             const std::vector<TagsType>& zones) {
    const auto& uuid = collection.getUuid();
    const auto& nss = collection.getNss();
    const auto shardKeyPattern = ShardKeyPattern{collection.getKeyPattern()};

    std::vector<MetadataInconsistencyItem> inconsistencies;
    auto previousZone = zones.begin();
    for (auto it = zones.begin(); it != zones.end(); it++) {
        const auto& zone = *it;

        // Skip the first iteration as we need to compare the current zone with the previous one.
        if (it == zones.begin()) {
            continue;
        }

        if (!shardKeyPattern.isShardKey(zone.getMinKey()) ||
            !shardKeyPattern.isShardKey(zone.getMaxKey())) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kCorruptedZoneShardKey,
                CorruptedZoneShardKeyDetails{nss, uuid, zone.toBSON(), shardKeyPattern.toBSON()}));
        }

        // As the zones are sorted by minKey, we can check if the previous zone maxKey is less than
        // the current zone minKey.
        const auto& minKey = zone.getMinKey();
        auto cmp = previousZone->getMaxKey().woCompare(minKey);
        if (cmp > 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kZonesRangeOverlap,
                ZonesRangeOverlapDetails{nss, uuid, previousZone->toBSON(), zone.toBSON()}));
        }

        previousZone = it;
    }

    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkCollectionShardingMetadataConsistency(
    OperationContext* opCtx, const CollectionType& collection) {
    std::vector<MetadataInconsistencyItem> inconsistencies;
    if (collection.getUnsplittable()) {
        const auto validKey = BSON("_id" << 1);
        if (collection.getKeyPattern().toBSON().woCompare(validKey) != 0) {
            inconsistencies.emplace_back(makeInconsistency(
                MetadataInconsistencyTypeEnum::kTrackedUnshardedCollectionHasInvalidKey,
                TrackedUnshardedCollectionHasInvalidKeyDetails{
                    collection.getNss(),
                    collection.getUuid(),
                    collection.getKeyPattern().toBSON()}));
        }
    }
    return inconsistencies;
}

std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistency(
    OperationContext* opCtx, const DatabaseType& dbInGlobalCatalog) {
    const auto dbName = dbInGlobalCatalog.getDbName();
    const auto dbVersionInGlobalCatalog = dbInGlobalCatalog.getVersion();
    const auto primaryShard = dbInGlobalCatalog.getPrimary();

    // TODO (SERVER-98118): Unconditionally return the inconsistencies found when we check for the
    // database metadata consistency optimistically - without serializing with the FCV.

    // Happy path: Check the consistency of the database metadata without serializing with the FCV.
    // In the most probable case, there is no concurrent FCV downgrade that could interfere with
    // this check and potentially result in false positives.
    //
    // If the database metadata is checked during an FCV downgrade, the execution may begin under
    // the assumption that shards are database-authoritative, but complete after the downgrade,
    // when the shard is no longer database-authoritative. This leads to fewer guarantees — for
    // example, the shard catalog may not be in sync with the global catalog.

    if (checkDatabaseMetadataConsistencyInShardCatalog(
            opCtx, dbName, dbVersionInGlobalCatalog, primaryShard)
            .empty()) {
        return {};
    }

    // Fallback path: Recheck database metadata consistency, this time serializing with the FCV.
    // This ensures that there are no concurrent FCV downgrades that might incorrectly invalidate
    // the assumption that the shard catalog is authoritative.

    FixedFCVRegion fixedFcvRegion(opCtx);

    if (!feature_flags::gShardAuthoritativeDbMetadataCRUD.isEnabled(
            VersionContext::getDecoration(opCtx), fixedFcvRegion->acquireFCVSnapshot())) {
        return {};
    }

    return checkDatabaseMetadataConsistencyInShardCatalog(
        opCtx, dbName, dbVersionInGlobalCatalog, primaryShard);
}

}  // namespace metadata_consistency_util
}  // namespace mongo
