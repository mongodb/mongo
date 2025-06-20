/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/request_types/placement_history_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {
namespace {

class ConfigsvrGetHistoricalPlacementCommand final
    : public TypedCommand<ConfigsvrGetHistoricalPlacementCommand> {
public:
    using Request = ConfigsvrGetHistoricalPlacement;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        ConfigsvrGetHistoricalPlacementResponse typedRun(OperationContext* opCtx) {
            const NamespaceString& nss = ns();

            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrGetHistoricalPlacement can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            // Set the operation context read concern level to majority for reads into the config
            // database.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);

            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

            // TODO (SERVER-83704) remove the following code block once 8.0 becomes last LTS.
            if (!feature_flags::gPlacementHistoryPostFCV3.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                // The content of config.placementHistory is pending to be reset and may not be
                // reliable. Use the current content of config.shards to compose an approximate
                // response.
                auto shardsWithOpTime = catalogClient->getAllShards(
                    opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
                std::vector<ShardId> shardIds;
                std::transform(shardsWithOpTime.value.begin(),
                               shardsWithOpTime.value.end(),
                               std::back_inserter(shardIds),
                               [](const ShardType& s) { return s.getName(); });
                HistoricalPlacement historicalPlacement{std::move(shardIds), false /*isExact*/};
                ConfigsvrGetHistoricalPlacementResponse response(std::move(historicalPlacement));
                return response;
            }

            boost::optional<NamespaceString> targetedNs = request().getTargetWholeCluster()
                ? (boost::optional<NamespaceString>)boost::none
                : nss;
            return ConfigsvrGetHistoricalPlacementResponse(
                catalogClient->getHistoricalPlacement(opCtx, request().getAt(), targetedNs));
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Allows to run queries concerning historical placement of a namespace in "
               "a controlled way.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrGetHistoricalPlacementCommand).forShard();

}  // namespace
}  // namespace mongo
