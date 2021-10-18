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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

class MoveChunkCmd : public ErrmsgCommandDeprecated {
public:
    MoveChunkCmd() : ErrmsgCommandDeprecated("moveChunk", "movechunk") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Example: move chunk that contains the doc {num : 7} to shard001\n"
               "  { movechunk : 'test.foo' , find : { num : 7 } , to : 'shard0001' }\n"
               "Example: move chunk with lower bound 0 and upper bound 10 to shard001\n"
               "  { movechunk : 'test.foo' , bounds : [ { num : 0 } , { num : 10 } ] "
               " , to : 'shard001' }\n";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::moveChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        Timer t;

        const NamespaceString nss(parseNs(dbname, cmdObj));

        const auto cm = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));

        const auto toElt = cmdObj["to"];
        uassert(ErrorCodes::TypeMismatch,
                "'to' must be of type String",
                toElt.type() == BSONType::String);
        const std::string toString = toElt.str();
        if (!toString.size()) {
            errmsg = "you have to specify where you want to move the chunk";
            return false;
        }

        const auto toStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, toString);
        if (!toStatus.isOK()) {
            LOGV2_OPTIONS(22755,
                          {logv2::UserAssertAfterLog(ErrorCodes::ShardNotFound)},
                          "Could not move chunk in {namespace} to {toShardId} because that shard"
                          " does not exist",
                          "moveChunk destination shard does not exist",
                          "toShardId"_attr = toString,
                          "namespace"_attr = nss.ns());
        }

        const auto to = toStatus.getValue();
        const auto forceJumboElt = cmdObj["forceJumbo"];
        const auto forceJumbo = forceJumboElt && forceJumboElt.Bool();

        // so far, chunk size serves test purposes; it may or may not become a supported parameter
        long long maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
        if (maxChunkSizeBytes == 0) {
            maxChunkSizeBytes =
                Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes();
        }

        BSONObj find = cmdObj.getObjectField("find");
        BSONObj bounds = cmdObj.getObjectField("bounds");

        // check that only one of the two chunk specification methods is used
        if (find.isEmpty() == bounds.isEmpty()) {
            errmsg = "need to specify either a find query, or both lower and upper bounds.";
            return false;
        }

        boost::optional<Chunk> chunk;

        if (!find.isEmpty()) {
            // find
            BSONObj shardKey =
                uassertStatusOK(cm.getShardKeyPattern().extractShardKeyFromQuery(opCtx, nss, find));
            if (shardKey.isEmpty()) {
                errmsg = str::stream() << "no shard key found in chunk query " << find;
                return false;
            }

            chunk.emplace(cm.findIntersectingChunkWithSimpleCollation(shardKey));
        } else {
            // bounds
            if (!cm.getShardKeyPattern().isShardKey(bounds[0].Obj()) ||
                !cm.getShardKeyPattern().isShardKey(bounds[1].Obj())) {
                errmsg = str::stream()
                    << "shard key bounds "
                    << "[" << bounds[0].Obj() << "," << bounds[1].Obj() << ")"
                    << " are not valid for shard key pattern " << cm.getShardKeyPattern().toBSON();
                return false;
            }

            BSONObj minKey = cm.getShardKeyPattern().normalizeShardKey(bounds[0].Obj());
            BSONObj maxKey = cm.getShardKeyPattern().normalizeShardKey(bounds[1].Obj());

            chunk.emplace(cm.findIntersectingChunkWithSimpleCollation(minKey));

            if (chunk->getMin().woCompare(minKey) != 0 || chunk->getMax().woCompare(maxKey) != 0) {
                errmsg = str::stream() << "no chunk found with the shard key bounds "
                                       << ChunkRange(minKey, maxKey).toString();
                return false;
            }
        }

        const auto secondaryThrottle =
            uassertStatusOK(MigrationSecondaryThrottleOptions::createFromCommand(cmdObj));

        ChunkType chunkType;
        chunkType.setCollectionUUID(*cm.getUUID());
        chunkType.setMin(chunk->getMin());
        chunkType.setMax(chunk->getMax());
        chunkType.setShard(chunk->getShardId());
        chunkType.setVersion(cm.getVersion());

        uassertStatusOK(configsvr_client::moveChunk(opCtx,
                                                    nss,
                                                    chunkType,
                                                    to->getId(),
                                                    maxChunkSizeBytes,
                                                    secondaryThrottle,
                                                    cmdObj["_waitForDelete"].trueValue() ||
                                                        cmdObj["waitForDelete"].trueValue(),
                                                    forceJumbo));

        Grid::get(opCtx)
            ->catalogCache()
            ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                nss, boost::none, chunk->getShardId());
        Grid::get(opCtx)
            ->catalogCache()
            ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                nss, boost::none, to->getId());

        result.append("millis", t.millis());
        return true;
    }

} moveChunk;

}  // namespace
}  // namespace mongo
