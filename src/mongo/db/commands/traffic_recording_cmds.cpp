// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/db/traffic_recorder_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class StartRecordingCommand final : public TypedCommand<StartRecordingCommand> {
public:
    using Request = StartTrafficRecording;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        StartReply typedRun(OperationContext* opCtx) {
            auto recordingID = TrafficRecorder::get(opCtx->getServiceContext())
                                   .start(request(), opCtx->getServiceContext());
            LOGV2(20506,
                  "** Warning: The recording file contains unencrypted user traffic. We recommend "
                  "that you limit retention of this file and store it on an encrypted filesystem "
                  "volume.");
            return StartReply(recordingID);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::trafficRecord}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
};
MONGO_REGISTER_COMMAND(StartRecordingCommand).forRouter().forShard();

class StopRecordingCommand final : public TypedCommand<StopRecordingCommand> {
public:
    using Request = StopTrafficRecording;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            TrafficRecorder::get(opCtx->getServiceContext()).stop(opCtx->getServiceContext());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::trafficRecord}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
};
MONGO_REGISTER_COMMAND(StopRecordingCommand).forRouter().forShard();


class GetRecordingStatusCommand final : public TypedCommand<GetRecordingStatusCommand> {
public:
    using Request = GetTrafficRecordingStatus;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        StatusReply typedRun(OperationContext* opCtx) {
            return TrafficRecorder::get(opCtx->getServiceContext()).status();
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::trafficRecord}));
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
};
MONGO_REGISTER_COMMAND(GetRecordingStatusCommand).forRouter().forShard();

}  // namespace
}  // namespace mongo
