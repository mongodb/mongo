// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ClusterUntrackUnsplittableCollectionCommand final
    : public TypedCommand<ClusterUntrackUnsplittableCollectionCommand> {
public:
    using Request = ClusterUntrackUnsplittableCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardsvrUntrackUnsplittableCollection shardsvrRequest(ns());
            shardsvrRequest.setDbName(NamespaceString::kAdminCommandNamespace.dbName());
            generic_argument_util::setMajorityWriteConcern(shardsvrRequest);

            // Route the command to the primary shard.
            sharding::router::DBPrimaryRouter router(opCtx, ns().dbName());
            router.route(
                Request::kCommandParameterFieldName,
                [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                    auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                        opCtx,
                        ns().dbName(),
                        dbInfo,
                        shardsvrRequest.toBSON(),
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        Shard::RetryPolicy::kIdempotent);

                    uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
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
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exposed for emergency use only. Safely unregisters from "
               "the sharding catalog a tracked unsharded collection. Requires the collection to be "
               "placed on its primary shard to succeed.";
    }
};

MONGO_REGISTER_COMMAND(ClusterUntrackUnsplittableCollectionCommand).forRouter();

}  // namespace
}  // namespace mongo
