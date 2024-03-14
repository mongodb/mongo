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

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

class RefreshQueryAnalyzerConfigurationCmd
    : public TypedCommand<RefreshQueryAnalyzerConfigurationCmd> {
public:
    using Request = RefreshQueryAnalyzerConfiguration;
    using Response = RefreshQueryAnalyzerConfigurationResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_refreshQueryAnalyzerConfiguration command is not supported on a standalone "
                    "mongod",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
            uassert(ErrorCodes::IllegalOperation,
                    "_refreshQueryAnalyzerConfiguration command is not supported on a multitenant "
                    "replica set",
                    !gMultitenancySupport);
            uassert(
                ErrorCodes::IllegalOperation,
                "_refreshQueryAnalyzerConfiguration command is not supported on a shardsvr mongod",
                !serverGlobalParams.clusterRole.isShardOnly());

            auto coodinator = analyze_shard_key::QueryAnalysisCoordinator::get(opCtx);
            auto configurations = coodinator->getNewConfigurationsForSampler(
                opCtx, request().getName(), request().getNumQueriesExecutedPerSecond());
            return {configurations};
        }

    private:
        NamespaceString ns() const {
            return NamespaceString(DatabaseName::kAdmin);
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

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Refreshes the query analyzer configurations for all collections.";
    }
};
MONGO_REGISTER_COMMAND(RefreshQueryAnalyzerConfigurationCmd).forShard();

}  // namespace

}  // namespace mongo
