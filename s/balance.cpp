// balance.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "../db/jsobj.h"
#include "../db/cmdline.h"

#include "balance.h"
#include "server.h"
#include "shard.h"
#include "config.h"
#include "chunk.h"

namespace mongo {
    
    Balancer balancer;

    Balancer::Balancer(){
        _balancedLastTime = 0;
    }

    bool Balancer::shouldIBalance( DBClientBase& conn ){
        BSONObj x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log(2) << "balancer: " << x << endl;
        
        if ( ! x.isEmpty() ){
            if ( x["who"].String() == _myid ){
                log(2) << "balancer: i'm the current balancer" << endl;
                return true;
            }
            
            BSONObj other = conn.findOne( ShardNS::mongos , x["who"].wrap( "_id" ) );
            massert( 13125 , (string)"can't find mongos: " + x["who"].String() , ! other.isEmpty() );

            int secsSincePing = (int)(( jsTime() - other["ping"].Date() ) / 1000 );
            log(2) << "current balancer is: " << other << " ping delay(secs): " << secsSincePing << endl;
            
            if ( secsSincePing < ( 60 * 10 ) ){
                return false;
            }
            
            log() << "balancer: going to take over" << endl;
            // we want to take over, so fall through to below
        }
        
        OID hack;
        hack.init();
        
        BSONObjBuilder updateQuery;
        updateQuery.append( "_id" , "balancer" );
        if ( x["x"].type() )
            updateQuery.append( x["x"] );
        else
            updateQuery.append( "x" , BSON( "$exists" << false ) );
        
        conn.update( ShardNS::settings , 
                     updateQuery.obj() ,
                     BSON( "$set" << BSON( "who" << _myid << "x" << hack ) ) ,
                     true );
        
        x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log() << "balancer: after update: " << x << endl;
        return _myid == x["who"].String() && hack == x["x"].OID();
    }
    
    int Balancer::balance( DBClientBase& conn ){
        log(1) << "i'm going to do some balancing" << endl;
        
        int numBalanced = 0;
        
        auto_ptr<DBClientCursor> cursor = conn.query( ShardNS::database , BSON( "partitioned" << true ) );
        while ( cursor->more() ){
            BSONObj db = cursor->next();

            // A database may be partitioned but not yet have a sharded collection. 
            // 'cursor' will point to docs that do not contain the "sharded" key. Since 
            // there'd be nothing to balance, we want to skip those here.

            BSONElement shardedColls = db["sharded"];
            if ( shardedColls.eoo() ){
                log(2) << "balancer: skipping database with no sharded collection (" 
                      << db["_id"].str() << ")" << endl;
                continue;
            }
            
            BSONObjIterator i( shardedColls.Obj() );
            while ( i.more() ){
                BSONElement e = i.next();
                BSONObj data = e.Obj().getOwned();
                string ns = e.fieldName();
                bool didAnything = balance( conn , ns , data );
                if ( didAnything )
                    numBalanced++;
            }
        }

        return numBalanced;
    }

    bool Balancer::balance( DBClientBase& conn , const string& ns , const BSONObj& data ){
        log(3) << "balancer: balance(" << ns << ")" << endl;

        map< string,vector<BSONObj> > shards;
        {
            auto_ptr<DBClientCursor> cursor = conn.query( ShardNS::chunk , QUERY( "ns" << ns ).sort( "min" ) );
            while ( cursor->more() ){
                BSONObj chunk = cursor->next();
                vector<BSONObj>& chunks = shards[chunk["shard"].String()];
                chunks.push_back( chunk.getOwned() );
            }
        }
        
        if ( shards.size() == 0 )
            return false;
        
        {
            vector<Shard> all;
            Shard::getAllShards( all );
            for ( vector<Shard>::iterator i=all.begin(); i!=all.end(); ++i ){
                // this just makes sure there is an entry in the map for every shard
                Shard s = *i;
                shards[s.getName()].size();
            }
        }

        pair<string,unsigned> min("",9999999);
        pair<string,unsigned> max("",0);
        
        for ( map< string,vector<BSONObj> >::iterator i=shards.begin(); i!=shards.end(); ++i ){
            string shard = i->first;
            unsigned size = i->second.size();
            
            if ( size < min.second ){
                min.first = shard;
                min.second = size;
            }
            
            if ( size > max.second ){
                max.first = shard;
                max.second = size;
            }
        }
        
        log(4) << "min: " << min.first << "\t" << min.second << endl;
        log(4) << "max: " << max.first << "\t" << max.second << endl;
        
        if( (int)( max.second - min.second) < ( _balancedLastTime ? 2 : 8 ) )
            return false;

        string from = max.first;
        string to = min.first;

        BSONObj chunkToMove = pickChunk( shards[from] , shards[to] );
        log() << "balancer: move a chunk from [" << from << "] to [" << to << "] " << chunkToMove << endl;        

        DBConfig * cfg = grid.getDBConfig( ns );
        assert( cfg );
        
        ChunkManagerPtr cm = cfg->getChunkManager( ns );
        assert( cm );
        
        ChunkPtr c = cm->findChunk( chunkToMove["min"].Obj() );
        if ( c->getMin().woCompare( chunkToMove["min"].Obj() ) ){
            // likely a split happened somewhere
            cm = cfg->getChunkManager( ns , true );
            assert( cm );
            c = cm->findChunk( chunkToMove["min"].Obj() );
            if ( c->getMin().woCompare( chunkToMove["min"].Obj() ) ){
                log() << "balancer: chunk mismatch after reload, ignoring will retry issue cm: " << c->getMin() << " min: " << chunkToMove["min"].Obj() << endl;
                return false;
            }
        }
        
        string errmsg;
        if ( c->moveAndCommit( Shard::make( to ) , errmsg ) ){
            return true;
        }

        log() << "balancer: MOVE FAILED **** " << errmsg << "\n"
              << "  from: " << from << " to: " << to << " chunk: " << chunkToMove << endl;
        return false;
    }
    
    BSONObj Balancer::pickChunk( vector<BSONObj>& from, vector<BSONObj>& to ){
        assert( from.size() > to.size() );
        
        if ( to.size() == 0 )
            return from[0];
        
        if ( from[0]["min"].Obj().woCompare( to[to.size()-1]["max"].Obj() , BSONObj() , false ) == 0 )
            return from[0];

        if ( from[from.size()-1]["max"].Obj().woCompare( to[0]["min"].Obj() , BSONObj() , false ) == 0 )
            return from[from.size()-1];

        return from[0];
    }
    
    void Balancer::ping(){
        assert( _myid.size() && _started );
        try {
            ScopedDbConnection conn( configServer.getPrimary() );
            ping( conn.conn() );
            conn.done();
        }
        catch ( std::exception& e ){
            log() << "bare ping failed: " << e.what() << endl;
        }
        
    }

    void Balancer::ping( DBClientBase& conn ){
        WriteConcern w = conn.getWriteConcern();
        conn.setWriteConcern( W_NONE );

        conn.update( ShardNS::mongos , 
                      BSON( "_id" << _myid ) , 
                      BSON( "$set" << BSON( "ping" << DATENOW << "up" << (int)(time(0)-_started) ) ) , 
                      true );

        conn.setWriteConcern( w);
    }
    
    bool Balancer::checkOIDs(){
        vector<Shard> all;
        Shard::getAllShards( all );
        
        map<int,Shard> oids;
        
        for ( vector<Shard>::iterator i=all.begin(); i!=all.end(); ++i ){
            Shard s = *i;
            BSONObj f = s.runCommand( "admin" , "features" );
            if ( f["oidMachine"].isNumber() ){
                int x = f["oidMachine"].numberInt();
                if ( oids.count(x) == 0 ){
                    oids[x] = s;
                }
                else {
                    log() << "error: 2 machines have " << x << " as oid machine piece " << s.toString() << " and " << oids[x].toString() << endl;
                    s.runCommand( "admin" , BSON( "features" << 1 << "oidReset" << 1 ) );
                    oids[x].runCommand( "admin" , BSON( "features" << 1 << "oidReset" << 1 ) );
                    return false;
                }
            }
            else {
                log() << "warning: oidMachine not set on: " << s.toString() << endl;
            }
        }
        return true;
    }

    void Balancer::run(){

        { // init stuff, don't want to do at static init
            StringBuilder buf;
            buf << ourHostname << ":" << cmdLine.port;
            _myid = buf.str();
            log(1) << "balancer myid: " << _myid << endl;
            
            _started = time(0);
        }
        
        ping();
        checkOIDs();

        while ( ! inShutdown() ){
            sleepsecs( 10 );
            
            try {
                ScopedDbConnection conn( configServer.getPrimary() );
                ping( conn.conn() );
                
                if ( ! checkOIDs() ){
                    uassert( 13258 , "oids broken after resetting!" , checkOIDs() );
                }
                                    
                int numBalanced = 0;
                if ( shouldIBalance( conn.conn() ) ){
                    numBalanced = balance( conn.conn() );
                }
                
                conn.done();
                _balancedLastTime = numBalanced;
            }
            catch ( std::exception& e ){
                log() << "caught exception while doing balance: " << e.what() << endl;
                continue;
            }
            
        }
    }

}
