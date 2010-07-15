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
#include "../client/distlock.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"
#include "chunk.h"

using namespace std;

namespace mongo {

    /**
     * this is the main entry for moveChunk
     * called to initial a move
     * usually by a mongos
     * this is called on the "from" side
     */
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
            BSONObj min  = cmdObj["min"].Obj();
            BSONObj max  = cmdObj["max"].Obj();
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
            
            if ( min.isEmpty() ){
                errmsg = "need to specify a min";
                return false;
            }

            if ( max.isEmpty() ){
                errmsg = "need to specify a max";
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
            
            DistributedLock lockSetup( ConnectionString( shardingState.getConfigServer() , ConnectionString::SYNC ) , ns );
            dist_lock_try dlk( &lockSetup , (string)"migrate-" + min.toString() );
            if ( ! dlk.got() ){
                errmsg = "someone else has the lock";
                result.append( "who" , dlk.other() );
                return false;
            }

            ShardChunkVersion maxVersion;
            string myOldShard;
            {
                ScopedDbConnection conn( shardingState.getConfigServer() );
                
                BSONObj x = conn->findOne( ShardNS::chunk , Query( BSON( "ns" << ns ) ).sort( BSON( "lastmod" << -1 ) ) );
                maxVersion = x["lastmod"];

                x = conn->findOne( ShardNS::chunk , shardId.wrap( "_id" ) );
                myOldShard = x["shard"].String();
                
                if ( myOldShard != fromShard.getName() ){
                    errmsg = "i'm out of date";
                    result.append( "from" , fromShard.getName() );
                    result.append( "official" , myOldShard );
                    return false;
                }
                
                if ( maxVersion < shardingState.getVersion( ns ) ){
                    errmsg = "official version less than mine?";;
                    result.appendTimestamp( "officialVersion" , maxVersion );
                    result.appendTimestamp( "myVersion" , shardingState.getVersion( ns ) );
                    return false;
                }

                conn.done();
            }
            
            // 3.
            {
                
                ScopedDbConnection conn( to );
                BSONObj res;
                bool ok = conn->runCommand( "admin" , 
                                            BSON( "_recvChunkStart" << ns <<
                                                  "from" << from <<
                                                  "min" << min <<
                                                  "max" << max
                                                  ) , 
                                            res );
                conn.done();

                if ( ! ok ){
                    errmsg = "_recvChunkStart failed: ";
                    errmsg += res["errmsg"].String();
                    result.append( "cause" , res );
                    return false;
                }

            }
            
            // 4. 
            for ( int i=0; i<86400; i++ ){ // don't want a single chunk move to take more than a day
                sleepsecs( 1 ); 
                ScopedDbConnection conn( to );
                BSONObj res;
                conn->runCommand( "admin" , BSON( "_recvChunkStatus" << 1 ) , res );
                res = res.getOwned();
                conn.done();
                
                log(0) << "_recvChunkStatus : " << res << endl;
                
                if ( res["state"].String() == "steady" )
                    break;
            }
            
            // 5.
            { 
                // 5.a
                ShardChunkVersion myVersion = maxVersion;
                ++myVersion;
                
                {
                    dblock lk;
                    assert( myVersion > shardingState.getVersion( ns ) );
                    shardingState.setVersion( ns , myVersion );
                    assert( myVersion == shardingState.getVersion( ns ) );
                }

                
                // 5.b
                {
                    BSONObj res;
                    ScopedDbConnection conn( to );
                    bool ok = conn->runCommand( "admin" , 
                                                BSON( "_recvChunkCommit" << 1 ) ,
                                                res );
                    conn.done();

                    if ( ! ok ){
                        log() << "_recvChunkCommit failed: " << res << endl;
                        errmsg = "_recvChunkCommit failed!";
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
                configServer.logChange( "moveChunk" , ns , BSON( "min" << min << "max" << max <<
                                                                 "from" << fromShard.getName() << 
                                                                 "to" << toShard.getName() ) );
            }
            
            
            // 6. 
            log( LL_WARNING ) << " deleting data before ensuring no more cursors TODO" << endl;
            
            // 7
            {
                writelock lk(ns);
                long long num = Helpers::removeRange( ns , min , max , true );
                log() << "moveChunk deleted: " << num << endl;
                result.appendNumber( "numDeleted" , num );
            }

            return true;
            
        }
        
    } moveChunkCmd;

    /* -----
       below this are the "to" side commands
       
       command to initiate
       worker thread
         does initial clone
         pulls initial change set
         keeps pulling
         keeps state
       command to get state
       commend to "commit"
    */

    class MigrateStatus {
    public:
        
        MigrateStatus(){
            active = false;
        }

        void prepare(){
            assert( ! active );
            state = READY;
            errmsg = "";

            numCloned = 0;
            numCatchup = 0;
            numSteady = 0;

            active = true;
        }

        void go(){
            try {
                _go();
            }
            catch ( std::exception& e ){
                state = ERROR;
                errmsg = e.what();
            }
            catch ( ... ){
                state = ERROR;
                errmsg = "UNKNOWN ERROR";
            }
        }

        void _go(){
            assert( active );
            assert( state == READY );
            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );

            ScopedDbConnection conn( from );
            conn->getLastError(); // just test connection
            
            state = CLONE;
            {
                auto_ptr<DBClientCursor> cursor = conn->query( ns , Query().minKey( min ).maxKey( max ) , /* QueryOption_Exhaust */ 0 );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();
                    {
                        writelock lk( ns );
                        Helpers::upsert( ns , o );
                    }
                    numCloned++;
                }
            }
            
            // TODO: indexes
            
            state = CATCHUP;
            // TODO

            state = STEADY;
            while ( state == STEADY || state == COMMIT_START ){
                // TODO

                if ( state == COMMIT_START )
                    break;
            }

            state = DONE;
            conn.done();

            active = false;
        }

        void status( BSONObjBuilder& b ){
            b.appendBool( "active" , active );
            if ( ! active )
                return;

            b.append( "ns" , ns );
            b.append( "from" , from );
            b.append( "min" , min );
            b.append( "max" , max );

            b.append( "state" , stateString() );

            {
                BSONObjBuilder bb( b.subobjStart( "counts" ) );
                bb.append( "cloned" , numCloned );
                bb.append( "catchup" , numCatchup );
                bb.append( "steady" , numSteady );
                bb.done();
            }


        }
        
        string stateString(){
            switch ( state ){
            case READY: return "ready";
            case CLONE: return "clone";
            case CATCHUP: return "catchup";
            case STEADY: return "steady";
            case COMMIT_START: return "commitStart";
            case DONE: return "done";
            case ERROR: return "error";
            }
            assert(0);
            return "";
        }

        bool startCommit(){
            if ( state != STEADY )
                return false;
            state = COMMIT_START;
            return true;
        }

        bool active;
        
        string ns;
        string from;
        
        BSONObj min;
        BSONObj max;
        
        long long numCloned;
        long long numCatchup;
        long long numSteady;

        enum State { READY , CLONE , CATCHUP , STEADY , COMMIT_START , DONE , ERROR } state;
        string errmsg;
        
    } migrateStatus;
    
    void migrateThread(){
        Client::initThread( "migrateThread" );
        migrateStatus.go();
        cc().shutdown();
    }

    class RecvChunkCommandHelper : public Command {
    public:
        RecvChunkCommandHelper( const char * name ) 
            : Command( name ){
        }
        
        virtual void help( stringstream& help ) const {
            help << "internal should not be calling this directly" << endl;
        }
        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 

    };
    
    class RecvChunkStartCommand : public RecvChunkCommandHelper {
    public:
        RecvChunkStartCommand() : RecvChunkCommandHelper( "_recvChunkStart" ){}

        virtual LockType locktype() const { return WRITE; }  // this is so don't have to do locking internally

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

            if ( migrateStatus.active ){
                errmsg = "migrate already in progress";
                return false;
            }

            migrateStatus.prepare();

            migrateStatus.ns = cmdObj.firstElement().String();
            migrateStatus.from = cmdObj["from"].String();
            migrateStatus.min = cmdObj["min"].Obj().getOwned();
            migrateStatus.max = cmdObj["max"].Obj().getOwned();
            
            // TODO: check data in range currently

            boost::thread m( migrateThread );
            
            result.appendBool( "started" , true );
            return true;
        }

    } recvChunkStartCmd;

    class RecvChunkStatusCommand : public RecvChunkCommandHelper {
    public:
        RecvChunkStatusCommand() : RecvChunkCommandHelper( "_recvChunkStatus" ){}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            migrateStatus.status( result );
            return 1;
        }
        
    } recvChunkStatusCommand;

    class RecvChunkCommitCommand : public RecvChunkCommandHelper {
    public:
        RecvChunkCommitCommand() : RecvChunkCommandHelper( "_recvChunkCommit" ){}
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            bool ok = migrateStatus.startCommit();
            migrateStatus.status( result );
            return ok;
        }

    } recvChunkCommitCommand;

    
}
