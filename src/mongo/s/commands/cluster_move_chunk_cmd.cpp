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
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::shared_ptr;
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

    virtual Status checkAuthForCommand(Client* client,
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

        auto scopedCM = uassertStatusOK(ScopedChunkManager::refreshAndGet(txn, nss));

        const string toString = cmdObj["to"].valuestrsafe();
        if (!toString.size()) {
            errmsg = "you have to specify where you want to move the chunk";
            return false;
        }

        const auto toStatus = Grid::get(txn)->shardRegistry()->getShard(txn, toString);
        if (!toStatus.isOK()) {
            string msg(str::stream() << "Could not move chunk in '" << nss.ns() << "' to shard '"
                                     << toString
                                     << "' because that shard does not exist");
            log() << msg;
            return appendCommandStatus(result, Status(ErrorCodes::ShardNotFound, msg));
        }

        const auto to = toStatus.getValue();

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

        auto const cm = scopedCM.cm();

        shared_ptr<Chunk> chunk;

        if (!find.isEmpty()) {
            // find
            BSONObj shardKey =
                uassertStatusOK(cm->getShardKeyPattern().extractShardKeyFromQuery(txn, find));
            if (shardKey.isEmpty()) {
                errmsg = str::stream() << "no shard key found in chunk query " << find;
                return false;
            }

            chunk = cm->findIntersectingChunkWithSimpleCollation(shardKey);
        } else {
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
        }

        const auto secondaryThrottle =
            uassertStatusOK(MigrationSecondaryThrottleOptions::createFromCommand(cmdObj));

        ChunkType chunkType;
        chunkType.setNS(nss.ns());
        chunkType.setMin(chunk->getMin());
        chunkType.setMax(chunk->getMax());
        chunkType.setShard(chunk->getShardId());
        chunkType.setVersion(cm->getVersion());

        uassertStatusOK(configsvr_client::moveChunk(txn,
                                                    chunkType,
                                                    to->getId(),
                                                    maxChunkSizeBytes,
                                                    secondaryThrottle,
                                                    cmdObj["_waitForDelete"].trueValue()));

        // Proactively refresh the chunk manager. Not strictly necessary, but this way it's
        // immediately up-to-date the next time it's used.
        scopedCM.db()->getChunkManagerIfExists(txn, nss.ns(), true);

        result.append("millis", t.millis());
        return true;
    }

} moveChunk;

}  // namespace
}  // namespace mongo
