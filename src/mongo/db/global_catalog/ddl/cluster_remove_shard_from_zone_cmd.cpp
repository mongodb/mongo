/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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
