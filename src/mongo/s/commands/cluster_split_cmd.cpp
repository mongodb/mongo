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
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_raii.h"
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

    virtual Status checkAuthForCommand(Client* client,
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

        auto scopedCM = uassertStatusOK(ScopedChunkManager::refreshAndGet(txn, nss));

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

        auto const cm = scopedCM.cm();

        shared_ptr<Chunk> chunk;

        if (!find.isEmpty()) {
            // find
            BSONObj shardKey =
                uassertStatusOK(cm->getShardKeyPattern().extractShardKeyFromQuery(txn, find));
            if (shardKey.isEmpty()) {
                errmsg = stream() << "no shard key found in chunk query " << find;
                return false;
            }

            chunk = cm->findIntersectingChunkWithSimpleCollation(txn, shardKey);
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

            chunk = cm->findIntersectingChunkWithSimpleCollation(txn, minKey);

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

            chunk = cm->findIntersectingChunkWithSimpleCollation(txn, middle);

            if (chunk->getMin().woCompare(middle) == 0 || chunk->getMax().woCompare(middle) == 0) {
                errmsg = str::stream() << "new split key " << middle
                                       << " is a boundary key of existing chunk "
                                       << "[" << chunk->getMin() << "," << chunk->getMax() << ")";
                return false;
            }
        }

        log() << "Splitting chunk "
              << redact(ChunkRange(chunk->getMin(), chunk->getMax()).toString())
              << " in collection " << nss.ns() << " on shard " << chunk->getShardId();

        BSONObj res;
        if (middle.isEmpty()) {
            uassertStatusOK(chunk->split(txn, Chunk::atMedian, nullptr));
        } else {
            uassertStatusOK(shardutil::splitChunkAtMultiplePoints(txn,
                                                                  chunk->getShardId(),
                                                                  nss,
                                                                  cm->getShardKeyPattern(),
                                                                  cm->getVersion(),
                                                                  chunk->getMin(),
                                                                  chunk->getMax(),
                                                                  {middle}));
        }

        // Proactively refresh the chunk manager. Not strictly necessary, but this way it's
        // immediately up-to-date the next time it's used.
        scopedCM.db()->getChunkManagerIfExists(txn, nss.ns(), true);

        return true;
    }

} splitChunk;

}  // namespace
}  // namespace mongo
