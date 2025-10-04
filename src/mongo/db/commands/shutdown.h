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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shutdown_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit.h"
#include "mongo/util/ntservice.h"

#include <string>
#include <vector>

namespace mongo {
Status stepDownForShutdown(OperationContext* opCtx,
                           const Milliseconds& waitTime,
                           bool forceShutdown) noexcept;

namespace shutdown_detail {
/**
 * Completes the shutdown. 'timeout' is the total time permitted for shutdown-related timeouts.
 * 'quiesceTime' is the remaining time allowed for quiescing.
 */
void finishShutdown(OperationContext* opCtx,
                    bool force,
                    Milliseconds timeout,
                    Milliseconds quiesceTime);
}  // namespace shutdown_detail

template <typename Derived>
class CmdShutdown : public TypedCommand<Derived> {
public:
    using Request = ShutdownRequest;

    class Invocation final : public TypedCommand<Derived>::InvocationBase {
    public:
        using Base = typename TypedCommand<Derived>::InvocationBase;
        using Base::Base;

        void typedRun(OperationContext* opCtx) {
            auto force = Base::request().getForce();
            Seconds timeout{Base::request().getTimeoutSecs()};

            auto getCurrentTime = [&] {
                return opCtx->getServiceContext()->getPreciseClockSource()->now();
            };

            auto shutdownStartTime = getCurrentTime();

            // Commands derived from CmdShutdown should define their own
            // `beginShutdown` methods.
            Derived::beginShutdown(opCtx, force, timeout);

            auto quiesceTime =
                std::max(shutdownStartTime + timeout - getCurrentTime(), Milliseconds{0});
            shutdown_detail::finishShutdown(opCtx, force, timeout, quiesceTime);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(Base::request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto client = opCtx->getClient();
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(Base::request().getDbName().tenantId()),
                        ActionType::shutdown));
        }
    };

    bool requiresAuth() const override {
        return true;
    }
    bool adminOnly() const override {
        return true;
    }
    bool localHostOnlyIfNoAuth() const override {
        return true;
    }
    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }
};

}  // namespace mongo
