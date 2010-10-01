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

#include "../pch.h"

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
            : _mutex( "ConnectionShardStatus" ){
        }

        S getSequence( DBClientBase * conn , const string& ns ){
            scoped_lock lk( _mutex );
            return _map[conn][ns];
        }

        void setSequence( DBClientBase * conn , const string& ns , const S& s ){
            scoped_lock lk( _mutex );
            _map[conn][ns] = s;
        }

        void reset( DBClientBase * conn ){
            scoped_lock lk( _mutex );
            _map.erase( conn );
        }

        mongo::mutex _mutex;  // protects state below
        map<DBClientBase*, map<string,unsigned long long> > _map;

    } connectionShardStatus;

    void resetShardVersion( DBClientBase * conn ){
        connectionShardStatus.reset( conn );
    }
    
    /**
     * @return true if had to do something
     */
    bool checkShardVersion( DBClientBase& conn , const string& ns , bool authoritative , int tryNumber ){
        // TODO: cache, optimize, etc...
        
        WriteBackListener::init( conn );

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf )
            return false;
        
        unsigned long long officialSequenceNumber = 0;
        
        ChunkManagerPtr manager;
        const bool isSharded = conf->isSharded( ns );
        if ( isSharded ){
            manager = conf->getChunkManager( ns , authoritative );
            officialSequenceNumber = manager->getSequenceNumber();
        }

        unsigned long long sequenceNumber = connectionShardStatus.getSequence(&conn,ns);
        if ( sequenceNumber == officialSequenceNumber ){
            return false;
        }


        ShardChunkVersion version = 0;
        if ( isSharded ){
            version = manager->getVersion( Shard::make( conn.getServerAddress() ) );
            assert( officialSequenceNumber == manager->getSequenceNumber() ); // this is to make sure there isn't a race condition
        }
        
        log(2) << " have to set shard version for conn: " << &conn << " ns:" << ns 
               << " my last seq: " << sequenceNumber << "  current: " << officialSequenceNumber 
               << " version: " << version << " manager: " << manager.get()
               << endl;
        
        BSONObj result;
        if ( setShardVersion( conn , ns , version , authoritative , result ) ){
            // success!
            log(1) << "      setShardVersion success!" << endl;
            connectionShardStatus.setSequence( &conn , ns , officialSequenceNumber );
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
    
}  // namespace mongo
