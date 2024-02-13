/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/xray_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#if __has_feature(xray_instrument)

#include <xray/xray_log_interface.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class StartXRayLoggingCmd final : public TypedCommand<StartXRayLoggingCmd> {
public:
    using Request = StartXRayLogging;

    std::string help() const override {
        return "Start XRay logging";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(8638310,
                    str::stream() << "XRay already running: " << __xray_log_get_current_mode(),
                    __xray_log_get_current_mode() == nullptr);


            auto selectStatus = __xray_log_select_mode(request().getMode().rawData());
            uassert(8638309,
                    str::stream() << "Failed to register XRay mode '" << request().getMode()
                                  << "' : " << selectStatus,
                    selectStatus == XRAY_REGISTRATION_OK);


            auto configStatus =
                __xray_log_init_mode(request().getMode().rawData(), request().getFlags().rawData());
            uassert(8638308,
                    str::stream() << "Failed to initialize XRay logging '" << request().getFlags()
                                  << "' : " << configStatus,
                    configStatus == XRAY_LOG_INITIALIZED);


            auto patchStatus = __xray_patch();
            uassert(8638307,
                    str::stream() << "Failed to patch XRay: " << patchStatus,
                    patchStatus == SUCCESS);


            LOGV2(8638306,
                  "Successfully started XRay tracing",
                  "mode"_attr = request().getMode(),
                  "flags"_attr = request().getFlags());
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};

MONGO_REGISTER_COMMAND(StartXRayLoggingCmd).forRouter().forShard();


class StopXRayLoggingCmd final : public TypedCommand<StopXRayLoggingCmd> {
public:
    using Request = StopXRayLogging;

    std::string help() const override {
        return "Stop XRay logging";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(8638305,
                    str::stream() << "XRay logging not running: " << __xray_log_get_current_mode(),
                    __xray_log_get_current_mode() != nullptr);


            auto finalizeStatus = __xray_log_finalize();
            uassert(8638304,
                    str::stream() << "Failed to finalize XRay logging: " << finalizeStatus,
                    finalizeStatus == XRAY_LOG_FINALIZED);


            auto unpatchStatus = __xray_unpatch();
            uassert(8638303,
                    str::stream() << "Failed to unpatch XRay: " << unpatchStatus,
                    unpatchStatus == SUCCESS);


            auto flushStatus = __xray_log_flushLog();
            uassert(8638302,
                    str::stream() << "Failed to flush XRay logs: " << flushStatus,
                    flushStatus == XRAY_LOG_FLUSHED);

            LOGV2(8638301, "Successfully stopped XRay tracing");
        }

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};

MONGO_REGISTER_COMMAND(StopXRayLoggingCmd).forRouter().forShard();

}  // namespace
}  // namespace mongo

#endif
