// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/remove_tags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigsvrRemoveTagsCommand final : public TypedCommand<ConfigsvrRemoveTagsCommand> {
public:
    using Request = ConfigsvrRemoveTags;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrRemoveTags can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(
                5748800, "_configsvrRemoveTags must be run as a retryable write", txnParticipant);

            {
                auto newClient =
                    opCtx->getServiceContext()->getService()->makeClient("RemoveTagsMetadata");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                const auto catalogClient =
                    ShardingCatalogManager::get(newOpCtxPtr.get())->localCatalogClient();
                uassertStatusOK(catalogClient->removeConfigDocuments(
                    newOpCtxPtr.get(),
                    TagsType::ConfigNS,
                    BSON(TagsType::ns(
                        NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))),
                    ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter()));
            }

            // Since we no write happened on this txnNumber, we need to make a dummy write so that
            // secondaries can be aware of this txn.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << "RemoveTagsMetadataStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
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
               "directly. Removes the zone tags for the specified ns.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrRemoveTagsCommand).forShard();

}  // namespace
}  // namespace mongo
