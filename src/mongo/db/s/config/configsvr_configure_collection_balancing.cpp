// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/configure_collection_balancing_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigsvrConfigureCollectionBalancingCmd final
    : public TypedCommand<ConfigsvrConfigureCollectionBalancingCmd> {
public:
    using Request = ConfigsvrConfigureCollectionBalancing;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName << " can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            const NamespaceString& nss = ns();

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace specified '" << nss.toStringForErrorMsg()
                                  << "'",
                    nss.isValid());

            // throws if collection does not exist or parameters are invalid
            ShardingCatalogManager::get(opCtx)->configureCollectionBalancing(
                opCtx,
                nss,
                request().getChunkSizeMB(),
                request().getDefragmentCollection(),
                request().getEnableAutoMerger(),
                request().getEnableBalancing());
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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrConfigureCollectionBalancingCmd).forShard();

}  // namespace
}  // namespace mongo
