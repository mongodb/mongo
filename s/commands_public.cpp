// s/commands_public.cpp


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

#include "stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../client/parallel.h"
#include "../db/commands.h"

#include "config.h"
#include "chunk.h"
#include "strategy.h"

namespace mongo {

    namespace dbgrid_pub_cmds {
        
        class PublicGridCommand : public Command {
        public:
            PublicGridCommand( const char * n ) : Command( n ){
            }
            virtual bool slaveOk(){
                return true;
            }
            virtual bool adminOnly() {
                return false;
            }
        protected:
            string getDBName( string ns ){
                return ns.substr( 0 , ns.size() - 5 );
            } 
            
            bool passthrough( DBConfig * conf, const BSONObj& cmdObj , BSONObjBuilder& result ){
                ScopedDbConnection conn( conf->getPrimary() );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                result.appendElements( res );
                conn.done();
                return ok;
            }
        };
        
        class NotAllowedOnShardedCollectionCmd : public PublicGridCommand {
        public:
            NotAllowedOnShardedCollectionCmd( const char * n ) : PublicGridCommand( n ){}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) = 0;
            
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                string dbName = getDBName( ns );
                string fullns = getFullNS( dbName , cmdObj );
                
                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                errmsg = "can't do command: " + name + " on sharded collection";
                return false;
            }
        };
        
        // ----

        class DropCmd : public PublicGridCommand {
        public:
            DropCmd() : PublicGridCommand( "drop" ){}
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

                string dbName = getDBName( ns );
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                log() << "DROP: " << fullns << endl;
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                ChunkManager * cm = conf->getChunkManager( fullns );
                massert( "how could chunk manager be null!" , cm );
                
                cm->drop();

                return 1;
            }
        } dropCmd;

        class DropDBCmd : public PublicGridCommand {
        public:
            DropDBCmd() : PublicGridCommand( "dropDatabase" ){}
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                BSONElement e = cmdObj.firstElement();
                
                if ( ! e.isNumber() || e.number() != 1 ){
                    errmsg = "invalid params";
                    return 0;
                }
                
                string dbName = getDBName( ns );
                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                log() << "DROP DATABASE: " << dbName << endl;

                if ( ! conf || ! conf->isShardingEnabled() ){
                    log(1) << "  passing though drop database for: " << dbName << endl;
                    return passthrough( conf , cmdObj , result );
                }
                
                if ( ! conf->dropDatabase( errmsg ) )
                    return false;

                result.append( "dropped" , dbName );
                return true;
            }
        } dropDBCmd;

        class CountCmd : public PublicGridCommand {
        public:
            CountCmd() : PublicGridCommand("count") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                string dbName = getDBName( ns );
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                BSONObj filter = cmdObj["query"].embeddedObject();
                
                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    ScopedDbConnection conn( conf->getPrimary() );
                    result.append( "n" , (double)conn->count( fullns , filter ) );
                    conn.done();
                    return true;
                }
                
                ChunkManager * cm = conf->getChunkManager( fullns );
                massert( "how could chunk manager be null!" , cm );
                
                vector<Chunk*> chunks;
                cm->getChunksForQuery( chunks , filter );
                
                unsigned long long total = 0;
                for ( vector<Chunk*>::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    Chunk * c = *i;
                    total += c->countObjects( filter );
                }
                
                result.append( "n" , (double)total );
                return true;
            }
        } countCmd;

        class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped"){}
            
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ){
                return dbName + "." + cmdObj.firstElement().valuestrsafe();
            }
            
        } convertToCappedCmd;


        class GroupCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            GroupCmd() : NotAllowedOnShardedCollectionCmd("group"){}
            
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ){
                return dbName + "." + cmdObj.firstElement().embeddedObjectUserCheck()["ns"].valuestrsafe();
            }
            
        } groupCmd;

        class DistinctCmd : public PublicGridCommand {
        public:
            DistinctCmd() : PublicGridCommand("distinct"){}
            virtual void help( stringstream &help ) const {
                help << "{ distinct : 'collection name' , key : 'a.b' }";
            }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                string dbName = getDBName( ns );
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                ChunkManager * cm = conf->getChunkManager( fullns );
                massert( "how could chunk manager be null!" , cm );
                
                vector<Chunk*> chunks;
                cm->getChunksForQuery( chunks , BSONObj() );
                
                set<BSONObj,BSONObjCmp> all;
                int size = 32;
                
                for ( vector<Chunk*>::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    Chunk * c = *i;

                    ScopedDbConnection conn( c->getShard() );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    conn.done();
                    
                    if ( ! ok ){
                        result.appendElements( res );
                        return false;
                    }
                    
                    BSONObjIterator it( res["values"].embeddedObjectUserCheck() );
                    while ( it.more() ){
                        BSONElement nxt = it.next();
                        BSONObjBuilder temp(32);
                        temp.appendAs( nxt , "x" );
                        all.insert( temp.obj() );
                    }

                }
                
                BSONObjBuilder b( size );
                int n=0;
                for ( set<BSONObj,BSONObjCmp>::iterator i = all.begin() ; i != all.end(); i++ ){
                    b.appendAs( i->firstElement() , b.numStr( n++ ).c_str() );
                }
                
                result.appendArray( "values" , b.obj() );
                return true;
            }
        } disinctCmd;

        class MRCmd : public PublicGridCommand {
        public:
            MRCmd() : PublicGridCommand( "mapreduce" ){}
            
            string getTmpName( const string& coll ){
                static int inc = 1;
                stringstream ss;
                ss << "tmp.mrs." << coll << "_" << time(0) << "_" << inc++;
                return ss.str();
            }

            BSONObj fixForShards( const BSONObj& orig , const string& output ){
                BSONObjBuilder b;
                BSONObjIterator i( orig );
                while ( i.more() ){
                    BSONElement e = i.next();
                    string fn = e.fieldName();
                    if ( fn == "map" || 
                         fn == "mapreduce" || 
                         fn == "reduce" ||
                         fn == "query" ||
                         fn == "sort" ||
                         fn == "verbose" ){
                        b.append( e );
                    }
                    else if ( fn == "keeptemp" ||
                              fn == "out" ||
                              fn == "finalize" ){
                        // we don't want to copy these
                    }
                    else {
                        uassert( (string)"don't know mr field: " + fn , 0 );
                    }
                }
                b.append( "out" , output );
                return b.obj();
            }
            
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                Timer t;

                string dbName = getDBName( ns );
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfig * conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                BSONObjBuilder timingBuilder;

                ChunkManager * cm = conf->getChunkManager( fullns );

                BSONObj q;
                if ( cmdObj["query"].type() == Object ){
                    q = cmdObj["query"].embeddedObjectUserCheck();
                }
                
                vector<Chunk*> chunks;
                cm->getChunksForQuery( chunks , q );
                
                const string shardedOutputCollection = getTmpName( collection );
                
                BSONObj shardedCommand = fixForShards( cmdObj , shardedOutputCollection );
                
                BSONObjBuilder finalCmd;
                finalCmd.append( "mapreduce.shardedfinish" , cmdObj );
                finalCmd.append( "shardedOutputCollection" , shardedOutputCollection );
                
                list< shared_ptr<Future::CommandResult> > futures;
                
                for ( vector<Chunk*>::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    Chunk * c = *i;
                    futures.push_back( Future::spawnCommand( c->getShard() , dbName , shardedCommand ) );
                }
                
                BSONObjBuilder shardresults;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ){
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ){
                        errmsg = "mongod mr failed: ";
                        errmsg += res->result().toString();
                        return 0;
                    }
                    shardresults.append( res->getServer() , res->result() );
                }
                
                finalCmd.append( "shards" , shardresults.obj() );
                timingBuilder.append( "shards" , t.millis() );

                Timer t2;
                ScopedDbConnection conn( conf->getPrimary() );
                BSONObj finalResult;
                if ( ! conn->runCommand( dbName , finalCmd.obj() , finalResult ) ){
                    errmsg = "final reduce failed: ";
                    errmsg += finalResult.toString();
                    return 0;
                }
                timingBuilder.append( "final" , t2.millis() );

                result.appendElements( finalResult );
                result.append( "timeMillis" , t.millis() );
                result.append( "timing" , timingBuilder.obj() );
                
                return 1;
            }
        } mrCmd;
    }
}
