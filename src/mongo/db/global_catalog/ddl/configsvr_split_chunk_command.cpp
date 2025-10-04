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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/split_chunk_request_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"

#include <string>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using std::string;

/**
 * Internal sharding command run on config servers to split a chunk.
 *
 * Format:
 * {
 *   _configsvrCommitChunkSplit: <string namespace>,
 *   collEpoch: <OID epoch>,
 *   min: <BSONObj chunkToSplitMin>,
 *   max: <BSONObj chunkToSplitMax>,
 *   splitPoints: [<BSONObj key>, ...],
 *   shard: <string shard>,
 *   writeConcern: <BSONObj>
 * }
 */

constexpr StringData kCollectionVersionField = "collectionVersion"_sd;

class ConfigSvrSplitChunkCommand : public BasicCommand {
public:
    ConfigSvrSplitChunkCommand() : BasicCommand("_configsvrCommitChunkSplit") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is sent by a shard to the sharding config server. Do "
               "not call directly. Receives, validates, and processes a SplitChunkRequest.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrCommitChunkSplit can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        auto parsedRequest = uassertStatusOK(SplitChunkRequest::parseFromConfigCommand(cmdObj));

        auto shardAndCollVers = uassertStatusOK(
            ShardingCatalogManager::get(opCtx)->commitChunkSplit(opCtx,
                                                                 parsedRequest.getNamespace(),
                                                                 parsedRequest.getEpoch(),
                                                                 parsedRequest.getTimestamp(),
                                                                 parsedRequest.getChunkRange(),
                                                                 parsedRequest.getSplitPoints(),
                                                                 parsedRequest.getShardName()));

        shardAndCollVers.collectionPlacementVersion.serialize(kCollectionVersionField, &result);
        shardAndCollVers.shardPlacementVersion.serialize(ChunkVersion::kChunkVersionField, &result);

        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrSplitChunkCommand).forShard();
}  // namespace
}  // namespace mongo
