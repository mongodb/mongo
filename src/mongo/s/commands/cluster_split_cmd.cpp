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
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::string;
using std::vector;

namespace {

class SplitCollectionCmd : public Command {
public:
    SplitCollectionCmd() : Command("split", false, "split") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << " example: - split the shard that contains give key\n"
             << "   { split : 'alleyinsider.blog.posts' , find : { ts : 1 } }\n"
             << " example: - split the shard that contains the key with this as the middle\n"
             << "   { split : 'alleyinsider.blog.posts' , middle : { ts : 1 } }\n"
             << " NOTE: this does not move the chunks, it just creates a logical separation.";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << nss.ns() << " is not a valid namespace",
                nss.isValid());

        auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
        if (!status.isOK()) {
            return appendCommandStatus(result, status.getStatus());
        }

        std::shared_ptr<DBConfig> config = status.getValue();
        if (!config->isSharded(nss.ns())) {
            config->reload(txn);

            if (!config->isSharded(nss.ns())) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::NamespaceNotSharded,
                                                  "ns [" + nss.ns() + " is not sharded."));
            }
        }

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

        // This refreshes the chunk metadata if stale.
        shared_ptr<ChunkManager> info = config->getChunkManager(txn, nss.ns(), true);
        shared_ptr<Chunk> chunk;

        if (!find.isEmpty()) {
            StatusWith<BSONObj> status =
                info->getShardKeyPattern().extractShardKeyFromQuery(txn, find);

            // Bad query
            if (!status.isOK()) {
                return appendCommandStatus(result, status.getStatus());
            }

            BSONObj shardKey = status.getValue();
            if (shardKey.isEmpty()) {
                errmsg = stream() << "no shard key found in chunk query " << find;
                return false;
            }

            chunk = info->findIntersectingChunk(txn, shardKey);
        } else if (!bounds.isEmpty()) {
            if (!info->getShardKeyPattern().isShardKey(bounds[0].Obj()) ||
                !info->getShardKeyPattern().isShardKey(bounds[1].Obj())) {
                errmsg = stream() << "shard key bounds "
                                  << "[" << bounds[0].Obj() << "," << bounds[1].Obj() << ")"
                                  << " are not valid for shard key pattern "
                                  << info->getShardKeyPattern().toBSON();
                return false;
            }

            BSONObj minKey = info->getShardKeyPattern().normalizeShardKey(bounds[0].Obj());
            BSONObj maxKey = info->getShardKeyPattern().normalizeShardKey(bounds[1].Obj());

            chunk = info->findIntersectingChunk(txn, minKey);
            invariant(chunk.get());

            if (chunk->getMin().woCompare(minKey) != 0 || chunk->getMax().woCompare(maxKey) != 0) {
                errmsg = stream() << "no chunk found with the shard key bounds "
                                  << "[" << minKey << "," << maxKey << ")";
                return false;
            }
        } else {
            // Middle
            if (!info->getShardKeyPattern().isShardKey(middle)) {
                errmsg = stream() << "new split key " << middle
                                  << " is not valid for shard key pattern "
                                  << info->getShardKeyPattern().toBSON();
                return false;
            }

            middle = info->getShardKeyPattern().normalizeShardKey(middle);

            // Check shard key size when manually provided
            Status status = ShardKeyPattern::checkShardKeySize(middle);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            chunk = info->findIntersectingChunk(txn, middle);
            invariant(chunk.get());

            if (chunk->getMin().woCompare(middle) == 0 || chunk->getMax().woCompare(middle) == 0) {
                errmsg = stream() << "new split key " << middle
                                  << " is a boundary key of existing chunk "
                                  << "[" << chunk->getMin() << "," << chunk->getMax() << ")";
                return false;
            }
        }

        invariant(chunk.get());

        log() << "splitting chunk [" << chunk->getMin() << "," << chunk->getMax() << ")"
              << " in collection " << nss.ns() << " on shard " << chunk->getShardId();

        BSONObj res;
        if (middle.isEmpty()) {
            uassertStatusOK(chunk->split(txn, Chunk::atMedian, nullptr));
        } else {
            uassertStatusOK(shardutil::splitChunkAtMultiplePoints(txn,
                                                                  chunk->getShardId(),
                                                                  nss,
                                                                  info->getShardKeyPattern(),
                                                                  info->getVersion(),
                                                                  chunk->getMin(),
                                                                  chunk->getMax(),
                                                                  {middle}));
        }

        // Proactively refresh the chunk manager. Not really necessary, but this way it's
        // immediately up-to-date the next time it's used.
        info->reload(txn);

        return true;
    }

} splitCollectionCmd;

}  // namespace
}  // namespace mongo
