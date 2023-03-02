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

MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss) {
    if (nss.isAdminDB()) {
        return MetadataConsistencyCommandLevelEnum::kClusterLevel;
    } else if (nss.isCollectionlessCursorNamespace()) {
        return MetadataConsistencyCommandLevelEnum::kDatabaseLevel;
    } else {
        return MetadataConsistencyCommandLevelEnum::kCollectionLevel;
    }
}

class ShardsvrCheckMetadataConsistencyCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistency;
    using Response = CursorInitialReply;

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

            const auto nss = ns();
            switch (getCommandLevel(nss)) {
                case MetadataConsistencyCommandLevelEnum::kClusterLevel:
                    return _runClusterLevel(opCtx, nss);
                case MetadataConsistencyCommandLevelEnum::kDatabaseLevel:
                    return _runDatabaseLevel(opCtx, nss);
                case MetadataConsistencyCommandLevelEnum::kCollectionLevel:
                    return _runCollectionLevel(nss);
                default:
                    MONGO_UNREACHABLE;
            }
        }

    private:
        Response _runClusterLevel(OperationContext* opCtx, const NamespaceString& nss) {
            uassert(
                ErrorCodes::InvalidNamespace,
                "cluster level mode must be run against the 'admin' database with {aggregate: 1}",
                nss.isCollectionlessCursorNamespace());

            std::vector<MetadataInconsistencyItem> inconsistenciesMerged;

            // Need to retrieve a list of databases which this shard is primary for and run the
            // command on each of them.
            const auto databases = Grid::get(opCtx)->catalogClient()->getAllDBs(
                opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
            const auto shardId = ShardingState::get(opCtx)->shardId();

            // TODO: SERVER-73976: Retrieve the list of databases which the given shard is the
            // primary db shard directly from configsvr.
            for (const auto& db : databases) {
                if (db.getPrimary() == shardId) {
                    const auto inconsistencies = _checkMetadataConsistencyOnParticipants(
                        opCtx, NamespaceString(db.getName(), nss.coll()));

                    inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                                 std::make_move_iterator(inconsistencies.begin()),
                                                 std::make_move_iterator(inconsistencies.end()));
                }
            }

            return metadata_consistency_util::makeCursor(
                opCtx, inconsistenciesMerged, nss, request().toBSON({}));
        }

        Response _runDatabaseLevel(OperationContext* opCtx, const NamespaceString& nss) {
            return metadata_consistency_util::makeCursor(
                opCtx,
                _checkMetadataConsistencyOnParticipants(opCtx, nss),
                nss,
                request().toBSON({}));
        }

        Response _runCollectionLevel(const NamespaceString& nss) {
            uasserted(ErrorCodes::NotImplemented,
                      "collection level mode command is not implemented");
        }

        // It first acquires a DDL lock on the database to prevent concurrent metadata changes, and
        // then sends a command to all shards to check for metadata inconsistencies. The responses
        // from all shards are merged together to produce a list of metadata inconsistencies to be
        // returned.
        std::vector<MetadataInconsistencyItem> _checkMetadataConsistencyOnParticipants(
            OperationContext* opCtx, const NamespaceString& nss) {
            const auto primaryShardId = ShardingState::get(opCtx)->shardId();
            std::vector<AsyncRequestsSender::Response> responses;

            {
                // Take a DDL lock on the database
                static constexpr StringData lockReason{"checkMetadataConsistency"_sd};
                auto ddlLockManager = DDLLockManager::get(opCtx);
                const auto dbDDLLock = ddlLockManager->lock(
                    opCtx, nss.db(), lockReason, DDLLockManager::kDefaultLockTimeout);

                std::vector<AsyncRequestsSender::Request> requests;

                // Shard requests
                ShardsvrCheckMetadataConsistencyParticipant participantRequest{nss};
                participantRequest.setPrimaryShardId(primaryShardId);
                const auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                for (const auto& shardId : participants) {
                    requests.emplace_back(shardId, participantRequest.toBSON({}));
                }

                // Config server request
                ConfigsvrCheckMetadataConsistency configRequest{nss};
                requests.emplace_back(ShardId::kConfigServerId, configRequest.toBSON({}));

                responses = sharding_util::processShardResponses(
                    opCtx,
                    nss.db(),
                    participantRequest.toBSON({}),
                    requests,
                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                    true /* throwOnError */);
            }

            const auto cursorManager = Grid::get(opCtx)->getCursorManager();

            // Merge responses
            std::vector<MetadataInconsistencyItem> inconsistenciesMerged;
            for (auto&& cmdResponse : responses) {
                auto response = uassertStatusOK(std::move(cmdResponse.swResponse));
                uassertStatusOK(getStatusFromCommandResult(response.data));

                auto transformedResponse = uassertStatusOK(storePossibleCursor(
                    opCtx,
                    cmdResponse.shardId,
                    *cmdResponse.shardHostAndPort,
                    response.data,
                    nss,
                    Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                    cursorManager,
                    {Privilege(ResourcePattern::forClusterResource(), ActionType::internal)}));

                auto checkMetadataConsistencyResponse = CursorInitialReply::parseOwned(
                    IDLParserContext("checkMetadataConsistency"), std::move(transformedResponse));
                const auto& cursor = checkMetadataConsistencyResponse.getCursor();

                auto& firstBatch = cursor->getFirstBatch();
                for (const auto& shardInconsistency : firstBatch) {
                    auto inconsistency = MetadataInconsistencyItem::parseOwned(
                        IDLParserContext("MetadataInconsistencyItem"),
                        shardInconsistency.getOwned());
                    inconsistenciesMerged.push_back(inconsistency);
                }

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
                    cursorManager->checkOutCursor(cursor->getCursorId(), opCtx, authChecker);
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
