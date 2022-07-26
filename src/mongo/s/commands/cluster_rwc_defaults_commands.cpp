/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Implements the setDefaultRWConcern command on mongos. Inherits from BasicCommand because this
 * command forwards the user's request to the config server and does not need to parse it.
 */
class ClusterSetDefaultRWConcernCommand : public BasicCommand {
public:
    ClusterSetDefaultRWConcernCommand() : BasicCommand("setDefaultRWConcern") {}

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            NamespaceString::kAdminDb.toString(),
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                opCtx->getWriteConcern()),
            Shard::RetryPolicy::kNotIdempotent));

        uassertStatusOK(cmdResponse.commandStatus);
        uassertStatusOK(cmdResponse.writeConcernStatus);

        // Quickly pick up the new defaults by setting them in the cache.
        auto newDefaults = RWConcernDefault::parse(IDLParserContext("ClusterSetDefaultRWConcern"),
                                                   cmdResponse.response);
        if (auto optWC = newDefaults.getDefaultWriteConcern()) {
            if (optWC->hasCustomWriteMode()) {
                LOGV2_WARNING(
                    6081700,
                    "A custom write concern is being set as the default write concern in a sharded "
                    "cluster. This set is unchecked, but if the custom write concern does not "
                    "exist on all shards in the cluster, errors will occur upon writes",
                    "customWriteConcern"_attr = stdx::get<std::string>(optWC->w));
            }
        }
        ReadWriteConcernDefaults::get(opCtx).setDefault(opCtx, std::move(newDefaults));

        CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.response, &result);
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForPrivilege(Privilege{ResourcePattern::forClusterResource(),
                                                      ActionType::setDefaultRWConcern})) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string help() const override {
        return "Sets the default read or write concern for a cluster";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} clusterSetDefaultRWConcernCommand;

/**
 * Implements the getDefaultRWConcern command on mongos.
 */
class ClusterGetDefaultRWConcernCommand final
    : public TypedCommand<ClusterGetDefaultRWConcernCommand> {
public:
    using Request = GetDefaultRWConcern;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        GetDefaultRWConcernResponse typedRun(OperationContext* opCtx) {
            auto& rwcDefaults = ReadWriteConcernDefaults::get(opCtx->getServiceContext());
            if (request().getInMemory().value_or(false)) {
                const auto rwcDefault = rwcDefaults.getDefault(opCtx);
                GetDefaultRWConcernResponse response;
                response.setRWConcernDefault(rwcDefault);
                response.setLocalUpdateWallClockTime(rwcDefault.localUpdateWallClockTime());
                response.setInMemory(true);
                return response;
            }

            // If not asking for the in-memory defaults, fetch them from the config server
            GetDefaultRWConcern configsvrRequest;
            configsvrRequest.setDbName(request().getDbName());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                NamespaceString::kAdminDb.toString(),
                applyReadWriteConcern(opCtx, this, configsvrRequest.toBSON({})),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);

            return GetDefaultRWConcernResponse::parse(
                IDLParserContext("ClusterGetDefaultRWConcernResponse"), cmdResponse.response);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{ResourcePattern::forClusterResource(),
                                                             ActionType::getDefaultRWConcern}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const override {
        return "Gets the default read or write concern for a cluster";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} clusterGetDefaultRWConcernCommand;

}  // namespace
}  // namespace mongo
