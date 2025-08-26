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

#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/process_interface/common_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <type_traits>
#include <typeinfo>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

/**
 * Returns the routing information for the namespace set on the passed ExpressionContext. Also
 * verifies that the ExpressionContext's UUID, if present, matches that of the routing table entry.
 */
StatusWith<std::unique_ptr<RoutingContext>> getAndValidateRoutingCtx(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const CollectionRoutingInfo& cri) {
    const auto& nss = expCtx->getNamespaceString();
    auto swRoutingCtx = getRoutingContext(expCtx->getOperationContext(), {nss});
    // Additionally check that the ExpressionContext's UUID matches the collection routing info.
    if (swRoutingCtx.isOK()) {
        if (expCtx->getUUID() && cri.hasRoutingTable() &&
            !cri.getChunkManager().uuidMatches(*expCtx->getUUID())) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The UUID of collection " << nss.toStringForErrorMsg()
                                  << " changed; it may have been dropped and re-created."};
        }
    }
    return swRoutingCtx;
}

MongoProcessInterface::SupportingUniqueIndex supportsUniqueKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const BSONObj& index,
    const std::set<FieldPath>& uniqueKeyPaths) {
    // Retrieve the collation from the index, or default to the simple collation.
    const auto collation = uassertStatusOK(
        CollatorFactoryInterface::get(expCtx->getOperationContext()->getServiceContext())
            ->makeFromBSON(index.hasField(IndexDescriptor::kCollationFieldName)
                               ? index.getObjectField(IndexDescriptor::kCollationFieldName)
                               : CollationSpec::kSimpleSpec));

    // SERVER-5335: The _id index does not report to be unique, but in fact is unique.
    auto isIdIndex =
        index[IndexDescriptor::kIndexNameFieldName].String() == IndexConstants::kIdIndexName;
    bool supports =
        (isIdIndex || index.getBoolField(IndexDescriptor::kUniqueFieldName)) &&
        !index.hasField(IndexDescriptor::kPartialFilterExprFieldName) &&
        CommonProcessInterface::keyPatternNamesExactPaths(
            index.getObjectField(IndexDescriptor::kKeyPatternFieldName), uniqueKeyPaths) &&
        CollatorInterface::collatorsMatch(collation.get(), expCtx->getCollator());
    if (!supports) {
        return MongoProcessInterface::SupportingUniqueIndex::None;
    }
    return index.getBoolField(IndexDescriptor::kSparseFieldName)
        ? MongoProcessInterface::SupportingUniqueIndex::NotNullish
        : MongoProcessInterface::SupportingUniqueIndex::Full;
}

}  // namespace

std::unique_ptr<MongoProcessInterface::WriteSizeEstimator>
MongosProcessInterface::getWriteSizeEstimator(OperationContext* opCtx,
                                              const NamespaceString& ns) const {
    return std::make_unique<TargetPrimaryWriteSizeEstimator>();
}

std::unique_ptr<Pipeline> MongosProcessInterface::preparePipelineForExecution(
    Pipeline* ownedPipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    // On mongos we can't have local cursors.
    tassert(5530900,
            "shardTargetingPolicy cannot be kNotAllowed on mongos",
            shardTargetingPolicy != ShardTargetingPolicy::kNotAllowed);

    return sharded_agg_helpers::preparePipelineForExecution(
        ownedPipeline, shardTargetingPolicy, std::move(readConcern));
}

std::unique_ptr<Pipeline> MongosProcessInterface::preparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggRequest,
    Pipeline* pipeline,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    // On mongos we can't have local cursors.
    tassert(7393502,
            "shardTargetingPolicy cannot be kNotAllowed on mongos",
            shardTargetingPolicy != ShardTargetingPolicy::kNotAllowed);
    std::unique_ptr<Pipeline> targetPipeline(pipeline);
    return sharded_agg_helpers::targetShardsAndAddMergeCursors(
        expCtx,
        std::make_pair(aggRequest, std::move(targetPipeline)),
        shardCursorsSortSpec,
        shardTargetingPolicy,
        std::move(readConcern));
}

BSONObj MongosProcessInterface::preparePipelineAndExplain(Pipeline* ownedPipeline,
                                                          ExplainOptions::Verbosity verbosity) {
    auto firstStage = ownedPipeline->peekFront();
    // We don't want to serialize and send a MergeCursors stage to the shards.
    if (firstStage &&
        (typeid(*firstStage) == typeid(DocumentSourceMerge) ||
         typeid(*firstStage) == typeid(DocumentSourceMergeCursors))) {
        ownedPipeline->popFront();
    }
    return sharded_agg_helpers::targetShardsForExplain(ownedPipeline);
}

boost::optional<Document> MongosProcessInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& filter,
    boost::optional<BSONObj> readConcern) {
    auto foreignExpCtx = makeCopyFromExpressionContext(expCtx, nss, collectionUUID);

    // Create the find command to be dispatched to the shard(s) in order to return the post-image.
    auto filterObj = filter.toBson();
    BSONObjBuilder cmdBuilder;
    bool findCmdIsByUuid(foreignExpCtx->getUUID());
    if (findCmdIsByUuid) {
        foreignExpCtx->getUUID()->appendToBuilder(&cmdBuilder, "find");
    } else {
        cmdBuilder.append("find", nss.coll());
    }
    cmdBuilder.append("filter", filterObj);
    if (readConcern) {
        cmdBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName, *readConcern);
    }

    try {
        auto findCmd = cmdBuilder.obj();
        const auto& foreignNss = foreignExpCtx->getNamespaceString();
        sharding::router::CollectionRouter router(
            expCtx->getOperationContext()->getServiceContext(), foreignNss);
        auto shardResults = router.route(
            foreignExpCtx->getOperationContext(),
            str::stream() << "Looking up document matching " << redact(filter.toBson()),
            [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                auto routingCtxPtr = uassertStatusOK(getAndValidateRoutingCtx(foreignExpCtx, cri));
                // Finalize the 'find' command object based on the routing table
                // information.
                if (findCmdIsByUuid && cri.hasRoutingTable()) {
                    // Find by UUID and shard versioning do not work together
                    // (SERVER-31946).  In the sharded case we've already checked the UUID,
                    // so find by namespace is safe.  In the unlikely case that the
                    // collection has been deleted and a new collection with the same name
                    // created through a different mongos or the collection had its shard
                    // key refined, the shard version will be detected as stale, as shard
                    // versions contain an 'epoch' field unique to the collection.
                    findCmd = findCmd.addField(BSON("find" << foreignNss.coll()).firstElement());
                    findCmdIsByUuid = false;
                }

                // Build the versioned requests to be dispatched to the shards. Typically,
                // only a single shard will be targeted here; however, in certain cases
                // where only the _id is present, we may need to scatter-gather the query to
                // all shards in order to find the document.
                auto requests =
                    getVersionedRequestsForTargetedShards(expCtx->getOperationContext(),
                                                          foreignNss,
                                                          cri,
                                                          findCmd,
                                                          filterObj,
                                                          CollationSpec::kSimpleSpec,
                                                          boost::none /*letParameters*/,
                                                          boost::none /*runtimeConstants*/);

                // Dispatch the requests. The 'establishCursors' method conveniently
                // prepares the result into a vector of cursor responses for us.
                return establishCursors(expCtx->getOperationContext(),
                                        Grid::get(expCtx->getOperationContext())
                                            ->getExecutorPool()
                                            ->getArbitraryExecutor(),
                                        foreignNss,
                                        ReadPreferenceSetting::get(expCtx->getOperationContext()),
                                        std::move(requests),
                                        false,
                                        routingCtxPtr.get());
            });

        // Iterate all shard results and build a single composite batch. We also enforce the
        // requirement that only a single document should have been returned from across the
        // cluster.
        std::vector<BSONObj> finalBatch;
        for (auto&& shardResult : shardResults) {
            auto& shardCursor = shardResult.getCursorResponse();
            finalBatch.insert(
                finalBatch.end(), shardCursor.getBatch().begin(), shardCursor.getBatch().end());
            // We should have at most 1 result, and the cursor should be exhausted.
            uassert(ErrorCodes::ChangeStreamFatalError,
                    str::stream() << "Shard cursor was unexpectedly open after lookup: "
                                  << shardResult.getHostAndPort()
                                  << ", id: " << shardCursor.getCursorId(),
                    shardCursor.getCursorId() == 0);
            uassert(ErrorCodes::ChangeStreamFatalError,
                    str::stream() << "found more than one document matching " << filter.toString()
                                  << " [" << finalBatch.begin()->toString() << ", "
                                  << std::next(finalBatch.begin())->toString() << "]",
                    finalBatch.size() <= 1u);
        }

        return (!finalBatch.empty() ? Document(finalBatch.front()) : boost::optional<Document>{});
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // If it's an unsharded collection which has been deleted and re-created, we may get a
        // NamespaceNotFound error when looking up by UUID.
        return boost::none;
    }
}

boost::optional<Document> MongosProcessInterface::lookupSingleDocumentLocally(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const Document& documentKey) {
    MONGO_UNREACHABLE_TASSERT(6148001);
}

std::vector<DatabaseName> MongosProcessInterface::getAllDatabases(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    return _getAllDatabasesOnAShardedCluster(opCtx, tenantId);
}

std::vector<BSONObj> MongosProcessInterface::runListCollections(OperationContext* opCtx,
                                                                const DatabaseName& db,
                                                                bool addPrimaryShard) {
    return _runListCollectionsCommandOnAShardedCluster(
        opCtx, NamespaceStringUtil::deserialize(db, ""), {.addPrimaryShard = addPrimaryShard});
}

BSONObj MongosProcessInterface::_reportCurrentOpForClient(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        expCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (auto txnRouter = TransactionRouter::get(clientOpCtx)) {
            txnRouter.reportState(clientOpCtx, &builder, true /* sessionIsActive */);
        }
    }

    return builder.obj();
}

void MongosProcessInterface::_reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                                              CurrentOpUserMode userMode,
                                                              std::vector<BSONObj>* ops) const {
    auto sessionCatalog = SessionCatalog::get(opCtx);

    const bool authEnabled = AuthorizationManager::get(opCtx->getService())->isAuthEnabled();

    // If the user is listing only their own ops, we use makeSessionFilterForAuthenticatedUsers
    // to create a pattern that will match against all authenticated usernames for the current
    // client. If the user is listing ops for all users, we create an empty pattern;
    // constructing an instance of SessionKiller::Matcher with this empty pattern will return
    // all sessions.
    auto sessionFilter = (authEnabled && userMode == CurrentOpUserMode::kExcludeOthers
                              ? makeSessionFilterForAuthenticatedUsers(opCtx)
                              : KillAllSessionsByPatternSet{{}});

    sessionCatalog->scanSessions({std::move(sessionFilter)}, [&](const ObservableSession& session) {
        if (!session.hasCurrentOperation()) {
            auto op =
                TransactionRouter::get(session).reportState(opCtx, false /* sessionIsActive */);
            if (!op.isEmpty()) {
                ops->emplace_back(op);
            }
        }
    });
}

void MongosProcessInterface::_reportCurrentOpsForTransactionCoordinators(
    OperationContext* opCtx, bool includeIdle, std::vector<BSONObj>* ops) const {};

void MongosProcessInterface::_reportCurrentOpsForPrimaryOnlyServices(
    OperationContext* opCtx,
    CurrentOpConnectionsMode connMode,
    CurrentOpSessionsMode sessionMode,
    std::vector<BSONObj>* ops) const {};

void MongosProcessInterface::_reportCurrentOpsForQueryAnalysis(OperationContext* opCtx,
                                                               std::vector<BSONObj>* ops) const {
    if (analyze_shard_key::supportsSamplingQueries(opCtx)) {
        analyze_shard_key::QueryAnalysisSampleTracker::get(opCtx).reportForCurrentOp(ops);
    }
}

std::vector<GenericCursor> MongosProcessInterface::getIdleCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    invariant(hasGlobalServiceContext());
    auto cursorManager =
        Grid::get(expCtx->getOperationContext()->getServiceContext())->getCursorManager();
    invariant(cursorManager);
    return cursorManager->getIdleCursors(expCtx->getOperationContext(), userMode);
}

std::vector<FieldPath> MongosProcessInterface::collectDocumentKeyFieldsActingAsRouter(
    OperationContext* opCtx, const NamespaceString& nss, RoutingContext* routingCtx) const {
    tassert(10292900, "RoutingContext has not been acquired.", routingCtx);
    const auto& cri = routingCtx->getCollectionRoutingInfo(nss);
    if (cri.isSharded()) {
        return CommonProcessInterface::shardKeyToDocumentKeyFields(
            cri.getChunkManager().getShardKeyPattern().getKeyPatternFields());
    }
    // We have no evidence this collection is sharded, so the document key is just _id.
    return {"_id"};
}

bool MongosProcessInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    // The RoutingContext is acquired and disposed of without validating the routing tables against
    // a shard here because this isSharded() check is only used for distributed query planning
    // optimizations; it doesn't affect query correctness. DO NOT use this function to make a
    // decision that affects query correctness.
    auto routingCtx = uassertStatusOK(getRoutingContext(opCtx, {nss}));
    return routingCtx->getCollectionRoutingInfo(nss).isSharded();
}

MongoProcessInterface::SupportingUniqueIndex
MongosProcessInterface::fieldsHaveSupportingUniqueIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& fieldPaths) const {
    // Get a list of indexes from a shard with correct indexes for the namespace. For an unsharded
    // collection, this is the current primary shard for the database, and for a sharded collection,
    // this is any shard that currently owns at least one chunk. This helper sends database and/or
    // shard versions to ensure this router is not stale, but will not automatically retry if either
    // version is stale.
    sharding::router::CollectionRouter router{expCtx->getOperationContext()->getServiceContext(),
                                              nss};
    return router.routeWithRoutingContext(
        expCtx->getOperationContext(),
        "MongosProcessInterface::fieldsHaveSupportingUniqueIndex"_sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            auto response =
                loadIndexesFromAuthoritativeShard(expCtx->getOperationContext(), routingCtx, nss);

            // If the namespace does not exist, then the field paths *must* be _id only.
            if (response.getStatus() == ErrorCodes::NamespaceNotFound) {
                return fieldPaths == std::set<FieldPath>{"_id"} ? SupportingUniqueIndex::Full
                                                                : SupportingUniqueIndex::None;
            }
            uassertStatusOK(response);

            const auto& indexes = response.getValue().docs;
            return std::accumulate(indexes.begin(),
                                   indexes.end(),
                                   SupportingUniqueIndex::None,
                                   [&expCtx, &fieldPaths](auto result, const auto& index) {
                                       return std::max(
                                           result, supportsUniqueKey(expCtx, index, fieldPaths));
                                   });
        });
}

MongosProcessInterface::DocumentKeyResolutionMetadata
MongosProcessInterface::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::set<FieldPath>> fieldPaths,
    boost::optional<ChunkVersion> targetCollectionPlacementVersion,
    const NamespaceString& outputNs) const {
    invariant(expCtx->getInRouter());
    uassert(51179,
            "Received unexpected 'targetCollectionPlacementVersion' on mongos",
            !targetCollectionPlacementVersion);

    if (fieldPaths) {
        auto supportingUniqueIndex = fieldsHaveSupportingUniqueIndex(expCtx, outputNs, *fieldPaths);
        uassert(51190,
                "Cannot find index to verify that join fields will be unique",
                supportingUniqueIndex != SupportingUniqueIndex::None);

        // If the user supplies the 'fields' array, we don't need to attach a ChunkVersion for
        // the shards since we are not at risk of 'guessing' the wrong shard key.
        return {*fieldPaths, boost::none, supportingUniqueIndex};
    }

    // TODO SERVER-95749 Avoid forced collection cache refresh and validate RoutingContext in
    // checkRoutingInfoEpochOrThrow().
    std::unique_ptr<RoutingContext> routingCtx;
    auto placementVersion = [&]() -> boost::optional<ChunkVersion> {
        const auto& catalogCache = Grid::get(expCtx->getOperationContext())->catalogCache();
        // In case there are multiple shards which will perform this stage in parallel, we need to
        // figure out and attach the collection's shard version to ensure each shard is talking
        // about the same version of the collection. This mongos will coordinate that. We force a
        // catalog refresh to do so because there is no shard versioning protocol on this namespace
        // and so we otherwise could not be sure this node is (or will become) at all recent. We
        // will also figure out and attach the 'joinFields' to send to the shards.

        // There are edge cases when the collection could be dropped or re-created during or near
        // the time of the operation (for example, during aggregation). This is okay - we are mostly
        // paranoid that this mongos is very stale and want to prevent returning an error if the
        // collection was dropped a long time ago. Because of this, we are okay with piggy-backing
        // off another thread's request to refresh the cache, simply waiting for that request to
        // return instead of forcing another refresh.
        catalogCache->onStaleCollectionVersion(outputNs, boost::none);

        routingCtx = uassertStatusOK(getRoutingContext(expCtx->getOperationContext(), {outputNs}));
        const auto& cri = routingCtx->getCollectionRoutingInfo(outputNs);
        if (!cri.isSharded()) {
            return boost::none;
        }

        return cri.getCollectionVersion().placementVersion();
    }();

    auto docKeyPaths = collectDocumentKeyFieldsActingAsRouter(
        expCtx->getOperationContext(), outputNs, routingCtx.get());
    return {std::set<FieldPath>(std::make_move_iterator(docKeyPaths.begin()),
                                std::make_move_iterator(docKeyPaths.end())),
            placementVersion,
            SupportingUniqueIndex::Full};
}

}  // namespace mongo
