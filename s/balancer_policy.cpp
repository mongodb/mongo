// balancer_policy.cpp

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
#include "../client/dbclient.h"
#include "config.h"

#include "balancer_policy.h"

namespace mongo {

    void BalancerPolicy::balance( DBClientBase& conn, vector<ChunkInfoPtr>* toBalance ){
        assert( toBalance );

        log(1) << "i'm going to do some balancing" << endl;        
        
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
                balance( conn , ns , data , toBalance );
            }
        }
    }

    void BalancerPolicy::balance( DBClientBase& conn , const string& ns , const BSONObj& data , vector<ChunkInfoPtr>* toBalance ){
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
            return;
        
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
            return;

        string from = max.first;
        string to = min.first;

        BSONObj chunkToMove = pickChunk( shards[from] , shards[to] );
        log() << "balancer: chose chunk from [" << from << "] to [" << to << "] " << chunkToMove << endl;        

        ChunkInfoPtr p ( new ChunkInfo( ns, to, from, chunkToMove ));
        toBalance->push_back( p );
    }

    BSONObj BalancerPolicy::pickChunk( vector<BSONObj>& from, vector<BSONObj>& to ){
        assert( from.size() > to.size() );
        
        if ( to.size() == 0 )
            return from[0];
        
        if ( from[0]["min"].Obj().woCompare( to[to.size()-1]["max"].Obj() , BSONObj() , false ) == 0 )
            return from[0];

        if ( from[from.size()-1]["max"].Obj().woCompare( to[0]["min"].Obj() , BSONObj() , false ) == 0 )
            return from[from.size()-1];

        return from[0];
    }    

}  // namespace mongo
