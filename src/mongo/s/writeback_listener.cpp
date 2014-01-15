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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include "writeback_listener.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/server.h"
#include "mongo/s/shard.h"
#include "mongo/s/version_manager.h"
#include "mongo/util/timer.h"

namespace mongo {

    unordered_map<string,WriteBackListener*> WriteBackListener::_cache;
    unordered_set<string> WriteBackListener::_seenSets;
    mongo::mutex WriteBackListener::_cacheLock("WriteBackListener");

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

        const string addr = conn.getServerAddress();

        {
            scoped_lock lk( _cacheLock );
            if ( _seenSets.count( addr ) ) {
                return;
            }
        }
        // we want to do writebacks on all rs nodes
        string errmsg;
        ConnectionString cs = ConnectionString::parse( conn.getServerAddress() , errmsg );
        uassert( 13641 , str::stream() << "can't parse host [" << conn.getServerAddress() << "]" , cs.isValid() );

        vector<HostAndPort> hosts = cs.getServers();

        for ( unsigned i=0; i<hosts.size(); i++ )
            init( hosts[i].toString() );

        scoped_lock lk( _cacheLock );
        _seenSets.insert( addr );

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

    void WriteBackListener::run() {

        int secsToSleep = 0;
        bool needsToReloadShardInfo = false;

        while ( ! inShutdown() ) {

            if ( ! Shard::isAShardNode( _addr ) ) {
                LOG(1) << _addr << " is not a shard node" << endl;
                sleepsecs( 60 );
                continue;
            }

            try {
                if (needsToReloadShardInfo) {
                    // It's possible this shard was removed
                    Shard::reloadShardInfo();
                    needsToReloadShardInfo = false;
                }

                ScopedDbConnection conn(_addr);

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
                conn.done();

                LOG(1) << "writebacklisten result from pre-2.6 server: " << result << endl;

                BSONObj data = result.getObjectField( "data" );
                if ( data.getBoolField( "writeBack" ) ) {
                    string ns = data["ns"].valuestrsafe();

                    int len; // not used, but needed for next call
                    Message msg( (void*)data["msg"].binData( len ) , false );

                    if ( !msg.header()->valid() ) {
                        warning() << "invalid writeback message detected: " << result << endl;
                    }
                }
                else if ( result["noop"].trueValue() ) {
                    // no-op
                }
                else {
                    log() << "unknown writeBack result: " << result << endl;
                }

                secsToSleep = 0;
                continue;
            }
            catch ( std::exception& e ) {
                // Attention! Do not call any method that would throw an exception
                // (or assert) in this block.

                if ( inShutdown() ) {
                    // we're shutting down, so just clean up
                    return;
                }

                log() << "WriteBackListener exception : " << e.what() << endl;
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
