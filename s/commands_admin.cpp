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

#include "../client/connpool.h"

#include "../db/dbmessage.h"
#include "../db/commands.h"
#include "../db/stats/counters.h"

#include "config.h"
#include "chunk.h"
#include "strategy.h"
#include "stats.h"

namespace mongo {

    extern string ourHostname;

    namespace dbgrid_cmds {

        set<string> dbgridCommands;

        class GridAdminCmd : public Command {
        public:
            GridAdminCmd( const char * n ) : Command( n ){
                dbgridCommands.insert( n );
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
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
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
            
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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

        class ListGridCommands : public GridAdminCmd {
        public:
            ListGridCommands() : GridAdminCmd("gridcommands") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

                BSONObjBuilder arr;
                int num=0;
                for ( set<string>::iterator i = dbgridCommands.begin(); i != dbgridCommands.end(); i++ ){
                    string s = BSONObjBuilder::numStr( num++ );
                    arr.append( s.c_str() , *i );
                }

                result.appendArray( "commands" , arr.done() );
                return true;
            }
        } listGridCommands;

        // ------------ database level commands -------------

        class ListDatabaseCommand : public GridAdminCmd {
        public:
            ListDatabaseCommand() : GridAdminCmd("listdatabases") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ShardConnection conn( configServer.getPrimary() );

                auto_ptr<DBClientCursor> cursor = conn->query( "config.databases" , BSONObj() );

                BSONObjBuilder list;
                int num = 0;
                while ( cursor->more() ){
                    string s = BSONObjBuilder::numStr( num++ );

                    BSONObj o = cursor->next();
                    list.append( s.c_str() , o["name"].valuestrsafe() );
                }

                result.appendArray("databases" , list.obj() );
                conn.done();

                return true;
            }
        } gridListDatabase;

        class MoveDatabasePrimaryCommand : public GridAdminCmd {
        public:
            MoveDatabasePrimaryCommand() : GridAdminCmd("moveprimary") { }
            virtual void help( stringstream& help ) const {
                help << " example: { moveprimary : 'foo' , to : 'localhost:9999' } TODO: locking? ";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string dbname = cmdObj["moveprimary"].valuestrsafe();

                if ( dbname.size() == 0 ){
                    errmsg = "no db";
                    return false;
                }

                if ( dbname == "config" ){
                    errmsg = "can't move config db";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( dbname , false );
                if ( ! config ){
                    errmsg = "can't find db!";
                    return false;
                }

                string to = cmdObj["to"].valuestrsafe();
                if ( ! to.size()  ){
                    errmsg = "you have to specify where you want to move it";
                    return false;
                }

                if ( config->getPrimary() == to ){
                    errmsg = "thats already the primary";
                    return false;
                }

                if ( ! grid.knowAboutShard( to ) ){
                    errmsg = "that server isn't known to me";
                    return false;
                }

                ShardConnection conn( configServer.getPrimary() );

                log() << "movePrimary: moving " << dbname << " primary from: " << config->getPrimary().toString() << " to: " << to << endl;

                // TODO LOCKING: this is not safe with multiple mongos


                ShardConnection toconn( to );

                // TODO AARON - we need a clone command which replays operations from clone start to now
                //              using a seperate smaller oplog
                BSONObj cloneRes;
                bool worked = toconn->runCommand( dbname.c_str() , BSON( "clone" << config->getPrimary().getConnString() ) , cloneRes );
                toconn.done();
                if ( ! worked ){
                    log() << "clone failed" << cloneRes << endl;
                    errmsg = "clone failed";
                    conn.done();
                    return false;
                }

                ShardConnection fromconn( config->getPrimary() );

                config->setPrimary( to );
                config->save( true );

                log() << "movePrimary:  dropping " << dbname << " from old" << endl;

                fromconn->dropDatabase( dbname.c_str() );
                fromconn.done();

                result << "primary" << to;

                conn.done();
                return true;
            }
        } movePrimary;

        class EnableShardingCmd : public GridAdminCmd {
        public:
            EnableShardingCmd() : GridAdminCmd( "enablesharding" ){}
            virtual void help( stringstream& help ) const {
                help
                    << "Enable sharding for a db. (Use 'shardcollection' command afterwards.)\n"
                    << "  { enablesharding : \"<dbname>\" }\n";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string dbname = cmdObj["enablesharding"].valuestrsafe();
                if ( dbname.size() == 0 ){
                    errmsg = "no db";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( dbname );
                if ( config->isShardingEnabled() ){
                    errmsg = "already enabled";
                    return false;
                }
                
                log() << "enabling sharding on: " << dbname << endl;

                config->enableSharding();
                config->save( true );

                return true;
            }
        } enableShardingCmd;

        // ------------ collection level commands -------------

        class ShardCollectionCmd : public GridAdminCmd {
        public:
            ShardCollectionCmd() : GridAdminCmd( "shardcollection" ){}
            virtual void help( stringstream& help ) const {
                help
                    << "Shard a collection.  Requires key.  Optional unique. Sharding must already be enabled for the database.\n"
                    << "  { enablesharding : \"<dbname>\" }\n";
            }
            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj["shardcollection"].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( ns );
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
                } else if (key.nFields() > 1){
                    errmsg = "compound shard keys not supported yet";
                    return false;
                }

                if ( ns.find( ".system." ) != string::npos ){
                    errmsg = "can't shard system namespaces";
                    return false;
                }
                
                ShardKeyPattern proposedKey( key );
                {
                    ShardConnection conn( config->getPrimary() );
                    BSONObjBuilder b; 
                    b.append( "ns" , ns ); 
                    b.appendBool( "unique" , true ); 
                    
                    auto_ptr<DBClientCursor> cursor = conn->query( config->getName() + ".system.indexes" , b.obj() );
                    while ( cursor->more() ){
                        BSONObj idx = cursor->next();
                        if ( proposedKey.uniqueAllowd( idx["key"].embeddedObjectUserCheck() ) )
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

                    conn.done();
                }
                
                log() << "CMD: shardcollection: " << cmdObj << endl;

                config->shardCollection( ns , key , cmdObj["unique"].trueValue() );
                config->save( true );

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
            
            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj["getShardVersion"].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "need to speciy fully namespace";
                    return false;
                }
                
                DBConfig * config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ){
                    errmsg = "ns not sharded.";
                    return false;
                }
                
                ChunkManager * cm = config->getChunkManager( ns );
                if ( ! cm ){
                    errmsg = "no chunk manager?";
                    return false;
                }
                
                result.appendTimestamp( "version" , cm->getVersion() );

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

            virtual bool _split( BSONObjBuilder& result , string&errmsg , const string& ns , ChunkManager * manager , Chunk& old , BSONObj middle ) = 0;

            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj[_name.c_str()].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( ns );
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
                
                ChunkManager * info = config->getChunkManager( ns );
                Chunk& old = info->findChunk( find );

                return _split( result , errmsg , ns , info , old , cmdObj.getObjectField( "middle" ) );
            }

        protected:
            string _name;
        };

        class SplitValueCommand : public SplitCollectionHelper {
        public:
            SplitValueCommand() : SplitCollectionHelper( "splitvalue" ){}
            virtual bool _split( BSONObjBuilder& result , string& errmsg , const string& ns , ChunkManager * manager , Chunk& old , BSONObj middle ){

                result << "shardinfo" << old.toString();

                result.appendBool( "auto" , middle.isEmpty() );

                if ( middle.isEmpty() )
                    middle = old.pickSplitPoint();

                result.append( "middle" , middle );

                return true;
            }

        } splitValueCmd;


        class SplitCollection : public SplitCollectionHelper {
        public:
            SplitCollection() : SplitCollectionHelper( "split" ){}
            virtual bool _split( BSONObjBuilder& result , string& errmsg , const string& ns , ChunkManager * manager , Chunk& old , BSONObj middle ){

                log() << "splitting: " << ns << "  shard: " << old << endl;

                if ( middle.isEmpty() )
                    old.split();
                else
                    old.split( middle );

                return true;
            }


        } splitCollectionCmd;

        class MoveChunkCmd : public GridAdminCmd {
        public:
            MoveChunkCmd() : GridAdminCmd( "movechunk" ){}
            virtual void help( stringstream& help ) const {
                help << "{ movechunk : 'test.foo' , find : { num : 1 } , to : 'localhost:30001' }";
            }
            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj["movechunk"].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( ns );
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

                log() << "CMD: movechunk: " << cmdObj << endl;

                ChunkManager * info = config->getChunkManager( ns );
                Chunk& c = info->findChunk( find );
                const Shard& from = c.getShard();

                if ( from == to ){
                    errmsg = "that chunk is already on that shard";
                    return false;
                }

                if ( ! c.moveAndCommit( to , errmsg ) )
                    return false;

                return true;
            }
        } moveChunkCmd;

        // ------------ server level commands -------------

        class ListShardsCmd : public GridAdminCmd {
        public:
            ListShardsCmd() : GridAdminCmd("listshards") { }
            virtual void help( stringstream& help ) const {
                help << "list all shards of the system";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ShardConnection conn( configServer.getPrimary() );

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
            AddShard() : GridAdminCmd("addshard") { }
            virtual void help( stringstream& help ) const {
                help << "add a new shard to the system";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ShardConnection conn( configServer.getPrimary() );
                
                
                string host = cmdObj["addshard"].valuestrsafe();
                
                if ( host == "localhost" || host.find( "localhost:" ) == 0 ||
                     host == "127.0.0.1" || host.find( "127.0.0.1:" ) == 0 ){
                    if ( ! cmdObj["allowLocal"].trueValue() ){
                        errmsg = 
                            "can't use localhost as a shard since all shards need to communicate.  "
                            "allowLocal to override for testing";
                        return false;
                    }
                }
                
                if ( host.find( ":" ) == string::npos ){
                    stringstream ss;
                    ss << host << ":" << CmdLine::ShardServerPort;
                    host = ss.str();
                }
                
                string name = "";
                if ( cmdObj["name"].type() == String )
                    name = cmdObj["name"].valuestrsafe();
                
                if ( name.size() == 0 ){
                    stringstream ss;
                    ss << "shard";
                    ss << conn->count( "config.shards" );
                    name = ss.str();
                }

                BSONObj shard;
                {
                    BSONObjBuilder b;
                    b.append( "_id" , name );
                    b.append( "host" , host );
                    if ( cmdObj["maxSize"].isNumber() )
                        b.append( cmdObj["maxSize"] );
                    shard = b.obj();
                }

                BSONObj old = conn->findOne( "config.shards" , BSON( "host" << host ) );
                if ( ! old.isEmpty() ){
                    result.append( "msg" , "host already used" );
                    conn.done();
                    return false;
                }
                
                try {
                    ShardConnection newShardConn( host );
                    newShardConn->getLastError();
                    newShardConn.done();
                }
                catch ( DBException& e ){
                    errmsg = "couldn't connect to new shard";
                    result.append( "host" , host );
                    result.append( "exception" , e.what() );
                    conn.done();
                    return false;
                }
                
                log() << "going to add shard: " << shard << endl;
                conn->insert( "config.shards" , shard );
                errmsg = conn->getLastError();
                if ( errmsg.size() ){
                    log() << "error adding shard: " << shard << " err: " << errmsg << endl;
                    return false;
                }
                result.append( "added" , shard["host"].valuestrsafe() );
                conn.done();
                return true;
            }
        } addServer;
        
        class RemoveShardCmd : public GridAdminCmd {
        public:
            RemoveShardCmd() : GridAdminCmd("removeshard") { }
            virtual void help( stringstream& help ) const {
                help << "remove a shard to the system.\nshard must be empty or command will return an error.";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                if ( 1 ){
                    errmsg = "removeshard not yet implemented";
                    return 0;
                }

                ShardConnection conn( configServer.getPrimary() );

                BSONObj server = BSON( "host" << cmdObj["removeshard"].valuestrsafe() );
                conn->remove( "config.shards" , server );

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
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("isdbgrid", 1);
                result.append("hostname", ourHostname);
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
            CmdIsMaster() : Command("ismaster") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
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
            virtual bool run(const char *dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
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
            CmdShardingGetPrevError() : Command("getpreverror") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
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
            CmdShardingGetLastError() : Command("getlasterror") { }
            virtual bool run(const char *nsraw, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                {
                    LastError * le = lastError.get();
                    assert( le );
                    if ( le->msg.size() ){
                        le->appendSelf( result );
                        return true;
                    }
                }
                
                string dbName = nsraw;
                dbName = dbName.substr( 0 , dbName.size() - 5 );
                
                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                ClientInfo * client = ClientInfo::get();
                set<string> * shards = client->getPrev();
                
                if ( shards->size() == 0 ){
                    result.appendNull( "err" );
                    return true;
                }
                
                if ( shards->size() == 1 ){
                    string theShard = *(shards->begin() );
                    result.append( "theshard" , theShard.c_str() );
                    ShardConnection conn( theShard );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    result.appendElements( res );
                    conn.done();
                    return ok;
                }
                
                vector<string> errors;
                for ( set<string>::iterator i = shards->begin(); i != shards->end(); i++ ){
                    string theShard = *i;
                    ShardConnection conn( theShard );
                    string temp = conn->getLastError();
                    if ( temp.size() )
                        errors.push_back( temp );
                    conn.done();
                }
                
                if ( errors.size() == 0 ){
                    result.appendNull( "err" );
                    return true;
                }
                
                result.append( "err" , errors[0].c_str() );
                
                BSONObjBuilder all;
                for ( unsigned i=0; i<errors.size(); i++ ){
                    all.append( all.numStr( i ).c_str() , errors[i].c_str() );
                }
                result.appendArray( "errs" , all.obj() );
                return true;
            }
        } cmdGetLastError;
        
    }
    
    class CmdListDatabases : public Command {
    public:
        CmdListDatabases() : Command("listDatabases") {}

        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool slaveOverrideOk() { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream& help ) const { help << "list databases on cluster"; }
        
        bool run(const char *ns, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            list<Shard> shards;
            Shard::getAllShards( shards );
            
            map<string,long long> sizes;
            map< string,shared_ptr<BSONObjBuilder> > dbShardInfo;

            for ( list<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ){
                Shard s = *i;
                BSONObj x = s.runCommand( "admin" , "listDatabases" );
                cout << s.toString() << "\t" << x.jsonString() << endl;
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
            
            BSONArrayBuilder bb( result.subarrayStart( "databases" ) );
            for ( map<string,long long>::iterator i=sizes.begin(); i!=sizes.end(); ++i ){
                string name = i->first;
                long long size = i->second;

                BSONObjBuilder temp;
                temp.append( "name" , name );
                temp.appendNumber( "size" , size );
                temp.appendBool( "empty" , size == 1 );
                temp.append( "shards" , dbShardInfo[name]->obj() );
                
                bb.append( temp.obj() );
            }
            bb.done();

            return 1;
        }

    } cmdListDatabases;


} // namespace mongo
