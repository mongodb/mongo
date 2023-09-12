/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_stream_state_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/set_change_stream_state_coordinator.h"
#include "mongo/db/set_change_stream_state_coordinator_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

/**
 * The command that should run in the replica-set to set the state of the change stream in the
 * serverless.
 */
class SetChangeStreamStateCommand final : public TypedCommand<SetChangeStreamStateCommand> {
public:
    using Request = SetChangeStreamStateCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Sets the change stream state in the serverless\n"
               "Usage:\n"
               "    {setChangeStreamState: 1, enabled: <true|false>}\n"
               "Fields:\n"
               "    enabled:  enable or disable the change stream";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    str::stream() << SetChangeStreamStateCommandRequest::kCommandName
                                  << " command is only supported in serverless",
                    change_stream_serverless_helpers::isChangeCollectionsModeActive());

            const auto tenantId =
                change_stream_serverless_helpers::resolveTenantId(request().getDbName().tenantId());
            uassert(ErrorCodes::BadValue,
                    str::stream() << SetChangeStreamStateCommandRequest::kCommandName
                                  << " command must be provided with a tenant id",
                    tenantId);

            // Prepare the payload for the 'SetChangeStreamStateCoordinator'.
            SetChangeStreamStateCoordinatorId coordinatorId;
            coordinatorId.setTenantId(tenantId);
            SetChangeStreamStateCoordinatorDocument coordinatorDoc{
                coordinatorId, request().getChangeStreamStateParameters().toBSON()};

            // Dispatch the request to the 'SetChangeStreamStateCoordinatorService'.
            const auto service = SetChangeStreamStateCoordinatorService::getService(opCtx);
            const auto instance = service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON());

            const auto coordinatorCompletionFuture = instance->getCompletionFuture();
            coordinatorCompletionFuture.get(opCtx);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::setChangeStreamState}));
        }
    };
};
MONGO_REGISTER_COMMAND(SetChangeStreamStateCommand).forShard();

class GetChangeStreamStateCommand final : public TypedCommand<GetChangeStreamStateCommand> {
public:
    using Request = GetChangeStreamStateCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Gets the change stream state in the serverless\n"
               "Usage:\n"
               "    {getChangeStreamState: 1}";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        auto typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    str::stream() << GetChangeStreamStateCommandRequest::kCommandName
                                  << " command is only supported in serverless",
                    change_stream_serverless_helpers::isChangeCollectionsModeActive());

            const auto tenantId =
                change_stream_serverless_helpers::resolveTenantId(request().getDbName().tenantId());
            uassert(ErrorCodes::BadValue,
                    str::stream() << GetChangeStreamStateCommandRequest::kCommandName
                                  << " command must be provided with a tenant id",
                    tenantId);


            // Set the change stream enablement state in the 'reply' object.
            GetChangeStreamStateCommandRequest::Reply reply{
                change_stream_serverless_helpers::isChangeStreamEnabled(opCtx, *tenantId)};

            return reply;
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::getChangeStreamState}));
        }
    };
};
MONGO_REGISTER_COMMAND(GetChangeStreamStateCommand).forShard();

}  // namespace
}  // namespace mongo
