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
#include "../db/cmdline.h"
#include "../db/queryoptimizer.h"
#include "../db/btree.h"

#include "../client/connpool.h"
#include "../client/distlock.h"

#include "../util/queue.h"
#include "../util/unittest.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"
#include "chunk.h"

using namespace std;

namespace mongo {

    class MoveTimingHelper {
    public:
        MoveTimingHelper( const string& where , const string& ns )
            : _where( where ) , _ns( ns ){
            _next = 1;
            _nextNote = 0;
        }

        ~MoveTimingHelper(){
            configServer.logChange( (string)"moveChunk." + _where , _ns, _b.obj() );
        }
        
        void done( int step ){
            assert( step == _next++ );
            
            stringstream ss;
            ss << "step" << step;
            string s = ss.str();
            
            CurOp * op = cc().curop();
            if ( op )
                op->setMessage( s.c_str() );
            else 
                log( LL_WARNING ) << "op is null in MoveTimingHelper::done" << endl;
            
            _b.appendNumber( s , _t.millis() );
            _t.reset();
        }
        
        
        void note( const string& s ){
            string field = "note";
            if ( _nextNote > 0 ){
                StringBuilder buf;
                buf << "note" << _nextNote;
                field = buf.str();
            }
            _nextNote++;
            
            _b.append( field , s );
        }

    private:
        Timer _t;

        string _where;
        string _ns;
        
        int _next;
        int _nextNote;
        
        BSONObjBuilder _b;

    };

    struct OldDataCleanup {
        string ns;
        BSONObj min;
        BSONObj max;
        set<CursorId> initial;
        void doRemove(){
            ShardForceVersionOkModeBlock sf;
            writelock lk(ns);
            RemoveSaver rs("moveChunk",ns,"post-cleanup");
            long long num = Helpers::removeRange( ns , min , max , true , false , cmdLine.moveParanoia ? &rs : 0 );
            log() << "moveChunk deleted: " << num << endl;
        }
    };
    
    void _cleanupOldData( OldDataCleanup cleanup ){
        Client::initThread( "cleanupOldData");
        log() << " (start) waiting to cleanup " << cleanup.ns << " from " << cleanup.min << " -> " << cleanup.max << "  # cursors:" << cleanup.initial.size() << endl;

        int loops = 0;
        Timer t;
        while ( t.seconds() < 900 ){ // 15 minutes
            assert( dbMutex.getState() == 0 );
            sleepmillis( 20 );
            
            set<CursorId> now;
            ClientCursor::find( cleanup.ns , now );            
            
            set<CursorId> left;
            for ( set<CursorId>::iterator i=cleanup.initial.begin(); i!=cleanup.initial.end(); ++i ){
                CursorId id = *i;
                if ( now.count(id) )
                    left.insert( id );
            }
            
            if ( left.size() == 0 )
                break;
            cleanup.initial = left;
            
            if ( ( loops++ % 200 ) == 0 ){
                log() << " (looping " << loops << ") waiting to cleanup " << cleanup.ns << " from " << cleanup.min << " -> " << cleanup.max << "  # cursors:" << cleanup.initial.size() << endl;
                
                stringstream ss;
                for ( set<CursorId>::iterator i=cleanup.initial.begin(); i!=cleanup.initial.end(); ++i ){
                    CursorId id = *i;
                    ss << id << " ";
                }
                log() << " cursors: " << ss.str() << endl;
            }
        }
        
        cleanup.doRemove();

        cc().shutdown();
    }

    void cleanupOldData( OldDataCleanup cleanup ){
        try {
            _cleanupOldData( cleanup );
        }
        catch ( std::exception& e ){
            log() << " error cleaning old data:" << e.what() << endl;
        }
        catch ( ... ){
            log() << " unknown error cleaning old data" << endl;
        }
    }

    class ChunkCommandHelper : public Command {
    public:
        ChunkCommandHelper( const char * name ) 
            : Command( name ){
        }
        
        virtual void help( stringstream& help ) const {
            help << "internal - should not be called directly" << endl;
        }
        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 

    };

    bool isInRange( const BSONObj& obj , const BSONObj& min , const BSONObj& max ){
        BSONObj k = obj.extractFields( min, true );

        return k.woCompare( min ) >= 0 && k.woCompare( max ) < 0;
    }


    class MigrateFromStatus {
    public:
        
        MigrateFromStatus(){
            _active = false;
            _inCriticalSection = false;
            _memoryUsed = 0;
        }

        void start( string ns , const BSONObj& min , const BSONObj& max ){
            assert( ! _active );
            
            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );
            assert( ns.size() );
            
            _ns = ns;
            _min = min;
            _max = max;
            
            assert( _deleted.size() == 0 );
            assert( _reload.size() == 0 );
            assert( _memoryUsed == 0 );

            _active = true;
        }
        
        void done(){
            _deleted.clear();
            _reload.clear();
            _memoryUsed = 0;
            
            _active = false;
            _inCriticalSection = false;
        }
        
        void logOp( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt ){
            if ( ! _active )
                return;

            if ( _ns != ns )
                return;
            
            char op = opstr[0];
            if ( op == 'n' || op =='c' || ( op == 'd' && opstr[1] == 'b' ) )
                return;

            BSONElement ide;
            if ( patt )
                ide = patt->getField( "_id" );
            else 
                ide = obj["_id"];
            
            if ( ide.eoo() ){
                log( LL_WARNING ) << "logOpForSharding got mod with no _id, ignoring  obj: " << obj << endl;
                return;
            }
            
            BSONObj it;

            switch ( opstr[0] ){
                
            case 'd': {
                // can't filter deletes :(
                _deleted.push_back( ide.wrap() );
                _memoryUsed += ide.size() + 5;
                return;
            }
                
            case 'i': 
                it = obj;
                break;
                
            case 'u': 
                if ( ! Helpers::findById( cc() , _ns.c_str() , ide.wrap() , it ) ){
                    log( LL_WARNING ) << "logOpForSharding couldn't find: " << ide << " even though should have" << endl;
                    return;
                }
                break;
                
            }
            
            if ( ! isInRange( it , _min , _max ) )
                return;
            
            _reload.push_back( ide.wrap() );
            _memoryUsed += ide.size() + 5;
        }

        void xfer( list<BSONObj> * l , BSONObjBuilder& b , const char * name , long long& size , bool explode ){
            static long long maxSize = 1024 * 1024;
            
            if ( l->size() == 0 || size > maxSize )
                return;
            
            BSONArrayBuilder arr(b.subarrayStart(name));
            
            list<BSONObj>::iterator i = l->begin(); 
            
            while ( i != l->end() && size < maxSize ){
                BSONObj t = *i;
                if ( explode ){
                    BSONObj it;
                    if ( Helpers::findById( cc() , _ns.c_str() , t, it ) ){
                        arr.append( it );
                        size += it.objsize();
                    }
                }
                else {
                    arr.append( t );
                }
                i = l->erase( i );
                size += t.objsize();
            }
            
            arr.done();
        }

        /**
         * called from the dest of a migrate
         * transfers mods from src to dest
         */
        bool transferMods( string& errmsg , BSONObjBuilder& b ){
            if ( ! _active ){
                errmsg = "no active migration!";
                return false;
            }

            long long size = 0;

            {
                readlock rl( _ns );
                Client::Context cx( _ns );

                xfer( &_deleted , b , "deleted" , size , false );
                xfer( &_reload , b , "reload" , size , true );
            }

            b.append( "size" , size );

            return true;
        }

        long long mbUsed() const { return _memoryUsed / ( 1024 * 1024 ); }

        bool _inCriticalSection;

    private:
        
        bool _active;

        string _ns;
        BSONObj _min;
        BSONObj _max;

        list<BSONObj> _reload;
        list<BSONObj> _deleted;
        long long _memoryUsed; // bytes in _reload + _deleted

    } migrateFromStatus;
    
    struct MigrateStatusHolder {
        MigrateStatusHolder( string ns , const BSONObj& min , const BSONObj& max ){
            migrateFromStatus.start( ns , min , max );
        }
        ~MigrateStatusHolder(){
            migrateFromStatus.done();
        }
    };

    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt ){
        migrateFromStatus.logOp( opstr , ns , obj , patt );
    }

    class TransferModsCommand : public ChunkCommandHelper{
    public:
        TransferModsCommand() : ChunkCommandHelper( "_transferMods" ){}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            return migrateFromStatus.transferMods( errmsg, result );
        }
    } transferModsCommand;

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
            //    a) lock mongod to make sure we know about all writes
            //    b) in a read lock, get all DiskLoc and sort so we can do as little seeking as possible
            //    c) tell to start transferring
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
            string ns = cmdObj.firstElement().str();
            string to = cmdObj["to"].str();
            string from = cmdObj["from"].str(); // my public address, a tad redundant, but safe
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

            MoveTimingHelper timing( "from" , ns );

            Shard fromShard( from );
            Shard toShard( to );
            
            log() << "got movechunk: " << cmdObj << endl;

            timing.done(1);
            // 2. 
            
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
                assert( x["shard"].type() );
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
            
            timing.done(2);
            
            // 3.
            MigrateStatusHolder statusHolder( ns , min , max );
            { // 3.a
                dblock lk;
                // this makes sure there wasn't a write inside the .cpp code we can miss
            }
            
            if ( 0 ) { // 3.b TODO(erh) - not done yet
                
                readlock l( ns );
                Client::Context ctx( ns );
                NamespaceDetails *d = nsdetails( ns.c_str() );
                if ( ! d ){
                    errmsg = "ns not found, should be impossible";
                    return false;
                }
                
                BSONObj keyPattern;
                // the copies are needed because the command destrory the input
                BSONObj minCopy = min.copy();
                BSONObj maxCopy = max.copy();
                IndexDetails *idx = indexDetailsForRange( ns.c_str() , errmsg , minCopy , maxCopy , keyPattern ); 
                if ( idx == NULL ){
                    return false;
                }

                set<DiskLoc> locs;
                
                BtreeCursor c( d , d->idxNo(*idx) , *idx , min , max , false , 1 );
                while ( c.ok() ){
                    DiskLoc dl = c.currLoc();
                    locs.insert( dl );
                    c.advance();
                    // TODO: should we yield? 
                }

                
                
            }
            
            { // 3.c
                
                ScopedDbConnection connTo( to );
                BSONObj res;
                bool ok = connTo->runCommand( "admin" , 
                                              BSON( "_recvChunkStart" << ns <<
                                                    "from" << from <<
                                                    "min" << min <<
                                                    "max" << max <<
                                                    "configServer" << configServer.modelServer()
                                                  ) , 
                                              res );
                connTo.done();

                if ( ! ok ){
                    errmsg = "_recvChunkStart failed: ";
                    assert( res["errmsg"].type() );
                    errmsg += res["errmsg"].String();
                    result.append( "cause" , res );
                    return false;
                }

            }
            timing.done( 3 );
            
            // 4. 
            for ( int i=0; i<86400; i++ ){ // don't want a single chunk move to take more than a day
                assert( dbMutex.getState() == 0 );
                sleepsecs( 1 ); 
                ScopedDbConnection conn( to );
                BSONObj res;
                bool ok = conn->runCommand( "admin" , BSON( "_recvChunkStatus" << 1 ) , res );
                res = res.getOwned();
                conn.done();
                
                log(0) << "_recvChunkStatus : " << res << " my mem used: " << migrateFromStatus.mbUsed() << endl;
                
                if ( ! ok || res["state"].String() == "fail" ){
                    log( LL_ERROR ) << "_recvChunkStatus error : " << res << endl;
                    errmsg = "_recvChunkStatus error";
                    result.append( "cause" ,res );
                    return false;
                }

                if ( res["state"].String() == "steady" )
                    break;

                if ( migrateFromStatus.mbUsed() > (500 * 1024 * 1024) ){
                    // this is too much memory for us to use for this
                    // so we're going to abort the migrate
                    ScopedDbConnection conn( to );
                    BSONObj res;
                    conn->runCommand( "admin" , BSON( "_recvChunkAbort" << 1 ) , res );
                    res = res.getOwned();
                    conn.done();
                    error() << "aborting migrate because too much memory used res: " << res << endl;
                    errmsg = "aborting migrate because too much memory used";
                    result.appendBool( "split" , true );
                    return false;
                }

                killCurrentOp.checkForInterrupt();
            }
            timing.done(4);

            // 5.
            { 
                // 5.a
                migrateFromStatus._inCriticalSection = true;
                ShardChunkVersion myVersion = maxVersion;
                myVersion.incMajor();
                
                {
                    dblock lk;
                    assert( myVersion > shardingState.getVersion( ns ) );
                    shardingState.setVersion( ns , myVersion );
                    assert( myVersion == shardingState.getVersion( ns ) );
                    log() << "moveChunk locking myself to: " << myVersion << endl;
                }

                
                // 5.b
                {
                    BSONObj res;
                    ScopedDbConnection connTo( to );
                    bool ok = connTo->runCommand( "admin" , 
                                                  BSON( "_recvChunkCommit" << 1 ) ,
                                                  res );
                    connTo.done();
                    log() << "moveChunk commit result: " << res << endl;
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
                        myVersion.incMinor();

                        temp2.appendTimestamp( "lastmod" , myVersion );
                        
                        shardingState.setVersion( ns , myVersion );

                        BSONObj chunkIDDoc = x["_id"].wrap();
                        conn->update( ShardNS::chunk , chunkIDDoc , BSON( "$set" << temp2.obj() ) );
                        
                        log() << "moveChunk updating self to: " << myVersion << " through " << chunkIDDoc << endl;
                    }
                    else {
                        log() << "moveChunk: i have no chunks left" << endl;
                        shardingState.setVersion( ns , 0 );
                    }
                }

                conn.done();
                migrateFromStatus._inCriticalSection = false;
                // 5.d
                configServer.logChange( "moveChunk" , ns , BSON( "min" << min << "max" << max <<
                                                                 "from" << fromShard.getName() << 
                                                                 "to" << toShard.getName() ) );
            }
            
            migrateFromStatus.done();
            timing.done(5);

            
            { // 6.
                OldDataCleanup c;
                c.ns = ns;
                c.min = min.getOwned();
                c.max = max.getOwned();
                ClientCursor::find( ns , c.initial );
                if ( c.initial.size() ){
                    log() << "forking for cleaning up chunk data" << endl;
                    boost::thread t( boost::bind( &cleanupOldData , c ) );
                }
                else {
                    log() << "doing delete inline" << endl;
                    // 7.
                    c.doRemove();
                }
                    
                
            }
            timing.done(6);            

            return true;
            
        }
        
    } moveChunkCmd;

    bool ShardingState::inCriticalMigrateSection(){
        return migrateFromStatus._inCriticalSection;
    }

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
            clonedBytes = 0;
            numCatchup = 0;
            numSteady = 0;

            active = true;
        }

        void go(){
            try {
                _go();
            }
            catch ( std::exception& e ){
                state = FAIL;
                errmsg = e.what();
                log( LL_ERROR ) << "migrate failed: " << e.what() << endl;
            }
            catch ( ... ){
                state = FAIL;
                errmsg = "UNKNOWN ERROR";
                log( LL_ERROR ) << "migrate failed with unknown exception" << endl;
            }
            active = false;
        }
        
        void _go(){
            MoveTimingHelper timing( "to" , ns );
            
            assert( active );
            assert( state == READY );
            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );
            
            ScopedDbConnection conn( from );
            conn->getLastError(); // just test connection

            { // 1. copy indexes
                auto_ptr<DBClientCursor> indexes = conn->getIndexes( ns );
                vector<BSONObj> all;
                while ( indexes->more() ){
                    all.push_back( indexes->next().getOwned() );
                }
                
                writelock lk( ns );
                Client::Context ct( ns );
                
                string system_indexes = cc().database()->name + ".system.indexes";
                for ( unsigned i=0; i<all.size(); i++ ){
                    BSONObj idx = all[i];
                    theDataFileMgr.insert( system_indexes.c_str() , idx.objdata() , idx.objsize() );
                }
                
                timing.done(1);
            }
            
            { // 2. delete any data already in range
                writelock lk( ns );
                RemoveSaver rs( "moveChunk" , ns , "preCleanup" );
                long long num = Helpers::removeRange( ns , min , max , true , false , cmdLine.moveParanoia ? &rs : 0 );
                if ( num )
                    log( LL_WARNING ) << "moveChunkCmd deleted data already in chunk # objects: " << num << endl;

                timing.done(2);
            }
            
            
            { // 3. initial bulk clone
                state = CLONE;
                auto_ptr<DBClientCursor> cursor = conn->query( ns , Query().minKey( min ).maxKey( max ) , /* QueryOption_Exhaust */ 0 );
                assert( cursor.get() );
                while ( cursor->more() ){
                    BSONObj o = cursor->next().getOwned();
                    {
                        writelock lk( ns );
                        Helpers::upsert( ns , o );
                    }
                    numCloned++;
                    clonedBytes += o.objsize();
                }

                timing.done(3);
            }
            
            { // 4. do bulk of mods
                state = CATCHUP;
                while ( true ){
                    BSONObj res;
                    if ( ! conn->runCommand( "admin" , BSON( "_transferMods" << 1 ) , res ) ){
                        state = FAIL;
                        errmsg = "_transferMods failed: ";
                        errmsg += res.toString();
                        log( LL_ERROR ) << "_transferMods failed: " << res << endl;
                        conn.done();
                        return;
                    }
                    if ( res["size"].number() == 0 )
                        break;
                    
                    apply( res );

                    if ( state == ABORT ){
                        timing.note( "aborted" );
                        return;
                    }
                }

                timing.done(4);
            }
            
            { // 5. wait for commit
                Timer timeWaitingForCommit;

                state = STEADY;
                while ( state == STEADY || state == COMMIT_START ){
                    BSONObj res;
                    if ( ! conn->runCommand( "admin" , BSON( "_transferMods" << 1 ) , res ) ){
                        log() << "_transferMods failed in STEADY state: " << res << endl;
                        errmsg = res.toString();
                        state = FAIL;
                        conn.done();
                        return;
                    }

                    if ( res["size"].number() > 0 && apply( res ) )
                        continue;
                    
                    if ( state == COMMIT_START )
                        break;

                    sleepmillis( 10 );
                }

                if ( state == ABORT ){
                    timing.note( "aborted" );
                    return;
                }
                
                if ( timeWaitingForCommit.seconds() > 86400 ){
                    state = FAIL;
                    errmsg = "timed out waiting for commit";
                    return;
                }

                timing.done(5);
            }
            
            state = DONE;
            conn.done();
        }

        void status( BSONObjBuilder& b ){
            b.appendBool( "active" , active );

            b.append( "ns" , ns );
            b.append( "from" , from );
            b.append( "min" , min );
            b.append( "max" , max );
            
            b.append( "state" , stateString() );
            if ( state == FAIL )
                b.append( "errmsg" , errmsg );
            {
                BSONObjBuilder bb( b.subobjStart( "counts" ) );
                bb.append( "cloned" , numCloned );
                bb.append( "clonedBytes" , clonedBytes );
                bb.append( "catchup" , numCatchup );
                bb.append( "steady" , numSteady );
                bb.done();
            }


        }

        bool apply( const BSONObj& xfer ){
            bool didAnything = false;
            
            if ( xfer["deleted"].isABSONObj() ){
                writelock lk(ns);
                Client::Context cx(ns);
                
                RemoveSaver rs( "moveChunk" , ns , "removedDuring" );

                BSONObjIterator i( xfer["deleted"].Obj() );
                while ( i.more() ){
                    BSONObj id = i.next().Obj();
                    Helpers::removeRange( ns , id , id, false , true , cmdLine.moveParanoia ? &rs : 0 );
                    didAnything = true;
                }
            }
            
            if ( xfer["reload"].isABSONObj() ){
                writelock lk(ns);
                Client::Context cx(ns);

                BSONObjIterator i( xfer["reload"].Obj() );
                while ( i.more() ){
                    BSONObj it = i.next().Obj();
                    Helpers::upsert( ns , it );
                    didAnything = true;
                }
            }

            return didAnything;
        }
        
        string stateString(){
            switch ( state ){
            case READY: return "ready";
            case CLONE: return "clone";
            case CATCHUP: return "catchup";
            case STEADY: return "steady";
            case COMMIT_START: return "commitStart";
            case DONE: return "done";
            case FAIL: return "fail";
            case ABORT: return "abort";
            }
            assert(0);
            return "";
        }

        bool startCommit(){
            if ( state != STEADY )
                return false;
            state = COMMIT_START;
            
            for ( int i=0; i<86400; i++ ){
                sleepmillis(1);
                if ( state == DONE )
                    return true;
            }
            log() << "startCommit never finished!" << endl;
            return false;
        }

        void abort(){
            state = ABORT;
            errmsg = "aborted";
        }

        bool active;
        
        string ns;
        string from;
        
        BSONObj min;
        BSONObj max;
        
        long long numCloned;
        long long clonedBytes;
        long long numCatchup;
        long long numSteady;

        enum State { READY , CLONE , CATCHUP , STEADY , COMMIT_START , DONE , FAIL , ABORT } state;
        string errmsg;
        
    } migrateStatus;
    
    void migrateThread(){
        Client::initThread( "migrateThread" );
        migrateStatus.go();
        cc().shutdown();
    }
    
    class RecvChunkStartCommand : public ChunkCommandHelper {
    public:
        RecvChunkStartCommand() : ChunkCommandHelper( "_recvChunkStart" ){}

        virtual LockType locktype() const { return WRITE; }  // this is so don't have to do locking internally

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            
            if ( migrateStatus.active ){
                errmsg = "migrate already in progress";
                return false;
            }
            
            if ( ! configServer.ok() )
                configServer.init( cmdObj["configServer"].String() );

            migrateStatus.prepare();

            migrateStatus.ns = cmdObj.firstElement().String();
            migrateStatus.from = cmdObj["from"].String();
            migrateStatus.min = cmdObj["min"].Obj().getOwned();
            migrateStatus.max = cmdObj["max"].Obj().getOwned();
            
            boost::thread m( migrateThread );
            
            result.appendBool( "started" , true );
            return true;
        }

    } recvChunkStartCmd;

    class RecvChunkStatusCommand : public ChunkCommandHelper {
    public:
        RecvChunkStatusCommand() : ChunkCommandHelper( "_recvChunkStatus" ){}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            migrateStatus.status( result );
            return 1;
        }
        
    } recvChunkStatusCommand;

    class RecvChunkCommitCommand : public ChunkCommandHelper {
    public:
        RecvChunkCommitCommand() : ChunkCommandHelper( "_recvChunkCommit" ){}
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            bool ok = migrateStatus.startCommit();
            migrateStatus.status( result );
            return ok;
        }

    } recvChunkCommitCommand;

    class RecvChunkAbortCommand : public ChunkCommandHelper {
    public:
        RecvChunkAbortCommand() : ChunkCommandHelper( "_recvChunkAbort" ){}
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            migrateStatus.abort();
            migrateStatus.status( result );
            return true;
        }

    } recvChunkAboortCommand;


    class IsInRangeTest : public UnitTest {
    public:
        void run(){
            BSONObj min = BSON( "x" << 1 );
            BSONObj max = BSON( "x" << 5 );

            assert( ! isInRange( BSON( "x" << 0 ) , min , max ) );
            assert( isInRange( BSON( "x" << 1 ) , min , max ) );
            assert( isInRange( BSON( "x" << 3 ) , min , max ) );
            assert( isInRange( BSON( "x" << 4 ) , min , max ) );
            assert( ! isInRange( BSON( "x" << 5 ) , min , max ) );
            assert( ! isInRange( BSON( "x" << 6 ) , min , max ) );
        }
    } isInRangeTest;
}
