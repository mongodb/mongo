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

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/commands/cluster_pipeline_cmd.h"
#include "mongo/s/grid.h"

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

    static void doCheckAuthorization(OperationContext* opCtx, const PrivilegeVector& privileges) {
        uassert(ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        Grid::get(opCtx)->assertShardingIsInitialized();

        // A cluster command on the config server may attempt to use a ShardLocal to target itself,
        // which triggers an invariant, so only shard servers can run this.
        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "Cannot explain a cluster aggregate command on a mongod");
    }

    static AggregateCommandRequest parseAggregationRequest(
        OperationContext* opCtx,
        const OpMsgRequest& opMsgRequest,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity,
        bool apiStrict) {
        // Replace clusterAggregate in the request body because the parser doesn't recognize it.
        auto modifiedRequestBody =
            opMsgRequest.body.replaceFieldNames(BSON(AggregateCommandRequest::kCommandName << 1));
        return aggregation_request_helper::parseFromBSON(
            opCtx,
            DatabaseNameUtil::deserialize(opMsgRequest.getValidatedTenantId(),
                                          opMsgRequest.getDatabase()),
            modifiedRequestBody,
            explainVerbosity,
            apiStrict);
    }
};
ClusterPipelineCommandBase<ClusterPipelineCommandD> clusterPipelineCmdD;

}  // namespace
}  // namespace mongo
