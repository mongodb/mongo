// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigsvrClearJumboFlagCommand final : public TypedCommand<ConfigsvrClearJumboFlagCommand> {
public:
    using Request = ConfigsvrClearJumboFlag;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrClearJumboFlag can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

            CollectionType collType;
            try {
                collType = catalogClient->getCollection(opCtx, nss, repl::ReadConcernArgs::kLocal);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                uasserted(ErrorCodes::NamespaceNotSharded,
                          str::stream() << "clearJumboFlag namespace " << nss.toStringForErrorMsg()
                                        << " is not sharded");
            }

            uassert(StaleEpochInfo(nss),
                    str::stream()
                        << "clearJumboFlag namespace " << nss.toStringForErrorMsg()
                        << " has a different epoch than mongos had in its routing table cache",
                    request().getEpoch() == collType.getEpoch());

            ShardingCatalogManager::get(opCtx)->clearJumboFlag(
                opCtx,
                nss,
                request().getEpoch(),
                ChunkRange{request().getMinKey(), request().getMaxKey()});
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
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
               "directly. Clears the jumbo flag of the chunk specified.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrClearJumboFlagCommand).forShard();

}  // namespace
}  // namespace mongo
