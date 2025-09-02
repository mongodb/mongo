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
#include "mongo/base/string_data.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class AddShardCmd : public TypedCommand<AddShardCmd> {
public:
    using Request = AddShard;
    using Response = AddShardResponse;

    AddShardCmd() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::InvalidOptions,
                    "addShard no longer supports maxSize field",
                    !unparsedRequest().body.hasField("maxSize"));

            const auto& target = request().getCommandParameter();
            if (target.type() != ConnectionString::ConnectionType::kStandalone &&
                target.type() != ConnectionString::ConnectionType::kReplicaSet) {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "Invalid connection string " << target.toString());
            }

            ConfigsvrAddShard configsvrRequest{target};
            configsvrRequest.setAddShardRequestBase(request().getAddShardRequestBase());
            configsvrRequest.setDbName(request().getDbName());

            const auto cmdResponseWithStatus =
                Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
                    opCtx,
                    kPrimaryOnlyReadPreference,
                    DatabaseName::kAdmin,
                    // TODO SERVER-91373: Remove appendMajorityWriteConcern
                    CommandHelpers::appendMajorityWriteConcern(
                        CommandHelpers::filterCommandRequestForPassthrough(
                            configsvrRequest.toBSON()),
                        opCtx->getWriteConcern()),
                    Shard::RetryPolicy::kIdempotent);

            Grid::get(opCtx)->shardRegistry()->reload(opCtx);

            const auto cmdResponse = uassertStatusOK(cmdResponseWithStatus);
            uassertStatusOK(cmdResponseWithStatus.getValue().commandStatus);

            return Response::parse(cmdResponse.response, IDLParserContext("addShardResponse"));
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::addShard));
        }
    };

    std::string help() const override {
        return "add a new shard to the system";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(AddShardCmd).forRouter();

}  // namespace
}  // namespace mongo
