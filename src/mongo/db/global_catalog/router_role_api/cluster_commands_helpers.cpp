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

#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/router_role_api/collection_uuid_mismatch.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/router_role_api/routing_context.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/shard_key_pattern_query_util.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/ddl/list_indexes_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace cluster::unsplittable {

ShardsvrReshardCollection makeMoveCollectionOrUnshardCollectionRequest(
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const boost::optional<ShardId>& destinationShard,
    ReshardingProvenanceEnum provenance,
    const boost::optional<bool>& performVerification,
    const boost::optional<std::int64_t>& oplogBatchApplierTaskCount) {
    ShardsvrReshardCollection shardsvrReshardCollection(nss);
    shardsvrReshardCollection.setDbName(dbName);

    ReshardCollectionRequest reshardCollectionRequest;
    reshardCollectionRequest.setKey(kUnsplittableCollectionShardKey);
    reshardCollectionRequest.setProvenance(provenance);

    if (destinationShard) {
        mongo::ShardKeyRange destinationRange(*destinationShard);
        destinationRange.setMin(kUnsplittableCollectionMinKey);
        destinationRange.setMax(kUnsplittableCollectionMaxKey);
        std::vector<mongo::ShardKeyRange> distribution = {destinationRange};
        reshardCollectionRequest.setShardDistribution(distribution);
    }

    reshardCollectionRequest.setForceRedistribution(true);
    reshardCollectionRequest.setNumInitialChunks(1);
    reshardCollectionRequest.setPerformVerification(performVerification);
    reshardCollectionRequest.setRecipientOplogBatchTaskCount(oplogBatchApplierTaskCount);

    shardsvrReshardCollection.setReshardCollectionRequest(std::move(reshardCollectionRequest));
    return shardsvrReshardCollection;
}

ShardsvrReshardCollection makeMoveCollectionRequest(
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const ShardId& destinationShard,
    ReshardingProvenanceEnum provenance,
    const boost::optional<bool>& performVerification,
    const boost::optional<std::int64_t>& oplogBatchApplierTaskCount) {
    return makeMoveCollectionOrUnshardCollectionRequest(
        dbName, nss, destinationShard, provenance, performVerification, oplogBatchApplierTaskCount);
}

ShardsvrReshardCollection makeUnshardCollectionRequest(
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const boost::optional<ShardId>& destinationShard,
    const boost::optional<bool>& performVerification,
    const boost::optional<std::int64_t>& oplogBatchApplierTaskCount) {
    return makeMoveCollectionOrUnshardCollectionRequest(
        dbName,
        nss,
        destinationShard,
        ReshardingProvenanceEnum::kUnshardCollection,
        performVerification,
        oplogBatchApplierTaskCount);
}

}  // namespace cluster::unsplittable

void appendWriteConcernErrorDetailToCmdResponse(const ShardId& shardId,
                                                WriteConcernErrorDetail wcError,
                                                BSONObjBuilder& responseBuilder) {
    auto status = wcError.toStatus();
    wcError.setStatus(
        status.withReason(str::stream() << status.reason() << " at " << shardId.toString()));

    responseBuilder.append("writeConcernError", wcError.toBSON());
}

void appendWriteConcernErrorToCmdResponse(const ShardId& shardId,
                                          const BSONElement& wcErrorElem,
                                          BSONObjBuilder& responseBuilder) {
    WriteConcernErrorDetail wcError = getWriteConcernErrorDetail(wcErrorElem);
    appendWriteConcernErrorDetailToCmdResponse(shardId, wcError, responseBuilder);
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContextWithDefaultsForTargeter(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& collation,
    const boost::optional<ExplainOptions::Verbosity>& verbosity,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {

    const auto noCollationSpecified = collation.isEmpty();
    auto&& cif = [&]() {
        if (noCollationSpecified) {
            return std::unique_ptr<CollatorInterface>{};
        } else {
            return uassertStatusOK(
                CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        }
    }();

    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces.emplace(nss, ResolvedNamespace(nss, std::vector<BSONObj>{}));

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .collator(std::move(cif))
                      .mongoProcessInterface(MongoProcessInterface::create(opCtx))
                      .ns(nss)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .fromRouter(true)
                      .bypassDocumentValidation(true)
                      .explain(verbosity)
                      .runtimeConstants(runtimeConstants)
                      .letParameters(letParameters)
                      .build();

    // Ignore the collator if the collection is untracked and the user did not specify a collator.
    if (!cri.hasRoutingTable() && noCollationSpecified) {
        expCtx->setIgnoreCollator();
    }
    return expCtx;
}

namespace {

std::vector<AsyncRequestsSender::Request> buildShardVersionedRequests(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const std::set<ShardId>& shardIds,
    const BSONObj& cmdObj,
    bool eligibleForSampling) {
    tassert(10162100, "Expected to find a routing table", cri.hasRoutingTable());
    const auto& cm = cri.getChunkManager();

    std::vector<AsyncRequestsSender::Request> requests;
    requests.reserve(shardIds.size());

    const auto targetedSampleId = eligibleForSampling
        ? analyze_shard_key::tryGenerateTargetedSampleId(expCtx->getOperationContext(),
                                                         nss,
                                                         cmdObj.firstElementFieldNameStringData(),
                                                         shardIds)
        : boost::none;

    auto appendSampleId = [&](const BSONObj& command, const ShardId& shardId) -> BSONObj {
        if (!targetedSampleId || !targetedSampleId->isFor(shardId)) {
            return command;
        }
        return analyze_shard_key::appendSampleId(command, targetedSampleId->getId());
    };

    if (cm.hasRoutingTable()) {
        for (const auto& shardId : shardIds) {
            BSONObj versionedCmd = appendShardVersion(cmdObj, cri.getShardVersion(shardId));
            versionedCmd = appendSampleId(versionedCmd, shardId);
            requests.emplace_back(shardId, std::move(versionedCmd));
        }
    }

    return requests;
}

AsyncRequestsSender::Request buildDatabaseVersionedRequest(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const ShardId& shardId,
    const BSONObj& cmdObj,
    bool eligibleForSampling) {
    tassert(10162101, "Expected to not find a routing table", !cri.hasRoutingTable());

    if (cri.getDbVersion().isFixed()) {
        // In case the database is fixed (e.g. 'admin' or 'config'), ignore the routing information
        // and create a request to the given targetted shard.
        return {shardId, cmdObj};
    }

    tassert(
        10162102,
        fmt::format("Expected exactly one shard matching the database primary shard when no shard "
                    "version is required. Found shard: {}, expected: {}, for namespace: {}.",
                    shardId.toString(),
                    cri.getDbPrimaryShardId().toString(),
                    nss.toStringForErrorMsg()),
        shardId == cri.getDbPrimaryShardId());

    BSONObj versionedCmd = appendShardVersion(cmdObj, ShardVersion::UNSHARDED());
    versionedCmd = appendDbVersionIfPresent(versionedCmd, cri.getDbVersion());

    const auto targetedSampleId = eligibleForSampling
        ? analyze_shard_key::tryGenerateTargetedSampleId(expCtx->getOperationContext(),
                                                         nss,
                                                         cmdObj.firstElementFieldNameStringData(),
                                                         {shardId})
        : boost::none;

    if (targetedSampleId && targetedSampleId->isFor(shardId)) {
        versionedCmd = analyze_shard_key::appendSampleId(versionedCmd, targetedSampleId->getId());
    }

    return {shardId, std::move(versionedCmd)};
}

}  // namespace

std::vector<AsyncRequestsSender::Request> buildVersionedRequests(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const std::set<ShardId>& shardIds,
    const BSONObj& cmdObj,
    bool eligibleForSampling) {
    if (cri.hasRoutingTable()) {
        return buildShardVersionedRequests(expCtx, nss, cri, shardIds, cmdObj, eligibleForSampling);
    }
    tassert(
        10162103,
        [&]() {
            std::vector<std::string> shardsStr;
            std::transform(shardIds.begin(),
                           shardIds.end(),
                           std::back_inserter(shardsStr),
                           [](const auto& shard) { return shard.toString(); });

            return fmt::format(
                "Expected exactly one shard. Found shardIds: [{}] for namespace: {}.",
                fmt::join(shardsStr, ", "),
                nss.toStringForErrorMsg());
        }(),
        shardIds.size() == 1);

    auto request = buildDatabaseVersionedRequest(
        expCtx, nss, cri, *shardIds.begin(), cmdObj, eligibleForSampling);
    return {std::move(request)};
}

namespace {

/**
 * Builds requests for each shard, that is affected by given query with given collation. Uses
 * buildVersionedRequests function to build the requests after determining the list of shards.
 *
 * If a shard is included in shardsToSkip, it will be excluded from the list returned to the
 * caller.
 */
std::vector<AsyncRequestsSender::Request> buildVersionedRequestsForTargetedShards(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const std::set<ShardId>& shardsToSkip,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation,
    bool eligibleForSampling = false) {
    std::set<ShardId> shardIds;
    if (!cri.hasRoutingTable()) {
        // The collection does not have a routing table. Target only the primary shard for the
        // database.
        shardIds.emplace(cri.getDbPrimaryShardId());
    } else {
        // The collection has a routing table. Target all shards that own chunks that match the
        // query.
        getShardIdsForQuery(expCtx, query, collation, cri.getChunkManager(), &shardIds);
    }
    for (const auto& shardToSkip : shardsToSkip) {
        shardIds.erase(shardToSkip);
    }
    return buildVersionedRequests(expCtx, nss, cri, shardIds, cmdObj, eligibleForSampling);
}

std::vector<AsyncRequestsSender::Response> gatherResponsesImpl(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    bool throwOnStaleShardVersionErrors,
    RoutingContext* routingCtx = nullptr) {

    // Send the requests.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        dbName,
        requests,
        readPref,
        retryPolicy);

    if (routingCtx && routingCtx->hasNss(nss)) {
        routingCtx->onRequestSentForNss(nss);
    }

    // Get the responses.
    std::vector<AsyncRequestsSender::Response> responses;  // Stores results by ShardId

    while (!ars.done()) {
        auto response = ars.next();

        auto status = response.swResponse.getStatus();
        if (status.isOK()) {
            // We successfully received a response.

            // Check for special errors that require throwing out any accumulated results.
            auto& responseObj = response.swResponse.getValue().data;
            status = getStatusFromCommandResult(responseObj);

            // If we specify to throw on stale shard version errors, then we will early exit
            // from examining results. Otherwise, we will allow stale shard version errors to
            // accumulate in the list of results.
            if (throwOnStaleShardVersionErrors &&
                ErrorCodes::isStaleShardVersionError(status.code())) {
                uassertStatusOK(status.withContext(str::stream()
                                                   << "got stale shardVersion response from shard "
                                                   << response.shardId << " at host "
                                                   << response.shardHostAndPort->toString()));
            }
            if (ErrorCodes::StaleDbVersion == status) {
                uassertStatusOK(status.withContext(
                    str::stream() << "got stale databaseVersion response from shard "
                                  << response.shardId << " at host "
                                  << response.shardHostAndPort->toString()));
            }
            if (ErrorCodes::CannotImplicitlyCreateCollection == status) {
                uassertStatusOK(status.withContext(
                    str::stream() << "got cannotImplicitlyCreateCollection response from shard "
                                  << response.shardId << " at host "
                                  << response.shardHostAndPort->toString()));
            }

            // In the case a read is performed against a view, the server can return an error
            // indicating that the underlying collection may be sharded. When this occurs the return
            // message will include an expanded view definition and collection namespace. We pass
            // the definition back to the caller by throwing the error. This allows the caller to
            // rewrite the request as an aggregation and retry it.
            if (ErrorCodes::CommandOnShardedViewNotSupportedOnMongod == status) {
                uassertStatusOK(status);
            }
        }
        responses.push_back(std::move(response));
    }

    return responses;
}

}  // namespace

std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    RoutingContext* routingCtx) {
    return gatherResponsesImpl(opCtx,
                               dbName,
                               nss,
                               readPref,
                               retryPolicy,
                               requests,
                               true /* throwOnStaleShardVersionErrors */,
                               routingCtx);
}

BSONObj appendDbVersionIfPresent(BSONObj cmdObj, const CachedDatabaseInfo& dbInfo) {
    return appendDbVersionIfPresent(std::move(cmdObj), dbInfo->getVersion());
}

BSONObj appendDbVersionIfPresent(BSONObj cmdObj, DatabaseVersion dbVersion) {
    if (dbVersion.isFixed()) {
        return cmdObj;
    }
    BSONObjBuilder cmdWithDbVersion(std::move(cmdObj));
    cmdWithDbVersion.append("databaseVersion", dbVersion.toBSON());
    return cmdWithDbVersion.obj();
}

BSONObj appendShardVersion(BSONObj cmdObj, ShardVersion version) {
    BSONObjBuilder cmdWithVersionBob(std::move(cmdObj));
    version.serialize(ShardVersion::kShardVersionField, &cmdWithVersionBob);
    return cmdWithVersionBob.obj();
}

BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              bool appendRC,
                              bool appendWC,
                              const BSONObj& cmdObj) {
    if (TransactionRouter::get(opCtx)) {
        // When running in a transaction, the rules are:
        // - Never apply writeConcern.  Applying writeConcern to terminal operations such as
        //   abortTransaction and commitTransaction is done directly by the TransactionRouter.
        // - Apply readConcern only if this is the first operation in the transaction.

        if (!opCtx->isStartingMultiDocumentTransaction()) {
            // Cannot apply either read or writeConcern, so short-circuit.
            return cmdObj;
        }

        if (!appendRC) {
            // First operation in transaction, but the caller has not requested readConcern be
            // applied, so there's nothing to do.
            return cmdObj;
        }

        // First operation in transaction, so ensure that writeConcern is not applied, then continue
        // and apply the readConcern.
        appendWC = false;
    }

    // Append all original fields to the new command.
    BSONObjBuilder output;
    bool seenReadConcern = false;
    bool seenWriteConcern = false;
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (appendRC && name == repl::ReadConcernArgs::kReadConcernFieldName) {
            seenReadConcern = true;
        }
        if (appendWC && name == WriteConcernOptions::kWriteConcernField) {
            seenWriteConcern = true;
        }
        if (!output.hasField(name)) {
            // If mongos selected atClusterTime, forward it to the shard.
            if (name == repl::ReadConcernArgs::kReadConcernFieldName &&
                readConcernArgs.wasAtClusterTimeSelected()) {
                output.appendElements(readConcernArgs.toBSON());
            } else {
                output.append(elem);
            }
        }
    }

    // Finally, add the new read/write concern.
    if (appendRC && !seenReadConcern) {
        output.appendElements(readConcernArgs.toBSON());
    }
    if (appendWC && !seenWriteConcern) {
        output.append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());
    }

    return output.obj();
}

BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              CommandInvocation* invocation,
                              const BSONObj& cmdObj) {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    const auto readConcernSupport = invocation->supportsReadConcern(
        readConcernArgs.getLevel(), readConcernArgs.isImplicitDefault());
    return applyReadWriteConcern(opCtx,
                                 readConcernSupport.readConcernSupport.isOK(),
                                 invocation->supportsWriteConcern(),
                                 cmdObj);
}

BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              BasicCommandWithReplyBuilderInterface* cmd,
                              const BSONObj& cmdObj) {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    const auto readConcernSupport = cmd->supportsReadConcern(
        cmdObj, readConcernArgs.getLevel(), readConcernArgs.isImplicitDefault());
    return applyReadWriteConcern(opCtx,
                                 readConcernSupport.readConcernSupport.isOK(),
                                 cmd->supportsWriteConcern(cmdObj),
                                 cmdObj);
}

std::vector<AsyncRequestsSender::Response> scatterGatherUnversionedTargetAllShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (auto&& shardId : Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx)) {
        requests.emplace_back(std::move(shardId), cmdObj);
    }

    return gatherResponses(opCtx, dbName, NamespaceString(dbName), readPref, retryPolicy, requests);
}

std::vector<AsyncRequestsSender::Response> scatterGatherUnversionedTargetConfigServerAndShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    stdx::unordered_set<ShardId> shardIds(allShardIds.begin(), allShardIds.end());
    auto configShardId = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();
    shardIds.insert(configShardId);

    std::vector<AsyncRequestsSender::Request> requests;
    for (auto&& shardId : shardIds)
        requests.emplace_back(std::move(shardId), cmdObj);

    return gatherResponses(opCtx, dbName, NamespaceString(dbName), readPref, retryPolicy, requests);
}

std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
    bool eligibleForSampling) {
    auto expCtx =
        makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                     nss,
                                                     routingCtx.getCollectionRoutingInfo(nss),
                                                     collation,
                                                     boost::none /*explainVerbosity*/,
                                                     letParameters,
                                                     runtimeConstants);
    return scatterGatherVersionedTargetByRoutingTable(expCtx,
                                                      routingCtx,
                                                      nss,
                                                      cmdObj,
                                                      readPref,
                                                      retryPolicy,
                                                      query,
                                                      collation,
                                                      eligibleForSampling);
}

[[nodiscard]] std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    bool eligibleForSampling) {
    const auto requests =
        buildVersionedRequestsForTargetedShards(expCtx,
                                                nss,
                                                routingCtx.getCollectionRoutingInfo(nss),
                                                {} /* shardsToSkip */,
                                                cmdObj,
                                                query,
                                                collation,
                                                eligibleForSampling);
    return gatherResponses(expCtx->getOperationContext(),
                           nss.dbName(),
                           nss,
                           readPref,
                           retryPolicy,
                           requests,
                           &routingCtx);
}

std::vector<AsyncRequestsSender::Response>
scatterGatherVersionedTargetByRoutingTableNoThrowOnStaleShardVersionErrors(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    const std::set<ShardId>& shardsToSkip,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               nss,
                                                               cri,
                                                               collation,
                                                               boost::none /*explainVerbosity*/,
                                                               letParameters,
                                                               runtimeConstants);
    const auto requests = buildVersionedRequestsForTargetedShards(
        expCtx, nss, cri, shardsToSkip, cmdObj, query, collation);

    return gatherResponsesImpl(opCtx,
                               nss.dbName(),
                               nss,
                               readPref,
                               retryPolicy,
                               requests,
                               false /* throwOnStaleShardVersionErrors */,
                               &routingCtx);
}

AsyncRequestsSender::Response executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const CachedDatabaseInfo& dbInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    // Attach only dbVersion
    const auto cmdObjWithDbVersion = appendDbVersionIfPresent(cmdObj, dbInfo);

    auto responses =
        gatherResponses(opCtx,
                        dbName,
                        NamespaceString(dbName),
                        readPref,
                        retryPolicy,
                        std::vector<AsyncRequestsSender::Request>{AsyncRequestsSender::Request(
                            dbInfo->getPrimary(), cmdObjWithDbVersion)});
    return std::move(responses.front());
}

AsyncRequestsSender::Response executeCommandAgainstShardWithMinKeyChunk(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy) {
    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               nss,
                                                               cri,
                                                               BSONObj() /*collation*/,
                                                               boost::none /*explainVerbosity*/,
                                                               boost::none /*letParameters*/,
                                                               boost::none /*runtimeConstants*/);

    const auto query = cri.isSharded()
        ? cri.getChunkManager().getShardKeyPattern().getKeyPattern().globalMin()
        : BSONObj();

    auto responses = gatherResponses(
        opCtx,
        nss.dbName(),
        nss,
        readPref,
        retryPolicy,
        buildVersionedRequestsForTargetedShards(
            expCtx, nss, cri, {} /* shardsToSkip */, cmdObj, query, BSONObj() /* collation */),
        &routingCtx);
    return std::move(responses.front());
}

// TODO SERVER-74478: Rewrite function to process AsyncRPC responses
RawResponsesResult appendRawResponses(
    OperationContext* opCtx,
    std::string* errmsg,
    BSONObjBuilder* output,
    const std::vector<AsyncRequestsSender::Response>& shardResponses,
    bool appendWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> successARSResponses;
    std::vector<std::pair<ShardId, BSONObj>> successResponsesReceived;
    std::vector<std::pair<ShardId, Status>> shardNotFoundErrorsReceived;

    // "Generic errors" are all errors that are not shardNotFound errors.
    std::vector<std::pair<ShardId, Status>> genericErrorsReceived;
    std::set<ShardId> shardsWithSuccessResponses;

    boost::optional<Status> firstStaleConfigErrorReceived;
    boost::optional<std::pair<ShardId, BSONElement>> firstWriteConcernErrorReceived;

    const auto processError = [&](const ShardId& shardId, const Status& status) {
        invariant(!status.isOK());
        // It is safe to pass `hasWriteConcernError` as false in the below check because operations
        // run inside transactions do not wait for write concern, except for commit and abort.
        if (TransactionRouter::get(opCtx) &&
            isTransientTransactionError(
                status.code(), false /*hasWriteConcernError*/, false /*isCommitOrAbort*/)) {
            // Re-throw on transient transaction errors to make sure appropriate error labels are
            // appended to the result.
            uassertStatusOK(status);
        }
        if (status.code() == ErrorCodes::ShardNotFound) {
            shardNotFoundErrorsReceived.emplace_back(shardId, status);
            return;
        }

        if (!firstStaleConfigErrorReceived && ErrorCodes::isStaleShardVersionError(status.code())) {
            firstStaleConfigErrorReceived.emplace(status);
        }

        genericErrorsReceived.emplace_back(shardId, status);
    };

    // Do a pass through all the received responses and group them into success, ShardNotFound, and
    // error responses.
    for (const auto& shardResponse : shardResponses) {
        const auto& shardId = shardResponse.shardId;

        const auto sendStatus = shardResponse.swResponse.getStatus();
        if (!sendStatus.isOK()) {
            processError(shardId, sendStatus);
            continue;
        }

        const auto& resObj = shardResponse.swResponse.getValue().data;
        if (!firstWriteConcernErrorReceived && resObj["writeConcernError"]) {
            firstWriteConcernErrorReceived.emplace(shardId, resObj["writeConcernError"]);
        }

        const auto commandStatus = getStatusFromCommandResult(resObj);
        if (!commandStatus.isOK()) {
            processError(shardId, commandStatus);
            continue;
        }

        successResponsesReceived.emplace_back(shardId, resObj);
        successARSResponses.emplace_back(shardResponse);
        shardsWithSuccessResponses.emplace(shardId);
    }

    // If all shards reported ShardNotFound, promote them all to generic errors.
    if (shardNotFoundErrorsReceived.size() == shardResponses.size()) {
        invariant(genericErrorsReceived.empty());
        genericErrorsReceived = std::move(shardNotFoundErrorsReceived);
    }

    // Append a 'raw' field containing the success responses and error responses.
    BSONObjBuilder rawShardResponses;
    const auto appendRawResponse = [&](const ShardId& shardId, const BSONObj& response) {
        // Try to report the response by the shard's full connection string.
        try {
            const auto shardConnString =
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId))
                    ->getConnString();
            rawShardResponses.append(shardConnString.toString(),
                                     CommandHelpers::filterCommandReplyForPassthrough(response));
        } catch (const ExceptionFor<ErrorCodes::ShardNotFound>&) {
            // If we could not get the shard's connection string, fall back to reporting the
            // response by shard id.
            rawShardResponses.append(shardId, response);
        }
    };
    for (const auto& success : successResponsesReceived) {
        appendRawResponse(success.first, success.second);
    }
    for (const auto& error : genericErrorsReceived) {
        BSONObjBuilder statusObjBob;
        CommandHelpers::appendCommandStatusNoThrow(statusObjBob, error.second);
        appendRawResponse(error.first, statusObjBob.obj());
    }
    output->append("raw", rawShardResponses.done());

    // Always add the WCE if any when the command fails
    if (firstWriteConcernErrorReceived &&
        (appendWriteConcernError || !genericErrorsReceived.empty())) {
        appendWriteConcernErrorToCmdResponse(
            firstWriteConcernErrorReceived->first, firstWriteConcernErrorReceived->second, *output);
    }

    // If there were no errors, report success.
    if (genericErrorsReceived.empty()) {
        return {
            true, shardsWithSuccessResponses, successARSResponses, firstStaleConfigErrorReceived};
    }

    // There was an error. Choose the first error as the top-level error.
    auto& firstError = genericErrorsReceived.front().second;

    if (firstError.code() == ErrorCodes::CollectionUUIDMismatch &&
        !firstError.extraInfo<CollectionUUIDMismatchInfo>()->actualCollection()) {
        // The first error is a CollectionUUIDMismatchInfo but it doesn't contain an actual
        // namespace. It's possible that the actual namespace is unsharded, in which case only the
        // error from the primary shard will contain this information. Iterate through the errors to
        // see if this is the case. Note that this can fail with unsplittable collections as we
        // might only contact the owning shard and not have any errors from the primary shard.
        bool hasFoundCollectionName = false;
        for (const auto& error : genericErrorsReceived) {
            if (error.second.code() == ErrorCodes::CollectionUUIDMismatch &&
                error.second.extraInfo<CollectionUUIDMismatchInfo>()->actualCollection()) {
                firstError = error.second;
                hasFoundCollectionName = true;
                break;
            }
        }
        // If we didn't find the error here we must contact the primary shard manually to populate
        // the CollectionUUIDMismatch with the correct collection name.
        if (!hasFoundCollectionName) {
            firstError = populateCollectionUUIDMismatch(opCtx, firstError);
        }
    }

    output->append("code", firstError.code());
    output->append("codeName", ErrorCodes::errorString(firstError.code()));
    if (auto extra = firstError.extraInfo()) {
        extra->serialize(output);
    }
    *errmsg = firstError.reason();

    return {false, shardsWithSuccessResponses, successARSResponses, firstStaleConfigErrorReceived};
}

bool appendEmptyResultSet(OperationContext* opCtx,
                          BSONObjBuilder& result,
                          Status status,
                          const NamespaceString& nss) {
    invariant(!status.isOK());

    CurOp::get(opCtx)->debug().additiveMetrics.nreturned = 0;
    CurOp::get(opCtx)->debug().nShards = 0;

    if (status == ErrorCodes::NamespaceNotFound) {
        // New (command) style reply
        appendCursorResponseObject(0LL, nss, BSONArray(), boost::none, &result);

        return true;
    }

    uassertStatusOK(status);
    return true;
}

std::set<ShardId> getTargetedShardsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const CollectionRoutingInfo& cri,
                                            const BSONObj& query,
                                            const BSONObj& collation) {
    if (cri.hasRoutingTable()) {
        // The collection has a routing table. Use it to decide which shards to target based on the
        // query and collation.
        std::set<ShardId> shardIds;
        getShardIdsForQuery(expCtx, query, collation, cri.getChunkManager(), &shardIds);
        return shardIds;
    }

    // The collection does not have a routing table. Target only the primary shard for the database.
    return {cri.getDbPrimaryShardId()};
}

std::set<ShardId> getTargetedShardsForCanonicalQuery(const CanonicalQuery& query,
                                                     const CollectionRoutingInfo& cri) {
    if (cri.hasRoutingTable()) {
        const auto& cm = cri.getChunkManager();

        // The collection has a routing table. Use it to decide which shards to target based on the
        // query and collation.

        // If the query has a hint or geo expression, fall back to re-creating a find command from
        // scratch. Hint can interfere with query planning, which we rely on for targeting. Shard
        // targeting modifies geo queries and this helper shouldn't have a side effect on 'query'.
        const auto& findCommand = query.getFindCommandRequest();
        if (!findCommand.getHint().isEmpty() ||
            QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(),
                                        MatchExpression::GEO_NEAR)) {
            return getTargetedShardsForQuery(
                query.getExpCtx(), cri, findCommand.getFilter(), findCommand.getCollation());
        }

        query.getExpCtx()->setUUID(cm.getUUID());

        // 'getShardIdsForCanonicalQuery' assumes that the ExpressionContext has the appropriate
        // collation set. Here, if the query collation is empty, we use the collection default
        // collation for targeting.
        const auto& collation = query.getFindCommandRequest().getCollation();
        if (collation.isEmpty() && cm.getDefaultCollator()) {
            auto defaultCollator = cm.getDefaultCollator();
            query.getExpCtx()->setCollator(defaultCollator->clone());
        }

        std::set<ShardId> shardIds;
        getShardIdsForCanonicalQuery(query, cm, &shardIds);
        return shardIds;
    }

    // In the event of an untracked collection, we will discover the collection default collation on
    // the primary shard. As such, we don't forward the simple collation.
    if (query.getFindCommandRequest().getCollation().isEmpty()) {
        query.getExpCtx()->setIgnoreCollator();
    }

    // The collection does not have a routing table. Target only the primary shard for the database.
    return {cri.getDbPrimaryShardId()};
}

std::vector<AsyncRequestsSender::Request> getVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               nss,
                                                               cri,
                                                               collation,
                                                               boost::none /*explainVerbosity*/,
                                                               letParameters,
                                                               runtimeConstants);
    return buildVersionedRequestsForTargetedShards(
        expCtx, nss, cri, {} /* shardsToSkip */, cmdObj, query, collation);
}

StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoForTxnCmd_DEPRECATED(
    OperationContext* opCtx, const NamespaceString& nss) {
    // When in a multi-document transaction, allow getting routing info from the
    // CatalogCache even though locks may be held. The CatalogCache will throw
    // CannotRefreshDueToLocksHeld if the entry is not already cached.
    //
    // Note that we only do this if we indeed hold a lock. Otherwise first executions on a mongos
    // would cause this to unnecessarily throw a transient CannotRefreshDueToLocksHeld error. This
    // would force the client to retry the entire transaction even if it hasn't yet executed
    // anything.
    const auto allowLocks =
        opCtx->inMultiDocumentTransaction() && shard_role_details::getLocker(opCtx)->isLocked();

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    invariant(catalogCache);

    auto argsAtClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    if (argsAtClusterTime) {
        return catalogCache->getCollectionRoutingInfoAt(
            opCtx, nss, argsAtClusterTime->asTimestamp(), allowLocks);
    }

    // Return the latest routing table if not running in a transaction with snapshot level read
    // concern.
    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        if (auto atClusterTime = txnRouter.getSelectedAtClusterTime()) {
            return catalogCache->getCollectionRoutingInfoAt(
                opCtx, nss, atClusterTime->asTimestamp(), allowLocks);
        }
    }

    return catalogCache->getCollectionRoutingInfo(opCtx, nss, allowLocks);
}

StatusWith<std::unique_ptr<RoutingContext>> getRoutingContext(
    OperationContext* opCtx, const std::vector<NamespaceString>& nssList, bool allowLocks) {
    try {
        auto routingCtx = std::make_unique<RoutingContext>(opCtx, std::move(nssList), allowLocks);
        return routingCtx;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<std::unique_ptr<RoutingContext>> getRoutingContextForTxnCmd(
    OperationContext* opCtx, const std::vector<NamespaceString>& nssList) {
    // When in a multi-document transaction, allow getting routing info from the
    // CatalogCache even though locks may be held. The CatalogCache will throw
    // CannotRefreshDueToLocksHeld if the entry is not already cached.
    //
    // Note that we only do this if we indeed hold a lock. Otherwise first executions on a mongos
    // would cause this to unnecessarily throw a transient CannotRefreshDueToLocksHeld error. This
    // would force the client to retry the entire transaction even if it hasn't yet executed
    // anything.
    const auto allowLocks =
        opCtx->inMultiDocumentTransaction() && shard_role_details::getLocker(opCtx)->isLocked();

    return getRoutingContext(opCtx, nssList, allowLocks);
}

CollectionRoutingInfo getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(
    OperationContext* opCtx, const NamespaceString& nss) {
    const auto& catalogCache = Grid::get(opCtx)->catalogCache();

    catalogCache->onStaleCollectionVersion(nss, boost::none /* wantedVersion */);

    auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Command not supported on unsharded collection "
                          << nss.toStringForErrorMsg(),
            cri.isSharded());
    return cri;
};

BSONObj forceReadConcernLocal(OperationContext* opCtx, BSONObj& cmd) {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto atClusterTime = readConcernArgs.getArgsAtClusterTime();
    auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime();
    BSONObjBuilder bob(cmd.removeField(repl::ReadConcernArgs::kReadConcernFieldName));

    repl::ReadConcernIdl newReadConcern;
    newReadConcern.setLevel(repl::ReadConcernLevel::kLocalReadConcern);
    // We should carry over the atClusterTime/afterClusterTime to keep causal consistency.
    if (atClusterTime) {
        // atClusterTime is only supported in snapshot readConcern, so we use afterClusterTime
        // instead.
        newReadConcern.setAfterClusterTime(atClusterTime);
    } else if (afterClusterTime) {
        newReadConcern.setAfterClusterTime(afterClusterTime);
    }

    {
        BSONObjBuilder newReadConcernBuilder(
            bob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName));
        newReadConcern.serialize(&newReadConcernBuilder);
    }

    return bob.obj();
}

StatusWith<Shard::QueryResponse> loadIndexesFromAuthoritativeShard(OperationContext* opCtx,
                                                                   RoutingContext& routingCtx,
                                                                   const NamespaceString& nss) {
    const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
    auto [indexShard, listIndexesCmd] = [&]() -> std::pair<std::shared_ptr<Shard>, BSONObj> {
        ListIndexes listIndexesCmd(nss);
        // For viewless timeseries collections, fetch the raw indexes instead of user-visible ones.
        // (Note that for all other collection types, this parameter has no effect.)
        // This is hardcoded, since all current callers of this function expect raw indexes.
        if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            listIndexesCmd.setRawData(true);
        }
        setReadWriteConcern(opCtx, listIndexesCmd, true, false);
        auto cmdNoVersion = listIndexesCmd.toBSON();

        // force the read concern level to "local" as other values are not supported for listIndexes
        cmdNoVersion = forceReadConcernLocal(opCtx, cmdNoVersion);

        if (cri.hasRoutingTable()) {
            // For a collection that has a routing table, we must load indexes from a shard with
            // chunks. For consistency with cluster listIndexes, load from the shard that owns
            // the minKey chunk.
            const auto& cm = cri.getChunkManager();
            const auto minKeyShardId = cm.getMinKeyShardIdWithSimpleCollation();
            return {
                uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, minKeyShardId)),
                appendShardVersion(cmdNoVersion, cri.getShardVersion(minKeyShardId))};
        } else {
            // For a collection without routing table, the primary shard will have correct indexes.
            // Attach dbVersion + shardVersion: UNSHARDED.
            const auto cmdObjWithShardVersion = !cri.getDbVersion().isFixed()
                ? appendShardVersion(cmdNoVersion, ShardVersion::UNSHARDED())
                : cmdNoVersion;
            return {uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(
                        opCtx, cri.getDbPrimaryShardId())),
                    appendDbVersionIfPresent(cmdObjWithShardVersion, cri.getDbVersion())};
        }
    }();

    routingCtx.onRequestSentForNss(nss);
    return indexShard->runExhaustiveCursorCommand(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        nss.dbName(),
        listIndexesCmd,
        opCtx->hasDeadline() ? opCtx->getRemainingMaxTimeMillis() : Milliseconds(-1));
}

StatusWith<boost::optional<int64_t>> addLimitAndSkipForShards(boost::optional<int64_t> limit,
                                                              boost::optional<int64_t> skip) {
    boost::optional<int64_t> newLimit;
    if (limit) {
        long long newLimitVal;
        if (overflow::add(*limit, skip.value_or(0), &newLimitVal)) {
            return Status(ErrorCodes::Overflow,
                          str::stream() << "sum of limit and skip cannot be represented as "
                                           "a 64-bit integer, limit: "
                                        << *limit << ", skip: " << skip.value_or(0));
        }

        newLimit = newLimitVal;
    }

    return newLimit;
}

RoutingContext& translateNssForRawDataAccordingToRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& originalNss,
    const CollectionRoutingInfoTargeter& targeter,
    RoutingContext& originalRoutingCtx,
    std::function<void(const NamespaceString& translatedNss)> translateNssFunc) {
    const bool shouldTranslateCmdForRawDataOperation =
        isRawDataOperation(opCtx) && targeter.timeseriesNamespaceNeedsRewrite(originalNss);

    if (shouldTranslateCmdForRawDataOperation) {
        const auto& nss = originalNss.makeTimeseriesBucketsNamespace();
        translateNssFunc(nss);

        // Swap the routingCtx with the CollectionRoutingInfoTargeter one, which
        // has been implicitly initialized with the buckets nss.
        originalRoutingCtx.skipValidation();
        return targeter.getRoutingCtx();
    }
    return originalRoutingCtx;
}

}  // namespace mongo
