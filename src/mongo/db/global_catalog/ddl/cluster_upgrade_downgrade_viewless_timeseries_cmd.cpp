// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.

#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/upgrade_downgrade_viewless_timeseries_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/testing_proctor.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class UpgradeDowngradeViewlessTimeseriesCommand
    : public TypedCommand<UpgradeDowngradeViewlessTimeseriesCommand> {
public:
    using Request = UpgradeDowngradeViewlessTimeseries;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            const auto nss = ns();

            ShardsvrUpgradeDowngradeViewlessTimeseries shardsvrReq(nss);
            shardsvrReq.setMode(request().getMode());

            generic_argument_util::setMajorityWriteConcern(shardsvrReq, &opCtx->getWriteConcern());

            sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
            router.route("upgradeDowngradeViewlessTimeseries",
                         [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                             auto cmdResponse =
                                 executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                     opCtx,
                                     nss.dbName(),
                                     dbInfo,
                                     shardsvrReq.toBSON(),
                                     ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                     Shard::RetryPolicy::kIdempotent);
                             const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                             uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                         });
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        // No auth needed because a test-only command is exclusively enabled via command line.
        void doCheckAuthorization(OperationContext* opCtx) const override {
            // Guardrail: ensure TestingProctor is enabled since this command is test only.
            tassert(11590606,
                    "upgradeDowngradeViewlessTimeseries command requires TestingProctor to be "
                    "enabled",
                    TestingProctor::instance().isEnabled());
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exposed for emergency use only. Converts timeseries "
               "collections from/to viewless timeseries format";
    }
};
MONGO_REGISTER_COMMAND(UpgradeDowngradeViewlessTimeseriesCommand).testOnly().forRouter();

}  // namespace
}  // namespace mongo
