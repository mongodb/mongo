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

#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    using boost::shared_ptr;
    using boost::scoped_ptr;
    using std::string;

namespace {

    class MoveChunkCmd : public Command {
    public:
        MoveChunkCmd() : Command("moveChunk", false, "movechunk") { }

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const {
            return false;
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

            if (!client->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                                                        ResourcePattern::forExactNamespace(
                                                            NamespaceString(parseNs(dbname,
                                                                                    cmdObj))),
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
                         BSONObjBuilder& result,
                         bool fromRepl) {

            ShardConnection::sync();

            Timer t;

            const NamespaceString nss(parseNs(dbname, cmdObj));

            boost::shared_ptr<DBConfig> config;

            {
                if (nss.size() == 0) {
                    return appendCommandStatus(result, Status(ErrorCodes::InvalidNamespace,
                                                              "no namespace specified"));
                }

                auto status = grid.catalogCache()->getDatabase(nss.db().toString());
                if (!status.isOK()) {
                    return appendCommandStatus(result, status.getStatus());
                }

                config = status.getValue();
            }

            if (!config->isSharded(nss.ns())) {
                config->reload();

                if (!config->isSharded(nss.ns())) {
                    return appendCommandStatus(result,
                                               Status(ErrorCodes::NamespaceNotSharded,
                                                      "ns [" + nss.ns() + " is not sharded."));
                }
            }

            string toString = cmdObj["to"].valuestrsafe();
            if (!toString.size()) {
                errmsg = "you have to specify where you want to move the chunk";
                return false;
            }

            Shard to = Shard::findIfExists(toString);
            if (!to.ok()) {
                string msg(str::stream() <<
                           "Could not move chunk in '" << nss.ns() <<
                           "' to shard '" << toString <<
                           "' because that shard does not exist");
                log() << msg;
                return appendCommandStatus(result,
                                           Status(ErrorCodes::ShardNotFound, msg));
            }

            // so far, chunk size serves test purposes; it may or may not become a supported parameter
            long long maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
            if (maxChunkSizeBytes == 0) {
                maxChunkSizeBytes = Chunk::MaxChunkSize;
            }

            BSONObj find = cmdObj.getObjectField("find");
            BSONObj bounds = cmdObj.getObjectField("bounds");

            // check that only one of the two chunk specification methods is used
            if (find.isEmpty() == bounds.isEmpty()) {
                errmsg = "need to specify either a find query, or both lower and upper bounds.";
                return false;
            }

            // This refreshes the chunk metadata if stale.
            ChunkManagerPtr info = config->getChunkManager(nss.ns(), true);
            ChunkPtr chunk;

            if (!find.isEmpty()) {

                StatusWith<BSONObj> status =
                    info->getShardKeyPattern().extractShardKeyFromQuery(find);

                // Bad query
                if (!status.isOK())
                    return appendCommandStatus(result, status.getStatus());

                BSONObj shardKey = status.getValue();

                if (shardKey.isEmpty()) {
                    errmsg = str::stream() << "no shard key found in chunk query " << find;
                    return false;
                }

                chunk = info->findIntersectingChunk(shardKey);
                verify(chunk.get());
            }
            else {

                // Bounds
                if (!info->getShardKeyPattern().isShardKey(bounds[0].Obj())
                        || !info->getShardKeyPattern().isShardKey(bounds[1].Obj())) {
                    errmsg = str::stream() << "shard key bounds " << "[" << bounds[0].Obj() << ","
                                           << bounds[1].Obj() << ")"
                                           << " are not valid for shard key pattern "
                                           << info->getShardKeyPattern().toBSON();
                    return false;
                }

                BSONObj minKey = info->getShardKeyPattern().normalizeShardKey(bounds[0].Obj());
                BSONObj maxKey = info->getShardKeyPattern().normalizeShardKey(bounds[1].Obj());

                chunk = info->findIntersectingChunk(minKey);
                verify(chunk.get());

                if (chunk->getMin().woCompare(minKey) != 0
                        || chunk->getMax().woCompare(maxKey) != 0) {

                    errmsg = str::stream() << "no chunk found with the shard key bounds " << "["
                                           << minKey << "," << maxKey << ")";
                    return false;
                }
            }

            const Shard& from = chunk->getShard();

            if (from == to) {
                errmsg = "that chunk is already on that shard";
                return false;
            }

            LOG(0) << "CMD: movechunk: " << cmdObj;

            StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMSCommand(cmdObj);

            if (!maxTimeMS.isOK()) {
                errmsg = maxTimeMS.getStatus().reason();
                return false;
            }

            scoped_ptr<WriteConcernOptions> writeConcern(new WriteConcernOptions());

            Status status = writeConcern->parseSecondaryThrottle(cmdObj, NULL);
            if (!status.isOK()){
                if (status.code() != ErrorCodes::WriteConcernNotDefined) {
                    errmsg = status.toString();
                    return false;
                }

                // Let the shard decide what write concern to use.
                writeConcern.reset();
            }

            BSONObj res;
            if (!chunk->moveAndCommit(to,
                                      maxChunkSizeBytes,
                                      writeConcern.get(),
                                      cmdObj["_waitForDelete"].trueValue(),
                                      maxTimeMS.getValue(),
                                      res)) {

                errmsg = "move failed";
                result.append("cause", res);

                if (!res["code"].eoo()) {
                    result.append(res["code"]);
                }

                return false;
            }

            result.append("millis", t.millis());

            return true;
        }

    } moveChunk;

} // namespace
} // namespace mongo
