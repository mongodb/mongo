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
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

class ShardsvrMergeChunksCommand : public TypedCommand<ShardsvrMergeChunksCommand> {
public:
    using Request = ShardsvrMergeChunks;

    ShardsvrMergeChunksCommand() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command to merge a contiguous range of chunks.\n"
               "Usage: { _shardsvrMergeChunks: <ns>, epoch: <epoch>, bounds: [<min key>, <max "
               "key>] }";
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto bounds = request().getBounds();
            uassertStatusOK(ChunkRange::validate(bounds));

            ChunkRange chunkRange(bounds[0], bounds[1]);

            auto scopedSplitOrMergeChunk(
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                    opCtx, ns(), chunkRange)));

            auto expectedEpoch = request().getEpoch();
            auto expectedTimestamp = request().getTimestamp();

            // Check that the preconditions for merge chunks are met and throw StaleShardVersion
            // otherwise.
            const auto metadataBeforeMerge = [&]() {
                uassertStatusOK(
                    FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                        opCtx, ns(), boost::none));
                const auto metadata =
                    checkCollectionIdentity(opCtx, ns(), expectedEpoch, expectedTimestamp);
                checkShardKeyPattern(opCtx, ns(), metadata, chunkRange);
                checkRangeOwnership(opCtx, ns(), metadata, chunkRange);
                return metadata;
            }();

            auto const shardingState = ShardingState::get(opCtx);

            ConfigSvrMergeChunks request{
                ns(), shardingState->shardId(), metadataBeforeMerge.getUUID(), chunkRange};
            request.setEpoch(expectedEpoch);
            request.setTimestamp(expectedTimestamp);
            request.setWriteConcern(defaultMajorityWriteConcernDoNotUse());

            auto cmdResponse =
                uassertStatusOK(Grid::get(opCtx)
                                    ->shardRegistry()
                                    ->getConfigShard()
                                    ->runCommandWithIndefiniteRetries(
                                        opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kAdmin,
                                        request.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            auto chunkVersionReceived = [&]() -> boost::optional<ChunkVersion> {
                // Old versions might not have the shardVersion field
                if (cmdResponse.response[ChunkVersion::kChunkVersionField]) {
                    return ChunkVersion::parse(
                        cmdResponse.response[ChunkVersion::kChunkVersionField]);
                }
                return boost::none;
            }();
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, ns(), std::move(chunkVersionReceived)));

            uassertStatusOKWithContext(cmdResponse.commandStatus, "Failed to commit chunk merge");
            uassertStatusOKWithContext(cmdResponse.writeConcernStatus,
                                       "Failed to commit chunk merge");
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
};
MONGO_REGISTER_COMMAND(ShardsvrMergeChunksCommand).forShard();

}  // namespace
}  // namespace mongo
