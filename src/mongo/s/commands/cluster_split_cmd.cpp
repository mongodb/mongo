/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Asks the mongod holding this chunk to find a key that approximately divides the specified chunk
 * in two. Throws on error or if the chunk is empty.
 */
BSONObj selectMedianKey(OperationContext* opCtx,
                        const ShardId& shardId,
                        const NamespaceString& nss,
                        const ShardKeyPattern& shardKeyPattern,
                        const ChunkRange& chunkRange) {
    BSONObjBuilder cmd;
    cmd.append("splitVector", nss.ns());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    chunkRange.append(&cmd);
    cmd.appendBool("force", true);

    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));

    auto cmdResponse = uassertStatusOK(
        shard->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                "admin",
                                                cmd.obj(),
                                                Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cmdResponse.commandStatus);

    BSONObjIterator it(cmdResponse.response.getObjectField("splitKeys"));
    if (it.more()) {
        return it.next().Obj().getOwned();
    }

    uasserted(ErrorCodes::CannotSplit,
              "Unable to find median in chunk, possibly because chunk is empty.");
}

class SplitCollectionCmd : public Command {
public:
    SplitCollectionCmd() : Command("split", false, "split") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void help(std::stringstream& help) const override {
        help << " example: - split the shard that contains give key\n"
             << "   { split : 'alleyinsider.blog.posts' , find : { ts : 1 } }\n"
             << " example: - split the shard that contains the key with this as the middle\n"
             << "   { split : 'alleyinsider.blog.posts' , middle : { ts : 1 } }\n"
             << " NOTE: this does not move the chunks, it just creates a logical separation.";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        auto routingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));
        const auto cm = routingInfo.cm();

        const BSONField<BSONObj> findField("find", BSONObj());
        const BSONField<BSONArray> boundsField("bounds", BSONArray());
        const BSONField<BSONObj> middleField("middle", BSONObj());

        BSONObj find;
        if (FieldParser::extract(cmdObj, findField, &find, &errmsg) == FieldParser::FIELD_INVALID) {
            return false;
        }

        BSONArray bounds;
        if (FieldParser::extract(cmdObj, boundsField, &bounds, &errmsg) ==
            FieldParser::FIELD_INVALID) {
            return false;
        }

        if (!bounds.isEmpty()) {
            if (!bounds.hasField("0")) {
                errmsg = "lower bound not specified";
                return false;
            }

            if (!bounds.hasField("1")) {
                errmsg = "upper bound not specified";
                return false;
            }
        }

        if (!find.isEmpty() && !bounds.isEmpty()) {
            errmsg = "cannot specify bounds and find at the same time";
            return false;
        }

        BSONObj middle;

        if (FieldParser::extract(cmdObj, middleField, &middle, &errmsg) ==
            FieldParser::FIELD_INVALID) {
            return false;
        }

        if (find.isEmpty() && bounds.isEmpty() && middle.isEmpty()) {
            errmsg = "need to specify find/bounds or middle";
            return false;
        }

        if (!find.isEmpty() && !middle.isEmpty()) {
            errmsg = "cannot specify find and middle together";
            return false;
        }

        if (!bounds.isEmpty() && !middle.isEmpty()) {
            errmsg = "cannot specify bounds and middle together";
            return false;
        }

        std::shared_ptr<Chunk> chunk;

        if (!find.isEmpty()) {
            // find
            BSONObj shardKey =
                uassertStatusOK(cm->getShardKeyPattern().extractShardKeyFromQuery(opCtx, find));
            if (shardKey.isEmpty()) {
                errmsg = stream() << "no shard key found in chunk query " << find;
                return false;
            }

            chunk = cm->findIntersectingChunkWithSimpleCollation(shardKey);
        } else if (!bounds.isEmpty()) {
            // bounds
            if (!cm->getShardKeyPattern().isShardKey(bounds[0].Obj()) ||
                !cm->getShardKeyPattern().isShardKey(bounds[1].Obj())) {
                errmsg = str::stream() << "shard key bounds "
                                       << "[" << bounds[0].Obj() << "," << bounds[1].Obj() << ")"
                                       << " are not valid for shard key pattern "
                                       << cm->getShardKeyPattern().toBSON();
                return false;
            }

            BSONObj minKey = cm->getShardKeyPattern().normalizeShardKey(bounds[0].Obj());
            BSONObj maxKey = cm->getShardKeyPattern().normalizeShardKey(bounds[1].Obj());

            chunk = cm->findIntersectingChunkWithSimpleCollation(minKey);

            if (chunk->getMin().woCompare(minKey) != 0 || chunk->getMax().woCompare(maxKey) != 0) {
                errmsg = str::stream() << "no chunk found with the shard key bounds "
                                       << ChunkRange(minKey, maxKey).toString();
                return false;
            }
        } else {
            // middle
            if (!cm->getShardKeyPattern().isShardKey(middle)) {
                errmsg = str::stream() << "new split key " << middle
                                       << " is not valid for shard key pattern "
                                       << cm->getShardKeyPattern().toBSON();
                return false;
            }

            middle = cm->getShardKeyPattern().normalizeShardKey(middle);

            // Check shard key size when manually provided
            uassertStatusOK(ShardKeyPattern::checkShardKeySize(middle));

            chunk = cm->findIntersectingChunkWithSimpleCollation(middle);

            if (chunk->getMin().woCompare(middle) == 0 || chunk->getMax().woCompare(middle) == 0) {
                errmsg = str::stream() << "new split key " << middle
                                       << " is a boundary key of existing chunk "
                                       << "[" << chunk->getMin() << "," << chunk->getMax() << ")";
                return false;
            }
        }

        // Once the chunk to be split has been determined, if the split point was explicitly
        // specified in the split command through the "middle" parameter, choose "middle" as the
        // splitPoint. Otherwise use the splitVector command with 'force' to ask the shard for the
        // middle of the chunk.
        const BSONObj splitPoint = !middle.isEmpty()
            ? middle
            : selectMedianKey(opCtx,
                              chunk->getShardId(),
                              nss,
                              cm->getShardKeyPattern(),
                              ChunkRange(chunk->getMin(), chunk->getMax()));

        log() << "Splitting chunk "
              << redact(ChunkRange(chunk->getMin(), chunk->getMax()).toString())
              << " in collection " << nss.ns() << " on shard " << chunk->getShardId() << " at key "
              << redact(splitPoint);

        uassertStatusOK(
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  chunk->getShardId(),
                                                  nss,
                                                  cm->getShardKeyPattern(),
                                                  cm->getVersion(),
                                                  ChunkRange(chunk->getMin(), chunk->getMax()),
                                                  {splitPoint}));

        Grid::get(opCtx)->catalogCache()->onStaleConfigError(std::move(routingInfo));

        return true;
    }

} splitChunk;

}  // namespace
}  // namespace mongo
