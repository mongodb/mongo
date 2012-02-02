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

    map<WriteBackListener::ConnectionIdent,WriteBackListener::WBStatus> WriteBackListener::_seenWritebacks;
    mongo::mutex WriteBackListener::_seenWritebacksLock("WriteBackListener::seen");

    WriteBackListener::WriteBackListener( const string& addr ) : _addr( addr ) {
        _name = str::stream() << "WriteBackListener-" << addr;
        log() << "creating WriteBackListener for: " << addr << " serverID: " << serverID << endl;
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
    BSONObj WriteBackListener::waitFor( const ConnectionIdent& ident, const OID& oid ) {
        Timer t;
        while ( t.minutes() < 60 ) {
            {
                scoped_lock lk( _seenWritebacksLock );
                WBStatus s = _seenWritebacks[ident];
                if ( oid < s.id ) {
                    // this means we're waiting for a GLE that already passed.
                    // it should be impossible because once we call GLE, no other
                    // writebacks should happen with that connection id

                    msgasserted( 14041 , str::stream() << "got writeback waitfor for older id " <<
                                 " oid: " << oid << " s.id: " << s.id << " ident: " << ident.toString() );
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
                LOG(1) << _addr << " is not a shard node" << endl;
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
                        result = result.getOwned();
                        log() <<  "writebacklisten command failed!  "  << result << endl;
                        conn.done();
                        continue;
                    }

                }

                LOG(1) << "writebacklisten result: " << result << endl;

                BSONObj data = result.getObjectField( "data" );
                if ( data.getBoolField( "writeBack" ) ) {
                    string ns = data["ns"].valuestrsafe();

                    ConnectionIdent cid( "" , 0 );
                    OID wid;
                    if ( data["connectionId"].isNumber() && data["id"].type() == jstOID ) {
                        string s = "";
                        if ( data["instanceIdent"].type() == String )
                            s = data["instanceIdent"].String();
                        cid = ConnectionIdent( s , data["connectionId"].numberLong() );
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

                    // TODO: The logic here could be refactored, but keeping to the original codepath for safety for now
                    ChunkManagerPtr manager = db->getChunkManagerIfExists( ns );
                    
                    if ( ! manager ) {
                        // I don't trust the above code
                        // for this to be valid, we would have to have gotten a writeback because
                        // a collection was sharded
                        // and then for the collection to be dropped between the time the write hit mongod
                        // and the time it gets here
                        // possible - but I think there are more likely cases
                        // and in that case a little slowness isn't a horrible issue
                        manager = db->getChunkManagerIfExists( ns , true , true );
                        if ( manager ) {
                            warning() << "after reload, getChunkManagerIfExists works, this is inefficient, but should be" << endl;
                        }
                        
                    }

                    LOG(1) << "connectionId: " << cid << " writebackId: " << wid << " needVersion : " << needVersion.toString()
                           << " mine : " << ( manager ? manager->getVersion().toString() : "(unknown)" )
                           << endl;

                    LOG(1) << m.toString() << endl;

                    if ( needVersion.isSet() && manager && needVersion <= manager->getVersion() ) {
                        // this means when the write went originally, the version was old
                        // if we're here, it means we've already updated the config, so don't need to do again
                        //db->getChunkManager( ns , true ); // SERVER-1349
                    }
                    else {
                        // we received a writeback object that was sent to a previous version of a shard
                        // the actual shard may not have the object the writeback operation is for
                        // we need to reload the chunk manager and get the new shard versions
                        manager = db->getChunkManager( ns , true );
                    }

                    // do request and then call getLastError
                    // we have to call getLastError so we can return the right fields to the user if they decide to call getLastError

                    BSONObj gle;
                    int attempts = 0;
                    while ( true ) {
                        attempts++;

                        try {
                            
                            Request r( m , 0 );
                            r.init();
                            
                            r.d().reservedField() |= DbMessage::Reserved_FromWriteback;
                            
                            ClientInfo * ci = r.getClientInfo();
                            if (!noauth) {
                                ci->getAuthenticationInfo()->authorize("admin", internalSecurity.user);
                            }
                            ci->noAutoSplit();
                            
                            r.process();
                            
                            ci->newRequest(); // this so we flip prev and cur shards
                            
                            BSONObjBuilder b;
                            if ( ! ci->getLastError( BSON( "getLastError" << 1 ) , b , true ) ) {
                                b.appendBool( "commandFailed" , true );
                            }
                            gle = b.obj();
                            
                            if ( gle["code"].numberInt() == 9517 ) {
                                log() << "writeback failed because of stale config, retrying attempts: " << attempts << endl;
                                if( ! db->getChunkManagerIfExists( ns , true, attempts > 2 ) ){
                                    uassert( 15884, str::stream() << "Could not reload chunk manager after " << attempts << " attempts.", attempts <= 4 );
                                    sleepsecs( attempts - 1 );
                                }
                                continue;
                            }

                            ci->clearSinceLastGetError();
                        }
                        catch ( DBException& e ) {
                            error() << "error processing writeback: " << e << endl;
                            BSONObjBuilder b;
                            b.append( "err" , e.toString() );
                            e.getInfo().append( b );
                            gle = b.obj();
                        }
                        
                        break;
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
            catch ( std::exception& e ) {

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
