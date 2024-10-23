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

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

/**
 * Mongos-side command for merging chunks, passes command to appropriate shard.
 */
class ClusterMergeChunksCommand : public ErrmsgCommandDeprecated {
public:
    ClusterMergeChunksCommand() : ErrmsgCommandDeprecated("mergeChunks") {}

    std::string help() const override {
        return "Merge Chunks command\n"
               "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ] }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(parseNs(dbName, cmdObj)),
                     ActionType::splitChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    // Required
    static BSONField<std::string> nsField;
    static BSONField<std::vector<BSONObj>> boundsField;

    // Used to send sharding state
    static BSONField<std::string> shardNameField;
    static BSONField<std::string> configField;


    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        std::vector<BSONObj> bounds;
        if (!FieldParser::extract(cmdObj, boundsField, &bounds, &errmsg)) {
            return false;
        }

        if (bounds.size() == 0) {
            errmsg = "no bounds were specified";
            return false;
        }

        if (bounds.size() != 2) {
            errmsg = "only a min and max bound may be specified";
            return false;
        }

        BSONObj minKey = bounds[0];
        BSONObj maxKey = bounds[1];

        if (minKey.isEmpty()) {
            errmsg = "no min key specified";
            return false;
        }

        if (maxKey.isEmpty()) {
            errmsg = "no max key specified";
            return false;
        }

        const auto cm = getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(opCtx, nss).cm;

        if (!cm.getShardKeyPattern().isShardKey(minKey) ||
            !cm.getShardKeyPattern().isShardKey(maxKey)) {
            errmsg = str::stream()
                << "shard key bounds "
                << "[" << minKey << "," << maxKey << ")"
                << " are not valid for shard key pattern " << cm.getShardKeyPattern().toBSON();
            return false;
        }

        minKey = cm.getShardKeyPattern().normalizeShardKey(minKey);
        maxKey = cm.getShardKeyPattern().normalizeShardKey(maxKey);

        const auto firstChunk = cm.findIntersectingChunkWithSimpleCollation(minKey);
        ChunkVersion placementVersion = cm.getVersion(firstChunk.getShardId());

        BSONObjBuilder remoteCmdObjB;
        remoteCmdObjB.append(cmdObj[ClusterMergeChunksCommand::nsField()]);
        remoteCmdObjB.append(cmdObj[ClusterMergeChunksCommand::boundsField()]);
        remoteCmdObjB.append(
            ClusterMergeChunksCommand::configField(),
            Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString().toString());
        remoteCmdObjB.append(ClusterMergeChunksCommand::shardNameField(),
                             firstChunk.getShardId().toString());
        remoteCmdObjB.append("epoch", placementVersion.epoch());
        remoteCmdObjB.append("timestamp", placementVersion.getTimestamp());

        BSONObj remoteResult;

        // Throws, but handled at level above.  Don't want to rewrap to preserve exception
        // formatting.
        auto shard = uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, firstChunk.getShardId()));

        auto response = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            remoteCmdObjB.obj(),
            Shard::RetryPolicy::kNotIdempotent));
        uassertStatusOK(response.commandStatus);

        Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(nss, boost::none);

        CommandHelpers::filterCommandReplyForPassthrough(response.response, &result);
        return true;
    }
};
MONGO_REGISTER_COMMAND(ClusterMergeChunksCommand).forRouter();

BSONField<std::string> ClusterMergeChunksCommand::nsField("mergeChunks");
BSONField<std::vector<BSONObj>> ClusterMergeChunksCommand::boundsField("bounds");

BSONField<std::string> ClusterMergeChunksCommand::configField("config");
BSONField<std::string> ClusterMergeChunksCommand::shardNameField("shardName");

}  // namespace
}  // namespace mongo
