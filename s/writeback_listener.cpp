// @file writeback_listener.cpp

/**
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

#include "../pch.h"

#include "../util/timer.h"

#include "config.h"
#include "grid.h"
#include "request.h"
#include "server.h"
#include "shard.h"
#include "util.h"

#include "writeback_listener.h"

namespace mongo {

    map<string,WriteBackListener*> WriteBackListener::_cache;
    mongo::mutex WriteBackListener::_cacheLock("WriteBackListener");

    set<OID> WriteBackListener::_seenWritebacks;
    mongo::mutex WriteBackListener::_seenWritebacksLock("WriteBackListener::seen");

    WriteBackListener::WriteBackListener( const string& addr ) : _addr( addr ){
        log() << "creating WriteBackListener for: " << addr << endl;
    }
    
    /* static */
    void WriteBackListener::init( DBClientBase& conn ){
        scoped_lock lk( _cacheLock );
        WriteBackListener*& l = _cache[conn.getServerAddress()];
        if ( l )
            return;
        l = new WriteBackListener( conn.getServerAddress() );
        l->go();
    }

    /* static */
    void WriteBackListener::waitFor( const OID& oid ){
        Timer t;
        for ( int i=0; i<5000; i++ ){
            {
                scoped_lock lk( _seenWritebacksLock );
                if ( _seenWritebacks.count( oid ) )
                    return;
            }
            sleepmillis( 10 );
        }
        stringstream ss;
        ss << "didn't get writeback for: " << oid << " after: " << t.millis() << " ms";
        uasserted( 13403 , ss.str() );
    }

    void WriteBackListener::run(){
        OID lastID;
        lastID.clear();
        int secsToSleep = 0;
        while ( ! inShutdown() && Shard::isMember( _addr ) ){
                
            if ( lastID.isSet() ){
                scoped_lock lk( _seenWritebacksLock );
                _seenWritebacks.insert( lastID );
                lastID.clear();
            }

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
                    {
                        BSONElement e = data["id"];
                        if ( e.type() == jstOID )
                            lastID = e.OID();
                    }
                    int len;

                    Message m( (void*)data["msg"].binData( len ) , false );
                    massert( 10427 ,  "invalid writeback message" , m.header()->valid() );                        

                    DBConfigPtr db = grid.getDBConfig( ns );
                    ShardChunkVersion needVersion( data["version"] );
                        
                    log(1) << "writeback id: " << lastID << " needVersion : " << needVersion.toString() 
                           << " mine : " << db->getChunkManager( ns )->getVersion().toString() << endl;// TODO change to log(3)
                        
                    if ( logLevel ) log(1) << debugString( m ) << endl;

                    if ( needVersion.isSet() && needVersion <= db->getChunkManager( ns )->getVersion() ){
                        // this means when the write went originally, the version was old
                        // if we're here, it means we've already updated the config, so don't need to do again
                        //db->getChunkManager( ns , true ); // SERVER-1349
                    }
                    else {
                        db->getChunkManager( ns , true );
                    }
                        
                    Request r( m , 0 );
                    r.init();
                    r.process();
                }
                else if ( result["noop"].trueValue() ){
                    // no-op
                }
                else {
                    log() << "unknown writeBack result: " << result << endl;
                }
                    
                conn.done();
                secsToSleep = 0;
                continue;
            }
            catch ( std::exception e ){

                if ( inShutdown() ){
                    // we're shutting down, so just clean up
                    return;
                }

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

}  // namespace mongo
