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

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::string;

namespace {

class MoveChunkCmd : public Command {
public:
    MoveChunkCmd() : Command("moveChunk", false, "movechunk") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << "Example: move chunk that contains the doc {num : 7} to shard001\n"
             << "  { movechunk : 'test.foo' , find : { num : 7 } , to : 'shard0001' }\n"
             << "Example: move chunk with lower bound 0 and upper bound 10 to shard001\n"
             << "  { movechunk : 'test.foo' , bounds : [ { num : 0 } , { num : 10 } ] "
             << " , to : 'shard001' }\n";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::moveChunk)) {
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
        Timer t;

        const NamespaceString nss(parseNs(dbname, cmdObj));

        std::shared_ptr<DBConfig> config;

        {
            auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
            if (!status.isOK()) {
                return appendCommandStatus(result, status.getStatus());
            }

            config = status.getValue();
        }

        if (!config->isSharded(nss.ns())) {
            config->reload(txn);

            if (!config->isSharded(nss.ns())) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::NamespaceNotSharded,
                                                  "ns [" + nss.ns() + " is not sharded."));
            }
        }

        const string toString = cmdObj["to"].valuestrsafe();
        if (!toString.size()) {
            errmsg = "you have to specify where you want to move the chunk";
            return false;
        }

        const auto to = grid.shardRegistry()->getShard(txn, toString);
        if (!to) {
            string msg(str::stream() << "Could not move chunk in '" << nss.ns() << "' to shard '"
                                     << toString
                                     << "' because that shard does not exist");
            log() << msg;
            return appendCommandStatus(result, Status(ErrorCodes::ShardNotFound, msg));
        }

        // so far, chunk size serves test purposes; it may or may not become a supported parameter
        long long maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
        if (maxChunkSizeBytes == 0) {
            maxChunkSizeBytes = Grid::get(txn)->getBalancerConfiguration()->getMaxChunkSizeBytes();
        }

        BSONObj find = cmdObj.getObjectField("find");
        BSONObj bounds = cmdObj.getObjectField("bounds");

        // check that only one of the two chunk specification methods is used
        if (find.isEmpty() == bounds.isEmpty()) {
            errmsg = "need to specify either a find query, or both lower and upper bounds.";
            return false;
        }

        // This refreshes the chunk metadata if stale.
        shared_ptr<ChunkManager> info = config->getChunkManager(txn, nss.ns(), true);
        shared_ptr<Chunk> chunk;

        if (!find.isEmpty()) {
            StatusWith<BSONObj> status =
                info->getShardKeyPattern().extractShardKeyFromQuery(txn, find);

            // Bad query
            if (!status.isOK())
                return appendCommandStatus(result, status.getStatus());

            BSONObj shardKey = status.getValue();

            if (shardKey.isEmpty()) {
                errmsg = str::stream() << "no shard key found in chunk query " << find;
                return false;
            }

            chunk = info->findIntersectingChunk(txn, shardKey);
        } else {
            // Bounds
            if (!info->getShardKeyPattern().isShardKey(bounds[0].Obj()) ||
                !info->getShardKeyPattern().isShardKey(bounds[1].Obj())) {
                errmsg = str::stream() << "shard key bounds "
                                       << "[" << bounds[0].Obj() << "," << bounds[1].Obj() << ")"
                                       << " are not valid for shard key pattern "
                                       << info->getShardKeyPattern().toBSON();
                return false;
            }

            BSONObj minKey = info->getShardKeyPattern().normalizeShardKey(bounds[0].Obj());
            BSONObj maxKey = info->getShardKeyPattern().normalizeShardKey(bounds[1].Obj());

            chunk = info->findIntersectingChunk(txn, minKey);

            if (chunk->getMin().woCompare(minKey) != 0 || chunk->getMax().woCompare(maxKey) != 0) {
                errmsg = str::stream() << "no chunk found with the shard key bounds "
                                       << ChunkRange(minKey, maxKey).toString();
                return false;
            }
        }

        const auto from = grid.shardRegistry()->getShard(txn, chunk->getShardId());
        if (from->getId() != to->getId()) {
            const auto secondaryThrottle =
                uassertStatusOK(MigrationSecondaryThrottleOptions::createFromCommand(cmdObj));

            ChunkType chunkType;
            chunkType.setNS(nss.ns());
            chunkType.setMin(chunk->getMin());
            chunkType.setMax(chunk->getMax());
            chunkType.setShard(chunk->getShardId());
            chunkType.setVersion(info->getVersion());

            uassertStatusOK(
                Balancer::get(txn)->moveSingleChunk(txn,
                                                    chunkType,
                                                    to->getId(),
                                                    maxChunkSizeBytes,
                                                    secondaryThrottle,
                                                    cmdObj["_waitForDelete"].trueValue()));
        }

        result.append("millis", t.millis());
        return true;
    }

} moveChunk;

}  // namespace
}  // namespace mongo
