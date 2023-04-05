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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

/**
 * Returns the routing information for the namespace set on the passed ExpressionContext. Also
 * verifies that the ExpressionContext's UUID, if present, matches that of the routing table entry.
 */
StatusWith<CollectionRoutingInfo> getCollectionRoutingInfo(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    auto swRoutingInfo = catalogCache->getCollectionRoutingInfo(expCtx->opCtx, expCtx->ns);
    // Additionally check that the ExpressionContext's UUID matches the collection routing info.
    if (swRoutingInfo.isOK() && expCtx->uuid && swRoutingInfo.getValue().cm.isSharded()) {
        if (!swRoutingInfo.getValue().cm.uuidMatches(*expCtx->uuid)) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The UUID of collection " << expCtx->ns.ns()
                                  << " changed; it may have been dropped and re-created."};
        }
    }
    return swRoutingInfo;
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const BSONObj& index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    // Retrieve the collation from the index, or default to the simple collation.
    const auto collation = uassertStatusOK(
        CollatorFactoryInterface::get(expCtx->opCtx->getServiceContext())
            ->makeFromBSON(index.hasField(IndexDescriptor::kCollationFieldName)
                               ? index.getObjectField(IndexDescriptor::kCollationFieldName)
                               : CollationSpec::kSimpleSpec));

    // SERVER-5335: The _id index does not report to be unique, but in fact is unique.
    auto isIdIndex = index[IndexDescriptor::kIndexNameFieldName].String() == "_id_";
    return (isIdIndex || index.getBoolField(IndexDescriptor::kUniqueFieldName)) &&
        !index.hasField(IndexDescriptor::kPartialFilterExprFieldName) &&
        CommonProcessInterface::keyPatternNamesExactPaths(
               index.getObjectField(IndexDescriptor::kKeyPatternFieldName), uniqueKeyPaths) &&
        CollatorInterface::collatorsMatch(collation.get(), expCtx->getCollator());
}

}  // namespace

std::unique_ptr<CommonProcessInterface::WriteSizeEstimator>
MongosProcessInterface::getWriteSizeEstimator(OperationContext* opCtx,
                                              const NamespaceString& ns) const {
    return std::make_unique<TargetPrimaryWriteSizeEstimator>();
}

std::unique_ptr<Pipeline, PipelineDeleter> MongosProcessInterface::attachCursorSourceToPipeline(
    Pipeline* ownedPipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    // On mongos we can't have local cursors.
    tassert(5530900,
            "shardTargetingPolicy cannot be kNotAllowed on mongos",
            shardTargetingPolicy != ShardTargetingPolicy::kNotAllowed);

    return sharded_agg_helpers::attachCursorToPipeline(
        ownedPipeline, shardTargetingPolicy, std::move(readConcern));
}

std::unique_ptr<Pipeline, PipelineDeleter> MongosProcessInterface::attachCursorSourceToPipeline(
    const AggregateCommandRequest& aggRequest,
    Pipeline* pipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    // On mongos we can't have local cursors.
    tassert(7393502,
            "shardTargetingPolicy cannot be kNotAllowed on mongos",
            shardTargetingPolicy != ShardTargetingPolicy::kNotAllowed);
    std::unique_ptr<Pipeline, PipelineDeleter> targetPipeline(pipeline,
                                                              PipelineDeleter(expCtx->opCtx));
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
    UUID collectionUUID,
    const Document& filter,
    boost::optional<BSONObj> readConcern) {
    auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID);

    // Create the find command to be dispatched to the shard(s) in order to return the post-image.
    auto filterObj = filter.toBson();
    BSONObjBuilder cmdBuilder;
    bool findCmdIsByUuid(foreignExpCtx->uuid);
    if (findCmdIsByUuid) {
        foreignExpCtx->uuid->appendToBuilder(&cmdBuilder, "find");
    } else {
        cmdBuilder.append("find", nss.coll());
    }
    cmdBuilder.append("filter", filterObj);
    cmdBuilder.append("allowSpeculativeMajorityRead", true);
    if (readConcern) {
        cmdBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName, *readConcern);
    }

    try {
        auto findCmd = cmdBuilder.obj();
        auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
        auto shardResults = shardVersionRetry(
            expCtx->opCtx,
            catalogCache,
            foreignExpCtx->ns,
            str::stream() << "Looking up document matching " << redact(filter.toBson()),
            [&]() -> std::vector<RemoteCursor> {
                // Verify that the collection exists, with the correct UUID.
                auto cri = uassertStatusOK(getCollectionRoutingInfo(foreignExpCtx));

                // Finalize the 'find' command object based on the routing table information.
                if (findCmdIsByUuid && cri.cm.isSharded()) {
                    // Find by UUID and shard versioning do not work together (SERVER-31946).  In
                    // the sharded case we've already checked the UUID, so find by namespace is
                    // safe.  In the unlikely case that the collection has been deleted and a new
                    // collection with the same name created through a different mongos or the
                    // collection had its shard key refined, the shard version will be detected as
                    // stale, as shard versions contain an 'epoch' field unique to the collection.
                    findCmd = findCmd.addField(BSON("find" << nss.coll()).firstElement());
                    findCmdIsByUuid = false;
                }

                // Build the versioned requests to be dispatched to the shards. Typically, only a
                // single shard will be targeted here; however, in certain cases where only the _id
                // is present, we may need to scatter-gather the query to all shards in order to
                // find the document.
                auto requests = getVersionedRequestsForTargetedShards(
                    expCtx->opCtx, nss, cri, findCmd, filterObj, CollationSpec::kSimpleSpec);

                // Dispatch the requests. The 'establishCursors' method conveniently prepares the
                // result into a vector of cursor responses for us.
                return establishCursors(
                    expCtx->opCtx,
                    Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
                    nss,
                    ReadPreferenceSetting::get(expCtx->opCtx),
                    std::move(requests),
                    false);
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

BSONObj MongosProcessInterface::_reportCurrentOpForClient(
    OperationContext* opCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps,
    CurrentOpBacktraceMode backtraceMode) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(opCtx,
                                    client,
                                    (truncateOps == CurrentOpTruncateMode::kTruncateOps),
                                    (backtraceMode == CurrentOpBacktraceMode::kIncludeBacktrace),
                                    &builder);

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

    const bool authEnabled =
        AuthorizationSession::get(opCtx->getClient())->getAuthorizationManager().isAuthEnabled();

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
    auto cursorManager = Grid::get(expCtx->opCtx->getServiceContext())->getCursorManager();
    invariant(cursorManager);
    return cursorManager->getIdleCursors(expCtx->opCtx, userMode);
}

bool MongosProcessInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    auto [cm, _] =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    return cm.isSharded();
}

bool MongosProcessInterface::fieldsHaveSupportingUniqueIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& fieldPaths) const {
    // Get a list of indexes from a shard with correct indexes for the namespace. For an unsharded
    // collection, this is the current primary shard for the database, and for a sharded collection,
    // this is any shard that currently owns at least one chunk. This helper sends database and/or
    // shard versions to ensure this router is not stale, but will not automatically retry if either
    // version is stale.
    auto response = loadIndexesFromAuthoritativeShard(expCtx->opCtx, nss);

    // If the namespace does not exist, then the field paths *must* be _id only.
    if (response.getStatus() == ErrorCodes::NamespaceNotFound) {
        return fieldPaths == std::set<FieldPath>{"_id"};
    }
    uassertStatusOK(response);

    const auto& indexes = response.getValue().docs;
    return std::any_of(indexes.begin(), indexes.end(), [&expCtx, &fieldPaths](const auto& index) {
        return supportsUniqueKey(expCtx, index, fieldPaths);
    });
}

std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
MongosProcessInterface::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::set<FieldPath>> fieldPaths,
    boost::optional<ChunkVersion> targetCollectionPlacementVersion,
    const NamespaceString& outputNs) const {
    invariant(expCtx->inMongos);
    uassert(51179,
            "Received unexpected 'targetCollectionPlacementVersion' on mongos",
            !targetCollectionPlacementVersion);

    if (fieldPaths) {
        uassert(51190,
                "Cannot find index to verify that join fields will be unique",
                fieldsHaveSupportingUniqueIndex(expCtx, outputNs, *fieldPaths));

        // If the user supplies the 'fields' array, we don't need to attach a ChunkVersion for
        // the shards since we are not at risk of 'guessing' the wrong shard key.
        return {*fieldPaths, boost::none};
    }

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
    boost::optional<ShardVersion> targetCollectionVersion =
        refreshAndGetCollectionVersion(expCtx, outputNs);
    targetCollectionPlacementVersion = targetCollectionVersion
        ? boost::make_optional(targetCollectionVersion->placementVersion())
        : boost::none;

    auto docKeyPaths = collectDocumentKeyFieldsActingAsRouter(expCtx->opCtx, outputNs);
    return {std::set<FieldPath>(std::make_move_iterator(docKeyPaths.begin()),
                                std::make_move_iterator(docKeyPaths.end())),
            targetCollectionPlacementVersion};
}

}  // namespace mongo
