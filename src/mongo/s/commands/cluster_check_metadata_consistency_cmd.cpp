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
#include "mongo/db/query/find_common.h"
#include "mongo/s/check_metadata_consistency_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"

namespace mongo {
namespace {

/*
 * Return the set of shards that are primaries for at least one database
 */
stdx::unordered_set<ShardId> getAllDbPrimaryShards(OperationContext* opCtx) {
    static const std::vector<BSONObj> rawPipeline{fromjson(R"({
        $group: {
            _id: '$primary'
        }
    })")};
    AggregateCommandRequest aggRequest{NamespaceString::kConfigDatabasesNamespace, rawPipeline};
    auto aggResponse = Grid::get(opCtx)->catalogClient()->runCatalogAggregation(
        opCtx, aggRequest, {repl::ReadConcernLevel::kMajorityReadConcern});

    stdx::unordered_set<ShardId> shardIds;
    shardIds.reserve(aggResponse.size() + 1);
    for (auto&& responseEntry : aggResponse) {
        shardIds.insert(responseEntry.firstElement().str());
    }
    // The config server is authoritative for config database
    shardIds.insert(ShardId::kConfigServerId);
    return shardIds;
}

MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss) {
    if (nss.isAdminDB()) {
        return MetadataConsistencyCommandLevelEnum::kClusterLevel;
    } else if (nss.isCollectionlessCursorNamespace()) {
        return MetadataConsistencyCommandLevelEnum::kDatabaseLevel;
    } else {
        return MetadataConsistencyCommandLevelEnum::kCollectionLevel;
    }
}

class CheckMetadataConsistencyCmd final : public TypedCommand<CheckMetadataConsistencyCmd> {
public:
    using Request = CheckMetadataConsistency;
    using Response = CursorInitialReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            const auto nss = ns();
            switch (getCommandLevel(nss)) {
                case MetadataConsistencyCommandLevelEnum::kClusterLevel:
                    return _runClusterLevel(opCtx, nss);
                case MetadataConsistencyCommandLevelEnum::kDatabaseLevel:
                    return _runDatabaseLevel(opCtx, nss);
                case MetadataConsistencyCommandLevelEnum::kCollectionLevel:
                    return _runCollectionLevel(opCtx, nss);
                default:
                    MONGO_UNREACHABLE;
            }
        }

    private:
        Response _runClusterLevel(OperationContext* opCtx, const NamespaceString& nss) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << Request::kCommandName
                                  << " command on admin database can only be run without "
                                     "collection name. Found unexpected collection name: "
                                  << nss.coll(),
                    nss.isCollectionlessCursorNamespace());

            std::vector<std::pair<ShardId, BSONObj>> requests;
            ShardsvrCheckMetadataConsistency shardsvrRequest{nss};
            shardsvrRequest.setCommonFields(request().getCommonFields());
            shardsvrRequest.setCursor(request().getCursor());

            // Send a request to all shards that are primaries for at least one database
            const auto shardOpKey = UUID::gen();
            auto shardRequestWithOpKey = appendOpKey(shardOpKey, shardsvrRequest.toBSON({}));
            for (auto&& shardId : getAllDbPrimaryShards(opCtx)) {
                requests.emplace_back(std::move(shardId), shardRequestWithOpKey.getOwned());
            }

            // Send a request to the configsvr to check cluster metadata consistency.
            const auto configOpKey = UUID::gen();
            ConfigsvrCheckClusterMetadataConsistency configsvrRequest;
            configsvrRequest.setDbName(DatabaseName::kAdmin);
            configsvrRequest.setCursor(request().getCursor());
            requests.emplace_back(ShardId::kConfigServerId,
                                  appendOpKey(configOpKey, configsvrRequest.toBSON({})));

            auto ccc = _establishCursors(opCtx, nss, requests, {shardOpKey, configOpKey});
            return _createInitialCursorReply(opCtx, nss, std::move(ccc));
        }

        Response _runDatabaseLevel(OperationContext* opCtx, const NamespaceString& nss) {
            auto ccc = _establishCursorOnDbPrimary(opCtx, nss);
            return _createInitialCursorReply(opCtx, nss, std::move(ccc));
        }

        Response _runCollectionLevel(OperationContext* opCtx, const NamespaceString& nss) {
            auto ccc = _establishCursorOnDbPrimary(opCtx, nss);
            return _createInitialCursorReply(opCtx, nss, std::move(ccc));
        }

        ClusterClientCursorGuard _establishCursorOnDbPrimary(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
            const CachedDatabaseInfo dbInfo =
                uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(
                    opCtx, nss.dbName().toStringWithTenantId()));

            ShardsvrCheckMetadataConsistency shardsvrRequest{nss};
            shardsvrRequest.setDbName(nss.dbName());
            shardsvrRequest.setCommonFields(request().getCommonFields());
            shardsvrRequest.setCursor(request().getCursor());
            // Attach db and shard version;
            auto cmdObj = appendDbVersionIfPresent(shardsvrRequest.toBSON({}), dbInfo);
            if (!dbInfo->getVersion().isFixed())
                cmdObj = appendShardVersion(std::move(cmdObj), ShardVersion::UNSHARDED());
            return _establishCursors(opCtx, nss, {{dbInfo->getPrimary(), std::move(cmdObj)}});
        }

        ClusterClientCursorGuard _establishCursors(
            OperationContext* opCtx,
            const NamespaceString& nss,
            const std::vector<std::pair<ShardId, BSONObj>>& requests,
            std::vector<OperationKey> opKeys = {}) {

            ClusterClientCursorParams params(
                nss,
                APIParameters::get(opCtx),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

            // Establish the cursors with a consistent shardVersion across shards.
            params.remotes = establishCursors(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()),
                requests,
                false /*allowPartialResults*/,
                Shard::RetryPolicy::kIdempotent,
                std::move(opKeys));

            // Transfer the established cursors to a ClusterClientCursor.
            return ClusterClientCursorImpl::make(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                std::move(params));
        }

        Response _createInitialCursorReply(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           ClusterClientCursorGuard&& ccc) {
            auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
            std::vector<BSONObj> firstBatch;
            const auto& cursorOpts = request().getCursor();
            const auto batchSize = [&] {
                if (cursorOpts && cursorOpts->getBatchSize()) {
                    return (long long)*cursorOpts->getBatchSize();
                } else {
                    return query_request_helper::kDefaultBatchSize;
                }
            }();
            FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                auto next = uassertStatusOK(ccc->next());
                if (next.isEOF()) {
                    // We reached end-of-stream, if all the remote cursors are exhausted, there is
                    // no hope of returning data and thus we need to close the mongos cursor as
                    // well.
                    cursorState = ClusterCursorManager::CursorState::Exhausted;
                    break;
                }

                auto nextObj = *next.getResult();

                // If adding this object will cause us to exceed the message size limit, then we
                // stash it for later.
                if (!responseSizeTracker.haveSpaceForNext(nextObj)) {
                    ccc->queueResult(nextObj);
                    break;
                }
                responseSizeTracker.add(nextObj);
                firstBatch.push_back(std::move(nextObj));
            }

            ccc->detachFromOperationContext();

            if (cursorState == ClusterCursorManager::CursorState::Exhausted) {
                CursorInitialReply resp;
                InitialResponseCursor initRespCursor{std::move(firstBatch)};
                initRespCursor.setResponseCursorBase({0LL /* cursorId */, nss});
                resp.setCursor(std::move(initRespCursor));
                return resp;
            }

            ccc->incNBatches();

            auto authUser =
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();

            // Determine whether the cursor we may eventually register will be single- or
            // multi-target.
            const auto cursorType = ccc->getNumRemotes() > 1
                ? ClusterCursorManager::CursorType::MultiTarget
                : ClusterCursorManager::CursorType::SingleTarget;

            // Register the cursor with the cursor manager for subsequent getMore's.
            auto clusterCursorId =
                uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
                    opCtx,
                    ccc.releaseCursor(),
                    nss,
                    cursorType,
                    ClusterCursorManager::CursorLifetime::Mortal,
                    authUser));

            CursorInitialReply resp;
            InitialResponseCursor initRespCursor{std::move(firstBatch)};
            initRespCursor.setResponseCursorBase({clusterCursorId, nss});
            resp.setCursor(std::move(initRespCursor));
            return resp;
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto isAuthorizedOnResource = [&](const ResourcePattern& resourcePattern) {
                return AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(resourcePattern,
                                                       ActionType::checkMetadataConsistency);
            };

            const auto nss = ns();
            switch (getCommandLevel(nss)) {
                case MetadataConsistencyCommandLevelEnum::kClusterLevel:
                    uassert(ErrorCodes::Unauthorized,
                            "Not authorized to check cluster metadata consistency",
                            isAuthorizedOnResource(ResourcePattern::forClusterResource()));
                    break;
                case MetadataConsistencyCommandLevelEnum::kDatabaseLevel:
                    uassert(ErrorCodes::Unauthorized,
                            str::stream()
                                << "Not authorized to check metadata consistency for database "
                                << nss.dbName().toStringForErrorMsg(),
                            isAuthorizedOnResource(ResourcePattern::forClusterResource()) ||
                                isAuthorizedOnResource(ResourcePattern::forDatabaseName(nss.db())));
                    break;
                case MetadataConsistencyCommandLevelEnum::kCollectionLevel:
                    uassert(ErrorCodes::Unauthorized,
                            str::stream()
                                << "Not authorized to check metadata consistency for collection "
                                << nss.toStringForErrorMsg(),
                            isAuthorizedOnResource(ResourcePattern::forClusterResource()) ||
                                isAuthorizedOnResource(ResourcePattern::forExactNamespace(nss)));
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }
    };
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(CheckMetadataConsistencyCmd,
                                       feature_flags::gCheckMetadataConsistency);

}  // namespace
}  // namespace mongo
