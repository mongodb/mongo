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
#include "mongo/db/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/write_ops/batch_write_op.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

BSONObj _createCmdObj(const BSONObj& writeCmd,
                      const StringData& commandName,
                      const BSONObj& targetDocId,
                      const NamespaceString& nss) {
    // Drop collation and writeConcern as
    // targeting by _id uses default collation and writeConcern cannot be specified for
    // commands run in internal transactions. This object will be used to construct the command
    // request used by clusterWriteWithoutShardKey.
    BSONObjBuilder writeCmdObjBuilder(
        writeCmd.removeFields(std::set<std::string>{"collation", "writeConcern"}));
    writeCmdObjBuilder.appendElementsUnique(BSON("$db" << nss.dbName().toString()));
    auto writeCmdObj = writeCmdObjBuilder.obj();

    // Parse original write command and set _id as query filter for new command object.
    if (commandName == "update") {
        auto parsedUpdateRequest = write_ops::UpdateCommandRequest::parse(
            IDLParserContext("_clusterWriteWithoutShardKey"), writeCmdObj);
        parsedUpdateRequest.getUpdates().front().setQ(targetDocId);
        return parsedUpdateRequest.toBSON(BSONObj());
    } else if (commandName == "delete") {
        auto parsedDeleteRequest = write_ops::DeleteCommandRequest::parse(
            IDLParserContext("_clusterWriteWithoutShardKey"), writeCmdObj);
        parsedDeleteRequest.getDeletes().front().setQ(targetDocId);
        return parsedDeleteRequest.toBSON(BSONObj());
    } else if (commandName == "findandmodify" || commandName == "findAndModify") {
        auto parsedFindAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
            IDLParserContext("_clusterWriteWithoutShardKey"), writeCmdObj);
        parsedFindAndModifyRequest.setQuery(targetDocId);
        return parsedFindAndModifyRequest.toBSON(BSONObj());
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

            const BSONObj cmdObj = _createCmdObj(writeCmd, commandName, targetDocId, nss);

            const auto cm = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
            uassert(ErrorCodes::InvalidOptions,
                    "_clusterWriteWithoutShardKey can only be run against sharded collections.",
                    cm.isSharded());

            ChunkVersion placementVersion = cm.getVersion(shardId);
            auto versionedCmdObj = appendShardVersion(
                cmdObj,
                ShardVersion(placementVersion, boost::optional<CollectionIndexes>(boost::none)));

            AsyncRequestsSender::Request arsRequest(shardId, versionedCmdObj);
            std::vector<AsyncRequestsSender::Request> arsRequestVector({arsRequest});

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                request().getDbName().toString(),
                std::move(arsRequestVector),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry);

            auto response = uassertStatusOK(ars.next().swResponse);
            return Response(response.data);
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
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(ClusterWriteWithoutShardKeyCmd,
                                       feature_flags::gFeatureFlagUpdateOneWithoutShardKey);

}  // namespace
}  // namespace mongo
