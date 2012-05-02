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

    // Global version manager
    VersionManager versionManager;

    // when running in sharded mode, use chunk shard version control
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
        case ConnectionString::SET:
           DBClientReplicaSet* set = (DBClientReplicaSet*) conn;
           return &( set->masterConn() );
        }

        verify( false );
        return NULL;
    }

    extern OID serverID;

    bool VersionManager::initShardVersionCB( DBClientBase * conn_in, BSONObj& result ){

        WriteBackListener::init( *conn_in );

        DBClientBase* conn = getVersionable( conn_in );
        verify( conn ); // errors thrown above

        BSONObjBuilder cmdBuilder;

        cmdBuilder.append( "setShardVersion" , "" );
        cmdBuilder.appendBool( "init", true );
        cmdBuilder.append( "configdb" , configServer.modelServer() );
        cmdBuilder.appendOID( "serverID" , &serverID );
        cmdBuilder.appendBool( "authoritative" , true );

        BSONObj cmd = cmdBuilder.obj();

        LOG(1) << "initializing shard connection to " << conn->toString() << endl;
        LOG(2) << "initial sharding settings : " << cmd << endl;

        bool ok = conn->runCommand( "admin" , cmd , result );

        // HACK for backwards compatibility with v1.8.x, v2.0.0 and v2.0.1
        // Result is false, but will still initialize serverID and configdb
        if( ! ok && ! result["errmsg"].eoo() && ( result["errmsg"].String() == "need to specify namespace"/* 2.0.1/2 */ ||
                                                  result["errmsg"].String() == "need to speciy namespace" /* 1.8 */ ))
        {
            ok = true;
        }

        LOG(3) << "initial sharding result : " << result << endl;

        return ok;

    }

    bool VersionManager::forceRemoteCheckShardVersionCB( const string& ns ){

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf ) return false;
        conf->reload();

        ChunkManagerPtr manager = conf->getChunkManagerIfExists( ns, true, true );
        if( ! manager ) return false;

        return true;

    }

    /**
     * @return true if had to do something
     */
    bool checkShardVersion( DBClientBase * conn_in , const string& ns , ChunkManagerPtr refManager, bool authoritative , int tryNumber ) {
        // TODO: cache, optimize, etc...

        WriteBackListener::init( *conn_in );

        DBConfigPtr conf = grid.getDBConfig( ns );
        if ( ! conf )
            return false;

        DBClientBase* conn = getVersionable( conn_in );
        verify(conn); // errors thrown above

        unsigned long long officialSequenceNumber = 0;

        ChunkManagerPtr manager;
        const bool isSharded = conf->isSharded( ns );
        if ( isSharded ) {
            manager = conf->getChunkManagerIfExists( ns , authoritative );
            // It's possible the chunk manager was reset since we checked whether sharded was true,
            // so must check this here.
            if( manager ) officialSequenceNumber = manager->getSequenceNumber();
        }

        // Check this manager against the reference manager
        if( isSharded && manager ){

            Shard shard = Shard::make( conn->getServerAddress() );
            if( refManager && ! refManager->compatibleWith( manager, shard ) ){
                throw SendStaleConfigException( ns, str::stream() << "manager (" << manager->getVersion( shard ).toString()  << " : " << manager->getSequenceNumber() << ") "
                                                                      << "not compatible with reference manager (" << refManager->getVersion( shard ).toString()  << " : " << refManager->getSequenceNumber() << ") "
                                                                      << "on shard " << shard.getName() << " (" << shard.getAddress().toString() << ")",
                                                refManager->getVersion( shard ), manager->getVersion( shard ) );
            }
        }
        else if( refManager ){
            Shard shard = Shard::make( conn->getServerAddress() );
            string msg( str::stream() << "not sharded ("
                        << ( (manager.get() == 0) ? string( "<none>" ) :
                                str::stream() << manager->getSequenceNumber() )
                        << ") but has reference manager ("
                        << refManager->getSequenceNumber() << ") "
                        << "on conn " << conn->getServerAddress() << " ("
                        << conn_in->getServerAddress() << ")" );

            throw SendStaleConfigException( ns, msg,
                    refManager->getVersion( shard ), ShardChunkVersion( 0 ));
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
            checkShardVersion( conn , ns , refManager, 1 , tryNumber + 1 );
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
            checkShardVersion( conn , ns , refManager, true , tryNumber + 1 );
            return true;
        }
        
        string errmsg = str::stream() << "setShardVersion failed host: " << conn->getServerAddress() << " " << result;
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
