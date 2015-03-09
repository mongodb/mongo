/**
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client_info.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/config.h"
#include "mongo/s/dbclient_multi_command.h"
#include "mongo/s/dbclient_shard_resolver.h"
#include "mongo/s/distlock.h"
#include "mongo/s/grid.h"
#include "mongo/s/strategy.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_shard.h"
#include "mongo/s/write_ops/batch_downconvert.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/print.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

    using boost::scoped_ptr;
    using boost::shared_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::list;
    using std::map;
    using std::set;
    using std::string;
    using std::stringstream;
    using std::vector;

    namespace dbgrid_cmds {

        class GridAdminCmd : public Command {
        public:
            GridAdminCmd( const char * n ) : Command( n , false, tolowerString(n).c_str() ) {

            }

            virtual bool slaveOk() const {
                return true;
            }

            virtual bool adminOnly() const {
                return true;
            }

            virtual bool isWriteCommandForConfigServer() const {
                return false;
            }
        };

        // ------------ server level commands -------------

        class ListShardsCmd : public GridAdminCmd {
        public:
            ListShardsCmd() : GridAdminCmd("listShards") { }
            virtual void help( stringstream& help ) const {
                help << "list all shards of the system";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::listShards);
                out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
            }
            bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( ShardType::ConfigNS , BSONObj() );
                while ( cursor->more() ) {
                    BSONObj o = cursor->next();
                    all.push_back( o );
                }

                result.append("shards" , all );
                conn.done();

                return true;
            }
        } listShardsCmd;

        /* a shard is a single mongod server or a replica pair.  add it (them) to the cluster as a storage partition. */
        class AddShard : public GridAdminCmd {
        public:
            AddShard() : GridAdminCmd("addShard") { }
            virtual void help( stringstream& help ) const {
                help << "add a new shard to the system";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::addShard);
                out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
            }
            bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg.clear();

                // get replica set component hosts
                ConnectionString servers = ConnectionString::parse( cmdObj.firstElement().valuestrsafe() , errmsg );
                if ( ! errmsg.empty() ) {
                    log() << "addshard request " << cmdObj << " failed:" << errmsg << endl;
                    return false;
                }

                // using localhost in server names implies every other process must use localhost addresses too
                vector<HostAndPort> serverAddrs = servers.getServers();
                for ( size_t i = 0 ; i < serverAddrs.size() ; i++ ) {
                    if ( serverAddrs[i].isLocalHost() != grid.allowLocalHost() ) {
                        errmsg = str::stream() << 
                            "can't use localhost as a shard since all shards need to communicate. " <<
                            "either use all shards and configdbs in localhost or all in actual IPs " << 
                            " host: " << serverAddrs[i].toString() << " isLocalHost:" << serverAddrs[i].isLocalHost();
                        
                        log() << "addshard request " << cmdObj << " failed: attempt to mix localhosts and IPs" << endl;
                        return false;
                    }

                    // it's fine if mongods of a set all use default port
                    if ( ! serverAddrs[i].hasPort() ) {
                        serverAddrs[i] = HostAndPort(serverAddrs[i].host(),
                                                     ServerGlobalParams::ShardServerPort);
                    }
                }

                // name is optional; addShard will provide one if needed
                string name = "";
                if ( cmdObj["name"].type() == String ) {
                    name = cmdObj["name"].valuestrsafe();
                }

                // maxSize is the space usage cap in a shard in MBs
                long long maxSize = 0;
                if ( cmdObj[ ShardType::maxSize() ].isNumber() ) {
                    maxSize = cmdObj[ ShardType::maxSize() ].numberLong();
                }

                audit::logAddShard(ClientBasic::getCurrent(), name, servers.toString(), maxSize);
                if ( ! grid.addShard( &name , servers , maxSize , errmsg ) ) {
                    log() << "addshard request " << cmdObj << " failed: " << errmsg << endl;
                    return false;
                }

                result << "shardAdded" << name;
                return true;
            }

        } addServer;

        /* See usage docs at:
         * http://dochub.mongodb.org/core/configuringsharding#ConfiguringSharding-Removingashard
         */
        class RemoveShardCmd : public GridAdminCmd {
        public:
            RemoveShardCmd() : GridAdminCmd("removeShard") { }
            virtual void help( stringstream& help ) const {
                help << "remove a shard to the system.";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::removeShard);
                out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
            }
            bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string target = cmdObj.firstElement().valuestrsafe();
                Shard s = Shard::make( target );
                if ( ! grid.knowAboutShard( s.getConnString() ) ) {
                    errmsg = "unknown shard";
                    return false;
                }

                ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

                if (conn->count(ShardType::ConfigNS,
                                BSON(ShardType::name() << NE << s.getName() <<
                                     ShardType::draining(true)))){
                    conn.done();
                    errmsg = "Can't have more than one draining shard at a time";
                    return false;
                }

                if (conn->count(ShardType::ConfigNS, BSON(ShardType::name() << NE << s.getName())) == 0){
                    conn.done();
                    errmsg = "Can't remove last shard";
                    return false;
                }

                BSONObj primaryDoc = BSON(DatabaseType::name.ne("local") <<
                                          DatabaseType::primary(s.getName()));
                BSONObj dbInfo; // appended at end of result on success
                {
                    boost::scoped_ptr<DBClientCursor> cursor (conn->query(DatabaseType::ConfigNS, primaryDoc));
                    if (cursor->more()) { // skip block and allocations if empty
                        BSONObjBuilder dbInfoBuilder;
                        dbInfoBuilder.append("note", "you need to drop or movePrimary these databases");
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
                BSONObj drainingDoc = BSON(ShardType::name() << s.getName() << ShardType::draining(true));
                BSONObj shardDoc = conn->findOne(ShardType::ConfigNS, drainingDoc);
                if ( shardDoc.isEmpty() ) {

                    // TODO prevent move chunks to this shard.

                    log() << "going to start draining shard: " << s.getName() << endl;
                    BSONObj newStatus = BSON( "$set" << BSON( ShardType::draining(true) ) );

                    Status status = clusterUpdate( ShardType::ConfigNS,
                                                   searchDoc,
                                                   newStatus,
                                                   false /* do no upsert */,
                                                   false /* multi */,
                                                   NULL );

                    if ( !status.isOK() ) {
                        errmsg = status.reason();
                        log() << "error starting remove shard: " << s.getName()
                              << " err: " << errmsg << endl;
                        return false;
                    }

                    BSONObj primaryLocalDoc = BSON(DatabaseType::name("local") <<
                                                   DatabaseType::primary(s.getName()));
                    PRINT(primaryLocalDoc);
                    if (conn->count(DatabaseType::ConfigNS, primaryLocalDoc)) {
                        log() << "This shard is listed as primary of local db. Removing entry." << endl;
                        Status status = clusterDelete( DatabaseType::ConfigNS,
                                                       BSON(DatabaseType::name("local")),
                                                       0 /* limit */,
                                                       NULL );

                        if ( !status.isOK() ) {
                            log() << "error removing local db: "
                                  << status.reason() << endl;
                            return false;
                        }
                    }

                    Shard::reloadShardInfo();

                    result.append( "msg"   , "draining started successfully" );
                    result.append( "state" , "started" );
                    result.append( "shard" , s.getName() );
                    result.appendElements(dbInfo);
                    conn.done();

                    // Record start in changelog
                    configServer.logChange( "removeShard.start",
                                            "",
                                            buildRemoveLogEntry( s, true ) );

                    return true;
                }

                // If the server has been completely drained, remove it from the ConfigDB.
                // Check not only for chunks but also databases.
                BSONObj shardIDDoc = BSON(ChunkType::shard(shardDoc[ShardType::name()].str()));
                long long chunkCount = conn->count(ChunkType::ConfigNS, shardIDDoc);
                long long dbCount = conn->count( DatabaseType::ConfigNS , primaryDoc );
                if ( ( chunkCount == 0 ) && ( dbCount == 0 ) ) {
                    log() << "going to remove shard: " << s.getName() << endl;
                    audit::logRemoveShard(ClientBasic::getCurrent(), s.getName());
                    Status status = clusterDelete( ShardType::ConfigNS,
                                                   searchDoc,
                                                   0, // limit
                                                   NULL );

                    if ( !status.isOK() ) {
                        errmsg = status.reason();
                        log() << "error concluding remove shard: " << s.getName()
                              << " err: " << errmsg << endl;
                        return false;
                    }

                    string shardName = shardDoc[ ShardType::name() ].str();
                    Shard::removeShard( shardName );
                    shardConnectionPool.removeHost( shardName );
                    ReplicaSetMonitor::remove( shardName, true );
                    Shard::reloadShardInfo();

                    result.append( "msg"   , "removeshard completed successfully" );
                    result.append( "state" , "completed" );
                    result.append( "shard" , s.getName() );
                    conn.done();

                    // Record finish in changelog
                    configServer.logChange( "removeShard", "", buildRemoveLogEntry( s, false ) );

                    return true;
                }

                // If the server is already in draining mode, just report on its progress.
                // Report on databases (not just chunks) that are left too.
                result.append( "msg"  , "draining ongoing" );
                result.append( "state" , "ongoing" );
                BSONObjBuilder inner;
                inner.append( "chunks" , chunkCount );
                inner.append( "dbs" , dbCount );
                result.append( "remaining" , inner.obj() );
                result.appendElements(dbInfo);

                conn.done();
                return true;
            }
        private:
            BSONObj buildRemoveLogEntry( Shard s, const bool isDraining ) {
                BSONObjBuilder details;
                details.append("shard", s.getName());
                details.append("isDraining", isDraining);

                return details.obj();
            }
        } removeShardCmd;


        // --------------- public commands ----------------

        class IsDbGridCmd : public Command {
        public:
            virtual bool isWriteCommandForConfigServer() const { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            IsDbGridCmd() : Command("isdbgrid") { }
            bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("isdbgrid", 1);
                result.append("hostname", getHostNameCached());
                return true;
            }
        } isdbgrid;

        class CmdIsMaster : public Command {
        public:
            virtual bool isWriteCommandForConfigServer() const { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "test if this is master half of a replica pair";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            CmdIsMaster() : Command("isMaster" , false , "ismaster") { }
            virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.appendBool("ismaster", true );
                result.append("msg", "isdbgrid");
                result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
                result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
                result.appendNumber("maxWriteBatchSize",
                                    BatchedCommandRequest::kMaxWriteBatchSize);
                result.appendDate("localTime", jsTime());

                // Mongos tries to keep exactly the same version range of the server it is
                // compiled for.
                result.append("maxWireVersion", maxWireVersion);
                result.append("minWireVersion", minWireVersion);

                return true;
            }
        } ismaster;

        class CmdWhatsMyUri : public Command {
        public:
            CmdWhatsMyUri() : Command("whatsmyuri") { }
            virtual bool slaveOk() const {
                return true;
            }
            virtual bool isWriteCommandForConfigServer() const { return false; }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            virtual void help( stringstream &help ) const {
                help << "{whatsmyuri:1}";
            }
            virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result << "you" << ClientInfo::get()->getRemote().toString();
                return true;
            }
        } cmdWhatsMyUri;


        class CmdShardingGetPrevError : public Command {
        public:
            virtual bool isWriteCommandForConfigServer() const { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "get previous error (since last reseterror command)";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            CmdShardingGetPrevError() : Command( "getPrevError" , false , "getpreverror") { }
            virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg += "getpreverror not supported for sharded environments";
                return false;
            }
        } cmdGetPrevError;

    }

} // namespace mongo
