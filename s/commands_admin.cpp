// s/commands_admin.cpp

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
*/

/* TODO
   _ concurrency control.
   _ limit() works right?
   _ KillCursors

   later
   _ secondary indexes
*/

#include "pch.h"
#include "../util/message.h"
#include "../util/processinfo.h"
#include "../util/stringutils.h"

#include "../client/connpool.h"

#include "../db/dbmessage.h"
#include "../db/commands.h"
#include "../db/stats/counters.h"

#include "config.h"
#include "chunk.h"
#include "grid.h"
#include "strategy.h"
#include "stats.h"

namespace mongo {

    namespace dbgrid_cmds {

        class GridAdminCmd : public Command {
        public:
            GridAdminCmd( const char * n ) : Command( n , false, tolowerString(n).c_str() ){
            }
            virtual bool slaveOk() const {
                return true;
            }
            virtual bool adminOnly() const {
                return true;
            }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; } 
        };

        // --------------- misc commands ----------------------

        class NetStatCmd : public GridAdminCmd {
        public:
            NetStatCmd() : GridAdminCmd("netstat") { }
            virtual void help( stringstream& help ) const {
                help << " shows status/reachability of servers in the cluster";
            }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                result.append("configserver", configServer.getPrimary().getConnString() );
                result.append("isdbgrid", 1);
                return true;
            }
        } netstat;
        
        class ServerStatusCmd : public Command {
        public:
            ServerStatusCmd() : Command( "serverStatus" , true ){
                _started = time(0);
            }
            
            virtual bool slaveOk() const { return true; }
            virtual LockType locktype() const { return NONE; } 
            
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
                result.append("uptime",(double) (time(0)-_started));
                result.appendDate( "localTime" , jsTime() );

                {
                    BSONObjBuilder t( result.subobjStart( "mem" ) );
                    
                    ProcessInfo p;
                    if ( p.supported() ){
                        t.appendNumber( "resident" , p.getResidentSize() );
                        t.appendNumber( "virtual" , p.getVirtualMemorySize() );
                        t.appendBool( "supported" , true );
                    }
                    else {
                        result.append( "note" , "not all mem info support on this platform" );
                        t.appendBool( "supported" , false );
                    }
                    
                    t.done();
                }

                {
                    BSONObjBuilder bb( result.subobjStart( "connections" ) );
                    bb.append( "current" , connTicketHolder.used() );
                    bb.append( "available" , connTicketHolder.available() );
                    bb.done();
                }
                
                {
                    BSONObjBuilder bb( result.subobjStart( "extra_info" ) );
                    bb.append("note", "fields vary by platform");
                    ProcessInfo p;
                    p.getExtraInfo(bb);
                    bb.done();
                }
                
                result.append( "opcounters" , globalOpCounters.getObj() );
                {
                    BSONObjBuilder bb( result.subobjStart( "ops" ) );
                    bb.append( "sharded" , opsSharded.getObj() );
                    bb.append( "notSharded" , opsNonSharded.getObj() );
                    bb.done();
                }

                result.append( "shardCursorType" , shardedCursorTypes.getObj() );
                
                {
                    BSONObjBuilder asserts( result.subobjStart( "asserts" ) );
                    asserts.append( "regular" , assertionCount.regular );
                    asserts.append( "warning" , assertionCount.warning );
                    asserts.append( "msg" , assertionCount.msg );
                    asserts.append( "user" , assertionCount.user );
                    asserts.append( "rollovers" , assertionCount.rollovers );
                    asserts.done();
                }

                return 1;
            }

            time_t _started;
        } cmdServerStatus;

        class FsyncCommand : public GridAdminCmd {
        public:
            FsyncCommand() : GridAdminCmd( "fsync" ){}
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                if ( cmdObj["lock"].trueValue() ){
                    errmsg = "can't do lock through mongos";
                    return false;
                }
                
                BSONObjBuilder sub;

                bool ok = true;
                int numFiles = 0;
                
                vector<Shard> shards;
                Shard::getAllShards( shards );
                for ( vector<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ){
                    Shard s = *i;

                    BSONObj x = s.runCommand( "admin" , "fsync" );
                    sub.append( s.getName() , x );

                    if ( ! x["ok"].trueValue() ){
                        ok = false;
                        errmsg = x["errmsg"].String();
                    }
                    
                    numFiles += x["numFiles"].numberInt();
                }
                
                result.append( "numFiles" , numFiles );
                result.append( "all" , sub.obj() );
                return ok;
            }
        } fsyncCmd;

        // ------------ database level commands -------------

        class MoveDatabasePrimaryCommand : public GridAdminCmd {
        public:
            MoveDatabasePrimaryCommand() : GridAdminCmd("movePrimary") { }
            virtual void help( stringstream& help ) const {
                help << " example: { moveprimary : 'foo' , to : 'localhost:9999' }";
                // TODO: locking?
            }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string dbname = cmdObj.firstElement().valuestrsafe();

                if ( dbname.size() == 0 ){
                    errmsg = "no db";
                    return false;
                }

                if ( dbname == "config" ){
                    errmsg = "can't move config db";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( dbname , false );
                if ( ! config ){
                    errmsg = "can't find db!";
                    return false;
                }

                string to = cmdObj["to"].valuestrsafe();
                if ( ! to.size()  ){
                    errmsg = "you have to specify where you want to move it";
                    return false;
                }
                Shard s = Shard::make( to );

                if ( config->getPrimary() == s.getConnString() ){
                    errmsg = "thats already the primary";
                    return false;
                }

                if ( ! grid.knowAboutShard( s.getConnString() ) ){
                    errmsg = "that server isn't known to me";
                    return false;
                }
                
                log() << "movePrimary: moving " << dbname << " primary from: " << config->getPrimary().toString() 
                      << " to: " << s.toString() << endl;

                // TODO LOCKING: this is not safe with multiple mongos

                ScopedDbConnection toconn( s.getConnString() );

                // TODO ERH - we need a clone command which replays operations from clone start to now
                //            can just use local.oplog.$main
                BSONObj cloneRes;
                bool worked = toconn->runCommand( dbname.c_str() , BSON( "clone" << config->getPrimary().getConnString() ) , cloneRes );
                toconn.done();

                if ( ! worked ){
                    log() << "clone failed" << cloneRes << endl;
                    errmsg = "clone failed";
                    return false;
                }

                ScopedDbConnection fromconn( config->getPrimary() );

                config->setPrimary( s.getConnString() );

                log() << "movePrimary:  dropping " << dbname << " from old" << endl;

                fromconn->dropDatabase( dbname.c_str() );
                fromconn.done();

                result << "primary " << s.toString();

                return true;
            }
        } movePrimary;

        class EnableShardingCmd : public GridAdminCmd {
        public:
            EnableShardingCmd() : GridAdminCmd( "enableSharding" ){}
            virtual void help( stringstream& help ) const {
                help
                    << "Enable sharding for a db. (Use 'shardcollection' command afterwards.)\n"
                    << "  { enablesharding : \"<dbname>\" }\n";
            }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string dbname = cmdObj.firstElement().valuestrsafe();
                if ( dbname.size() == 0 ){
                    errmsg = "no db";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( dbname );
                if ( config->isShardingEnabled() ){
                    errmsg = "already enabled";
                    return false;
                }
                
                log() << "enabling sharding on: " << dbname << endl;

                config->enableSharding();

                return true;
            }
        } enableShardingCmd;

        // ------------ collection level commands -------------

        class ShardCollectionCmd : public GridAdminCmd {
        public:
            ShardCollectionCmd() : GridAdminCmd( "shardCollection" ){}

            virtual void help( stringstream& help ) const {
                help
                    << "Shard a collection.  Requires key.  Optional unique. Sharding must already be enabled for the database.\n"
                    << "  { enablesharding : \"<dbname>\" }\n";
            }

            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isShardingEnabled() ){
                    errmsg = "sharding not enabled for db";
                    return false;
                }

                if ( config->isSharded( ns ) ){
                    errmsg = "already sharded";
                    return false;
                }

                BSONObj key = cmdObj.getObjectField( "key" );
                if ( key.isEmpty() ){
                    errmsg = "no shard key";
                    return false;
                }

                BSONForEach(e, key){
                    if (!e.isNumber() || e.number() != 1.0){
                        errmsg = "shard keys must all be ascending";
                        return false;
                    }
                }

                if ( ns.find( ".system." ) != string::npos ){
                    errmsg = "can't shard system namespaces";
                    return false;
                }

                // Sharding interacts with indexing in at least two ways:
                //
                // 1. A unique index must have the sharding key as its prefix. Otherwise maintainig uniqueness would
                // require coordinated access to all shards. Trying to shard a collection with such an index is not
                // allowed.
                // 
                // 2. Sharding a collection requires an index over the sharding key. That index must be create upfront.
                // The rationale is that sharding a non-empty collection would need to create the index and that could
                // be slow. Requiring the index upfront allows the admin to plan before sharding and perhaps use 
                // background index construction. One exception to the rule: empty collections. It's fairly easy to
                // create the index as part of the sharding process.
                //
                // We enforce both these conditions in what comes next.

                {
                    ShardKeyPattern proposedKey( key );
                    bool hasShardIndex = false;

                    ScopedDbConnection conn( config->getPrimary() );
                    BSONObjBuilder b; 
                    b.append( "ns" , ns ); 

                    auto_ptr<DBClientCursor> cursor = conn->query( config->getName() + ".system.indexes" , b.obj() );
                    while ( cursor->more() ){
                        BSONObj idx = cursor->next();

                        // Is index key over the sharding key? Remember that.
                        if ( key.woCompare( idx["key"].embeddedObjectUserCheck() ) == 0 ){
                            hasShardIndex = true;
                        }

                        // Not a unique index? Move on.
                        if ( idx["unique"].eoo() || ! idx["unique"].Bool() )
                            continue;

                        // Shard key is prefix of unique index? Move on.
                        if ( proposedKey.isPrefixOf( idx["key"].embeddedObjectUserCheck() ) )
                            continue;

                        errmsg = (string)"can't shard collection with unique index on: " + idx.toString();
                        conn.done();
                        return false;
                    }

                    BSONObj res = conn->findOne( config->getName() + ".system.namespaces" , BSON( "name" << ns ) );
                    if ( res["options"].type() == Object && res["options"].embeddedObject()["capped"].trueValue() ){
                        errmsg = "can't shard capped collection";
                        conn.done();
                        return false;
                    }

                    if ( ! hasShardIndex && ( conn->count( ns ) != 0 ) ){
                        errmsg = "please create an index over the sharding key before sharding.";
                        return false;
                    }
                
                    conn.done();
                }

                tlog() << "CMD: shardcollection: " << cmdObj << endl;

                config->shardCollection( ns , key , cmdObj["unique"].trueValue() );

                result << "collectionsharded" << ns;
                return true;
            }
        } shardCollectionCmd;

        class GetShardVersion : public GridAdminCmd {
        public:
            GetShardVersion() : GridAdminCmd( "getShardVersion" ){}
            virtual void help( stringstream& help ) const {
                help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
            }
            
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "need to speciy fully namespace";
                    return false;
                }
                
                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ){
                    errmsg = "ns not sharded.";
                    return false;
                }
                
                ChunkManagerPtr cm = config->getChunkManager( ns );
                if ( ! cm ){
                    errmsg = "no chunk manager?";
                    return false;
                }
                cm->_printChunks();
                result.appendTimestamp( "version" , cm->getVersion().toLong() );

                return 1;
            }
        } getShardVersionCmd;

        class SplitCollectionHelper : public GridAdminCmd {
        public:
            SplitCollectionHelper( const char * name ) : GridAdminCmd( name ) , _name( name ){}
            virtual void help( stringstream& help ) const {
                help
                    << " example: { split : 'alleyinsider.blog.posts' , find : { ts : 1 } } - split the shard that contains give key \n"
                    << " example: { split : 'alleyinsider.blog.posts' , middle : { ts : 1 } } - split the shard that contains the key with this as the middle \n"
                    << " NOTE: this does not move move the chunks, it merely creates a logical seperation \n"
                    ;
            }

            virtual bool _split( BSONObjBuilder& result , string&errmsg , const string& ns , ChunkManagerPtr manager , ChunkPtr old , BSONObj middle ) = 0;

            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ShardConnection::sync();

                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ){
                    errmsg = "ns not sharded.  have to shard before can split";
                    return false;
                }

                BSONObj find = cmdObj.getObjectField( "find" );
                if ( find.isEmpty() ){
                    find = cmdObj.getObjectField( "middle" );

                    if ( find.isEmpty() ){
                        errmsg = "need to specify find or middle";
                        return false;
                    }
                }
                
                ChunkManagerPtr info = config->getChunkManager( ns );
                ChunkPtr old = info->findChunk( find );

                return _split( result , errmsg , ns , info , old , cmdObj.getObjectField( "middle" ) );
            }

        protected:
            string _name;
        };

        class SplitValueCommand : public SplitCollectionHelper {
        public:
            SplitValueCommand() : SplitCollectionHelper( "splitValue" ){}
            virtual bool _split( BSONObjBuilder& result , string& errmsg , const string& ns , ChunkManagerPtr manager , ChunkPtr old , BSONObj middle ){

                result << "shardinfo" << old->toString();

                result.appendBool( "auto" , middle.isEmpty() );

                if ( middle.isEmpty() )
                    middle = old->pickSplitPoint();

                result.append( "middle" , middle );

                return true;
            }

        } splitValueCmd;


        class SplitCollection : public SplitCollectionHelper {
        public:
            SplitCollection() : SplitCollectionHelper( "split" ){}
            virtual bool _split( BSONObjBuilder& result , string& errmsg , const string& ns , ChunkManagerPtr manager , ChunkPtr old , BSONObj middle ){
                assert( old.get() );
                log() << "splitting: " << ns << "  shard: " << old << endl;

                if ( middle.isEmpty() )
                    old->split();
                else {
                    vector<BSONObj> splitPoints;
                    splitPoints.push_back( middle );
                    old->multiSplit( splitPoints );
                }

                return true;
            }


        } splitCollectionCmd;

        class MoveChunkCmd : public GridAdminCmd {
        public:
            MoveChunkCmd() : GridAdminCmd( "moveChunk" ){}
            virtual void help( stringstream& help ) const {
                help << "{ movechunk : 'test.foo' , find : { num : 1 } , to : 'localhost:30001' }";
            }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ShardConnection::sync();

                Timer t;
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ){
                    errmsg = "ns not sharded.  have to shard before can move a chunk";
                    return false;
                }

                BSONObj find = cmdObj.getObjectField( "find" );
                if ( find.isEmpty() ){
                    errmsg = "need to specify find.  see help";
                    return false;
                }

                string toString = cmdObj["to"].valuestrsafe();
                if ( ! toString.size()  ){
                    errmsg = "you have to specify where you want to move the chunk";
                    return false;
                }
                
                Shard to = Shard::make( toString );

                tlog() << "CMD: movechunk: " << cmdObj << endl;

                ChunkManagerPtr info = config->getChunkManager( ns );
                ChunkPtr c = info->findChunk( find );
                const Shard& from = c->getShard();

                if ( from == to ){
                    errmsg = "that chunk is already on that shard";
                    return false;
                }
                
                if ( ! c->moveAndCommit( to , errmsg ) )
                    return false;

                result.append( "millis" , t.millis() );
                return true;
            }
        } moveChunkCmd;

        // ------------ server level commands -------------

        class ListShardsCmd : public GridAdminCmd {
        public:
            ListShardsCmd() : GridAdminCmd("listShards") { }
            virtual void help( stringstream& help ) const {
                help << "list all shards of the system";
            }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( "config.shards" , BSONObj() );
                while ( cursor->more() ){
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
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                errmsg.clear();

                // get replica set component hosts
                ConnectionString servers = ConnectionString::parse( cmdObj.firstElement().valuestrsafe() , errmsg );
                if ( ! errmsg.empty() ){
                    log() << "addshard request " << cmdObj << " failed:" << errmsg << endl;
                    return false;
                }

                // using localhost in server names implies every other process must use locahost addresses too
                vector<HostAndPort> serverAddrs = servers.getServers();
                for ( size_t i = 0 ; i < serverAddrs.size() ; i++ ){ 
                    if ( serverAddrs[i].isLocalHost() != grid.allowLocalHost() ){
                        errmsg = "can't use localhost as a shard since all shards need to communicate. "
                                 "either use all shards and configdbs in localhost or all in actual IPs " ;
                        log() << "addshard request " << cmdObj << " failed: attempt to mix localhosts and IPs" << endl;
                        return false;
                    }

                    // it's fine if mongods of a set all use default port
                    if ( ! serverAddrs[i].hasPort() ){
                        serverAddrs[i].setPort( CmdLine::ShardServerPort );
                    }
                }

                // name is optional; addShard will provide one if needed
                string name = "";
                if ( cmdObj["name"].type() == String ) {
                    name = cmdObj["name"].valuestrsafe();
                } 

                // maxSize is the space usage cap in a shard in MBs
                long long maxSize = 0;
                if ( cmdObj[ ShardFields::maxSize.name() ].isNumber() ){
                    maxSize = cmdObj[ ShardFields::maxSize.name() ].numberLong();
                }
                
                if ( ! grid.addShard( &name , servers , maxSize , errmsg ) ){
                    log() << "addshard request " << cmdObj << " failed: " << errmsg << endl;
                    return false;
                }

                result << "shardAdded" << name;
                return true;
            }

        } addServer;

        /* See usage docs at:
         * http://www.mongodb.org/display/DOCS/Configuring+Sharding#ConfiguringSharding-Removingashard
         */
        class RemoveShardCmd : public GridAdminCmd {
        public:
            RemoveShardCmd() : GridAdminCmd("removeShard") { }
            virtual void help( stringstream& help ) const {
                help << "remove a shard to the system.";
            }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string target = cmdObj.firstElement().valuestrsafe();
                Shard s = Shard::make( target );
                if ( ! grid.knowAboutShard( s.getConnString() ) ){
                    errmsg = "unknown shard";
                    return false;
                }

                ScopedDbConnection conn( configServer.getPrimary() );

                // If the server is not yet draining chunks, put it in draining mode.
                BSONObj searchDoc = BSON( "_id" << s.getName() );
                BSONObj drainingDoc = BSON( "_id" << s.getName() << ShardFields::draining(true) );
                BSONObj shardDoc = conn->findOne( "config.shards", drainingDoc );
                if ( shardDoc.isEmpty() ){

                    // TODO prevent move chunks to this shard.

                    log() << "going to start draining shard: " << s.getName() << endl;
                    BSONObj newStatus = BSON( "$set" << BSON( ShardFields::draining(true) ) );
                    conn->update( "config.shards" , searchDoc , newStatus, false /* do no upsert */);

                    errmsg = conn->getLastError();
                    if ( errmsg.size() ){
                        log() << "error starting remove shard: " << s.getName() << " err: " << errmsg << endl;
                        return false;
                    }

                    Shard::reloadShardInfo();

                    result.append( "msg"   , "draining started successfully" );
                    result.append( "state" , "started" ); 
                    result.append( "shard" , s.getName() );
                    conn.done();
                    return true;
                }

                // If the server has been completely drained, remove it from the ConfigDB.
                // Check not only for chunks but also databases.
                BSONObj shardIDDoc = BSON( "shard" << shardDoc[ "_id" ].str() );
                long long chunkCount = conn->count( "config.chunks" , shardIDDoc );
                BSONObj primaryDoc = BSON( "primary" << shardDoc[ "_id" ].str() );
                long long dbCount = conn->count( "config.databases" , primaryDoc );
                if ( ( chunkCount == 0 ) && ( dbCount == 0 ) ){
                    log() << "going to remove shard: " << s.getName() << endl;                    
                    conn->remove( "config.shards" , searchDoc );

                    errmsg = conn->getLastError();
                    if ( errmsg.size() ){
                        log() << "error concluding remove shard: " << s.getName() << " err: " << errmsg << endl;
                        return false;
                    }

                    Shard::removeShard( shardDoc[ "_id" ].str() );
                    Shard::reloadShardInfo();

                    result.append( "msg"   , "removeshard completed successfully" );
                    result.append( "state" , "completed" );
                    result.append( "shard" , s.getName() );
                    conn.done();
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

                conn.done();
                return true;
            }
        } removeShardCmd;


        // --------------- public commands ----------------

        class IsDbGridCmd : public Command {
        public:
            virtual LockType locktype() const { return NONE; } 
            virtual bool slaveOk() const {
                return true;
            }
            IsDbGridCmd() : Command("isdbgrid") { }
            bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("isdbgrid", 1);
                result.append("hostname", getHostNameCached());
                return true;
            }
        } isdbgrid;

        class CmdIsMaster : public Command {
        public:
            virtual LockType locktype() const { return NONE; } 
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "test if this is master half of a replica pair";
            }
            CmdIsMaster() : Command("isMaster" , false , "ismaster") { }
            virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("ismaster", 1.0 );
                result.append("msg", "isdbgrid");
                return true;
            }
        } ismaster;

        class CmdWhatsMyUri : public Command {
        public:
            CmdWhatsMyUri() : Command("whatsmyuri") { }
            virtual bool logTheOp() {
                return false; // the modification will be logged directly
            }
            virtual bool slaveOk() const {
                return true;
            }
            virtual LockType locktype() const { return NONE; } 
            virtual bool requiresAuth() {
                return false;
            }
            virtual void help( stringstream &help ) const {
                help << "{whatsmyuri:1}";
            }        
            virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result << "you" << ClientInfo::get()->getRemote();
                return true;
            }
        } cmdWhatsMyUri;
        

        class CmdShardingGetPrevError : public Command {
        public:
            virtual LockType locktype() const { return NONE; } 
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "get previous error (since last reseterror command)";
            }
            CmdShardingGetPrevError() : Command( "getPrevError" , false , "getpreverror") { }
            virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg += "getpreverror not supported for sharded environments";
                return false;
            }
        } cmdGetPrevError;

        class CmdShardingGetLastError : public Command {
        public:
            virtual LockType locktype() const { return NONE; } 
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "check for an error on the last command executed";
            }
            CmdShardingGetLastError() : Command("getLastError" , false , "getlasterror") { }
            
            void addWriteBack( vector<OID>& all , const BSONObj& o ){
                BSONElement e = o["writeback"];

                if ( e.type() == jstOID )
                    all.push_back( e.OID() );
            }
            
            void handleWriteBacks( vector<OID>& all ){
                if ( all.size() == 0 )
                    return;
                
                for ( unsigned i=0; i<all.size(); i++ ){
                    waitForWriteback( all[i] );
                }
            }
            
            virtual bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                LastError *le = lastError.disableForCommand();
                {
                    assert( le );
                    if ( le->msg.size() && le->nPrev == 1 ){
                        le->appendSelf( result );
                        return true;
                    }
                }
                
                ClientInfo * client = ClientInfo::get();
                set<string> * shards = client->getPrev();
                
                if ( shards->size() == 0 ){
                    result.appendNull( "err" );
                    return true;
                }

                //log() << "getlasterror enter: " << shards->size() << endl;


                vector<OID> writebacks;
                
                // handle single server
                if ( shards->size() == 1 ){
                    string theShard = *(shards->begin() );
                    result.append( "theshard" , theShard.c_str() );
                    ShardConnection conn( theShard , "" );
                    BSONObj res;
                    bool ok = conn->runCommand( dbName , cmdObj , res );
                    //log() << "\t" << res << endl;
                    result.appendElements( res );
                    conn.done();
                    result.append( "singleShard" , theShard );
                    addWriteBack( writebacks , res );
                    
                    // hit other machines just to block
                    for ( set<string>::const_iterator i=client->sinceLastGetError().begin(); i!=client->sinceLastGetError().end(); ++i ){
                        string temp = *i;
                        if ( temp == theShard )
                            continue;
                        
                        ShardConnection conn( temp , "" );
                        addWriteBack( writebacks , conn->getLastErrorDetailed() );
                        conn.done();
                    }
                    client->clearSinceLastGetError();
                    handleWriteBacks( writebacks );
                    return ok;
                }
                
                BSONArrayBuilder bbb( result.subarrayStart( "shards" ) );

                long long n = 0;

                // hit each shard
                vector<string> errors;
                for ( set<string>::iterator i = shards->begin(); i != shards->end(); i++ ){
                    string theShard = *i;
                    bbb.append( theShard );
                    ShardConnection conn( theShard , "" );
                    BSONObj res;
                    bool ok = conn->runCommand( dbName , cmdObj , res );
                    addWriteBack( writebacks, res );
                    string temp = DBClientWithCommands::getLastErrorString( res );
                    if ( ok == false || temp.size() )
                        errors.push_back( temp );
                    n += res["n"].numberLong();
                    conn.done();
                }
                
                bbb.done();
                
                result.appendNumber( "n" , n );

                // hit other machines just to block
                for ( set<string>::const_iterator i=client->sinceLastGetError().begin(); i!=client->sinceLastGetError().end(); ++i ){
                    string temp = *i;
                    if ( shards->count( temp ) )
                        continue;
                    
                    ShardConnection conn( temp , "" );
                    addWriteBack( writebacks, conn->getLastErrorDetailed() );
                    conn.done();
                }
                client->clearSinceLastGetError();

                if ( errors.size() == 0 ){
                    result.appendNull( "err" );
                    handleWriteBacks( writebacks );
                    return true;
                }
                
                result.append( "err" , errors[0].c_str() );
                
                BSONObjBuilder all;
                for ( unsigned i=0; i<errors.size(); i++ ){
                    all.append( all.numStr( i ) , errors[i].c_str() );
                }
                result.appendArray( "errs" , all.obj() );
                handleWriteBacks( writebacks );
                return true;
            }
        } cmdGetLastError;
        
    }
    
    class CmdListDatabases : public Command {
    public:
        CmdListDatabases() : Command("listDatabases", false , "listdatabases" ) {}

        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool slaveOverrideOk() { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream& help ) const { help << "list databases on cluster"; }
        
        bool run(const string& , BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector<Shard> shards;
            Shard::getAllShards( shards );
            
            map<string,long long> sizes;
            map< string,shared_ptr<BSONObjBuilder> > dbShardInfo;

            for ( vector<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ){
                Shard s = *i;
                BSONObj x = s.runCommand( "admin" , "listDatabases" );

                BSONObjIterator j( x["databases"].Obj() );
                while ( j.more() ){
                    BSONObj theDB = j.next().Obj();
                    
                    string name = theDB["name"].String();
                    long long size = theDB["sizeOnDisk"].numberLong();

                    long long& totalSize = sizes[name];
                    if ( size == 1 ){
                        if ( totalSize <= 1 )
                            totalSize = 1;
                    }
                    else
                        totalSize += size;
                    
                    shared_ptr<BSONObjBuilder>& bb = dbShardInfo[name];
                    if ( ! bb.get() )
                        bb.reset( new BSONObjBuilder() );
                    bb->appendNumber( s.getName() , size );
                }
                
            }
            
            long long totalSize = 0;

            BSONArrayBuilder bb( result.subarrayStart( "databases" ) );
            for ( map<string,long long>::iterator i=sizes.begin(); i!=sizes.end(); ++i ){
                string name = i->first;
                long long size = i->second;
                totalSize += size;
                
                BSONObjBuilder temp;
                temp.append( "name" , name );
                temp.appendNumber( "size" , size );
                temp.appendBool( "empty" , size == 1 );
                temp.append( "shards" , dbShardInfo[name]->obj() );
                
                bb.append( temp.obj() );
            }
            bb.done();

            result.appendNumber( "totalSize" , totalSize );
            result.appendNumber( "totalSizeMb" , totalSize / ( 1024 * 1024 ) );
            
            return 1;
        }

    } cmdListDatabases;

    class CmdCloseAllDatabases : public Command {
    public:
        CmdCloseAllDatabases() : Command("closeAllDatabases", false , "closeAllDatabases" ) {}
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool slaveOverrideOk() { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream& help ) const { help << "Not supported sharded"; }
        
        bool run(const string& , BSONObj& jsobj, string& errmsg, BSONObjBuilder& /*result*/, bool /*fromRepl*/) {
            errmsg = "closeAllDatabases isn't supported through mongos";
            return false;
        }
    } cmdCloseAllDatabases;


} // namespace mongo
