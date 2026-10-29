// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/add_shard_to_zone_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <memory>
#include <string>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

/**
 * {
 *   addShardToZone: <string shardName>,
 *   zone: <string zoneName>
 * }
 */
class AddShardToZoneCmd : public TypedCommand<AddShardToZoneCmd> {
public:
    using Request = AddShardToZone;

    AddShardToZoneCmd() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            BSONObjBuilder cmdBuilder;
            ConfigsvrAddShardToZone cmd(std::string{getShard()}, std::string{request().getZone()});
            generic_argument_util::setMajorityWriteConcern(cmd);
            cmd.serialize(&cmdBuilder);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponseStatus =
                uassertStatusOK(configShard->runCommand(opCtx,
                                                        kPrimaryOnlyReadPreference,
                                                        DatabaseName::kAdmin,
                                                        cmdBuilder.obj(),
                                                        Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(cmdResponseStatus.commandStatus);
        }

    private:
        NamespaceString ns() const override {
            return {};
        }

        std::string_view getShard() const {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());

            if (as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                    ActionType::enableSharding)) {
                return;
            }
            if (as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString::kConfigsvrShardsNamespace),
                    ActionType::update)) {
                return;
            }

            uasserted(ErrorCodes::Unauthorized, "Unauthorized");
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Adds a shard to zone.";
    }
};
MONGO_REGISTER_COMMAND(AddShardToZoneCmd).forRouter();

}  // namespace
}  // namespace mongo
