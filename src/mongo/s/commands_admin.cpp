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
#include "../util/net/message.h"
#include "../util/net/listen.h"
#include "../util/processinfo.h"
#include "../util/stringutils.h"
#include "../util/version.h"
#include "../util/timer.h"

#include "../client/connpool.h"
#include "mongo/client/dbclientcursor.h"

#include "../db/dbmessage.h"
#include "../db/commands.h"
#include "../db/stats/counters.h"

#include "config.h"
#include "chunk.h"
#include "grid.h"
#include "strategy.h"
#include "stats.h"
#include "writeback_listener.h"
#include "client.h"
#include "../util/ramlog.h"

namespace mongo {

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

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }

            bool okForConfigChanges( string& errmsg ) {
                string e;
                if ( ! configServer.allUp(e) ) {
                    errmsg = str::stream() << "not all config servers are up: " << e;
                    return false;
                }
                return true;
            }
        };

        // --------------- misc commands ----------------------

        class NetStatCmd : public GridAdminCmd {
        public:
            NetStatCmd() : GridAdminCmd("netstat") { }
            virtual void help( stringstream& help ) const {
                help << " shows status/reachability of servers in the cluster";
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("configserver", configServer.getPrimary().getConnString() );
                result.append("isdbgrid", 1);
                return true;
            }
        } netstat;

        class FlushRouterConfigCmd : public GridAdminCmd {
        public:
            FlushRouterConfigCmd() : GridAdminCmd("flushRouterConfig") { }
            virtual void help( stringstream& help ) const {
                help << "flush all router config";
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                grid.flushConfig();
                result.appendBool( "flushed" , true );
                return true;
            }
        } flushRouterConfigCmd;


        class ServerStatusCmd : public Command {
        public:
            ServerStatusCmd() : Command( "serverStatus" , true ) {
                _started = time(0);
            }

            virtual bool slaveOk() const { return true; }
            virtual LockType locktype() const { return NONE; }

            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
                result.append( "host" , prettyHostName() );
                result.append("version", versionString);
                result.append("process","mongos");
                result.append("uptime",(double) (time(0)-_started));
                result.appendDate( "localTime" , jsTime() );

                {
                    BSONObjBuilder t( result.subobjStart( "mem" ) );

                    ProcessInfo p;
                    if ( p.supported() ) {
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

                {
                    BSONObjBuilder bb( result.subobjStart( "network" ) );
                    networkCounter.append( bb );
                    bb.done();
                }

                {
                    RamLog* rl = RamLog::get( "warnings" );
                    verify(rl);
                    
                    if (rl->lastWrite() >= time(0)-(10*60)){ // only show warnings from last 10 minutes
                        vector<const char*> lines;
                        rl->get( lines );
                        
                        BSONArrayBuilder arr( result.subarrayStart( "warnings" ) );
                        for ( unsigned i=std::max(0,(int)lines.size()-10); i<lines.size(); i++ )
                            arr.append( lines[i] );
                        arr.done();
                    }
                }

                return 1;
            }

            time_t _started;
        } cmdServerStatus;

        class FsyncCommand : public GridAdminCmd {
        public:
            FsyncCommand() : GridAdminCmd( "fsync" ) {}
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                if ( cmdObj["lock"].trueValue() ) {
                    errmsg = "can't do lock through mongos";
                    return false;
                }

                BSONObjBuilder sub;

                bool ok = true;
                int numFiles = 0;

                vector<Shard> shards;
                Shard::getAllShards( shards );
                for ( vector<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ) {
                    Shard s = *i;

                    BSONObj x = s.runCommand( "admin" , "fsync" );
                    sub.append( s.getName() , x );

                    if ( ! x["ok"].trueValue() ) {
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
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string dbname = cmdObj.firstElement().valuestrsafe();

                if ( dbname.size() == 0 ) {
                    errmsg = "no db";
                    return false;
                }

                if ( dbname == "config" ) {
                    errmsg = "can't move config db";
                    return false;
                }

                // Flush the configuration
                // This can't be perfect, but it's better than nothing.
                grid.flushConfig();

                DBConfigPtr config = grid.getDBConfig( dbname , false );
                if ( ! config ) {
                    errmsg = "can't find db!";
                    return false;
                }

                string to = cmdObj["to"].valuestrsafe();
                if ( ! to.size()  ) {
                    errmsg = "you have to specify where you want to move it";
                    return false;
                }
                Shard s = Shard::make( to );

                if ( config->getPrimary() == s.getConnString() ) {
                    errmsg = "it is already the primary";
                    return false;
                }

                if ( ! grid.knowAboutShard( s.getConnString() ) ) {
                    errmsg = "that server isn't known to me";
                    return false;
                }

                log() << "Moving " << dbname << " primary from: " << config->getPrimary().toString()
                      << " to: " << s.toString() << endl;

                // Locking enabled now...
                DistributedLock lockSetup( configServer.getConnectionString(), dbname + "-movePrimary" );
                dist_lock_try dlk;

                // Distributed locking added.
                try{
                    dlk = dist_lock_try( &lockSetup , string("Moving primary shard of ") + dbname );
                }
                catch( LockException& e ){
	                errmsg = str::stream() << "error locking distributed lock to move primary shard of " << dbname << causedBy( e );
	                warning() << errmsg << endl;
	                return false;
                }

                if ( ! dlk.got() ) {
	                errmsg = (string)"metadata lock is already taken for moving " + dbname;
	                return false;
                }

                set<string> shardedColls;
                config->getAllShardedCollections( shardedColls );

                BSONArrayBuilder barr;
                barr.append( shardedColls );

                ScopedDbConnection toconn( s.getConnString() );

                // TODO ERH - we need a clone command which replays operations from clone start to now
                //            can just use local.oplog.$main
                BSONObj cloneRes;
                bool worked = toconn->runCommand( dbname.c_str() , BSON( "clone" << config->getPrimary().getConnString() << "collsToIgnore" << barr.arr() ) , cloneRes );
                toconn.done();

                if ( ! worked ) {
                    log() << "clone failed" << cloneRes << endl;
                    errmsg = "clone failed";
                    return false;
                }

                string oldPrimary = config->getPrimary().getConnString();

                ScopedDbConnection fromconn( config->getPrimary() );

                config->setPrimary( s.getConnString() );

                if( shardedColls.empty() ){

                    // TODO: Collections can be created in the meantime, and we should handle in the future.
                    log() << "movePrimary dropping database on " << oldPrimary << ", no sharded collections in " << dbname << endl;

                    try {
                        fromconn->dropDatabase( dbname.c_str() );
                    }
                    catch( DBException& e ){
                        e.addContext( str::stream() << "movePrimary could not drop the database " << dbname << " on " << oldPrimary );
                        throw;
                    }

                }
                else if( cloneRes["clonedColls"].type() != Array ){

                    // Legacy behavior from old mongod with sharded collections, *do not* delete database,
                    // but inform user they can drop manually (or ignore).
                    warning() << "movePrimary legacy mongod behavior detected, user must manually remove unsharded collections in "
                          << "database " << dbname << " on " << oldPrimary << endl;

                }
                else {

                    // We moved some unsharded collections, but not all
                    BSONObjIterator it( cloneRes["clonedColls"].Obj() );

                    while( it.more() ){
                        BSONElement el = it.next();
                        if( el.type() == String ){
                            try {
                                log() << "movePrimary dropping cloned collection " << el.String() << " on " << oldPrimary << endl;
                                fromconn->dropCollection( el.String() );
                            }
                            catch( DBException& e ){
                                e.addContext( str::stream() << "movePrimary could not drop the cloned collection " << el.String() << " on " << oldPrimary );
                                throw;
                            }
                        }
                    }
                }

                fromconn.done();

                result << "primary " << s.toString();

                return true;
            }
        } movePrimary;

        class EnableShardingCmd : public GridAdminCmd {
        public:
            EnableShardingCmd() : GridAdminCmd( "enableSharding" ) {}
            virtual void help( stringstream& help ) const {
                help
                        << "Enable sharding for a db. (Use 'shardcollection' command afterwards.)\n"
                        << "  { enablesharding : \"<dbname>\" }\n";
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string dbname = cmdObj.firstElement().valuestrsafe();
                if ( dbname.size() == 0 ) {
                    errmsg = "no db";
                    return false;
                }
                
                if ( dbname == "admin" ) {
                    errmsg = "can't shard the admin db";
                    return false;
                }
                if ( dbname == "local" ) {
                    errmsg = "can't shard the local db";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( dbname );
                if ( config->isShardingEnabled() ) {
                    errmsg = "already enabled";
                    return false;
                }
                
                if ( ! okForConfigChanges( errmsg ) )
                    return false;
                
                log() << "enabling sharding on: " << dbname << endl;

                config->enableSharding();

                return true;
            }
        } enableShardingCmd;

        // ------------ collection level commands -------------

        class ShardCollectionCmd : public GridAdminCmd {
        public:
            ShardCollectionCmd() : GridAdminCmd( "shardCollection" ) {}

            virtual void help( stringstream& help ) const {
                help
                        << "Shard a collection.  Requires key.  Optional unique. Sharding must already be enabled for the database.\n"
                        << "  { enablesharding : \"<dbname>\" }\n";
            }

            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "no ns";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isShardingEnabled() ) {
                    errmsg = "sharding not enabled for db";
                    return false;
                }

                if ( config->isSharded( ns ) ) {
                    errmsg = "already sharded";
                    return false;
                }

                BSONObj key = cmdObj.getObjectField( "key" );
                if ( key.isEmpty() ) {
                    errmsg = "no shard key";
                    return false;
                }

                BSONForEach(e, key) {
                    if (!e.isNumber() || e.number() != 1.0) {
                        errmsg = "shard keys must all be ascending";
                        return false;
                    }
                }

                if ( ns.find( ".system." ) != string::npos ) {
                    errmsg = "can't shard system namespaces";
                    return false;
                }

                if ( ! okForConfigChanges( errmsg ) )
                    return false;

                // Sharding interacts with indexing in at least three ways:
                //
                // 1. A unique index must have the sharding key as its prefix. Otherwise maintaining uniqueness would
                // require coordinated access to all shards. Trying to shard a collection with such an index is not
                // allowed.
                //
                // 2. Sharding a collection requires an index over the sharding key. That index must be create upfront.
                // The rationale is that sharding a non-empty collection would need to create the index and that could
                // be slow. Requiring the index upfront allows the admin to plan before sharding and perhaps use
                // background index construction. One exception to the rule: empty collections. It's fairly easy to
                // create the index as part of the sharding process.
                //
                // 3. If unique : true is specified, we require that the sharding index be unique or created as unique.
                //
                // We enforce both these conditions in what comes next.

                bool careAboutUnique = cmdObj["unique"].trueValue();

                {
                    ShardKeyPattern proposedKey( key );
                    bool hasShardIndex = false;
                    bool hasUniqueShardIndex = false;

                    ScopedDbConnection conn( config->getPrimary() );
                    BSONObjBuilder b;
                    b.append( "ns" , ns );

                    BSONArrayBuilder allIndexes;

                    auto_ptr<DBClientCursor> cursor = conn->query( config->getName() + ".system.indexes" , b.obj() );
                    while ( cursor->more() ) {
                        BSONObj idx = cursor->next();

                        allIndexes.append( idx );

                        bool idIndex = ! idx["name"].eoo() && idx["name"].String() == "_id_";
                        bool uniqueIndex = ( ! idx["unique"].eoo() && idx["unique"].trueValue() ) ||
                        		           idIndex;

                        // Is index key over the sharding key? Remember that.
                        if ( key.woCompare( idx["key"].embeddedObjectUserCheck() ) == 0 ) {

                            if( idx["sparse"].trueValue() ){
                                errmsg = (string)"can't shard collection " + ns + " with sparse shard key index";
                                conn.done();
                                return false;
                            }

                            hasShardIndex = true;
                            hasUniqueShardIndex = uniqueIndex;
                            continue;
                        }

                        // Not a unique index? Move on.
                        if ( ! uniqueIndex || idIndex )
                            continue;

                        // Shard key is prefix of unique index? Move on.
                        if ( proposedKey.isPrefixOf( idx["key"].embeddedObjectUserCheck() ) )
                            continue;

                        errmsg = str::stream() << "can't shard collection '" << ns << "' with unique index on: " + idx.toString()
                                               << ", uniqueness can't be maintained across unless shard key index is a prefix";
                        conn.done();
                        return false;
                    }

                    if( careAboutUnique && hasShardIndex && ! hasUniqueShardIndex ){
                        errmsg = (string)"can't shard collection " + ns + ", shard key index not unique and unique index explicitly specified";
                        conn.done();
                        return false;
                    }

                    BSONObj res = conn->findOne( config->getName() + ".system.namespaces" , BSON( "name" << ns ) );
                    if ( res["options"].type() == Object && res["options"].embeddedObject()["capped"].trueValue() ) {
                        errmsg = "can't shard capped collection";
                        conn.done();
                        return false;
                    }

                    if ( hasShardIndex ) {
                        // make sure there are no null entries in the sharding index
                        BSONObjBuilder cmd;
                        cmd.append( "checkShardingIndex" , ns );
                        cmd.append( "keyPattern" , key );
                        BSONObj cmdObj = cmd.obj();
                        if ( ! conn->runCommand( "admin" , cmdObj , res )) {
                            errmsg = res["errmsg"].str();
                            conn.done();
                            return false;
                        }
                    }

                    if ( ! hasShardIndex && ( conn->count( ns ) != 0 ) ) {
                        errmsg = "please create an index over the sharding key before sharding.";
                        result.append( "proposedKey" , key );
                        result.appendArray( "curIndexes" , allIndexes.done() );
                        conn.done();
                        return false;
                    }

                    conn.done();
                }

                tlog() << "CMD: shardcollection: " << cmdObj << endl;

//                vector<BSONObj> pts;
//                if (cmdObj.hasField("splitPoints")) {
//                    if ( cmdObj.getField("splitPoints").type() != Array ) {
//                        errmsg = "Value of splitPoints must be an array of objects";
//                        return false;
//                    }
//
//                    vector<BSONElement> elmts = cmdObj.getField("splitPoints").Array();
//                    for ( unsigned i = 0 ; i < elmts.size() ; ++i) {
//                        if ( elmts[i].type() != Object ) {
//                            errmsg = "Elements in the splitPoints array must be objects";
//                            return false;
//                        }
//                        pts.push_back( elmts[i].Obj() );
//                    }
//                }
                config->shardCollection( ns , key , careAboutUnique );

                result << "collectionsharded" << ns;
                return true;
            }
        } shardCollectionCmd;

        class GetShardVersion : public GridAdminCmd {
        public:
            GetShardVersion() : GridAdminCmd( "getShardVersion" ) {}
            virtual void help( stringstream& help ) const {
                help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
            }

            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "need to specify fully namespace";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ) {
                    errmsg = "ns not sharded.";
                    return false;
                }

                ChunkManagerPtr cm = config->getChunkManagerIfExists( ns );
                if ( ! cm ) {
                    errmsg = "no chunk manager?";
                    return false;
                }
                cm->_printChunks();
                result.appendTimestamp( "version" , cm->getVersion().toLong() );

                return 1;
            }
        } getShardVersionCmd;

        class SplitCollectionCmd : public GridAdminCmd {
        public:
            SplitCollectionCmd() : GridAdminCmd( "split" ) {}
            virtual void help( stringstream& help ) const {
                help
                        << " example: - split the shard that contains give key \n"
                        << " { split : 'alleyinsider.blog.posts' , find : { ts : 1 } }\n"
                        << " example: - split the shard that contains the key with this as the middle \n"
                        << " { split : 'alleyinsider.blog.posts' , middle : { ts : 1 } }\n"
                        << " NOTE: this does not move move the chunks, it merely creates a logical separation \n"
                        ;
            }

            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

                if ( ! okForConfigChanges( errmsg ) )
                    return false;

                ShardConnection::sync();

                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "no ns";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ) {
                    config->reload();
                    if ( ! config->isSharded( ns ) ) {
                        errmsg = "ns not sharded.  have to shard before can split";
                        return false;
                    }
                }

                BSONObj find = cmdObj.getObjectField( "find" );
                if ( find.isEmpty() ) {
                    find = cmdObj.getObjectField( "middle" );

                    if ( find.isEmpty() ) {
                        errmsg = "need to specify find or middle";
                        return false;
                    }
                }

                ChunkManagerPtr info = config->getChunkManager( ns );
                ChunkPtr chunk = info->findChunk( find );
                BSONObj middle = cmdObj.getObjectField( "middle" );

                verify( chunk.get() );
                log() << "splitting: " << ns << "  shard: " << chunk << endl;

                BSONObj res;
                bool worked;
                if ( middle.isEmpty() ) {
                    BSONObj ret = chunk->singleSplit( true /* force a split even if not enough data */ , res );
                    worked = !ret.isEmpty();
                }
                else {
                    // sanity check if the key provided is a valid split point
                    if ( ( middle == chunk->getMin() ) || ( middle == chunk->getMax() ) ) {
                        errmsg = "cannot split on initial or final chunk's key";
                        return false;
                    }

                    if (!fieldsMatch(middle, info->getShardKey().key())){
                        errmsg = "middle has different fields (or different order) than shard key";
                        return false;
                    }

                    vector<BSONObj> splitPoints;
                    splitPoints.push_back( middle );
                    worked = chunk->multiSplit( splitPoints , res );
                }

                if ( !worked ) {
                    errmsg = "split failed";
                    result.append( "cause" , res );
                    return false;
                }
                config->getChunkManager( ns , true );
                return true;
            }
        } splitCollectionCmd;

        class MoveChunkCmd : public GridAdminCmd {
        public:
            MoveChunkCmd() : GridAdminCmd( "moveChunk" ) {}
            virtual void help( stringstream& help ) const {
                help << "{ movechunk : 'test.foo' , find : { num : 1 } , to : 'localhost:30001' }";
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

                if ( ! okForConfigChanges( errmsg ) )
                    return false;

                ShardConnection::sync();

                Timer t;
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "no ns";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ) {
                    config->reload();
                    if ( ! config->isSharded( ns ) ) {
                        errmsg = "ns not sharded.  have to shard before we can move a chunk";
                        return false;
                    }
                }

                BSONObj find = cmdObj.getObjectField( "find" );
                if ( find.isEmpty() ) {
                    errmsg = "need to specify find.  see help";
                    return false;
                }

                string toString = cmdObj["to"].valuestrsafe();
                if ( ! toString.size()  ) {
                    errmsg = "you have to specify where you want to move the chunk";
                    return false;
                }

                Shard to = Shard::make( toString );

                // so far, chunk size serves test purposes; it may or may not become a supported parameter
                long long maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
                if ( maxChunkSizeBytes == 0 ) {
                    maxChunkSizeBytes = Chunk::MaxChunkSize;
                }

                tlog() << "CMD: movechunk: " << cmdObj << endl;

                ChunkManagerPtr info = config->getChunkManager( ns );
                ChunkPtr c = info->findChunk( find );
                const Shard& from = c->getShard();

                if ( from == to ) {
                    errmsg = "that chunk is already on that shard";
                    return false;
                }

                BSONObj res;
                if ( ! c->moveAndCommit( to , maxChunkSizeBytes , res ) ) {
                    errmsg = "move failed";
                    result.append( "cause" , res );
                    return false;
                }
                
                // preemptively reload the config to get new version info
                config->getChunkManager( ns , true );

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
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                ScopedDbConnection conn( configServer.getPrimary() );

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( "config.shards" , BSONObj() );
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
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
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
                if ( cmdObj[ ShardFields::maxSize.name() ].isNumber() ) {
                    maxSize = cmdObj[ ShardFields::maxSize.name() ].numberLong();
                }

                if ( ! grid.addShard( &name , servers , maxSize , errmsg ) ) {
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
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string target = cmdObj.firstElement().valuestrsafe();
                Shard s = Shard::make( target );
                if ( ! grid.knowAboutShard( s.getConnString() ) ) {
                    errmsg = "unknown shard";
                    return false;
                }

                ScopedDbConnection conn( configServer.getPrimary() );

                if (conn->count("config.shards", BSON("_id" << NE << s.getName() << ShardFields::draining(true)))){
                    conn.done();
                    errmsg = "Can't have more than one draining shard at a time";
                    return false;
                }

                if (conn->count("config.shards", BSON("_id" << NE << s.getName())) == 0){
                    conn.done();
                    errmsg = "Can't remove last shard";
                    return false;
                }

                BSONObj primaryDoc = BSON( "_id" << NE << "local" << "primary" << s.getName() );
                BSONObj dbInfo; // appended at end of result on success
                {
                    boost::scoped_ptr<DBClientCursor> cursor (conn->query("config.databases", primaryDoc));
                    if (cursor->more()) { // skip block and allocations if empty
                        BSONObjBuilder dbInfoBuilder;
                        dbInfoBuilder.append("note", "you need to drop or movePrimary these databases");
                        BSONArrayBuilder dbs(dbInfoBuilder.subarrayStart("dbsToMove"));

                        while (cursor->more()){
                            BSONObj db = cursor->nextSafe();
                            dbs.append(db["_id"]);
                        }
                        dbs.doneFast();

                        dbInfo = dbInfoBuilder.obj();
                    }
                }

                // If the server is not yet draining chunks, put it in draining mode.
                BSONObj searchDoc = BSON( "_id" << s.getName() );
                BSONObj drainingDoc = BSON( "_id" << s.getName() << ShardFields::draining(true) );
                BSONObj shardDoc = conn->findOne( "config.shards", drainingDoc );
                if ( shardDoc.isEmpty() ) {

                    // TODO prevent move chunks to this shard.

                    log() << "going to start draining shard: " << s.getName() << endl;
                    BSONObj newStatus = BSON( "$set" << BSON( ShardFields::draining(true) ) );
                    conn->update( "config.shards" , searchDoc , newStatus, false /* do no upsert */);

                    errmsg = conn->getLastError();
                    if ( errmsg.size() ) {
                        log() << "error starting remove shard: " << s.getName() << " err: " << errmsg << endl;
                        return false;
                    }

                    BSONObj primaryLocalDoc = BSON("_id" << "local" <<  "primary" << s.getName() );
                    PRINT(primaryLocalDoc);
                    if (conn->count("config.databases", primaryLocalDoc)) {
                        log() << "This shard is listed as primary of local db. Removing entry." << endl;
                        conn->remove("config.databases", BSON("_id" << "local"));
                        errmsg = conn->getLastError();
                        if ( errmsg.size() ) {
                            log() << "error removing local db: " << errmsg << endl;
                            return false;
                        }
                    }

                    Shard::reloadShardInfo();

                    result.append( "msg"   , "draining started successfully" );
                    result.append( "state" , "started" );
                    result.append( "shard" , s.getName() );
                    result.appendElements(dbInfo);
                    conn.done();
                    return true;
                }

                // If the server has been completely drained, remove it from the ConfigDB.
                // Check not only for chunks but also databases.
                BSONObj shardIDDoc = BSON( "shard" << shardDoc[ "_id" ].str() );
                long long chunkCount = conn->count( "config.chunks" , shardIDDoc );
                long long dbCount = conn->count( "config.databases" , primaryDoc );
                if ( ( chunkCount == 0 ) && ( dbCount == 0 ) ) {
                    log() << "going to remove shard: " << s.getName() << endl;
                    conn->remove( "config.shards" , searchDoc );

                    errmsg = conn->getLastError();
                    if ( errmsg.size() ) {
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
                result.appendElements(dbInfo);

                conn.done();
                return true;
            }
        } removeShardCmd;


        // --------------- public commands ----------------

        class IsDbGridCmd : public Command {
        public:
            virtual LockType locktype() const { return NONE; }
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() const {
                return true;
            }
            IsDbGridCmd() : Command("isdbgrid") { }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
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
            virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.appendBool("ismaster", true );
                result.append("msg", "isdbgrid");
                result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
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
            virtual void help( stringstream &help ) const {
                help << "{whatsmyuri:1}";
            }
            virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
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
            virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
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

            virtual bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                LastError *le = lastError.disableForCommand();
                {
                    verify( le );
                    if ( le->msg.size() && le->nPrev == 1 ) {
                        le->appendSelf( result );
                        return true;
                    }
                }

                ClientInfo * client = ClientInfo::get();
                return client->getLastError( cmdObj , result );
            }
        } cmdGetLastError;

    }

    class CmdShardingResetError : public Command {
    public:
        CmdShardingResetError() : Command( "resetError" , false , "reseterror" ) {}

        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const {
            return true;
        }

        bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            LastError *le = lastError.get();
            if ( le )
                le->reset();

            ClientInfo * client = ClientInfo::get();
            set<string> * shards = client->getPrev();

            for ( set<string>::iterator i = shards->begin(); i != shards->end(); i++ ) {
                string theShard = *i;
                ShardConnection conn( theShard , "" );
                BSONObj res;
                conn->runCommand( dbName , cmdObj , res );
                conn.done();
            }

            return true;
        }
    } cmdShardingResetError;

    class CmdListDatabases : public Command {
    public:
        CmdListDatabases() : Command("listDatabases", true , "listdatabases" ) {}

        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream& help ) const { help << "list databases on cluster"; }

        bool run(const string& , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector<Shard> shards;
            Shard::getAllShards( shards );

            map<string,long long> sizes;
            map< string,shared_ptr<BSONObjBuilder> > dbShardInfo;

            for ( vector<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ) {
                Shard s = *i;
                BSONObj x = s.runCommand( "admin" , "listDatabases" );

                BSONObjIterator j( x["databases"].Obj() );
                while ( j.more() ) {
                    BSONObj theDB = j.next().Obj();

                    string name = theDB["name"].String();
                    long long size = theDB["sizeOnDisk"].numberLong();

                    long long& totalSize = sizes[name];
                    if ( size == 1 ) {
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
            for ( map<string,long long>::iterator i=sizes.begin(); i!=sizes.end(); ++i ) {
                string name = i->first;

                if ( name == "local" ) {
                    // we don't return local
                    // since all shards have their own independent local
                    continue;
                }

                long long size = i->second;
                totalSize += size;

                BSONObjBuilder temp;
                temp.append( "name" , name );
                temp.appendNumber( "sizeOnDisk" , size );
                temp.appendBool( "empty" , size == 1 );
                temp.append( "shards" , dbShardInfo[name]->obj() );

                bb.append( temp.obj() );
            }
            
            if ( sizes.find( "config" ) == sizes.end() ){
                ScopedDbConnection conn( configServer.getPrimary() );
                BSONObj x;
                if ( conn->simpleCommand( "config" , &x , "dbstats" ) ){
                    BSONObjBuilder b;
                    b.append( "name" , "config" );
                    b.appendBool( "empty" , false );
                    if ( x["fileSize"].type() )
                        b.appendAs( x["fileSize"] , "sizeOnDisk" );
                    else
                        b.append( "sizeOnDisk" , 1 );
                    bb.append( b.obj() );
                }
                else {
                    bb.append( BSON( "name" << "config" ) );
                }
                conn.done();
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
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream& help ) const { help << "Not supported sharded"; }

        bool run(const string& , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& /*result*/, bool /*fromRepl*/) {
            errmsg = "closeAllDatabases isn't supported through mongos";
            return false;
        }
    } cmdCloseAllDatabases;


    class CmdReplSetGetStatus : public Command {
    public:
        CmdReplSetGetStatus() : Command("replSetGetStatus"){}
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream& help ) const { help << "Not supported through mongos"; }

        bool run(const string& , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            if ( jsobj["forShell"].trueValue() )
                lastError.disableForCommand();

            errmsg = "replSetGetStatus is not supported through mongos";
            result.append("info", "mongos"); // see sayReplSetMemberState
            return false;
        }
    } cmdReplSetGetStatus;

    CmdShutdown cmdShutdown;

    void CmdShutdown::help( stringstream& help ) const {
        help << "shutdown the database.  must be ran against admin db and "
             << "either (1) ran from localhost or (2) authenticated.";
    }

    bool CmdShutdown::run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        return shutdownHelper();
    }

} // namespace mongo
