// @file strategy.cpp

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

#include "pch.h"

#include "../client/connpool.h"
#include "../db/commands.h"

#include "grid.h"
#include "request.h"
#include "server.h"
#include "writeback_listener.h"

#include "strategy.h"

namespace mongo {

    // ----- Strategy ------

    void Strategy::doWrite( int op , Request& r , const Shard& shard , bool checkVersion ){
        ShardConnection conn( shard , r.getns() );
        if ( ! checkVersion )
            conn.donotCheckVersion();
        else if ( conn.setVersion() ){
            conn.done();
            throw StaleConfigException( r.getns() , "doWRite" , true );
        }
        conn->say( r.m() );
        conn.done();
    }
    
    void Strategy::doQuery( Request& r , const Shard& shard ){
        try{
            ShardConnection dbcon( shard , r.getns() );
            DBClientBase &c = dbcon.conn();
            
            Message response;
            bool ok = c.call( r.m(), response);

            {
                QueryResult *qr = (QueryResult *) response.singleData();
                if ( qr->resultFlags() & ResultFlag_ShardConfigStale ){
                    dbcon.done();
                    throw StaleConfigException( r.getns() , "Strategy::doQuery" );
                }
            }

            uassert( 10200 , "mongos: error calling db", ok);
            r.reply( response , c.getServerAddress() );
            dbcon.done();
        }
        catch ( AssertionException& e ) {
            BSONObjBuilder err;
            e.getInfo().append( err );
            BSONObj errObj = err.done();
            replyToQuery(ResultFlag_ErrSet, r.p() , r.m() , errObj);
        }
    }
    
    void Strategy::insert( const Shard& shard , const char * ns , const BSONObj& obj ){
        ShardConnection dbcon( shard , ns );
        if ( dbcon.setVersion() ){
            dbcon.done();
            throw StaleConfigException( ns , "for insert" );
        }
        dbcon->insert( ns , obj );
        dbcon.done();
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

        map<DBClientBase*, map<string,unsigned long long> > _map;
        mongo::mutex _mutex;
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
    
    
}
