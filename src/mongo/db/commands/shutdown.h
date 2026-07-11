// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/modules.h"
#include "mongo/util/ntservice.h"


namespace mongo {
[[MONGO_MOD_PUBLIC]] Status stepDownForShutdown(OperationContext* opCtx,
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
