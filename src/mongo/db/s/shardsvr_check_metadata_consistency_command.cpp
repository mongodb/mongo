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
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/store_possible_cursor.h"
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
                ShardsvrCheckMetadataConsistencyParticipant participantRequest{nss};
                participantRequest.setPrimaryShardId(primaryShardId);
                const auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                responses = sharding_util::sendCommandToShards(
                    opCtx,
                    nss.db(),
                    participantRequest.toBSON({}),
                    participants,
                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
            }

            // Merge responses from shards
            auto inconsistenciesMerged = _parseResponsesFromShards(opCtx, nss, responses);

            return metadata_consistency_util::_makeCursor(
                opCtx, inconsistenciesMerged, nss, request().toBSON({}), request().getCursor());
        }

    private:
        std::vector<MetadataInconsistencyItem> _parseResponsesFromShards(
            OperationContext* opCtx,
            const NamespaceString& nss,
            std::vector<AsyncRequestsSender::Response>& responses) {
            std::vector<MetadataInconsistencyItem> inconsistenciesMerged;
            for (auto&& cmdResponse : responses) {
                auto response = uassertStatusOK(std::move(cmdResponse.swResponse));
                uassertStatusOK(getStatusFromCommandResult(response.data));

                // TODO: SERVER-72667: Add privileges for getMore()
                auto transformedResponse = uassertStatusOK(
                    storePossibleCursor(opCtx,
                                        cmdResponse.shardId,
                                        *cmdResponse.shardHostAndPort,
                                        response.data,
                                        nss,
                                        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                        Grid::get(opCtx)->getCursorManager(),
                                        {}));

                auto checkMetadataConsistencyResponse =
                    CheckMetadataConsistencyResponse::parseOwned(
                        IDLParserContext("checkMetadataConsistencyResponse"),
                        std::move(transformedResponse));
                const auto& cursor = checkMetadataConsistencyResponse.getCursor();

                auto& shardInconsistencies = cursor.getFirstBatch();
                inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                             std::make_move_iterator(shardInconsistencies.begin()),
                                             std::make_move_iterator(shardInconsistencies.end()));

                const auto cursorManager = Grid::get(opCtx)->getCursorManager();

                const auto authzSession = AuthorizationSession::get(opCtx->getClient());
                const auto authChecker =
                    [&authzSession](const boost::optional<UserName>& userName) -> Status {
                    return authzSession->isCoauthorizedWith(userName)
                        ? Status::OK()
                        : Status(ErrorCodes::Unauthorized, "User not authorized to access cursor");
                };

                // Check out the cursor. If the cursor is not found, all data was retrieve in the
                // first batch.
                auto pinnedCursor =
                    cursorManager->checkOutCursor(cursor.getId(), opCtx, authChecker);
                if (!pinnedCursor.isOK()) {
                    continue;
                }

                // Iterate over the cursor and merge the results.
                while (true) {
                    auto next = pinnedCursor.getValue()->next();
                    if (!next.isOK() || next.getValue().isEOF()) {
                        break;
                    }

                    if (auto data = next.getValue().getResult()) {
                        auto inconsistency = MetadataInconsistencyItem::parseOwned(
                            IDLParserContext("MetadataInconsistencyItem"), data.get().getOwned());

                        inconsistenciesMerged.push_back(inconsistency);
                    }
                }
            }

            return inconsistenciesMerged;
        }

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
