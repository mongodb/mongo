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
        ShardConnection dbcon( shard );
        DBClientBase &_c = dbcon.conn();
        
        /* TODO FIX - do not case and call DBClientBase::say() */
        DBClientConnection&c = dynamic_cast<DBClientConnection&>(_c);
        c.port().say( r.m() );
        
        dbcon.done();
    }

    void Strategy::doQuery( Request& r , const Shard& shard ){
        try{
            ShardConnection dbcon( shard );
            DBClientBase &c = dbcon.conn();
            
            checkShardVersion( c , r.getns() );
            
            Message response;
            bool ok = c.call( r.m(), response);

            {
                QueryResult *qr = (QueryResult *) response.data;
                if ( qr->resultFlags() & QueryResult::ResultFlag_ShardConfigStale ){
                    dbcon.done();
                    throw StaleConfigException( r.getns() , "Strategy::doQuery" );
                }
            }

            uassert( 10200 , "mongos: error calling db", ok);
            r.reply( response );
            dbcon.done();
        }
        catch ( AssertionException& e ) {
            BSONObjBuilder err;
            err.append("$err", string("mongos: ") + (e.msg.empty() ? "assertion during query" : e.msg));
            err.append("code",e.getCode());
            BSONObj errObj = err.done();
            replyToQuery(QueryResult::ResultFlag_ErrSet, r.p() , r.m() , errObj);
        }
    }
    
    void Strategy::insert( const Shard& shard , const char * ns , const BSONObj& obj ){
        ShardConnection dbcon( shard );
        checkShardVersion( dbcon.conn() , ns );
        dbcon->insert( ns , obj );
        dbcon.done();
    }

    map<DBClientBase*,unsigned long long> checkShardVersionLastSequence;

    class WriteBackListener : public BackgroundJob {
    protected:
        
        WriteBackListener( const string& addr ) : _addr( addr ){
            cout << "creating WriteBackListener for: " << addr << endl;
        }
        
        void run(){
            int secsToSleep = 0;
            while ( 1 ){
                try {
                    ScopedDbConnection conn( _addr );
                    
                    BSONObj result;
                    
                    {
                        BSONObjBuilder cmd;
                        cmd.appendOID( "writebacklisten" , &serverID );
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
                        massert( 10427 ,  "invalid writeback message" , m.data->valid() );                        

                        grid.getDBConfig( ns )->getChunkManager( ns , true );
                        
                        Request r( m , 0 );
                        r.process();
                    }
                    else {
                        log() << "unknown writeBack result: " << result << endl;
                    }
                    
                    conn.done();
                    secsToSleep = 0;
                }
                catch ( std::exception e ){
                    log() << "WriteBackListener exception : " << e.what() << endl;
                }
                catch ( ... ){
                    log() << "WriteBackListener uncaught exception!" << endl;
                }
                secsToSleep++;
                sleepsecs(secsToSleep);
                if ( secsToSleep > 10 )
                    secsToSleep = 0;
            }
        }
        
    private:
        string _addr;
        static map<string,WriteBackListener*> _cache;

    public:
        static void init( DBClientBase& conn ){
            WriteBackListener*& l = _cache[conn.getServerAddress()];
            if ( l )
                return;
            l = new WriteBackListener( conn.getServerAddress() );
            l->go();
        }

    };

    map<string,WriteBackListener*> WriteBackListener::_cache;
    

    void checkShardVersion( DBClientBase& conn , const string& ns , bool authoritative ){
        // TODO: cache, optimize, etc...
        
        WriteBackListener::init( conn );

        DBConfig * conf = grid.getDBConfig( ns );
        if ( ! conf )
            return;
        
        ShardChunkVersion version = 0;
        unsigned long long officialSequenceNumber = 0;

        if ( conf->isSharded( ns ) ){
            ChunkManager * manager = conf->getChunkManager( ns , authoritative );
            officialSequenceNumber = manager->getSequenceNumber();
            version = manager->getVersion( Shard::make( conn.getServerAddress() ) );
        }

        unsigned long long & sequenceNumber = checkShardVersionLastSequence[ &conn ];        
        if ( sequenceNumber == officialSequenceNumber )
            return;
        
        log(2) << " have to set shard version for conn: " << &conn << " ns:" << ns << " my last seq: " << sequenceNumber << "  current: " << officialSequenceNumber << endl;

        BSONObj result;
        if ( setShardVersion( conn , ns , version , authoritative , result ) ){
            // success!
            log(1) << "      setShardVersion success!" << endl;
            sequenceNumber = officialSequenceNumber;
            return;
        }

        log(1) << "       setShardVersion failed!\n" << result << endl;

        if ( result.getBoolField( "need_authoritative" ) )
            massert( 10428 ,  "need_authoritative set but in authoritative mode already" , ! authoritative );
        
        if ( ! authoritative ){
            checkShardVersion( conn , ns , 1 );
            return;
        }
        
        log() << "     setShardVersion failed: " << result << endl;
        massert( 10429 , (string)"setShardVersion failed! " + result.jsonString() , 0 );
    }
    
    bool setShardVersion( DBClientBase & conn , const string& ns , ShardChunkVersion version , bool authoritative , BSONObj& result ){

        BSONObjBuilder cmdBuilder;
        cmdBuilder.append( "setShardVersion" , ns.c_str() );
        cmdBuilder.append( "configdb" , configServer.modelServer() );
        cmdBuilder.appendTimestamp( "version" , version );
        cmdBuilder.appendOID( "serverID" , &serverID );
        if ( authoritative )
            cmdBuilder.appendBool( "authoritative" , 1 );
        BSONObj cmd = cmdBuilder.obj();
        
        log(1) << "    setShardVersion  " << conn.getServerAddress() << "  " << ns << "  " << cmd << " " << &conn << endl;
        
        return conn.runCommand( "admin" , cmd , result );
    }

    bool lockNamespaceOnServer( const Shard& shard, const string& ns ){
        ShardConnection conn( shard );
        bool res = lockNamespaceOnServer( conn.conn() , ns );
        conn.done();
        return res;
    }

    bool lockNamespaceOnServer( DBClientBase& conn , const string& ns ){
        BSONObj lockResult;
        return setShardVersion( conn , ns , grid.getNextOpTime() , true , lockResult );
    }

    
}
