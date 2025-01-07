/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ReshardCollectionCmd : public TypedCommand<ReshardCollectionCmd> {
public:
    using Request = ReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& nss = ns();

            ShardsvrReshardCollection shardsvrReshardCollection(nss);
            shardsvrReshardCollection.setDbName(request().getDbName());

            ReshardCollectionRequest reshardCollectionRequest;
            reshardCollectionRequest.setKey(request().getKey());
            reshardCollectionRequest.setUnique(request().getUnique());
            reshardCollectionRequest.setCollation(request().getCollation());
            reshardCollectionRequest.set_presetReshardedChunks(
                request().get_presetReshardedChunks());
            reshardCollectionRequest.setZones(request().getZones());
            reshardCollectionRequest.setNumInitialChunks(request().getNumInitialChunks());
            reshardCollectionRequest.setCollectionUUID(request().getCollectionUUID());

            if (!resharding::gFeatureFlagReshardingImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "Resharding improvements is not enabled, reject shardDistribution parameter",
                    !request().getShardDistribution().has_value());
                uassert(
                    ErrorCodes::InvalidOptions,
                    "Resharding improvements is not enabled, reject forceRedistribution parameter",
                    !request().getForceRedistribution().has_value());
                uassert(ErrorCodes::InvalidOptions,
                        "Resharding improvements is not enabled, reject reshardingUUID parameter",
                        !request().getReshardingUUID().has_value());
                uassert(ErrorCodes::InvalidOptions,
                        "Resharding improvements is not enabled, reject feature flag "
                        "moveCollection or unshardCollection",
                        !resharding::gFeatureFlagMoveCollection.isEnabled(
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                            !resharding::gFeatureFlagUnshardCollection.isEnabled(
                                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            }
            reshardCollectionRequest.setShardDistribution(request().getShardDistribution());
            reshardCollectionRequest.setForceRedistribution(request().getForceRedistribution());
            reshardCollectionRequest.setReshardingUUID(request().getReshardingUUID());
            reshardCollectionRequest.setRelaxed(request().getRelaxed());
            if (resharding::gFeatureFlagMoveCollection.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                resharding::gFeatureFlagUnshardCollection.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                reshardCollectionRequest.setProvenance(ProvenanceEnum::kReshardCollection);
            }
            reshardCollectionRequest.setImplicitlyCreateIndex(request().getImplicitlyCreateIndex());

            if (resharding::gfeatureFlagReshardingNumSamplesPerChunk.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                reshardCollectionRequest.setNumSamplesPerChunk(request().getNumSamplesPerChunk());
            }

            shardsvrReshardCollection.setReshardCollectionRequest(
                std::move(reshardCollectionRequest));

            auto catalogCache = Grid::get(opCtx)->catalogCache();
            const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.dbName()));

            generic_argument_util::setMajorityWriteConcern(shardsvrReshardCollection,
                                                           &opCtx->getWriteConcern());
            auto cmdResponse = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                opCtx,
                DatabaseName::kAdmin,
                dbInfo,
                shardsvrReshardCollection.toBSON(),
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
                                                           ActionType::reshardCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Reshard an already sharded collection on a new shard key.";
    }
};
MONGO_REGISTER_COMMAND(ReshardCollectionCmd).forRouter();

}  // namespace
}  // namespace mongo
