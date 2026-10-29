// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ClusterAbortRewriteCollectionCmd : public TypedCommand<ClusterAbortRewriteCollectionCmd> {
public:
    using Request = AbortRewriteCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            LOGV2(8328901, "Beginning rewrite collection abort operation", logAttrs(ns()));

            ConfigsvrAbortReshardCollection configsvrAbortReshardCollection(nss);
            configsvrAbortReshardCollection.setDbName(request().getDbName());
            configsvrAbortReshardCollection.setProvenance(
                ReshardingProvenanceEnum::kRewriteCollection);
            generic_argument_util::setMajorityWriteConcern(configsvrAbortReshardCollection,
                                                           &opCtx->getWriteConcern());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configsvrAbortReshardCollection.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(cmdResponse.commandStatus);
            uassertStatusOK(cmdResponse.writeConcernStatus);
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
        return "Abort an in-progress rewrite collection operation for this collection.";
    }
};

MONGO_REGISTER_COMMAND(ClusterAbortRewriteCollectionCmd).forRouter();

}  // namespace
}  // namespace mongo
