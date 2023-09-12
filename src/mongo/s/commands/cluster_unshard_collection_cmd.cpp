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
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {

class ClusterUnshardCollectionCmd final : public TypedCommand<ClusterUnshardCollectionCmd> {
public:
    using Request = UnshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(
                ErrorCodes::CommandNotSupported,
                "Resharding improvements is not enabled, cannot perform unshardCollection command.",
                resharding::gFeatureFlagReshardingImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility));

            const auto& nss = ns();
            ShardsvrReshardCollection shardsvrReshardCollection(nss);
            shardsvrReshardCollection.setDbName(request().getDbName());

            ReshardCollectionRequest reshardCollectionRequest;
            reshardCollectionRequest.setKey(BSON("_id" << 1));
            reshardCollectionRequest.setProvenance(ProvenanceEnum::kUnshardCollection);

            ShardId toShard;
            if (request().getToShard().has_value()) {
                toShard = request().getToShard().get();
            } else {
                toShard = shardutil::selectLeastLoadedShard(opCtx);
            }

            std::vector<mongo::ShardKeyRange> destinationShard = {toShard};
            reshardCollectionRequest.setShardDistribution(destinationShard);
            reshardCollectionRequest.setForceRedistribution(true);
            reshardCollectionRequest.setNumInitialChunks(1);

            shardsvrReshardCollection.setReshardCollectionRequest(
                std::move(reshardCollectionRequest));

            LOGV2(8018400,
                  "Running a reshard collection command for the unshard collection request.",
                  "dbName"_attr = request().getDbName(),
                  "toShard"_attr = request().getToShard());

            const auto dbInfo =
                uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.dbName()));

            auto cmdResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                DatabaseName::kAdmin,
                dbInfo,
                CommandHelpers::appendMajorityWriteConcern(shardsvrReshardCollection.toBSON({}),
                                                           opCtx->getWriteConcern()),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::unshardCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Unshard a sharded collection.";
    }
};

MONGO_REGISTER_COMMAND(ClusterUnshardCollectionCmd)
    .requiresFeatureFlag(&resharding::gFeatureFlagUnshardCollection)
    .forRouter();

}  // namespace
}  // namespace mongo
