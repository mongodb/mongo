// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/commands/cluster_fsync_unlock_cmd_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {
class FsyncUnlockCommand : public TypedCommand<FsyncUnlockCommand> {
public:
    using Request = ClusterFsyncUnlock;


    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Intermediate wrapper to interface with ReplyBuilderInterface.
         */
        class Response {
        public:
            Response(BSONObj obj) : _obj(std::move(obj)) {}

            void serialize(BSONObjBuilder* builder) const {
                builder->appendElements(_obj);
            }

        private:
            const BSONObj _obj;
        };

        Response typedRun(OperationContext* opCtx) {
            ClusterFsyncUnlock fsyncUnlockCmd;
            fsyncUnlockCmd.setDbName(request().getDbName());
            setReadWriteConcern(opCtx, fsyncUnlockCmd, this);

            auto responses = scatterGatherUnversionedTargetConfigServerAndShards(
                opCtx,
                DatabaseName::kAdmin,
                CommandHelpers::filterCommandRequestForPassthrough(fsyncUnlockCmd.toBSON()),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent);

            BSONObjBuilder result;
            std::string errmsg;
            const auto rawResponsesResult = appendRawResponses(opCtx, &errmsg, &result, responses);

            if (!errmsg.empty()) {
                CommandHelpers::appendSimpleCommandStatus(
                    result, rawResponsesResult.responseOK, errmsg);
            }

            return Response(result.obj());
        }

    private:
        NamespaceString ns() const override {
            return {};
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto authorizationSession = AuthorizationSession::get(opCtx->getClient());
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                authorizationSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(authorizationSession->getUserTenantId()),
                    ActionType::unlock));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "invoke fsync unlock on all shards belonging to the cluster";
    }
};
MONGO_REGISTER_COMMAND(FsyncUnlockCommand).forRouter();

}  // namespace
}  // namespace mongo
