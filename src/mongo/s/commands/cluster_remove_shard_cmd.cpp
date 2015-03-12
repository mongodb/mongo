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

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_shard.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;

namespace {

    BSONObj buildRemoveLogEntry(Shard s, bool isDraining) {
        BSONObjBuilder details;
        details.append("shard", s.getName());
        details.append("isDraining", isDraining);

        return details.obj();
    }


    class RemoveShardCmd : public Command {
    public:
        RemoveShardCmd() : Command("removeShard", false, "removeshard") { }

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
            help << "remove a shard from the system.";
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {

            ActionSet actions;
            actions.addAction(ActionType::removeShard);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        virtual bool run(OperationContext* txn,
                         const std::string& dbname,
                         BSONObj& cmdObj,
                         int options,
                         std::string& errmsg,
                         BSONObjBuilder& result,
                         bool fromRepl) {

            const string target = cmdObj.firstElement().valuestrsafe();

            Shard s = Shard::findIfExists(target);
            if (s == Shard::EMPTY) {
                string msg(str::stream() <<
                           "Could not drop shard '" << target <<
                           "' because it does not exist");
                log() << msg;
                return appendCommandStatus(result,
                                           Status(ErrorCodes::ShardNotFound, msg));
            }

            ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

            if (conn->count(ShardType::ConfigNS,
                            BSON(ShardType::name() << NE << s.getName()
                                                   << ShardType::draining(true)))) {
                conn.done();
                errmsg = "Can't have more than one draining shard at a time";
                return false;
            }

            if (conn->count(ShardType::ConfigNS,
                            BSON(ShardType::name() << NE << s.getName())) == 0) {
                conn.done();
                errmsg = "Can't remove last shard";
                return false;
            }

            BSONObj primaryDoc =
                        BSON(DatabaseType::name.ne("local") << DatabaseType::primary(s.getName()));

            BSONObj dbInfo; // appended at end of result on success
            {
                boost::scoped_ptr<DBClientCursor> cursor(conn->query(DatabaseType::ConfigNS,
                                                                     primaryDoc));
                if (cursor->more()) {
                    // Skip block and allocations if empty
                    BSONObjBuilder dbInfoBuilder;
                    dbInfoBuilder.append("note",
                                         "you need to drop or movePrimary these databases");

                    BSONArrayBuilder dbs(dbInfoBuilder.subarrayStart("dbsToMove"));
                    while (cursor->more()){
                        BSONObj db = cursor->nextSafe();
                        dbs.append(db[DatabaseType::name()]);
                    }
                    dbs.doneFast();

                    dbInfo = dbInfoBuilder.obj();
                }
            }

            // If the server is not yet draining chunks, put it in draining mode.
            BSONObj searchDoc = BSON(ShardType::name() << s.getName());
            BSONObj drainingDoc =
                        BSON(ShardType::name() << s.getName() << ShardType::draining(true));

            BSONObj shardDoc = conn->findOne(ShardType::ConfigNS, drainingDoc);
            if (shardDoc.isEmpty()) {
                log() << "going to start draining shard: " << s.getName();
                BSONObj newStatus = BSON("$set" << BSON(ShardType::draining(true)));

                Status status = clusterUpdate(ShardType::ConfigNS,
                                              searchDoc,
                                              newStatus,
                                              false,
                                              false,
                                              NULL);
                if (!status.isOK()) {
                    errmsg = status.reason();
                    log() << "error starting remove shard: " << s.getName()
                          << " err: " << errmsg;
                    return false;
                }

                BSONObj primaryLocalDoc = BSON(DatabaseType::name("local") <<
                                               DatabaseType::primary(s.getName()));
                PRINT(primaryLocalDoc);

                if (conn->count(DatabaseType::ConfigNS, primaryLocalDoc)) {
                    log() << "This shard is listed as primary of local db. Removing entry.";

                    Status status = clusterDelete(DatabaseType::ConfigNS,
                                                  BSON(DatabaseType::name("local")),
                                                  0,
                                                  NULL);
                    if (!status.isOK()) {
                        log() << "error removing local db: "
                              << status.reason();
                        return false;
                    }
                }

                Shard::reloadShardInfo();

                result.append("msg", "draining started successfully");
                result.append("state", "started");
                result.append("shard", s.getName());
                result.appendElements(dbInfo);

                conn.done();

                // Record start in changelog
                configServer.logChange("removeShard.start",
                                       "",
                                       buildRemoveLogEntry(s, true));

                return true;
            }

            // If the server has been completely drained, remove it from the ConfigDB. Check not
            // only for chunks but also databases.
            BSONObj shardIDDoc = BSON(ChunkType::shard(shardDoc[ShardType::name()].str()));
            long long chunkCount = conn->count(ChunkType::ConfigNS, shardIDDoc);
            long long dbCount = conn->count(DatabaseType::ConfigNS, primaryDoc);

            if ((chunkCount == 0) && (dbCount == 0)) {
                log() << "going to remove shard: " << s.getName();
                audit::logRemoveShard(ClientBasic::getCurrent(), s.getName());

                Status status = clusterDelete(ShardType::ConfigNS,
                                              searchDoc,
                                              0,
                                              NULL);
                if (!status.isOK()) {
                    errmsg = status.reason();
                    log() << "error concluding remove shard: " << s.getName()
                          << " err: " << errmsg;
                    return false;
                }

                const string shardName = shardDoc[ShardType::name()].str();
                Shard::removeShard(shardName);
                shardConnectionPool.removeHost(shardName);
                ReplicaSetMonitor::remove(shardName, true);

                Shard::reloadShardInfo();

                result.append("msg", "removeshard completed successfully");
                result.append("state", "completed");
                result.append("shard", s.getName());

                conn.done();

                // Record finish in changelog
                configServer.logChange("removeShard", "", buildRemoveLogEntry(s, false));

                return true;
            }

            // If the server is already in draining mode, just report on its progress.
            // Report on databases (not just chunks) that are left too.
            result.append("msg", "draining ongoing");
            result.append("state", "ongoing");
            BSONObjBuilder inner;
            inner.append("chunks", chunkCount);
            inner.append("dbs", dbCount);
            result.append("remaining", inner.obj());
            result.appendElements(dbInfo);

            conn.done();
            return true;
        }

    } removeShardCmd;

} // namespace
} // namespace mongo
