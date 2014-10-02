// @file version_manager.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/version_manager.h"

#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException
#include "mongo/util/log.h"

namespace mongo {

    // Global version manager
    VersionManager versionManager;

    /**
     * Tracking information, per-connection, of the latest chunk manager iteration or sequence
     * number that was used to send a shard version over this connection.
     * When the chunk manager is replaced, implying new versions were loaded, the chunk manager
     * sequence number is iterated by 1 and connections need to re-send shard versions.
     */
    struct ConnectionShardStatus {

        ConnectionShardStatus()
            : _mutex( "ConnectionShardStatus" ) {
        }

        bool getSequence(DBClientBase * conn,
                         const string& ns,
                         unsigned long long* sequence) {

            scoped_lock lk(_mutex);

            SequenceMap::const_iterator seenConnIt = _map.find(conn->getConnectionId());
            if (seenConnIt == _map.end())
                return false;

            map<string, unsigned long long>::const_iterator seenNSIt = seenConnIt->second.find(ns);
            if (seenNSIt == seenConnIt->second.end())
                return false;

            *sequence = seenNSIt->second;
            return true;
        }

        void setSequence( DBClientBase * conn , const string& ns , const unsigned long long& s ) {
            scoped_lock lk( _mutex );
            _map[conn->getConnectionId()][ns] = s;
        }

        void reset( DBClientBase * conn ) {
            scoped_lock lk( _mutex );
            _map.erase( conn->getConnectionId() );
        }

        // protects _map
        mongo::mutex _mutex;

        // a map from a connection into ChunkManager's sequence number for each namespace
        typedef map<unsigned long long, map<string,unsigned long long> > SequenceMap;
        SequenceMap _map;

    } connectionShardStatus;

    void VersionManager::resetShardVersionCB( DBClientBase * conn ) {
        connectionShardStatus.reset( conn );
    }

    bool VersionManager::isVersionableCB( DBClientBase* conn ){
        return conn->type() == ConnectionString::MASTER || conn->type() == ConnectionString::SET;
    }

    DBClientBase* getVersionable( DBClientBase* conn ){

        switch ( conn->type() ) {
        case ConnectionString::INVALID:
           massert( 15904, str::stream() << "cannot set version on invalid connection " << conn->toString(), false );
           return NULL;
        case ConnectionString::MASTER:
           return conn;
        case ConnectionString::PAIR:
           massert( 15905, str::stream() << "cannot set version or shard on pair connection " << conn->toString(), false );
           return NULL;
        case ConnectionString::SYNC:
           massert( 15906, str::stream() << "cannot set version or shard on sync connection " << conn->toString(), false );
           return NULL;
        case ConnectionString::CUSTOM:
           massert( 16334, str::stream() << "cannot set version or shard on custom connection " << conn->toString(), false );
           return NULL;
        case ConnectionString::SET:
           DBClientReplicaSet* set = (DBClientReplicaSet*) conn;
           return &( set->masterConn() );
        }

        verify( false );
        return NULL;
    }

    bool VersionManager::forceRemoteCheckShardVersionCB( const string& ns ){

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf ) return false;
        conf->reload();

        // If we don't have a collection, don't refresh the chunk manager
        if( nsGetCollection( ns ).size() == 0 ) return false;

        ChunkManagerPtr manager = conf->getChunkManagerIfExists( ns, true, true );
        if( ! manager ) return false;

        return true;

    }

    /**
     * @return true if had to do something
     */
    bool checkShardVersion( DBClientBase * conn_in , const string& ns , ChunkManagerPtr refManager, bool authoritative , int tryNumber ) {
        // TODO: cache, optimize, etc...

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf )
            return false;

        DBClientBase* conn = getVersionable( conn_in );
        verify(conn); // errors thrown above

        unsigned long long officialSequenceNumber = 0;

        ShardPtr primary;
        ChunkManagerPtr manager;
        if (authoritative)
            conf->getChunkManagerIfExists(ns, true);

        conf->getChunkManagerOrPrimary(ns, manager, primary);

        if (manager)
            officialSequenceNumber = manager->getSequenceNumber();

        // Check this manager against the reference manager
        if( manager ){
            Shard shard = Shard::make(conn->getServerAddress());
            if( refManager && ! refManager->compatibleWith( manager, shard ) ){
                throw SendStaleConfigException( ns, str::stream() << "manager (" << manager->getVersion( shard ).toString()  << " : " << manager->getSequenceNumber() << ") "
                                                                      << "not compatible with reference manager (" << refManager->getVersion( shard ).toString()  << " : " << refManager->getSequenceNumber() << ") "
                                                                      << "on shard " << shard.getName() << " (" << shard.getAddress().toString() << ")",
                                                refManager->getVersion( shard ), manager->getVersion( shard ) );
            }
        }
        else if( refManager ){
            Shard shard = Shard::make(conn->getServerAddress());
            string msg( str::stream() << "not sharded ("
                        << ( (manager.get() == 0) ? string( "<none>" ) :
                                str::stream() << manager->getSequenceNumber() )
                        << ") but has reference manager ("
                        << refManager->getSequenceNumber() << ") "
                        << "on conn " << conn->getServerAddress() << " ("
                        << conn_in->getServerAddress() << ")" );

            throw SendStaleConfigException( ns, msg,
                    refManager->getVersion( shard ), ChunkVersion( 0, 0, OID() ));
        }

        // Do not send setShardVersion to collections on the config servers - this causes problems
        // when config servers are also shards and get SSV with conflicting names.
        // TODO: Make config servers regular shards
        if (primary && primary->getName() == "config") {
            return false;
        }

        // Has the ChunkManager been reloaded since the last time we updated the shard version over
        // this connection?  If we've never updated the shard version, do so now.
        unsigned long long sequenceNumber = 0;
        if (connectionShardStatus.getSequence(conn, ns, &sequenceNumber)) {
            if (sequenceNumber == officialSequenceNumber) {
                return false;
            }
        }

        // Now that we're sure we're sending SSV, get the shard we're sending it to
        Shard shard = Shard::make(conn->getServerAddress());

        ChunkVersion version = ChunkVersion(0, 0, OID());
        if (manager)
            version = manager->getVersion(shard);

        LOG(1) << "setting shard version of " << version << " for " << ns << " on shard "
               << shard.toString();

        LOG(3) << "last version sent with chunk manager iteration " << sequenceNumber
               << ", current chunk manager iteration is " << officialSequenceNumber;

        BSONObj result;
        if ( setShardVersion( *conn , ns , version , manager , authoritative , result ) ) {
            // success!
            LOG(1) << "      setShardVersion success: " << result << endl;
            connectionShardStatus.setSequence( conn , ns , officialSequenceNumber );
            return true;
        }

        LOG(1) << "       setShardVersion failed!\n" << result << endl;

        if ( result["need_authoritative"].trueValue() )
            massert( 10428 ,  "need_authoritative set but in authoritative mode already" , ! authoritative );

        if ( ! authoritative ) {
            // use the original connection and get a fresh versionable connection
            // since conn can be invalidated (or worse, freed) after the failure
            checkShardVersion(conn_in, ns, refManager, 1, tryNumber + 1);
            return true;
        }
        
        if ( result["reloadConfig"].trueValue() ) {
            if( result["version"].timestampTime() == 0 ){

                warning() << "reloading full configuration for " << conf->getName()
                          << ", connection state indicates significant version changes" << endl;

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
                << "going to retry checkShardVersion shard: " << shard.toString() << " " << result;
            sleepmillis( 10 * tryNumber );
            // use the original connection and get a fresh versionable connection
            // since conn can be invalidated (or worse, freed) after the failure
            checkShardVersion(conn_in, ns, refManager, true, tryNumber + 1);
            return true;
        }
        
        string errmsg = str::stream() << "setShardVersion failed shard: " << shard.toString()
                                          << " " << result;
        log() << "     " << errmsg << endl;
        massert( 10429 , errmsg , 0 );
        return true;
    }

    bool VersionManager::checkShardVersionCB( DBClientBase* conn_in , const string& ns , bool authoritative , int tryNumber ) {
        return checkShardVersion( conn_in, ns, ChunkManagerPtr(), authoritative, tryNumber );
    }

    bool VersionManager::checkShardVersionCB( ShardConnection* conn_in , bool authoritative , int tryNumber ) {
        return checkShardVersion( conn_in->get(), conn_in->getNS(), conn_in->getManager(), authoritative, tryNumber );
    }

}  // namespace mongo
