// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangRewriteCollectionBeforeRunningReshardCollection);

class ClusterRewriteCollectionCmd final : public TypedCommand<ClusterRewriteCollectionCmd> {
public:
    using Request = RewriteCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& nss = ns();

            ReshardCollectionRequest reshardCollectionRequest;

            // ForceRedistribution is set to true to force resharding.
            reshardCollectionRequest.setForceRedistribution(true);

            // Other parameter values are passed through to reshardCollection.
            reshardCollectionRequest.setZones(request().getZones());
            reshardCollectionRequest.setNumInitialChunks(request().getNumInitialChunks());
            reshardCollectionRequest.setPerformVerification(request().getPerformVerification());

            if (resharding::gfeatureFlagReshardingNumSamplesPerChunk.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                reshardCollectionRequest.setNumSamplesPerChunk(request().getNumSamplesPerChunk());
            }

            reshardCollectionRequest.setDemoMode(request().getDemoMode());
            reshardCollectionRequest.setProvenance(ReshardingProvenanceEnum::kRewriteCollection);

            ShardsvrReshardCollection rewriteCollectionRequest(nss);
            rewriteCollectionRequest.setDbName(request().getDbName());
            rewriteCollectionRequest.setReshardCollectionRequest(
                std::move(reshardCollectionRequest));

            generic_argument_util::setMajorityWriteConcern(rewriteCollectionRequest,
                                                           &opCtx->getWriteConcern());

            if (MONGO_unlikely(hangRewriteCollectionBeforeRunningReshardCollection.shouldFail())) {
                LOGV2(11342700,
                      "Hanging rewriteCollection due to failpoint "
                      "'hangRewriteCollectionBeforeRunningReshardCollection'");
                hangRewriteCollectionBeforeRunningReshardCollection.pauseWhileSet(opCtx);
            }

            LOGV2(8328900,
                  "Running a reshard collection command for the rewrite collection request.",
                  "dbName"_attr = request().getDbName());

            sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
            router.route(Request::kCommandParameterFieldName,
                         [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                             auto cmdResponse =
                                 executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                     opCtx,
                                     DatabaseName::kAdmin,
                                     dbInfo,
                                     rewriteCollectionRequest.toBSON(),
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
                                                           ActionType::rewriteCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Rewrite a sharded collection on its existing shard key.";
    }
};

MONGO_REGISTER_COMMAND(ClusterRewriteCollectionCmd).forRouter();

}  // namespace
}  // namespace mongo
