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

#include "pch.h"

#include "../util/timer.h"

#include "config.h"
#include "grid.h"
#include "request.h"
#include "server.h"
#include "shard.h"
#include "util.h"
#include "client.h"

#include "writeback_listener.h"

namespace mongo {

    map<string,WriteBackListener*> WriteBackListener::_cache;
    set<string> WriteBackListener::_seenSets;
    mongo::mutex WriteBackListener::_cacheLock("WriteBackListener");

    map<ConnectionId,WriteBackListener::WBStatus> WriteBackListener::_seenWritebacks;
    mongo::mutex WriteBackListener::_seenWritebacksLock("WriteBackListener::seen");

    WriteBackListener::WriteBackListener( const string& addr ) : _addr( addr ) {
        log() << "creating WriteBackListener for: " << addr << endl;
    }

    /* static */
    void WriteBackListener::init( DBClientBase& conn ) {
        
        if ( conn.type() == ConnectionString::SYNC ) {
            // don't want write back listeners for config servers
            return;
        }

        if ( conn.type() != ConnectionString::SET ) {
            init( conn.getServerAddress() );
            return;
        }
        

        {
            scoped_lock lk( _cacheLock );
            if ( _seenSets.count( conn.getServerAddress() ) )
                return;
        }

        // we want to do writebacks on all rs nodes
        string errmsg;
        ConnectionString cs = ConnectionString::parse( conn.getServerAddress() , errmsg );
        uassert( 13641 , str::stream() << "can't parse host [" << conn.getServerAddress() << "]" , cs.isValid() );

        vector<HostAndPort> hosts = cs.getServers();
        
        for ( unsigned i=0; i<hosts.size(); i++ )
            init( hosts[i].toString() );

    }
    
    /* static */
    void WriteBackListener::init( const string& host ) {
        scoped_lock lk( _cacheLock );
        WriteBackListener*& l = _cache[host];
        if ( l )
            return;
        l = new WriteBackListener( host );
        l->go();
    }

    /* static */
    BSONObj WriteBackListener::waitFor( ConnectionId connectionId, const OID& oid ) {
        Timer t;
        for ( int i=0; i<5000; i++ ) {
            {
                scoped_lock lk( _seenWritebacksLock );
                WBStatus s = _seenWritebacks[connectionId];
                if ( oid < s.id ) {
                    // this means we're waiting for a GLE that already passed.
                    // it should be impossible becauseonce we call GLE, no other
                    // writebacks should happen with that connection id
                    msgasserted( 13633 , str::stream() << "got writeback waitfor for older id " <<
                                 " oid: " << oid << " s.id: " << s.id << " connectionId: " << connectionId );
                }
                else if ( oid == s.id ) {
                    return s.gle;
                }
                
            }
            sleepmillis( 10 );
        }
        uasserted( 13403 , str::stream() << "didn't get writeback for: " << oid << " after: " << t.millis() << " ms" );
        throw 1; // never gets here
    }

    void WriteBackListener::run() {
        int secsToSleep = 0;
        while ( ! inShutdown() ) {
            
            if ( ! Shard::isAShardNode( _addr ) ) {
                log(1) << _addr << " is not a shard node" << endl;
                sleepsecs( 60 );
                continue;
            }

            try {
                ScopedDbConnection conn( _addr );

                BSONObj result;

                {
                    BSONObjBuilder cmd;
                    cmd.appendOID( "writebacklisten" , &serverID ); // Command will block for data
                    if ( ! conn->runCommand( "admin" , cmd.obj() , result ) ) {
                        log() <<  "writebacklisten command failed!  "  << result << endl;
                        conn.done();
                        continue;
                    }

                }

                log(1) << "writebacklisten result: " << result << endl;

                BSONObj data = result.getObjectField( "data" );
                if ( data.getBoolField( "writeBack" ) ) {
                    string ns = data["ns"].valuestrsafe();

                    ConnectionId cid = 0;
                    OID wid;
                    if ( data["connectionId"].isNumber() && data["id"].type() == jstOID ) {
                        cid = data["connectionId"].numberLong();
                        wid = data["id"].OID();
                    }
                    else {
                        warning() << "mongos/mongod version mismatch (1.7.5 is the split)" << endl;
                    }

                    int len; // not used, but needed for next call
                    Message m( (void*)data["msg"].binData( len ) , false );
                    massert( 10427 ,  "invalid writeback message" , m.header()->valid() );

                    DBConfigPtr db = grid.getDBConfig( ns );
                    ShardChunkVersion needVersion( data["version"] );

                    log(1) << "connectionId: " << cid << " writebackId: " << wid << " needVersion : " << needVersion.toString()
                           << " mine : " << db->getChunkManager( ns )->getVersion().toString() << endl;// TODO change to log(3)

                    if ( logLevel ) log(1) << debugString( m ) << endl;

                    if ( needVersion.isSet() && needVersion <= db->getChunkManager( ns )->getVersion() ) {
                        // this means when the write went originally, the version was old
                        // if we're here, it means we've already updated the config, so don't need to do again
                        //db->getChunkManager( ns , true ); // SERVER-1349
                    }
                    else {
                        // we received a writeback object that was sent to a previous version of a shard
                        // the actual shard may not have the object the writeback operation is for
                        // we need to reload the chunk manager and get the new shard versions
                        db->getChunkManager( ns , true );
                    }

                    // do request and then call getLastError
                    // we have to call getLastError so we can return the right fields to the user if they decide to call getLastError

                    BSONObj gle;
                    try {
                        
                        Request r( m , 0 );
                        r.init();

                        ClientInfo * ci = r.getClientInfo();
                        ci->noAutoSplit();

                        r.process();
                        
                        ci->newRequest(); // this so we flip prev and cur shards

                        BSONObjBuilder b;
                        if ( ! ci->getLastError( BSON( "getLastError" << 1 ) , b , true ) ) {
                            b.appendBool( "commandFailed" , true );
                        }
                        gle = b.obj();

                        ci->clearSinceLastGetError();
                    }
                    catch ( DBException& e ) {
                        error() << "error processing writeback: " << e << endl;
                        BSONObjBuilder b;
                        b.append( "err" , e.toString() );
                        e.getInfo().append( b );
                        gle = b.obj();
                    }

                    {
                        scoped_lock lk( _seenWritebacksLock );
                        WBStatus& s = _seenWritebacks[cid];
                        s.id = wid;
                        s.gle = gle;
                    }
                }
                else if ( result["noop"].trueValue() ) {
                    // no-op
                }
                else {
                    log() << "unknown writeBack result: " << result << endl;
                }

                conn.done();
                secsToSleep = 0;
                continue;
            }
            catch ( std::exception e ) {

                if ( inShutdown() ) {
                    // we're shutting down, so just clean up
                    return;
                }

                log() << "WriteBackListener exception : " << e.what() << endl;

                // It's possible this shard was removed
                Shard::reloadShardInfo();
            }
            catch ( ... ) {
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
