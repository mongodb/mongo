// dbgrid/request.cpp

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
#include "shard.h"
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

                if ( ! grid.knowAboutServer( to ) ){
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
        
        class PartitionCmd : public GridAdminCmd {
        public:
            PartitionCmd() : GridAdminCmd( "partition" ){}
            virtual void help( stringstream& help ) const {
                help 
                    << "turns on partitioning for a db.  have to do this before sharding, etc.. will work.\n"
                    << "  { partition : \"alleyinsider\" }\n";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string dbname = cmdObj["partition"].valuestrsafe();
                if ( dbname.size() == 0 ){
                    errmsg = "no db";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( dbname );
                if ( config->isPartitioned() ){
                    errmsg = "already partitioned";
                    return false;
                }

                config->turnOnPartitioning();
                config->save( true );

                result << "ok" << 1;
                return true;
            }
        } partitionCmd;

        // ------------ collection level commands -------------
        
        class ShardCmd : public GridAdminCmd {
        public:
            ShardCmd() : GridAdminCmd( "shard" ){}
            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj["shard"].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( ns );
                if ( ! config->isPartitioned() ){
                    errmsg = "db not partitioned ";
                    return false;
                }
                
                if ( config->sharded( ns ) ){
                    errmsg = "already sharded";
                    return false;
                }
                
                BSONObj key = cmdObj.getObjectField( "key" );
                if ( key.isEmpty() ){
                    errmsg = "no shard key";
                    return false;
                }
                
                config->turnOnSharding( ns , key );
                config->save( true );

                result << "ok" << 1;
                return true;
            }            
        } shardCmd;
            
        
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
            
            virtual bool _split( BSONObjBuilder& result , string&errmsg , const string& ns , ShardManager * manager , Shard& old , BSONObj middle ) = 0;
            
            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj[_name.c_str()].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( ns );
                if ( ! config->sharded( ns ) ){
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
                
                ShardManager * info = config->getShardManager( ns );
                Shard& old = info->findShard( find );
                

                return _split( result , errmsg , ns , info , old , cmdObj.getObjectField( "middle" ) );
            }

        protected:
            string _name;
        };

        class SplitValueCommand : public SplitCollectionHelper {
        public:
            SplitValueCommand() : SplitCollectionHelper( "splitvalue" ){}
            virtual bool _split( BSONObjBuilder& result , string& errmsg , const string& ns , ShardManager * manager , Shard& old , BSONObj middle ){
                
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
            virtual bool _split( BSONObjBuilder& result , string& errmsg , const string& ns , ShardManager * manager , Shard& old , BSONObj middle ){
                
                log() << "splitting: " << ns << "  shard: " << old << endl;
                
                unsigned long long nextTS = grid.getNextOpTime();
                ScopedDbConnection conn( old.getServer() );
                BSONObj lockResult;
                if ( ! setShardVersion( conn.conn() , ns , nextTS , true , lockResult ) ){
                    log() << "setShardVersion for split failed!" << endl;
                    errmsg = "setShardVersion failed to lock server.  is someone else doing something?";
                    return false;
                }
                conn.done();

                if ( middle.isEmpty() )
                    old.split();
                else
                    old.split( middle );
                
                manager->save();
                
                result << "ok" << 1;
                return true;
            }            
            

        } splitCollectionCmd;

        class MoveShard : public GridAdminCmd {
        public:
            MoveShard() : GridAdminCmd( "moveshard" ){}
            virtual void help( stringstream& help ) const {
                help << "{ moveshard : 'test.foo' , find : { num : 1 } , to : 'localhost:30001' }";
            }
            bool run(const char *cmdns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string ns = cmdObj["moveshard"].valuestrsafe();
                if ( ns.size() == 0 ){
                    errmsg = "no ns";
                    return false;
                }

                DBConfig * config = grid.getDBConfig( ns );
                if ( ! config->sharded( ns ) ){
                    errmsg = "ns not sharded.  have to split before can move a shard";
                    return false;
                }
                
                BSONObj find = cmdObj.getObjectField( "find" );
                if ( find.isEmpty() ){
                    errmsg = "need to specify find.  see help";
                    return false;
                }

                string to = cmdObj["to"].valuestrsafe();
                if ( ! to.size()  ){
                    errmsg = "you have to specify where you want to move it";
                    return false;
                }
                
                ShardManager * info = config->getShardManager( ns );
                Shard& s = info->findShard( find );                
                string from = s.getServer();

                if ( s.getServer() == to ){
                    errmsg = "that shard is already on that server";
                    return false;
                }

                if ( ! grid.knowAboutServer( to ) ){
                    errmsg = "that server isn't known to me";
                    return false;
                }
                
                log() << "ns: " << ns << " moving shard: " << s << " to: " << to << endl;
                
                // copyCollection
                ScopedDbConnection toconn( to );
                BSONObj cloneRes;

                
                BSONObj filter;
                {
                    BSONObjBuilder b;
                    s.getFilter( b );
                    filter = b.obj();
                }

                bool worked = toconn->runCommand( config->getName().c_str() , 
                                                  BSON( "cloneCollection" << ns << 
                                                        "from" << from <<
                                                        "query" << filter
                                                        ) ,
                                                  cloneRes
                                                  );
                
                toconn.done();
                if ( ! worked ){
                    errmsg = (string)"cloneCollection failed: " + cloneRes.toString();
                    return false;
                }
                
                // update config db
                s.setServer( to );
                
                // need to increment version # for old server
                Shard * randomShardOnOldServer = info->findShardOnServer( from );
                if ( randomShardOnOldServer )
                    randomShardOnOldServer->_markModified();

                info->save();

                // delete old data
                ScopedDbConnection fromconn( from );
                fromconn->remove( ns.c_str() , filter );
                string removeerror = fromconn->getLastError();
                fromconn.done();
                if ( removeerror.size() ){
                    errmsg = (string)"error removing old data:" + removeerror;
                    return false;
                }

                
                result << "ok" << 1;
                return true;
            }
        } moveShardCmd;

        // ------------ server level commands -------------
        
        class ListServers : public GridAdminCmd {
        public:
            ListServers() : GridAdminCmd("listservers") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( "config.servers" , BSONObj() );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();
                    all.push_back( o );
                }
                
                result.append("servers" , all );
                result.append("ok" , 1 );
                conn.done();

                return true;
            }
        } listServers;
        
        
        class AddServer : public GridAdminCmd {
        public:
            AddServer() : GridAdminCmd("addserver") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );
                
                BSONObj server = BSON( "host" << cmdObj["addserver"].valuestrsafe() );
                
                BSONObj old = conn->findOne( "config.servers" , server );
                if ( ! old.isEmpty() ){
                    result.append( "ok" , 0.0 );
                    result.append( "msg" , "already exists" );
                    conn.done();
                    return false;
                }
                
                conn->insert( "config.servers" , server );
                result.append( "ok", 1 );
                result.append( "added" , server["host"].valuestrsafe() );
                conn.done();
                return true;
            }
        } addServer;
        
        class RemoveServer : public GridAdminCmd {
        public:
            RemoveServer() : GridAdminCmd("removeserver") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );
                
                BSONObj server = BSON( "host" << cmdObj["removeserver"].valuestrsafe() );
                conn->remove( "config.servers" , server );
                
                conn.done();
                return true;
            }
        } removeServer;

        
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
                return true;
            }
        } isdbgrid;
        
        class CmdIsMaster : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            CmdIsMaster() : Command("ismaster") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("ismaster", 0.0);
                result.append("msg", "isdbgrid");
                return true;
            }
        } ismaster;
        
        class CmdShardGetPrevError : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            CmdShardGetPrevError() : Command("getpreverror") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg += "getpreverror not supported on mongos";
                result << "ok" << 0;
                return false;
            }
        } cmdGetPrevError;
        
        class CmdShardGetLastError : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            CmdShardGetLastError() : Command("getplasterror") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg += "getlasterror not working yet";
                result << "ok" << 0;
                return false;
            }
        } cmdGetLastError;
        
    }

} // namespace mongo
