/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_fsync_unlock_cmd_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"

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
            BSONObj fsyncUnlockCmdObj = BSON("fsyncUnlock" << 1);

            auto responses = scatterGatherUnversionedTargetConfigServerAndShards(
                opCtx,
                DatabaseName::kAdmin,
                applyReadWriteConcern(
                    opCtx,
                    this,
                    CommandHelpers::filterCommandRequestForPassthrough(fsyncUnlockCmdObj)),
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
                    ActionType::fsyncUnlock));
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
