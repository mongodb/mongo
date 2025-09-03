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

        void typedRun(OperationContext* opCtx) {
            TrafficRecorder::get(opCtx->getServiceContext())
                .start(request(), opCtx->getServiceContext());
            LOGV2(20506,
                  "** Warning: The recording file contains unencrypted user traffic. We recommend "
                  "that you limit retention of this file and store it on an encrypted filesystem "
                  "volume.");
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

        void typedRun(OperationContext* opCtx) {
            // Not yet implemented
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
