// d_migrate.cpp

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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"
#include "chunk.h"

using namespace std;

namespace mongo {

    class MoveChunkCommand : public Command {
    public:
        MoveChunkCommand() : Command( "moveChunk" ){}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // 1. parse options
            // 2. make sure my view is complete and lock
            // 3. start migrate
            // 4. pause till migrate caught up
            // 5. LOCK
            //    a) update my config, essentially locking
            //    b) finish migrate
            //    c) update config server
            //    d) logChange to config server
            // 6. wait for all current cursors to expire
            // 7. remove data locally
            
            // -------------------------------
            
            // 1.
            string ns = cmdObj.firstElement().String();
            string to = cmdObj["to"].String();
            string from = cmdObj["from"].String(); // my public address, a tad redundant, but safe
            BSONObj filter = cmdObj["filter"].Obj();
            BSONElement shardId = cmdObj["shardId"];
            
            if ( ns.empty() ){
                errmsg = "need to specify namespace in command";
                return false;
            }
            
            if ( to.empty() ){
                errmsg = "need to specify server to move shard to";
                return false;
            }
            if ( from.empty() ){
                errmsg = "need to specify server to move shard from (redundat i know)";
                return false;
            }
            
            if ( filter.isEmpty() ){
                errmsg = "need to specify a filter";
                return false;
            }
            
            if ( shardId.eoo() ){
                errmsg = "need shardId";
                return false;
            }
            
            if ( ! shardingState.enabled() ){
                if ( cmdObj["configdb"].type() != String ){
                    errmsg = "sharding not enabled";
                    return false;
                }
                string configdb = cmdObj["configdb"].String();
                shardingState.enable( configdb );
                configServer.init( configdb );
            }



            Shard fromShard( from );
            Shard toShard( to );
            
            log() << "got movechunk: " << cmdObj << endl;
        
            // 2. TODO
            
            ShardChunkVersion maxVersion;
            string myOldShard;
            {
                ScopedDbConnection conn( shardingState.getConfigServer() );

                BSONObj x = conn->findOne( ShardNS::chunk , Query( BSON( "ns" << ns ) ).sort( BSON( "lastmod" << -1 ) ) );
                maxVersion = x["lastmod"];

                x = conn->findOne( ShardNS::chunk , shardId.wrap( "_id" ) );
                myOldShard = x["shard"].String();
                conn.done();
            }

            // 3.
            BSONObj startRes;
            {
                
                ScopedDbConnection conn( to );
                bool ok = conn->runCommand( "admin" , 
                                            BSON( "startCloneCollection" << ns <<
                                                  "from" << from <<
                                                  "query" << filter 
                                                  ) , 
                                            startRes );
                conn.done();

                if ( ! ok ){
                    errmsg = "startCloneCollection failed: ";
                    errmsg += startRes["errmsg"].String();
                    result.append( "cause" , startRes );
                    return false;
                }

            }
            
            // 4. 
            sleepsecs( 2 ); // TODO: this is temp
            
            // 5.
            { 
                // 5.a
                ShardChunkVersion myVersion = maxVersion;
                ++myVersion;
                
                {
                    dblock lk;
                    shardingState.setVersion( ns , myVersion );
                    assert( myVersion == shardingState.getVersion( ns ) );
                }

                BSONObj res;
                
                
                // 5.b
                {
                    ScopedDbConnection conn( to );
                    bool ok = conn->runCommand( "admin" , 
                                                startRes["finishToken"].wrap( "finishCloneCollection" ) ,
                                                res );
                    conn.done();

                    if ( ! ok ){
                        errmsg = "finishCloneCollection failed!";
                        result.append( "cause" , res );
                        return false;
                    }
                }
                
                // 5.c
                ScopedDbConnection conn( shardingState.getConfigServer() );
                
                BSONObjBuilder temp;
                temp.append( "shard" , toShard.getName() );
                temp.appendTimestamp( "lastmod" , myVersion );

                conn->update( ShardNS::chunk , shardId.wrap( "_id" ) , BSON( "$set" << temp.obj() ) );
                
                { 
                    // update another random chunk
                    BSONObj x = conn->findOne( ShardNS::chunk , Query( BSON( "ns" << ns << "shard" << myOldShard ) ).sort( BSON( "lastmod" << -1 ) ) );
                    if ( ! x.isEmpty() ){
                        
                        BSONObjBuilder temp2;
                        ++myVersion;
                        temp2.appendTimestamp( "lastmod" , myVersion );
                        
                        shardingState.setVersion( ns , myVersion );

                        conn->update( ShardNS::chunk , x["_id"].wrap() , BSON( "$set" << temp2.obj() ) );
                        
                    }
                    else {
                        //++myVersion;
                        shardingState.setVersion( ns , 0 );
                    }
                }

                conn.done();
                
                // 5.d
                configServer.logChange( "moveChunk" , ns , BSON( "range" << filter << "from" << fromShard.getName() << "to" << toShard.getName() ) );
            }
            
            
            // 6. 
            log( LL_WARNING ) << " deleting data before ensuring no more cursors TODO" << endl;
            
            // 7
            {
                writelock lk(ns);
                Client::Context ctx(ns);
                long long num = deleteObjects( ns.c_str() , filter , false , true );
                log() << "moveChunk deleted: " << num << endl;
                result.appendNumber( "numDeleted" , num );
            }

            return true;
            
        }
        
    } moveChunkCmd;

}
