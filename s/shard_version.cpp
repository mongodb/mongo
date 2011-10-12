// @file shard_version.cpp

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

#include "chunk.h"
#include "config.h"
#include "grid.h"
#include "util.h"
#include "shard.h"
#include "writeback_listener.h"

#include "shard_version.h"

namespace mongo {

    // when running in sharded mode, use chunk shard version control

    static bool checkShardVersion( DBClientBase & conn , const string& ns , bool authoritative = false , int tryNumber = 1 );
    static void resetShardVersion( DBClientBase * conn );

    void installChunkShardVersioning() {
        //
        // Overriding no-op behavior in shardconnection.cpp
        //
        // TODO: Better encapsulate this mechanism.
        //
        checkShardVersionCB = checkShardVersion;
        resetShardVersionCB = resetShardVersion;
    }

    struct ConnectionShardStatus {

        typedef unsigned long long S;

        ConnectionShardStatus()
            : _mutex( "ConnectionShardStatus" ) {
        }

        S getSequence( DBClientBase * conn , const string& ns ) {
            scoped_lock lk( _mutex );
            return _map[conn][ns];
        }

        void setSequence( DBClientBase * conn , const string& ns , const S& s ) {
            scoped_lock lk( _mutex );
            _map[conn][ns] = s;
        }

        void reset( DBClientBase * conn ) {
            scoped_lock lk( _mutex );
            _map.erase( conn );
        }

        // protects _map
        mongo::mutex _mutex;

        // a map from a connection into ChunkManager's sequence number for each namespace
        map<DBClientBase*, map<string,unsigned long long> > _map;

    } connectionShardStatus;

    void resetShardVersion( DBClientBase * conn ) {
        connectionShardStatus.reset( conn );
    }

    /**
     * @return true if had to do something
     */
    bool checkShardVersion( DBClientBase& conn_in , const string& ns , bool authoritative , int tryNumber ) {
        // TODO: cache, optimize, etc...

        WriteBackListener::init( conn_in );

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf )
            return false;

        DBClientBase* conn = 0;

        switch ( conn_in.type() ) {
        case ConnectionString::INVALID:
            assert(0);
            break;
        case ConnectionString::MASTER:
            // great
            conn = &conn_in;
            break;
        case ConnectionString::PAIR:
            assert( ! "pair not support for sharding" );
            break;
        case ConnectionString::SYNC:
            // TODO: we should check later that we aren't actually sharded on this
            conn = &conn_in;
            break;
        case ConnectionString::SET:
            DBClientReplicaSet* set = (DBClientReplicaSet*)&conn_in;
            conn = &(set->masterConn());
            break;
        }

        assert(conn);

        unsigned long long officialSequenceNumber = 0;

        ChunkManagerPtr manager;
        const bool isSharded = conf->isSharded( ns );
        if ( isSharded ) {
            manager = conf->getChunkManagerIfExists( ns , authoritative );
            // It's possible the chunk manager was reset since we checked whether sharded was true,
            // so must check this here.
            if( manager ) officialSequenceNumber = manager->getSequenceNumber();
        }

        // has the ChunkManager been reloaded since the last time we updated the connection-level version?
        // (ie., last time we issued the setShardVersions below)
        unsigned long long sequenceNumber = connectionShardStatus.getSequence(conn,ns);
        if ( sequenceNumber == officialSequenceNumber ) {
            return false;
        }


        ShardChunkVersion version = 0;
        if ( isSharded && manager ) {
            version = manager->getVersion( Shard::make( conn->getServerAddress() ) );
        }

        if( version == 0 ){
            LOG(0) << "resetting shard version of " << ns << " on " << conn->getServerAddress() << ", " <<
                      ( ! isSharded ? "no longer sharded" :
                      ( ! manager ? "no chunk manager found" :
                                    "version is zero" ) ) << endl;
        }


        LOG(2) << " have to set shard version for conn: " << conn << " ns:" << ns
               << " my last seq: " << sequenceNumber << "  current: " << officialSequenceNumber
               << " version: " << version << " manager: " << manager.get()
               << endl;

        BSONObj result;
        if ( setShardVersion( *conn , ns , version , authoritative , result ) ) {
            // success!
            LOG(1) << "      setShardVersion success: " << result << endl;
            connectionShardStatus.setSequence( conn , ns , officialSequenceNumber );
            return true;
        }

        LOG(1) << "       setShardVersion failed!\n" << result << endl;

        if ( result["need_authoritative"].trueValue() )
            massert( 10428 ,  "need_authoritative set but in authoritative mode already" , ! authoritative );

        if ( ! authoritative ) {
            checkShardVersion( *conn , ns , 1 , tryNumber + 1 );
            return true;
        }
        
        if ( result["reloadConfig"].trueValue() ) {
            if( result["version"].timestampTime() == 0 ){
                // reload db
                conf->reload();
            }
            else {
                // reload config
                conf->getChunkManager( ns , true );
            }
        }

        const int maxNumTries = 7;
        if ( tryNumber < maxNumTries ) {
            LOG( tryNumber < ( maxNumTries / 2 ) ? 1 : 0 ) 
                << "going to retry checkShardVersion host: " << conn->getServerAddress() << " " << result << endl;
            sleepmillis( 10 * tryNumber );
            checkShardVersion( *conn , ns , true , tryNumber + 1 );
            return true;
        }
        
        string errmsg = str::stream() << "setShardVersion failed host: " << conn->getServerAddress() << " " << result;
        log() << "     " << errmsg << endl;
        massert( 10429 , errmsg , 0 );
        return true;
    }

}  // namespace mongo
