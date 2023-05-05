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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/update/update_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_find_and_modify_cmd.h"
#include "mongo/s/commands/cluster_write_cmd.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/write_ops/batch_write_op.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

// Returns true if the update or projection in the query requires information from the original
// query.
bool requiresOriginalQuery(OperationContext* opCtx,
                           const boost::optional<UpdateRequest>& updateRequest,
                           const boost::optional<NamespaceString> nss = boost::none,
                           const boost::optional<BSONObj>& query = boost::none,
                           const boost::optional<BSONObj>& projection = boost::none) {
    if (updateRequest) {
        ParsedUpdate parsedUpdate(
            opCtx, &updateRequest.get(), ExtensionsCallbackNoop(), CollectionPtr::null);
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
        auto expCtx =
            make_intrusive<ExpressionContext>(opCtx, findCommand, nullptr /* collator */, false);
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
        return proj.requiresMatchDetails();
    }
    return false;
}

BSONObj _createCmdObj(OperationContext* opCtx,
                      const ShardId& shardId,
                      const NamespaceString& nss,
                      const StringData& commandName,
                      const BSONObj& writeCmd,
                      const BSONObj& targetDocId) {
    const auto cri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
    uassert(ErrorCodes::InvalidOptions,
            "_clusterWriteWithoutShardKey can only be run against sharded collections.",
            cri.cm.isSharded());
    const auto shardVersion = cri.getShardVersion(shardId);
    // For time-series collections, the 'targetDocId' corresponds to a measurement document's '_id'
    // field which is not guaranteed to exist and does not uniquely identify a measurement so we
    // cannot use this ID to reliably target a document in this write phase. Instead, we will
    // forward the full query to the chosen shard and it will be executed again on the target shard.
    BSONObjBuilder queryBuilder(nss.isTimeseriesBucketsCollection() ? BSONObj() : targetDocId);

    // Parse into OpMsgRequest to append the $db field, which is required for command
    // parsing.
    const auto opMsgRequest = OpMsgRequest::fromDBAndBody(nss.db(), writeCmd);

    // Parse original write command and set _id as query filter for new command object.
    if (commandName == write_ops::UpdateCommandRequest::kCommandName) {
        auto updateRequest = write_ops::UpdateCommandRequest::parse(
            IDLParserContext("_clusterWriteWithoutShardKeyForUpdate"), opMsgRequest.body);

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

        // If the original query contains either a positional operator ($) or targets a time-series
        // collection, include the original query alongside the target doc.
        auto updateOpWithNamespace = UpdateRequest(updateRequest.getUpdates().front());
        updateOpWithNamespace.setNamespaceString(updateRequest.getNamespace());
        if (requiresOriginalQuery(opCtx, updateOpWithNamespace) ||
            nss.isTimeseriesBucketsCollection()) {
            queryBuilder.appendElementsUnique(updateRequest.getUpdates().front().getQ());
        } else {
            // Unset the collation because targeting by _id uses default collation.
            updateRequest.getUpdates().front().setCollation(boost::none);
        }

        updateRequest.getUpdates().front().setQ(queryBuilder.obj());

        auto batchedCommandRequest = BatchedCommandRequest(updateRequest);
        batchedCommandRequest.setShardVersion(shardVersion);
        return batchedCommandRequest.toBSON();
    } else if (commandName == write_ops::DeleteCommandRequest::kCommandName) {
        auto deleteRequest = write_ops::DeleteCommandRequest::parse(
            IDLParserContext("_clusterWriteWithoutShardKeyForDelete"), opMsgRequest.body);

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

        // If the query targets a time-series collection, include the original query alongside the
        // target doc.
        if (nss.isTimeseriesBucketsCollection()) {
            queryBuilder.appendElementsUnique(deleteRequest.getDeletes().front().getQ());
        } else {
            // Unset the collation because targeting by _id uses default collation.
            deleteRequest.getDeletes().front().setCollation(boost::none);
        }

        deleteRequest.getDeletes().front().setQ(queryBuilder.obj());

        auto batchedCommandRequest = BatchedCommandRequest(deleteRequest);
        batchedCommandRequest.setShardVersion(shardVersion);
        return batchedCommandRequest.toBSON();
    } else if (commandName == write_ops::FindAndModifyCommandRequest::kCommandName ||
               commandName == write_ops::FindAndModifyCommandRequest::kCommandAlias) {
        auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
            IDLParserContext("_clusterWriteWithoutShardKeyForFindAndModify"), opMsgRequest.body);

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
                nss.isTimeseriesBucketsCollection()) {
                queryBuilder.appendElementsUnique(findAndModifyRequest.getQuery());
            } else {
                // Unset the collation because targeting by _id uses default collation.
                findAndModifyRequest.setCollation(boost::none);
            }
        } else {
            // If the original query includes a positional operator ($) or targets a time-series
            // collection, include the original query alongside the target doc.
            if (requiresOriginalQuery(opCtx,
                                      boost::none,
                                      findAndModifyRequest.getNamespace(),
                                      findAndModifyRequest.getQuery(),
                                      findAndModifyRequest.getFields().value_or(BSONObj())) ||
                nss.isTimeseriesBucketsCollection()) {
                queryBuilder.appendElementsUnique(findAndModifyRequest.getQuery());
            } else {
                // Unset the collation because targeting by _id uses default collation.
                findAndModifyRequest.setCollation(boost::none);
            }
        }

        findAndModifyRequest.setQuery(queryBuilder.obj());

        // Drop the writeConcern as it cannot be specified for commands run in internal
        // transactions. This object will be used to construct the command request used by
        // _clusterWriteWithoutShardKey.
        findAndModifyRequest.setWriteConcern(boost::none);
        return appendShardVersion(findAndModifyRequest.toBSON({}), shardVersion);
    } else {
        uasserted(ErrorCodes::InvalidOptions,
                  "_clusterWriteWithoutShardKey only supports update, delete, and "
                  "findAndModify commands.");
    }
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
                    "_clusterWriteWithoutShardKey can only be run on Mongos",
                    isMongos());

            uassert(ErrorCodes::IllegalOperation,
                    "_clusterWriteWithoutShardKey must be run in a transaction.",
                    opCtx->inMultiDocumentTransaction());

            const auto writeCmd = request().getWriteCmd();
            const auto shardId = ShardId(request().getShardId().toString());
            LOGV2(6962400,
                  "Running write phase for a write without a shard key.",
                  "clientWriteRequest"_attr = writeCmd,
                  "shardId"_attr = shardId);

            const NamespaceString nss(
                CommandHelpers::parseNsCollectionRequired(ns().dbName(), writeCmd));
            const auto targetDocId = request().getTargetDocId();
            const auto commandName = writeCmd.firstElementFieldNameStringData();

            const BSONObj cmdObj =
                _createCmdObj(opCtx, shardId, nss, commandName, writeCmd, targetDocId);

            AsyncRequestsSender::Request arsRequest(shardId, cmdObj);
            std::vector<AsyncRequestsSender::Request> arsRequestVector({arsRequest});

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                request().getDbName(),
                std::move(arsRequestVector),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry);

            auto response = uassertStatusOK(ars.next().swResponse);
            // We uassert on the extracted write status in order to preserve error labels for the
            // transaction api to use in case of a retry.
            uassertStatusOK(getStatusFromWriteCommandReply(response.data));

            return Response(response.data, shardId.toString());
        }

    private:
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
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
    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ClusterWriteWithoutShardKeyCmd,
                                       feature_flags::gFeatureFlagUpdateOneWithoutShardKey);

}  // namespace
}  // namespace mongo
