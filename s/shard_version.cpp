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

    static bool isVersionable( DBClientBase * conn );
    static bool initShardVersion( DBClientBase & conn, BSONObj& result );
    static bool checkShardVersion( DBClientBase & conn , const string& ns , bool authoritative = false , int tryNumber = 1 );
    static void resetShardVersion( DBClientBase * conn );

    void installChunkShardVersioning() {
        //
        // Overriding no-op behavior in shardconnection.cpp
        //
        // TODO: Better encapsulate this mechanism.
        //
        isVersionableCB = isVersionable;
        initShardVersionCB = initShardVersion;
        checkShardVersionCB = checkShardVersion;
        resetShardVersionCB = resetShardVersion;
    }

    struct ConnectionShardStatus {

        typedef unsigned long long S;

        ConnectionShardStatus()
            : _mutex( "ConnectionShardStatus" ) {
        }

        bool isInitialized( DBClientBase * conn ){
            scoped_lock lk( _mutex );
            return _init.find( conn ) != _init.end();
        }

        void setInitialized( DBClientBase * conn ){
            // At this point, conn may be deleted, *do not access*
            scoped_lock lk( _mutex );
            _init.insert( conn );
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
            _init.erase( conn );
        }

        // protects _maps
        mongo::mutex _mutex;

        // a map from a connection into ChunkManager's sequence number for each namespace
        map<DBClientBase*, map<string,unsigned long long> > _map;
        set<DBClientBase*> _init;

    } connectionShardStatus;

    void resetShardVersion( DBClientBase * conn ) {
        connectionShardStatus.reset( conn );
    }

    bool isVersionable( DBClientBase* conn ){
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

        assert( false );
        return NULL;
    }

    extern OID serverID;

    bool initShardVersion( DBClientBase& conn_in, BSONObj& result ){

        WriteBackListener::init( conn_in );

        DBClientBase* conn = getVersionable( &conn_in );
        assert( conn ); // errors thrown above

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

        // Conn may be deleted here - *do not access again* - css is an exception, since just uses ptr address
        connectionShardStatus.setInitialized( conn );
        conn = NULL;

        // HACK for backwards compatibility with v1.8.x, v2.0.0 and v2.0.1
        // Result is false, but will still initialize serverID and configdb
        // Not master does not initialize serverID and configdb, but we ignore since if the connection is not master,
        // we are not setting the shard version at all
        if( ! ok && ! result["errmsg"].eoo() && ( result["errmsg"].String() == "need to specify namespace"/* 2.0.1/2 */ ||
                                                  result["errmsg"].String() == "need to speciy namespace" /* 1.8 */ ||
                                                  result["errmsg"].String() == "not master" /* both */ ) )
        {
            ok = true;
        }

        LOG(3) << "initial sharding result : " << result << endl;

        return ok;

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

        DBClientBase* conn = getVersionable( &conn_in );
        assert(conn); // errors thrown above

        if( ! connectionShardStatus.isInitialized( conn ) ){
            BSONObj result;
            uassert( 15918, str::stream() << "cannot initialize version on shard " << conn->getServerAddress() << causedBy( result.toString() ), initShardVersion( *conn, result ) );
        }

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
            LOG(2) << "resetting shard version of " << ns << " on " << conn->getServerAddress() << ", " <<
                      ( ! isSharded ? "no longer sharded" :
                      ( ! manager ? "no chunk manager found" :
                                    "version is zero" ) ) << endl;
        }

        LOG(2) << " have to set shard version for conn: " << conn << " ns:" << ns
               << " my last seq: " << sequenceNumber << "  current: " << officialSequenceNumber
               << " version: " << version << " manager: " << manager.get()
               << endl;

        BSONObj result;
        // Save the server address, cannot access if fails
        string serverAddress = conn->getServerAddress();
        if ( setShardVersion( *conn , ns , version , authoritative , result ) ) {
            // success!
            LOG(1) << "      setShardVersion success: " << result << endl;
            connectionShardStatus.setSequence( conn , ns , officialSequenceNumber );
            return true;
        }

        // At this point, it is no longer safe to use the pointer to conn, we do not know its state
        conn = NULL;

        LOG(1) << "       setShardVersion failed!\n" << result << endl;

        if ( result["need_authoritative"].trueValue() )
            massert( 10428 ,  "need_authoritative set but in authoritative mode already" , ! authoritative );

        if ( ! authoritative ) {
            checkShardVersion( conn_in , ns , 1 , tryNumber + 1 );
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
                << "going to retry checkShardVersion host: " << serverAddress << " " << result << endl;
            sleepmillis( 10 * tryNumber );
            checkShardVersion( conn_in , ns , true , tryNumber + 1 );
            return true;
        }
        
        string errmsg = str::stream() << "setShardVersion failed host: " << serverAddress << " " << result;
        log() << "     " << errmsg << endl;
        massert( 10429 , errmsg , 0 );
        return true;
    }

}  // namespace mongo
