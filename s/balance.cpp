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

#include "stdafx.h"

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
    }

    bool Balancer::shouldIBalance( DBClientBase& conn ){
        BSONObj x = conn.findOne( ShardNS::settings , BSON( "_id" << "balancer" ) );
        log(3) << "balancer: " << x << endl;
        
        if ( ! x.isEmpty() ){
            if ( x["who"].String() == _myid ){
                log(3) << "balancer: i'm the current balancer" << endl;
                return true;
            }
            
            BSONObj other = conn.findOne( ShardNS::mongos , x["who"].wrap( "_id" ) );
            massert( 13125 , (string)"can't find mongos: " + x["who"].String() , ! other.isEmpty() );

            int secsSincePing = (int)(( jsTime() - other["ping"].Date() ) / 1000 );
            log(3) << "current balancer is: " << other << " ping delay(secs): " << secsSincePing << endl;
            
            if ( secsSincePing < ( 60 * 10 ) ){
                return false;
            }
            
            log(3) << "balancer: going to take over" << endl;
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
        log(3) << "balancer: after update: " << x << endl;
        return _myid == x["who"].String() && hack == x["x"].OID();
    }
    
    void Balancer::balance( DBClientBase& conn ){
        log() << "i'm going to do some balancing" << endl;
        
        auto_ptr<DBClientCursor> cursor = conn.query( ShardNS::database , BSON( "partitioned" << true ) );
        while ( cursor->more() ){
            BSONObj db = cursor->next();
            BSONObjIterator i( db["sharded"].Obj() );
            while ( i.more() ){
                BSONElement e = i.next();
                BSONObj data = e.Obj().getOwned();
                string ns = e.fieldName();
                balance( conn , ns , data );
            }
        }
    }

    void Balancer::balance( DBClientBase& conn , const string& ns , const BSONObj& data ){
        log(4) << "balancer: balance(" << ns << ")" << endl;

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
            return;

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
        
        log(6) << "min: " << min.first << "\t" << min.second << endl;
        log(6) << "max: " << max.first << "\t" << max.second << endl;
        
        if ( ( max.second - min.second ) < 10 )
            return;

        string from = max.first;
        string to = min.first;

        BSONObj chunkToMove = pickChunk( shards[from] , shards[to] );
        log(1) << "balancer: move a chunk from [" << from << "] to [" << to << "] " << chunkToMove << endl;        

        DBConfig * cfg = grid.getDBConfig( ns );
        assert( cfg );
        
        ChunkManager * cm = cfg->getChunkManager( ns );
        assert( cm );
        
        Chunk& c = cm->findChunk( chunkToMove["min"].Obj() );
        assert( c.getMin().woCompare( chunkToMove["min"].Obj() ) == 0 );
        
        string errmsg;
        if ( c.moveAndCommit( to , errmsg ) )
            return;

        log() << "balancer: MOVE FAILED **** " << errmsg << "\n"
              << "  from: " << from << " to: " << to << " chunk: " << chunkToMove << endl;
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
    

    void Balancer::run(){

        { // init stuff, don't want to do at static init
            StringBuilder buf;
            buf << ourHostname << ":" << cmdLine.port;
            _myid = buf.str();
            log(1) << "balancer myid: " << _myid << endl;
            
            _started = time(0);
        }


        while ( ! inShutdown() ){
            sleepsecs( 30 );
            
            try {
                ShardConnection conn( configServer.getPrimary() );
                conn->update( ShardNS::mongos , 
                              BSON( "_id" << _myid ) , 
                              BSON( "$set" << BSON( "ping" << DATENOW << "up" << (int)(time(0)-_started) ) ) , 
                              true );
                
                if ( shouldIBalance( conn.conn() ) ){
                    balance( conn.conn() );
                }
                
                conn.done();
            }
            catch ( std::exception& e ){
                log() << "caught exception while doing mongos ping: " << e.what() << endl;
                continue;
            }
            
        }
    }

}
