/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
