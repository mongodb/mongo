// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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

            auto span = otel::traces::Span::start(
                opCtx, otel::traces::span_names::kReshardCollectionCmdInvocationTypedRun);

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

            reshardCollectionRequest.setShardDistribution(request().getShardDistribution());
            reshardCollectionRequest.setForceRedistribution(request().getForceRedistribution());
            reshardCollectionRequest.setUserReshardingUUID(request().getUserReshardingUUID());
            reshardCollectionRequest.setRelaxed(request().getRelaxed());
            if (resharding::gFeatureFlagMoveCollection.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                resharding::gFeatureFlagUnshardCollection.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                reshardCollectionRequest.setProvenance(
                    ReshardingProvenanceEnum::kReshardCollection);
            }
            reshardCollectionRequest.setPerformVerification(request().getPerformVerification());

            if (resharding::gfeatureFlagReshardingNumSamplesPerChunk.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                reshardCollectionRequest.setNumSamplesPerChunk(request().getNumSamplesPerChunk());
            }

            reshardCollectionRequest.setDemoMode(request().getDemoMode());

            shardsvrReshardCollection.setReshardCollectionRequest(
                std::move(reshardCollectionRequest));

            generic_argument_util::setMajorityWriteConcern(shardsvrReshardCollection,
                                                           &opCtx->getWriteConcern());

            sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
            router.route(Request::kCommandParameterFieldName,
                         [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                             auto cmdResponse =
                                 executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                     opCtx,
                                     DatabaseName::kAdmin,
                                     dbInfo,
                                     shardsvrReshardCollection.toBSON(),
                                     ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                     Shard::RetryPolicy::kIdempotent);

                             const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                             uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                         });
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
