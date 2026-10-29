// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace analyze_shard_key {

namespace {

class ConfigureQueryAnalyzerCmd : public TypedCommand<ConfigureQueryAnalyzerCmd> {
public:
    using Request = ConfigureQueryAnalyzer;
    using Response = ConfigureQueryAnalyzerResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& nss = ns();
            uassertStatusOK(validateNamespace(nss));

            sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
            return router.route(
                Request::kCommandName,
                [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                    auto cmdObj =
                        CommandHelpers::filterCommandRequestForPassthrough(request().toBSON());

                    const auto swResponse =
                        executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                            opCtx,
                            DatabaseName::kAdmin,
                            dbInfo,
                            cmdObj,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent);

                    uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(swResponse));

                    auto remoteResponse = uassertStatusOK(swResponse.swResponse).data;
                    auto response = ConfigureQueryAnalyzerResponse::parse(
                        remoteResponse, IDLParserContext("clusterConfigureQueryAnalyzer"));
                    return response;
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
                                                           ActionType::configureQueryAnalyzer));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Starts or stops collecting metrics about read and write queries against "
               "collection.";
    }
};
MONGO_REGISTER_COMMAND(ConfigureQueryAnalyzerCmd).forRouter();

}  // namespace

}  // namespace analyze_shard_key
}  // namespace mongo
