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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {

class ClusterMoveCollectionCmd final : public TypedCommand<ClusterMoveCollectionCmd> {
public:
    using Request = MoveCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& nss = ns();
            auto moveCollectionRequest = cluster::unsplittable::makeMoveCollectionRequest(
                request().getDbName(),
                nss,
                request().getToShard(),
                ReshardingProvenanceEnum::kMoveCollection,
                request().getPerformVerification(),
                request().getOplogBatchApplierTaskCount());
            generic_argument_util::setMajorityWriteConcern(moveCollectionRequest,
                                                           &opCtx->getWriteConcern());

            LOGV2(7973800,
                  "Running a reshard collection command for the move collection request.",
                  "dbName"_attr = request().getDbName(),
                  "toShard"_attr = request().getToShard());

            sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
            router.route(Request::kCommandParameterFieldName,
                         [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                             auto cmdResponse =
                                 executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                     opCtx,
                                     DatabaseName::kAdmin,
                                     dbInfo,
                                     moveCollectionRequest.toBSON(),
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
                                                           ActionType::moveCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Move an unsharded collection from source shard to destination shard.";
    }
};

MONGO_REGISTER_COMMAND(ClusterMoveCollectionCmd)
    .requiresFeatureFlag(resharding::gFeatureFlagMoveCollection)
    .forRouter();

}  // namespace
}  // namespace mongo
