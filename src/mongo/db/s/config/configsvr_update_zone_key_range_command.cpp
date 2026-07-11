// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/s/request_types/update_zone_key_range_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ConfigsvrUpdateZoneKeyRangeCommand : public TypedCommand<ConfigsvrUpdateZoneKeyRangeCommand> {
public:
    using Request = ConfigsvrUpdateZoneKeyRange;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrAssignKeyRangeToZone can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // Set the operation context read concern level to local for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            if (isRemove()) {
                ShardingCatalogManager::get(opCtx)->removeKeyRangeFromZone(opCtx, ns(), getRange());
            } else {
                ShardingCatalogManager::get(opCtx)->assignKeyRangeToZone(
                    opCtx, ns(), getRange(), *request().getZone());
            }
        }

    private:
        bool isRemove() const {
            if (request().getZone()) {
                return false;
            }
            return true;
        }

        ChunkRange getRange() const {
            BSONObj minKey = request().getMin();
            BSONObj maxKey = request().getMax();

            uassertStatusOK(ChunkRange::validate(minKey, maxKey));
            return ChunkRange(minKey, maxKey);
        }

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

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Validates and assigns a new range to a zone.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrUpdateZoneKeyRangeCommand).forShard();

}  // namespace
}  // namespace mongo
