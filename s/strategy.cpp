/*
 *    Copyright (C) 2010 10gen Inc.
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

// stragegy.cpp

#include "pch.h"
#include "request.h"
#include "../util/background.h"
#include "../client/connpool.h"
#include "../db/commands.h"
#include "server.h"

namespace mongo {

    // ----- Strategy ------

    void Strategy::doWrite( int op , Request& r , const Shard& shard ){
        ShardConnection conn( shard , r.getns() );
        conn->say( r.m() );
        conn.done();
    }

    void Strategy::doQuery( Request& r , const Shard& shard ){
        try{
            ShardConnection dbcon( shard , r.getns() );
            DBClientBase &c = dbcon.conn();
            
            Message response;
            bool ok = c.call( r.m(), response);

            {
                QueryResult *qr = (QueryResult *) response.singleData();
                if ( qr->resultFlags() & ResultFlag_ShardConfigStale ){
                    dbcon.done();
                    throw StaleConfigException( r.getns() , "Strategy::doQuery" );
                }
            }

            uassert( 10200 , "mongos: error calling db", ok);
            r.reply( response , c.getServerAddress() );
            dbcon.done();
        }
        catch ( AssertionException& e ) {
            BSONObjBuilder err;
            e.getInfo().append( err );
            BSONObj errObj = err.done();
            replyToQuery(ResultFlag_ErrSet, r.p() , r.m() , errObj);
        }
    }
    
    void Strategy::insert( const Shard& shard , const char * ns , const BSONObj& obj ){
        ShardConnection dbcon( shard , ns );
        if ( dbcon.setVersion() ){
            dbcon.done();
            throw StaleConfigException( ns , "for insert" );
        }
        dbcon->insert( ns , obj );
        dbcon.done();
    }

    map< pair<DBClientBase*,string> ,unsigned long long> checkShardVersionLastSequence;

    class WriteBackListener : public BackgroundJob {
    protected:
        string name() { return "WriteBackListener"; }
        WriteBackListener( const string& addr ) : _addr( addr ){
            log() << "creating WriteBackListener for: " << addr << endl;
        }
        
        void run(){
            int secsToSleep = 0;
            while ( Shard::isMember( _addr ) ){
                try {
                    ScopedDbConnection conn( _addr );
                    
                    BSONObj result;
                    
                    {
                        BSONObjBuilder cmd;
                        cmd.appendOID( "writebacklisten" , &serverID ); // Command will block for data
                        if ( ! conn->runCommand( "admin" , cmd.obj() , result ) ){
                            log() <<  "writebacklisten command failed!  "  << result << endl;
                            conn.done();
                            continue;
                        }

                    }
                    
                    log(1) << "writebacklisten result: " << result << endl;
                    
                    BSONObj data = result.getObjectField( "data" );
                    if ( data.getBoolField( "writeBack" ) ){
                        string ns = data["ns"].valuestrsafe();

                        int len;

                        Message m( (void*)data["msg"].binData( len ) , false );
                        massert( 10427 ,  "invalid writeback message" , m.header()->valid() );                        

                        DBConfigPtr db = grid.getDBConfig( ns );
                        ShardChunkVersion needVersion( data["version"] );
                        
                        log(1) << "writeback needVersion : " << needVersion.toString() << " mine : " << db->getChunkManager( ns )->getVersion().toString() << endl;// TODO change to log(3)
                        
                        if ( needVersion.isSet() && needVersion <= db->getChunkManager( ns )->getVersion() ){
                            // this means when the write went originally, the version was old
                            // if we're here, it means we've already updated the config, so don't need to do again
                            db->getChunkManager( ns , true ); // TEMP TEMP TEMP ERH
                        }
                        else {
                            db->getChunkManager( ns , true );
                        }
                        
                        Request r( m , 0 );
                        r.process();
                    }
                    else {
                        log() << "unknown writeBack result: " << result << endl;
                    }
                    
                    conn.done();
                    secsToSleep = 0;
                    continue;
                }
                catch ( std::exception e ){
                    log() << "WriteBackListener exception : " << e.what() << endl;

                    // It's possible this shard was removed
                    Shard::reloadShardInfo();                    
                }
                catch ( ... ){
                    log() << "WriteBackListener uncaught exception!" << endl;
                }
                secsToSleep++;
                sleepsecs(secsToSleep);
                if ( secsToSleep > 10 )
                    secsToSleep = 0;
            }

            log() << "WriteBackListener exiting : address no longer in cluster " << _addr;

        }
        
    private:
        string _addr;

        static map<string,WriteBackListener*> _cache;
        static mongo::mutex _lock;
        
    public:
        static void init( DBClientBase& conn ){
            scoped_lock lk( _lock );
            WriteBackListener*& l = _cache[conn.getServerAddress()];
            if ( l )
                return;
            l = new WriteBackListener( conn.getServerAddress() );
            l->go();
        }

    };

    map<string,WriteBackListener*> WriteBackListener::_cache;
    mongo::mutex WriteBackListener::_lock("WriteBackListener");
    
    /**
     * @return true if had to do something
     */
    bool checkShardVersion( DBClientBase& conn , const string& ns , bool authoritative , int tryNumber ){
        // TODO: cache, optimize, etc...
        
        WriteBackListener::init( conn );

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf )
            return false;
        
        ShardChunkVersion version = 0;
        unsigned long long officialSequenceNumber = 0;
        
        ChunkManagerPtr manager;
        const bool isSharded = conf->isSharded( ns );
        if ( isSharded ){
            manager = conf->getChunkManager( ns , authoritative );
            officialSequenceNumber = manager->getSequenceNumber();
        }

        unsigned long long & sequenceNumber = checkShardVersionLastSequence[ make_pair(&conn,ns) ];        
        if ( sequenceNumber == officialSequenceNumber )
            return false;

        if ( isSharded ){
            version = manager->getVersion( Shard::make( conn.getServerAddress() ) );
        }
        
        log(2) << " have to set shard version for conn: " << &conn << " ns:" << ns 
               << " my last seq: " << sequenceNumber << "  current: " << officialSequenceNumber 
               << " version: " << version << " manager: " << manager.get()
               << endl;
        
        BSONObj result;
        if ( setShardVersion( conn , ns , version , authoritative , result ) ){
            // success!
            log(1) << "      setShardVersion success!" << endl;
            sequenceNumber = officialSequenceNumber;
            dassert( sequenceNumber == checkShardVersionLastSequence[ make_pair(&conn,ns) ] );
            return true;
        }
        
        log(1) << "       setShardVersion failed!\n" << result << endl;

        if ( result.getBoolField( "need_authoritative" ) )
            massert( 10428 ,  "need_authoritative set but in authoritative mode already" , ! authoritative );
        
        if ( ! authoritative ){
            checkShardVersion( conn , ns , 1 , tryNumber + 1 );
            return true;
        }
        
        if ( tryNumber < 4 ){
            log(1) << "going to retry checkShardVersion" << endl;
            sleepmillis( 10 );
            checkShardVersion( conn , ns , 1 , tryNumber + 1 );
            return true;
        }

        log() << "     setShardVersion failed: " << result << endl;
        massert( 10429 , (string)"setShardVersion failed! " + result.jsonString() , 0 );
        return true;
    }
    
    
}
