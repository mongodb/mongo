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

#include "pch.h"
#include "../client/dbclient.h"
#include "config.h"

#include "balancer_policy.h"

namespace mongo {

    BalancerPolicy::ChunkInfo* BalancerPolicy::balance( const string& ns, 
                                                        const map< string, BSONObj>& shardLimitsMap,  
                                                        const map< string,vector<BSONObj> >& shardToChunksMap, 
                                                        int balancedLastTime ){
        
        pair<string,unsigned> min("",9999999);
        pair<string,unsigned> max("",0);
        
        for ( map< string,vector<BSONObj> >::const_iterator i=shardToChunksMap.begin(); i!=shardToChunksMap.end(); ++i ){
            const string& shard = i->first;
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
        
        if( (int)( max.second - min.second) < ( balancedLastTime ? 2 : 8 ) )
            return NULL;

        const string from = max.first;
        const string to = min.first;
        const vector<BSONObj>& chunksFrom = shardToChunksMap.find( from )->second;
        const vector<BSONObj>& chunksTo = shardToChunksMap.find( to )->second;  
        BSONObj chunkToMove = pickChunk( chunksFrom , chunksTo );

        log() << "balancer: chose chunk from [" << from << "] to [" << to << "] " << chunkToMove << endl;        

        return new ChunkInfo( ns, to, from, chunkToMove );
    }

    BSONObj BalancerPolicy::pickChunk( const vector<BSONObj>& from, const vector<BSONObj>& to ){
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
