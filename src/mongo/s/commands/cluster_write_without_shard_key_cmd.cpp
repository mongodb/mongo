/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/commands/query_cmd/explain_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_util.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
struct TargetedWriteRequest {
    DatabaseName requestDbName;
    NamespaceString nss;
    BSONObj cmdObj;
    std::unique_ptr<RoutingContext> routingCtx;
};

// Returns true if the update or projection in the query requires information from the original
// query.
bool requiresOriginalQuery(OperationContext* opCtx,
                           const boost::optional<UpdateRequest>& updateRequest,
                           const boost::optional<NamespaceString> nss = boost::none,
                           const boost::optional<BSONObj>& query = boost::none,
                           const boost::optional<BSONObj>& projection = boost::none) {
    if (updateRequest) {
        ParsedUpdateForMongos parsedUpdate(opCtx, &updateRequest.get());
        uassertStatusOK(parsedUpdate.parseRequest());
        if (parsedUpdate.getDriver()->needMatchDetails()) {
            return true;
        }
    }

    // Only findAndModify can specify a projection for the pre/post image via the 'fields'
    // parameter.
    if (projection && !projection->isEmpty()) {
        auto findCommand = FindCommandRequest(*nss);
        findCommand.setFilter(*query);
        findCommand.setProjection(*projection);

        // We can ignore the collation for this check since we're only checking if the field name in
        // the projection requires extra information from the query.
        auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, findCommand).build();
        auto res = MatchExpressionParser::parse(findCommand.getFilter(),
                                                expCtx,
                                                ExtensionsCallbackNoop(),
                                                MatchExpressionParser::kAllowAllSpecialFeatures);
        auto proj = projection_ast::parseAndAnalyze(expCtx,
                                                    *projection,
                                                    res.getValue().get(),
                                                    *query,
                                                    ProjectionPolicies::findProjectionPolicies(),
                                                    false /* shouldOptimize */);
        return proj.requiresMatchDetails() ||
            DepsTracker::needsTextScoreMetadata(proj.metadataDeps());
    }
    return false;
}

/*
 * Helper function to construct a write request against the targetDocId for the write phase.
 *
 * Returns a TargetedWriteRequest struct.
 */
TargetedWriteRequest makeTargetWriteRequest(OperationContext* opCtx,
                                            const ShardId& shardId,
                                            const DatabaseName& dbName,
                                            const BSONObj& writeCmd,
                                            const BSONObj& targetDocId) {
    const auto commandName = writeCmd.firstElementFieldNameStringData();

    // Parse into OpMsgRequest to append the $db field, which is required for command
    // parsing.
    auto opMsgRequest =
        OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                    dbName,
                                    CommandHelpers::filterCommandRequestForPassthrough(writeCmd));

    DatabaseName requestDbName = dbName;
    boost::optional<BulkWriteCommandRequest> bulkWriteRequest;
    auto nss = [&] {
        if (commandName == BulkWriteCommandRequest::kCommandName) {
            bulkWriteRequest = BulkWriteCommandRequest::parse(
                opMsgRequest.body, IDLParserContext("_clusterWriteWithoutShardKeyForBulkWrite"));
            tassert(7298305,
                    "Only bulkWrite with a single op is allowed in _clusterWriteWithoutShardKey",
                    bulkWriteRequest->getOps().size() == 1);
            auto op = BulkWriteCRUDOp(bulkWriteRequest->getOps()[0]);
            tassert(
                7298306,
                str::stream()
                    << op.getType()
                    << " is not a supported opType for bulkWrite in _clusterQueryWithoutShardKey",
                op.getType() == BulkWriteCRUDOp::kUpdate ||
                    op.getType() == BulkWriteCRUDOp::kDelete);
            requestDbName = DatabaseName::kAdmin;
            return bulkWriteRequest->getNsInfo()[op.getNsInfoIdx()].getNs();
        } else {
            return CommandHelpers::parseNsCollectionRequired(dbName, writeCmd);
        }
    }();

    if (isRawDataOperation(opCtx) &&
        CollectionRoutingInfoTargeter{opCtx, nss}.timeseriesNamespaceNeedsRewrite(nss)) {
        nss = nss.makeTimeseriesBucketsNamespace();
    }

    auto swRoutingCtx = getRoutingContextForTxnCmd(opCtx, {nss});
    uassertStatusOK(swRoutingCtx.getStatus());
    auto& routingCtx = swRoutingCtx.getValue();
    const auto& cri = routingCtx->getCollectionRoutingInfo(nss);

    uassert(ErrorCodes::NamespaceNotSharded,
            "_clusterWriteWithoutShardKey can only be run against sharded collections.",
            cri.isSharded());
    const auto shardVersion = cri.getShardVersion(shardId);
    // For time-series collections, the 'targetDocId' corresponds to a measurement document's '_id'
    // field which is not guaranteed to exist and does not uniquely identify a measurement so we
    // cannot use this ID to reliably target a document in this write phase. Instead, we will
    // forward the full query to the chosen shard and it will be executed again on the target shard.
    const bool isTrackedTimeseries =
        CollectionRoutingInfoTargeter{opCtx, nss}.isTrackedTimeSeriesNamespace();
    BSONObjBuilder queryBuilder(isTrackedTimeseries ? BSONObj() : targetDocId);

    auto cmdObj = [&]() {
        // Parse original write command and set _id as query filter for new command object.
        if (commandName == BulkWriteCommandRequest::kCommandName) {
            invariant(bulkWriteRequest.has_value());
            auto op = BulkWriteCRUDOp(bulkWriteRequest->getOps()[0]);

            NamespaceInfoEntry newNsEntry = bulkWriteRequest->getNsInfo()[op.getNsInfoIdx()];
            newNsEntry.setShardVersion(shardVersion);

            if (op.getType() == BulkWriteCRUDOp::kUpdate) {
                // The update case.
                auto updateOp = op.getUpdate();

                if (updateOp->getSampleId()) {
                    bulkWriteRequest->setOriginalQuery(updateOp->getFilter());
                    bulkWriteRequest->setOriginalCollation(updateOp->getCollation());
                }

                // If the original query contains either a positional operator ($) or targets a
                // time-series collection, include the original query alongside the target doc.
                BulkWriteUpdateOp newUpdateOp = *updateOp;
                auto updateOpWithNamespace =
                    UpdateRequest(bulk_write_common::makeUpdateOpEntryFromUpdateOp(updateOp));
                updateOpWithNamespace.setNamespaceString(nss);
                updateOpWithNamespace.setLetParameters(bulkWriteRequest->getLet());
                updateOpWithNamespace.setBypassEmptyTsReplacement(
                    bulkWriteRequest->getBypassEmptyTsReplacement());

                if (requiresOriginalQuery(opCtx, updateOpWithNamespace) || isTrackedTimeseries) {
                    queryBuilder.appendElementsUnique(updateOp->getFilter());
                } else {
                    // Unset the collation and sort because targeting by _id uses default collation
                    // and we should uniquely target a single document by _id.
                    newUpdateOp.setCollation(boost::none);
                    newUpdateOp.setSort(boost::none);
                }

                newUpdateOp.setFilter(queryBuilder.obj());
                newUpdateOp.setNsInfoIdx(0);
                bulkWriteRequest->setOps({newUpdateOp});
            } else {
                // The delete case.
                auto deleteOp = op.getDelete();

                if (deleteOp->getSampleId()) {
                    bulkWriteRequest->setOriginalQuery(deleteOp->getFilter());
                    bulkWriteRequest->setOriginalCollation(deleteOp->getCollation());
                }

                // If the query targets a time-series collection, include the original query
                // alongside the target doc.
                BulkWriteDeleteOp newDeleteOp = *deleteOp;
                if (isTrackedTimeseries) {
                    queryBuilder.appendElementsUnique(deleteOp->getFilter());
                } else {
                    // Unset the collation because targeting by _id uses default collation.
                    newDeleteOp.setCollation(boost::none);
                }

                newDeleteOp.setFilter(queryBuilder.obj());
                newDeleteOp.setNsInfoIdx(0);
                bulkWriteRequest->setOps({newDeleteOp});
            }
            bulkWriteRequest->setNsInfo({newNsEntry});
            generic_argument_util::prepareRequestForInternalTransactionPassthrough(
                *bulkWriteRequest);

            return bulkWriteRequest->toBSON();
        } else if (commandName == write_ops::UpdateCommandRequest::kCommandName) {
            if (isRawDataOperation(opCtx) && isTrackedTimeseries &&
                nss.isTimeseriesBucketsCollection()) {
                opMsgRequest.body =
                    rewriteCommandForRawDataOperation<write_ops::UpdateCommandRequest>(
                        opMsgRequest.body, nss.coll());
            }
            auto updateRequest = write_ops::UpdateCommandRequest::parse(
                opMsgRequest.body, IDLParserContext("_clusterWriteWithoutShardKeyForUpdate"));

            // The original query and collation are sent along with the modified command for the
            // purposes of query sampling.
            if (updateRequest.getUpdates().front().getSampleId()) {
                auto writeCommandRequestBase =
                    write_ops::WriteCommandRequestBase(updateRequest.getWriteCommandRequestBase());
                writeCommandRequestBase.setOriginalQuery(updateRequest.getUpdates().front().getQ());
                writeCommandRequestBase.setOriginalCollation(
                    updateRequest.getUpdates().front().getCollation());
                updateRequest.setWriteCommandRequestBase(writeCommandRequestBase);
            }

            // If the original query contains either a positional operator ($) or targets a
            // time-series collection, include the original query alongside the target doc.
            auto updateOpWithNamespace = UpdateRequest(updateRequest.getUpdates().front());
            updateOpWithNamespace.setNamespaceString(updateRequest.getNamespace());
            updateOpWithNamespace.setLetParameters(updateRequest.getLet());
            updateOpWithNamespace.setBypassEmptyTsReplacement(
                updateRequest.getBypassEmptyTsReplacement());

            if (requiresOriginalQuery(opCtx, updateOpWithNamespace) || isTrackedTimeseries) {
                queryBuilder.appendElementsUnique(updateRequest.getUpdates().front().getQ());
            } else {
                // Unset the collation and sort because targeting by _id uses default collation and
                // we should uniquely target a single document by _id.
                updateRequest.getUpdates().front().setCollation(boost::none);
                updateRequest.getUpdates().front().setSort(boost::none);
            }

            updateRequest.getUpdates().front().setQ(queryBuilder.obj());

            generic_argument_util::prepareRequestForInternalTransactionPassthrough(updateRequest);

            auto batchedCommandRequest = BatchedCommandRequest(updateRequest);
            batchedCommandRequest.setShardVersion(shardVersion);
            return batchedCommandRequest.toBSON();
        } else if (commandName == write_ops::DeleteCommandRequest::kCommandName) {
            if (isRawDataOperation(opCtx) && isTrackedTimeseries &&
                nss.isTimeseriesBucketsCollection()) {
                opMsgRequest.body =
                    rewriteCommandForRawDataOperation<write_ops::DeleteCommandRequest>(
                        opMsgRequest.body, nss.coll());
            }
            auto deleteRequest = write_ops::DeleteCommandRequest::parse(
                opMsgRequest.body, IDLParserContext("_clusterWriteWithoutShardKeyForDelete"));

            // The original query and collation are sent along with the modified command for the
            // purposes of query sampling.
            if (deleteRequest.getDeletes().front().getSampleId()) {
                auto writeCommandRequestBase =
                    write_ops::WriteCommandRequestBase(deleteRequest.getWriteCommandRequestBase());
                writeCommandRequestBase.setOriginalQuery(deleteRequest.getDeletes().front().getQ());
                writeCommandRequestBase.setOriginalCollation(
                    deleteRequest.getDeletes().front().getCollation());
                deleteRequest.setWriteCommandRequestBase(writeCommandRequestBase);
            }

            // If the query targets a time-series collection, include the original query alongside
            // the target doc.
            if (isTrackedTimeseries) {
                queryBuilder.appendElementsUnique(deleteRequest.getDeletes().front().getQ());
            } else {
                // Unset the collation because targeting by _id uses default collation.
                deleteRequest.getDeletes().front().setCollation(boost::none);
            }

            deleteRequest.getDeletes().front().setQ(queryBuilder.obj());

            generic_argument_util::prepareRequestForInternalTransactionPassthrough(deleteRequest);

            auto batchedCommandRequest = BatchedCommandRequest(deleteRequest);
            batchedCommandRequest.setShardVersion(shardVersion);
            return batchedCommandRequest.toBSON();
        } else if (commandName == write_ops::FindAndModifyCommandRequest::kCommandName ||
                   commandName == write_ops::FindAndModifyCommandRequest::kCommandAlias) {
            if (isRawDataOperation(opCtx) && isTrackedTimeseries &&
                nss.isTimeseriesBucketsCollection()) {
                opMsgRequest.body =
                    rewriteCommandForRawDataOperation<write_ops::FindAndModifyCommandRequest>(
                        opMsgRequest.body, nss.coll());
            }
            auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
                opMsgRequest.body,
                IDLParserContext("_clusterWriteWithoutShardKeyForFindAndModify"));

            // The original query and collation are sent along with the modified command for the
            // purposes of query sampling.
            if (findAndModifyRequest.getSampleId()) {
                findAndModifyRequest.setOriginalQuery(findAndModifyRequest.getQuery());
                findAndModifyRequest.setOriginalCollation(findAndModifyRequest.getCollation());
            }

            if (findAndModifyRequest.getUpdate()) {
                auto updateRequest = UpdateRequest{};
                updateRequest.setNamespaceString(findAndModifyRequest.getNamespace());
                update::makeUpdateRequest(opCtx, findAndModifyRequest, boost::none, &updateRequest);

                // If the original query contains either a positional operator ($) or targets a
                // time-series collection, include the original query alongside the target doc.
                if (requiresOriginalQuery(opCtx,
                                          updateRequest,
                                          findAndModifyRequest.getNamespace(),
                                          findAndModifyRequest.getQuery(),
                                          findAndModifyRequest.getFields().value_or(BSONObj())) ||
                    isTrackedTimeseries) {
                    queryBuilder.appendElementsUnique(findAndModifyRequest.getQuery());
                } else {
                    // Unset the collation and sort because targeting by _id uses default collation
                    // and we should uniquely target a single document by _id.
                    findAndModifyRequest.setCollation(boost::none);
                    findAndModifyRequest.setSort(boost::none);
                }
            } else {
                // If the original query includes a positional operator ($) or targets a time-series
                // collection, include the original query alongside the target doc.
                if (requiresOriginalQuery(opCtx,
                                          boost::none,
                                          findAndModifyRequest.getNamespace(),
                                          findAndModifyRequest.getQuery(),
                                          findAndModifyRequest.getFields().value_or(BSONObj())) ||
                    isTrackedTimeseries) {
                    queryBuilder.appendElementsUnique(findAndModifyRequest.getQuery());
                } else {
                    // Unset the collation and sort because targeting by _id uses default collation
                    // and we should uniquely target a single document by _id.
                    findAndModifyRequest.setCollation(boost::none);
                    findAndModifyRequest.setSort(boost::none);
                }
            }

            findAndModifyRequest.setQuery(queryBuilder.obj());
            findAndModifyRequest.setShardVersion(shardVersion);
            generic_argument_util::prepareRequestForInternalTransactionPassthrough(
                findAndModifyRequest);

            return findAndModifyRequest.toBSON();
        } else {
            uasserted(ErrorCodes::InvalidOptions,
                      "_clusterWriteWithoutShardKey only supports update, delete, and "
                      "findAndModify commands.");
        }
    }();

    return TargetedWriteRequest{
        std::move(requestDbName), std::move(nss), cmdObj, std::move(routingCtx)};
}

class ClusterWriteWithoutShardKeyCmd : public TypedCommand<ClusterWriteWithoutShardKeyCmd> {
public:
    using Request = ClusterWriteWithoutShardKey;
    using Response = ClusterWriteWithoutShardKeyResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_clusterWriteWithoutShardKey must be run in a transaction.",
                    opCtx->inMultiDocumentTransaction());

            const auto writeCmd = request().getWriteCmd();
            const auto shardId = ShardId(std::string{request().getShardId()});
            const auto targetDocId = request().getTargetDocId();
            LOGV2_DEBUG(6962400,
                        2,
                        "Running write phase for a write without a shard key.",
                        "clientWriteRequest"_attr = redact(writeCmd),
                        "shardId"_attr = redact(shardId));

            if (OptionalBool::parseFromBSON(writeCmd[kRawDataFieldName])) {
                isRawDataOperation(opCtx) = true;
            }

            const auto targetedWriteRequest =
                makeTargetWriteRequest(opCtx, shardId, ns().dbName(), writeCmd, targetDocId);

            return routing_context_utils::runAndValidate(
                *targetedWriteRequest.routingCtx, [&](RoutingContext& routingCtx) {
                    LOGV2_DEBUG(7298307,
                                2,
                                "Constructed targeted write command for a write without shard key",
                                "cmdObj"_attr = redact(targetedWriteRequest.cmdObj));

                    AsyncRequestsSender::Request arsRequest(shardId, targetedWriteRequest.cmdObj);
                    std::vector<AsyncRequestsSender::Request> arsRequestVector({arsRequest});

                    MultiStatementTransactionRequestsSender ars(
                        opCtx,
                        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                        targetedWriteRequest.requestDbName,
                        std::move(arsRequestVector),
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        Shard::RetryPolicy::kNoRetry);

                    routingCtx.onRequestSentForNss(targetedWriteRequest.nss);

                    auto response = uassertStatusOK(ars.next().swResponse);
                    // We uassert on the extracted write status in order to preserve error labels
                    // for the transaction api to use in case of a retry.
                    uassertStatusOK(getStatusFromWriteCommandReply(response.data));
                    if (targetedWriteRequest.cmdObj.firstElementFieldNameStringData() ==
                            BulkWriteCommandRequest::kCommandName &&
                        response.data[BulkWriteCommandReply::kNErrorsFieldName].Int() != 0) {
                        // It was a bulkWrite, extract the first and only reply item and uassert on
                        // error so that we can fail the internal transaction correctly.
                        auto bulkWriteResponse = BulkWriteCommandReply::parse(
                            response.data,
                            IDLParserContext("BulkWriteCommandReply_clusterWriteWithoutShardKey"));
                        const auto& replyItems = bulkWriteResponse.getCursor().getFirstBatch();
                        tassert(7298309,
                                "unexpected bulkWrite reply for writes without shard key",
                                replyItems.size() == 1);
                        uassertStatusOK(replyItems[0].getStatus());
                    }

                    LOGV2_DEBUG(7298308,
                                2,
                                "Finished targeted write command for a write without shard key",
                                "response"_attr = redact(response.data));

                    return Response(response.data, shardId.toString());
                });
        }

    private:
        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            const auto shardId = ShardId(std::string{request().getShardId()});
            auto vts = auth::ValidatedTenancyScope::get(opCtx);
            const auto writeCmdObj = [&] {
                const auto explainCmdObj = request().getWriteCmd();
                const auto opMsgRequestExplainCmd =
                    OpMsgRequestBuilder::create(vts, ns().dbName(), explainCmdObj);
                auto explainRequest = ExplainCommandRequest::parse(
                    opMsgRequestExplainCmd.body,
                    IDLParserContext("_clusterWriteWithoutShardKeyExplain"));
                return explainRequest.getCommandParameter().getOwned();
            }();

            const auto targetedWriteRequest = makeTargetWriteRequest(
                opCtx, shardId, ns().dbName(), writeCmdObj, request().getTargetDocId());

            return routing_context_utils::runAndValidate(
                *targetedWriteRequest.routingCtx, [&](RoutingContext& routingCtx) {
                    const auto explainCmdObj =
                        ClusterExplain::wrapAsExplain(targetedWriteRequest.cmdObj, verbosity);

                    AsyncRequestsSender::Request arsRequest(shardId, explainCmdObj);
                    std::vector<AsyncRequestsSender::Request> arsRequestVector({arsRequest});

                    Timer timer;
                    MultiStatementTransactionRequestsSender ars(
                        opCtx,
                        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                        targetedWriteRequest.requestDbName,
                        std::move(arsRequestVector),
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        Shard::RetryPolicy::kNoRetry);

                    routingCtx.onRequestSentForNss(targetedWriteRequest.nss);

                    auto response = ars.next();
                    uassertStatusOK(response.swResponse);

                    const auto millisElapsed = timer.millis();

                    auto bodyBuilder = result->getBodyBuilder();
                    uassertStatusOK(
                        ClusterExplain::buildExplainResult(makeBlankExpressionContext(opCtx, ns()),
                                                           {response},
                                                           ClusterExplain::kWriteOnShards,
                                                           millisElapsed,
                                                           writeCmdObj,
                                                           &bodyBuilder));
                });
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    // In the current implementation of the Stable API, sub-operations run under a command in the
    // Stable API where a client specifies {apiStrict: true} are expected to also be Stable API
    // compliant, when they technically should not be. To satisfy this requirement, this command is
    // marked as part of the Stable API, but is not truly a part of it, since it is an internal-only
    // command.
    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }
};

MONGO_REGISTER_COMMAND(ClusterWriteWithoutShardKeyCmd).forRouter();

}  // namespace
}  // namespace mongo
