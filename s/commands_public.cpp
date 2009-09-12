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
                    result.append( "ok" , 1 );
                    return true;
                }
                
                ChunkManager * cm = conf->getChunkManager( fullns );
                massert( "how could chunk manager be null!" , cm );
                
                vector<Chunk*> chunks;
                cm->getChunksForQuery( chunks , filter );
                
                unsigned long long total = 0;
                for ( vector<Chunk*>::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    Chunk * c = *i;
                    total += c->countObjects();
                }
                
                result.append( "n" , (double)total );
                result.append( "ok" , 1 );
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
                result.append( "ok" , 1 );
                return true;
            }
        } disinctCmd;
    }
}
