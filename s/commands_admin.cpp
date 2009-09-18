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

#include "stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../db/commands.h"

#include "config.h"
#include "chunk.h"
#include "strategy.h"

namespace mongo {

    extern string ourHostname;

    namespace dbgrid_cmds {

        set<string> dbgridCommands;

        class GridAdminCmd : public Command {
        public:
            GridAdminCmd( const char * n ) : Command( n ){
                dbgridCommands.insert( n );
            }
            virtual bool slaveOk(){
                return true;
            }
            virtual bool adminOnly() {
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
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                result.append("configserver", configServer.getPrimary() );
                result.append("isdbgrid", 1);
                return true;
            }
        } netstat;

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
                result.append("ok" , 1 );
                return true;
            }
        } listGridCommands;

        // ------------ database level commands -------------

        class ListDatabaseCommand : public GridAdminCmd {
        public:
            ListDatabaseCommand() : GridAdminCmd("listdatabases") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );

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

                if ( to == config->getPrimary() ){
                    errmsg = "thats already the primary";
                    return false;
                }

                if ( ! grid.knowAboutShard( to ) ){
                    errmsg = "that server isn't known to me";
                    return false;
                }

                ScopedDbConnection conn( configServer.getPrimary() );

                log() << "moving " << dbname << " primary from: " << config->getPrimary() << " to: " << to << endl;

                // TODO LOCKING: this is not safe with multiple mongos


                ScopedDbConnection toconn( to );

                // TODO AARON - we need a clone command which replays operations from clone start to now
                //              using a seperate smaller oplog
                BSONObj cloneRes;
                bool worked = toconn->runCommand( dbname.c_str() , BSON( "clone" << config->getPrimary() ) , cloneRes );
                toconn.done();
                if ( ! worked ){
                    log() << "clone failed" << cloneRes << endl;
                    errmsg = "clone failed";
                    conn.done();
                    return false;
                }

                ScopedDbConnection fromconn( config->getPrimary() );

                config->setPrimary( to );
                config->save( true );

                log() << " dropping " << dbname << " from old" << endl;

                fromconn->dropDatabase( dbname.c_str() );
                fromconn.done();

                result << "ok" << 1;
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

                config->enableSharding();
                config->save( true );

                result << "ok" << 1;
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
                }

                if ( ns.find( ".system." ) != string::npos ){
                    errmsg = "can't shard system namespaces";
                    return false;
                }
                
                {
                    ScopedDbConnection conn( config->getPrimary() );
                    BSONObjBuilder b; 
                    b.append( "ns" , ns ); 
                    b.appendBool( "unique" , true ); 
                    if ( conn->count( config->getName() + ".system.indexes" , b.obj() ) ){
                        errmsg = "can't shard collection with unique indexes";
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

                config->shardCollection( ns , key , cmdObj["unique"].trueValue() );
                config->save( true );

                result << "collectionsharded" << ns;
                result << "ok" << 1;
                return true;
            }
        } shardCollectionCmd;


        class SplitCollectionHelper : public GridAdminCmd {
        public:
            SplitCollectionHelper( const char * name ) : GridAdminCmd( name ) , _name( name ){}
            virtual void help( stringstream& help ) const {
                help
                    << " example: { shard : 'alleyinsider.blog.posts' , find : { ts : 1 } } - split the shard that contains give key \n"
                    << " example: { shard : 'alleyinsider.blog.posts' , middle : { ts : 1 } } - split the shard that contains the key with this as the middle \n"
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
                bool middle = false;
                if ( find.isEmpty() ){
                    find = cmdObj.getObjectField( "middle" );
                    middle = true;
                }

                if ( find.isEmpty() ){
                    errmsg = "need to specify find or middle";
                    return false;
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

                result << "ok" << 1;
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

                result << "ok" << 1;
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

                string to = cmdObj["to"].valuestrsafe();
                if ( ! to.size()  ){
                    errmsg = "you have to specify where you want to move the chunk";
                    return false;
                }

                ChunkManager * info = config->getChunkManager( ns );
                Chunk& c = info->findChunk( find );
                string from = c.getShard();

                if ( from == to ){
                    errmsg = "that chunk is already on that shard";
                    return false;
                }

                if ( ! grid.knowAboutShard( to ) ){
                    errmsg = "that shard isn't known to me";
                    return false;
                }

                if ( ! c.moveAndCommit( to , errmsg ) )
                    return false;

                result << "ok" << 1;
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
                ScopedDbConnection conn( configServer.getPrimary() );

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( "config.shards" , BSONObj() );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();
                    all.push_back( o );
                }

                result.append("shards" , all );
                result.append("ok" , 1 );
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
                ScopedDbConnection conn( configServer.getPrimary() );

                BSONObj shard;
                {
                    BSONObjBuilder b;
                    b.append( "host" , cmdObj["addshard"].valuestrsafe() );
                    if ( cmdObj["maxSize"].isNumber() )
                        b.append( cmdObj["maxSize"] );
                    shard = b.obj();
                }

                BSONObj old = conn->findOne( "config.shards" , shard );
                if ( ! old.isEmpty() ){
                    result.append( "ok" , 0.0 );
                    result.append( "msg" , "already exists" );
                    conn.done();
                    return false;
                }

                try {
                    ScopedDbConnection newShardConn( shard["host"].valuestrsafe() );
                    newShardConn->getLastError();
                    newShardConn.done();
                }
                catch ( DBException& e ){
                    errmsg = "couldn't connect to new shard";
                    result.append( "host" , shard["host"].valuestrsafe() );
                    result.append( "exception" , e.what() );
                    result.append( "ok" , 0 );
                    conn.done();
                    return false;
                }

                conn->insert( "config.shards" , shard );
                result.append( "ok", 1 );
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

                ScopedDbConnection conn( configServer.getPrimary() );

                BSONObj server = BSON( "host" << cmdObj["removeshard"].valuestrsafe() );
                conn->remove( "config.shards" , server );

                conn.done();
                return true;
            }
        } removeShardCmd;


        // --------------- public commands ----------------

        class IsDbGridCmd : public Command {
        public:
            virtual bool slaveOk() {
                return true;
            }
            IsDbGridCmd() : Command("isdbgrid") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("isdbgrid", 1);
                result.append("hostname", ourHostname);
                result << "ok" << 1;
                return true;
            }
        } isdbgrid;

        class CmdIsMaster : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "test if this is master half of a replica pair";
            }
            CmdIsMaster() : Command("ismaster") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("ismaster", 1.0 );
                result.append("msg", "isdbgrid");
                result.append("ok" , 1 );
                return true;
            }
        } ismaster;

        class CmdShardingGetPrevError : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "get previous error (since last reseterror command)";
            }
            CmdShardingGetPrevError() : Command("getpreverror") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg += "getpreverror not supported for sharded environments";
                result << "ok" << 0;
                return false;
            }
        } cmdGetPrevError;

        class CmdShardingGetLastError : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            virtual void help( stringstream& help ) const {
                help << "check for an error on the last command executed";
            }
            CmdShardingGetLastError() : Command("getlasterror") { }
            virtual bool run(const char *nsraw, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                string dbName = nsraw;
                dbName = dbName.substr( 0 , dbName.size() - 5 );
                
                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                ClientInfo * client = ClientInfo::get();
                set<string> * shards = client->getPrev();
                
                if ( shards->size() == 0 ){
                    result.appendNull( "err" );
                    result.append( "ok" , 1 );
                    return true;
                }
                
                if ( shards->size() == 1 ){
                    string theShard = *(shards->begin() );
                    result.append( "theshard" , theShard.c_str() );
                    ScopedDbConnection conn( theShard );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    result.appendElements( res );
                    conn.done();
                    return ok;
                }
                
                vector<string> errors;
                for ( set<string>::iterator i = shards->begin(); i != shards->end(); i++ ){
                    string theShard = *i;
                    ScopedDbConnection conn( theShard );
                    string temp = conn->getLastError();
                    if ( temp.size() )
                        errors.push_back( temp );
                    conn.done();
                }
                
                if ( errors.size() == 0 ){
                    result.appendNull( "err" );
                    result.append( "ok" , 1 );
                    return true;
                }
                
                result.append( "err" , errors[0].c_str() );
                
                BSONObjBuilder all;
                for ( unsigned i=0; i<errors.size(); i++ ){
                    all.append( all.numStr( i ).c_str() , errors[i].c_str() );
                }
                result.appendArray( "errs" , all.obj() );
                result.append( "ok" , 1 );
                return true;
            }
        } cmdGetLastError;
        
    }

} // namespace mongo
