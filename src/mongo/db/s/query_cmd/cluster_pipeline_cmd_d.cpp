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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/commands/query_cmd/cluster_pipeline_cmd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

/**
 * Implements the cluster aggregate command on mongod.
 */
struct ClusterPipelineCommandD {
    static constexpr StringData kName = "clusterAggregate"_sd;

    static const std::set<std::string>& getApiVersions() {
        return kNoApiVersions;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     const OpMsgRequest& opMsgRequest,
                                     const PrivilegeVector& privileges) {
        uassert(ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(opMsgRequest.getValidatedTenantId()),
                        ActionType::internal));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        Grid::get(opCtx)->assertShardingIsInitialized();

        // A cluster command on the config server may attempt to use a ShardLocal to target itself,
        // which triggers an invariant, so only shard servers can run this.
        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "Cannot explain a cluster aggregate command on a mongod");
    }

    static AggregateCommandRequest parseAggregationRequest(
        const OpMsgRequest& opMsgRequest,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) {
        // Replace clusterAggregate in the request body because the parser doesn't recognize it.
        auto modifiedRequestBody =
            opMsgRequest.body.replaceFieldNames(BSON(AggregateCommandRequest::kCommandName << 1));
        return aggregation_request_helper::parseFromBSON(
            modifiedRequestBody, opMsgRequest.validatedTenancyScope, explainVerbosity);
    }
};
MONGO_REGISTER_COMMAND(ClusterPipelineCommandBase<ClusterPipelineCommandD>).forShard();

}  // namespace
}  // namespace mongo
