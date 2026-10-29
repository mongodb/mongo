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
#include "mongo/db/global_catalog/ddl/remove_shard_from_zone_request_type.h"
#include "mongo/db/global_catalog/type_tags.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

/**
 * {
 *   removeShardFromZone: <string shardName>,
 *   zone: <string zoneName>
 * }
 */
class RemoveShardFromZoneCmd : public BasicCommand {
public:
    RemoveShardFromZoneCmd() : BasicCommand("removeShardFromZone", "removeshardfromzone") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "removes a shard from the zone";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const final {
        auto* as = AuthorizationSession::get(opCtx->getClient());

        if (as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                ActionType::enableSharding)) {
            return Status::OK();
        }

        // Fallback on permissions to directly modify the shard config.
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString::kConfigsvrShardsNamespace),
                ActionType::update)) {
            return {ErrorCodes::Unauthorized, "Unauthorized"};
        }

        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::find)) {
            return {ErrorCodes::Unauthorized, "Unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto parsedRequest =
            uassertStatusOK(RemoveShardFromZoneRequest::parseFromMongosCommand(cmdObj));

        BSONObjBuilder cmdBuilder;
        parsedRequest.appendAsConfigCommand(&cmdBuilder);

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponseStatus = uassertStatusOK(configShard->runCommand(
            opCtx,
            kPrimaryOnlyReadPreference,
            DatabaseName::kAdmin,
            CommandHelpers::appendMajorityWriteConcern(cmdBuilder.obj(), opCtx->getWriteConcern()),
            Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponseStatus.commandStatus);
        return true;
    }
};
MONGO_REGISTER_COMMAND(RemoveShardFromZoneCmd).forRouter();

}  // namespace
}  // namespace mongo
