// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/balancer_collection_status_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class BalancerCollectionStatusCmd final : public TypedCommand<BalancerCollectionStatusCmd> {
public:
    using Request = BalancerCollectionStatus;
    using Response = BalancerCollectionStatusResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        std::string_view kStatusField = "status"sv;

        Response typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            ConfigsvrBalancerCollectionStatus configsvrRequest(nss);
            configsvrRequest.setDbName(request().getDbName());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configsvrRequest.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);

            return Response::parse(cmdResponse.response,
                                   IDLParserContext("BalancerCollectionStatusResponse"));
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
                                                           ActionType::enableSharding));
        }
    };

    std::string help() const override {
        return "command to check whether the chunks of a given collection are in a quiesced state "
               "or there are any which need to be moved because of (1) draining shards, (2) zone "
               "violation or (3) imbalance between shards";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(BalancerCollectionStatusCmd).forRouter();

}  // namespace
}  // namespace mongo
