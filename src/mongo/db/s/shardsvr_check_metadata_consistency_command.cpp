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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ShardsvrCheckMetadataConsistencyCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistency;
    using Response = CheckMetadataConsistencyResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            const auto& nss = ns();
            const auto& primaryShardId = ShardingState::get(opCtx)->shardId();
            std::vector<AsyncRequestsSender::Response> responses;

            {
                // Take a DDL lock on the database
                static constexpr StringData lockReason{"checkMetadataConsistency"_sd};
                auto ddlLockManager = DDLLockManager::get(opCtx);
                const auto dbDDLLock = ddlLockManager->lock(
                    opCtx, nss.db(), lockReason, DDLLockManager::kDefaultLockTimeout);

                // Send command to all shards
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                std::vector<AsyncRequestsSender::Request> requests;
                std::set<ShardId> initializedShards;

                for (const auto& shardId : participants) {
                    ShardsvrCheckMetadataConsistencyParticipant
                        checkMetadataConsistencyParticipantRequest(nss);
                    checkMetadataConsistencyParticipantRequest.setDbName(nss.db());
                    checkMetadataConsistencyParticipantRequest.setPrimaryShardId(primaryShardId);

                    requests.emplace_back(shardId,
                                          checkMetadataConsistencyParticipantRequest.toBSON({}));

                    initializedShards.emplace(shardId);
                }

                responses = gatherResponses(opCtx,
                                            nss.db(),
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            Shard::RetryPolicy::kIdempotent,
                                            requests);
            }

            // Merge responses from shards
            std::vector<MetadataInconsistency> inconsistenciesMerged;
            for (auto&& asyncResponse : responses) {
                auto response = uassertStatusOK(std::move(asyncResponse.swResponse));
                uassertStatusOK(getStatusFromCommandResult(response.data));

                auto data = CheckMetadataConsistencyResponse::parseOwned(
                    IDLParserContext("checkMetadataConsistencyResponse"), std::move(response.data));

                auto& shardInconsistencies = data.getInconsistencies();
                inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                             std::make_move_iterator(shardInconsistencies.begin()),
                                             std::make_move_iterator(shardInconsistencies.end()));
            }

            return Response{std::move(inconsistenciesMerged)};
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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

} shardsvrCheckMetadataConsistencyCommand;

}  // namespace
}  // namespace mongo
