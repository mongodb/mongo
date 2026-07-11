// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/sharding_environment/cluster_commands_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/versioning_protocol/catalog_cache_diagnostics_helpers.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ClusterGetDatabaseVersionCommand final
    : public TypedCommand<ClusterGetDatabaseVersionCommand> {
public:
    using Request = ClusterGetDatabaseVersion;
    using Response = GetDatabaseVersionResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            if (request().getLatestCached()) {
                BSONObjBuilder responseBuilder;
                catalog_cache_diagnostics_helpers::appendLatestCachedDbInfo(
                    opCtx, &responseBuilder, ns().dbName());
                uassert(ErrorCodes::NamespaceNotFound,
                        "Database not found in the catalog cache",
                        !responseBuilder.hasField("global"));
                return Response::parse(responseBuilder.obj());
            } else {
                auto catalogCache = Grid::get(opCtx)->catalogCache();
                const auto dbInfo =
                    uassertStatusOK(catalogCache->getDatabase(opCtx, ns().dbName()));
                return {dbInfo->getPrimary(), dbInfo->getVersion()};
            }
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getCommandParameter());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forDatabaseName(ns().dbName()),
                            ActionType::getDatabaseVersion));
        }
    };

private:
    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return " example: { getDatabaseVersion : 'foo' } ";
    }
};
MONGO_REGISTER_COMMAND(ClusterGetDatabaseVersionCommand).forRouter();

}  // namespace
}  // namespace mongo
