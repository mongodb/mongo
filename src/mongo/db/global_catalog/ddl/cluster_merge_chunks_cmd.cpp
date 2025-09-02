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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {
class ClusterMergeChunksCommand : public TypedCommand<ClusterMergeChunksCommand> {
public:
    using Request = ClusterMergeChunks;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Merge Chunks command\n"
               "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ] }";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            auto bounds = request().getBounds();
            uassertStatusOK(ChunkRange::validate(bounds));

            BSONObj minKey = bounds[0];
            BSONObj maxKey = bounds[1];

            const auto cri = getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(opCtx, ns());

            const auto& cm = cri.getChunkManager();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "shard key bounds "
                                  << "[" << minKey << "," << maxKey << ")"
                                  << " are not valid for shard key pattern "
                                  << cm.getShardKeyPattern().toBSON(),
                    (cm.getShardKeyPattern().isShardKey(minKey) &&
                     cm.getShardKeyPattern().isShardKey(maxKey)));

            minKey = cm.getShardKeyPattern().normalizeShardKey(minKey);
            maxKey = cm.getShardKeyPattern().normalizeShardKey(maxKey);

            const auto firstChunk = cm.findIntersectingChunkWithSimpleCollation(minKey);
            ChunkVersion placementVersion = cm.getVersion(firstChunk.getShardId());

            BSONObjBuilder cmdBuilder;
            ShardsvrMergeChunks cmd(ns(), bounds, placementVersion.epoch());
            cmd.setTimestamp(placementVersion.getTimestamp());
            cmd.serialize(&cmdBuilder);

            BSONObj remoteResult;

            // Throws, but handled at level above.  Don't want to rewrap to preserve exception
            // formatting.
            auto shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, firstChunk.getShardId()));

            auto response = uassertStatusOK(
                shard->runCommand(opCtx,
                                  ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                  DatabaseName::kAdmin,
                                  cmdBuilder.obj(),
                                  Shard::RetryPolicy::kNotIdempotent));
            uassertStatusOK(response.commandStatus);

            Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(ns(), boost::none);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::splitChunk));
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterMergeChunksCommand).forRouter();

}  // namespace
}  // namespace mongo
