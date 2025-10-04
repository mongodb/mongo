/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/shard_key_pattern_query_util.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/cluster_commands_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ClearJumboFlagCommand final : public TypedCommand<ClearJumboFlagCommand> {
public:
    using Request = ClusterClearJumboFlag;

    class Invocation : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::clearJumboFlag));
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            const auto cm = getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(opCtx, ns())
                                .getChunkManager();

            uassert(ErrorCodes::InvalidOptions,
                    "bounds can only have exactly 2 elements",
                    !request().getBounds() || request().getBounds()->size() == 2);

            uassert(ErrorCodes::InvalidOptions,
                    "cannot specify bounds and find at the same time",
                    !(request().getFind() && request().getBounds()));

            uassert(ErrorCodes::InvalidOptions,
                    "need to specify find or bounds",
                    request().getFind() || request().getBounds());

            boost::optional<Chunk> chunk;

            if (request().getFind()) {
                BSONObj shardKey = uassertStatusOK(extractShardKeyFromBasicQuery(
                    opCtx, ns(), cm.getShardKeyPattern(), *request().getFind()));
                uassert(51260,
                        str::stream()
                            << "no shard key found in chunk query " << *request().getFind(),
                        !shardKey.isEmpty());

                chunk.emplace(cm.findIntersectingChunkWithSimpleCollation(shardKey));
            } else {
                auto boundsArray = *request().getBounds();
                BSONObj minKey = cm.getShardKeyPattern().normalizeShardKey(boundsArray.front());
                BSONObj maxKey = cm.getShardKeyPattern().normalizeShardKey(boundsArray.back());

                chunk.emplace(cm.findIntersectingChunkWithSimpleCollation(minKey));

                uassert(51261,
                        str::stream() << "no chunk found with the shard key bounds "
                                      << ChunkRange(minKey, maxKey).toString(),
                        chunk->getMin().woCompare(minKey) == 0 &&
                            chunk->getMax().woCompare(maxKey) == 0);
            }

            ConfigsvrClearJumboFlag configCmd(
                ns(), cm.getVersion().epoch(), chunk->getMin(), chunk->getMax());
            configCmd.setDbName(request().getDbName());
            generic_argument_util::setMajorityWriteConcern(configCmd, &opCtx->getWriteConcern());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        DatabaseName::kAdmin,
                                        configCmd.toBSON(),
                                        Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);
            uassertStatusOK(cmdResponse.writeConcernStatus);
        }
    };

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }


    std::string help() const override {
        return "clears the jumbo flag of the chunk that contains the key\n"
               "   { clearJumboFlag : 'alleyinsider.blog.posts' , find : { ts : 1 } }\n";
    }
};
MONGO_REGISTER_COMMAND(ClearJumboFlagCommand).forRouter();

}  // namespace
}  // namespace mongo
