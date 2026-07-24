// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_metrics_filtering_allowlist_command_gen.h"
#include "mongo/db/metrics_policy_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {

class CmdListMetricsFilteringAllowlist final
    : public TypedCommand<CmdListMetricsFilteringAllowlist> {
public:
    using Request = ListMetricsFilteringAllowlist;
    using Response = ListMetricsFilteringAllowlistReply;

    std::string help() const override {
        return "Internal test command that returns the metrics filtering allowlist for a given "
               "category";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        NamespaceString ns() const override {
            return NamespaceString::kEmpty;
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const override {}

        Response typedRun(OperationContext* opCtx) {
            auto& metricsPolicyManager = MetricsPolicyManager::get(opCtx);

            switch (request().getCategory()) {
                case MetricsCategoryEnum::serverStatus: {
                    return {metricsPolicyManager.getServerStatusAllowlistPaths()};
                }
                case MetricsCategoryEnum::replSetGetStatus: {
                    return {metricsPolicyManager.getReplSetGetStatusAllowlistPaths()};
                }
                case MetricsCategoryEnum::collStats: {
                    return {metricsPolicyManager.getCollStatsAllowlistPaths()};
                }
            }

            uasserted(ErrorCodes::BadValue, "Unsupported metrics category");
        }
    };
};

MONGO_REGISTER_COMMAND(CmdListMetricsFilteringAllowlist).testOnly().forShard().forRouter();

}  // namespace mongo
