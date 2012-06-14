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

#include "mongo/s/balancer_policy.h"
#include "mongo/s/config.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"

namespace mongo {

    string TagRange::toString() const {
        return str::stream() << min << " -->> " << max << "  on  " << tag;
    }


    DistributionStatus::DistributionStatus( const ShardInfoMap& shardInfo,
                                            const ShardToChunksMap& shardToChunksMap )
        : _shardInfo( shardInfo ), _shardChunks( shardToChunksMap ) {
    }
        

    unsigned DistributionStatus::numberOfChunks( const string& shard ) const {
        ShardToChunksMap::const_iterator i = _shardChunks.find( shard );
        if ( i == _shardChunks.end() )
            return 0;
        return i->second.size();
    }

    string DistributionStatus::getShardRequiredToShed() const {
        string shard;
        unsigned maxChunks = 0;
        
        for ( ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i ) {

            if ( ! i->second.isSizeMaxed() && ! i->second.isDraining() )
                continue;

            unsigned myChunks = numberOfChunks( i->first );
            if ( myChunks == 0 )
                continue;

            if ( i->second.hasOpsQueued() ) {
                warning() << "want to shed load from " << i->first << " but can't because it has ops queued" << endl;
                continue;
            }
            
            if ( myChunks <= maxChunks )
                continue;
            
            shard = i->first;
            maxChunks = myChunks;
        }
        
        return shard;
    }


    string DistributionStatus::getBestReceieverShard( const string& tag ) const {
        string best;
        unsigned minChunks = numeric_limits<unsigned>::max();
        
        for ( ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i ) {
            
            if ( i->second.isSizeMaxed() || i->second.isDraining() || i->second.hasOpsQueued() )
                continue;
            
            if ( tag.size() > 0 && ! i->second.hasTag( tag ) )
                continue;

            unsigned myChunks = numberOfChunks( i->first );
            if ( myChunks >= minChunks )
                continue;
            
            best = i->first;
            minChunks = myChunks;
        }

        return best;
    }

    string DistributionStatus::getMostOverloadedShard() const {
        string worst;
        unsigned maxChunks = 0;

        for ( ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i ) {
            
            if ( i->second.hasOpsQueued() ) {
                // we can't move stuff off anyway
                continue;
            }
            
            unsigned myChunks = numberOfChunks( i->first );
            if ( myChunks <= maxChunks )
                continue;
            
            worst = i->first;
            maxChunks = myChunks;
        }
        
        return worst;
    }
    

    const vector<BSONObj>& DistributionStatus::getChunks( const string& shard ) const { 
        ShardToChunksMap::const_iterator i = _shardChunks.find(shard);
        verify( i != _shardChunks.end() );
        return i->second;
    }

    void DistributionStatus::addTagRange( const TagRange& range ) {
        _tagRanges.push_back( range );
        _allTags.insert( range.tag );
    }

    string DistributionStatus::getTagForChunk( const BSONObj& chunk ) const {
        // TODO, use a map or set or something
        for ( unsigned i=0 ; i<_tagRanges.size(); i++ ) {
            if ( chunk["min"].Obj() < _tagRanges[i].min )
                continue;
            if ( chunk["max"].Obj() > _tagRanges[i].max )
                continue;
            return _tagRanges[i].tag;
        }
        return "";
    }

    void DistributionStatus::dump() const {
        log() << "DistributionStatus" << endl;
        log() << "  shards" << endl;
        for ( ShardInfoMap::const_iterator i = _shardInfo.begin(); i != _shardInfo.end(); ++i ) {
            log() << "      " << i->first << "\t" << i->second.toString() << endl;
            ShardToChunksMap::const_iterator j = _shardChunks.find( i->first );
            verify( j != _shardChunks.end() );
            const vector<BSONObj>& v = j->second;
            for ( unsigned x = 0; x < v.size(); x++ )
                log() << "          " << v[x] << endl;
        }
        
        if ( _tagRanges.size() > 0 ) {
            log() << " tag ranges" << endl;
            
            for ( unsigned i=0; i<_tagRanges.size(); i++ ) 
                log() << "     " << _tagRanges[i].toString() << endl;
        }
    }

    MigrateInfo* BalancerPolicy::balance( const string& ns,
                                          const DistributionStatus& distribution, 
                                          int balancedLastTime ) {


        // 1) check for shards that policy require to us to move off of
        //    draining, maxSize
        // 2) then we look for "local" in balance for a tag
        // 3) then we look for global in balance
        
        // ----

        // 1) check things we have to move
        string shardWeHaveToDrain = distribution.getShardRequiredToShed();
        if ( shardWeHaveToDrain.size() > 0 ) {
            return shed( ns, distribution, shardWeHaveToDrain );
        }
        

        // 2) lets look at tags
        if ( distribution.tags().size() > 0 ) {
            log() << "we've got tags!" << endl;
        }


        // 3)
        pair<string,unsigned> min("",numeric_limits<unsigned>::max());
        pair<string,unsigned> max("",0);
        
        min.first = distribution.getBestReceieverShard( "" );
        if ( min.first.size() == 0 ) {
            log() << "no available shards to take chunks" << endl;
            return NULL;
        }
        min.second = distribution.numberOfChunks( min.first );

        max.first = distribution.getMostOverloadedShard();
        if ( max.first.size() == 0 ) {
            LOG(1) << "no shards overloaded" << endl;
            return NULL;
        }
        max.second = distribution.numberOfChunks( max.first );

        LOG(1) << "collection : " << ns << endl;
        LOG(1) << "donor      : " << max.second << " chunks on " << max.first << endl;
        LOG(1) << "receiver   : " << min.second << " chunks on " << min.first << endl;

        // Solving imbalances takes a higher priority than draining shards. Many shards can
        // be draining at once but we choose only one of them to cater to per round.
        // Important to start balanced, so when there are few chunks any imbalance must be fixed.
        const int imbalance = max.second - min.second;
        int threshold = 8;
        if (balancedLastTime || max.second < 20) threshold = 2;
        else if (max.second < 80) threshold = 4;
        string from, to;
        if ( imbalance >= threshold ) {
            from = max.first;
            to = min.first;

        }
        else {
            // Everything is balanced here!
            return NULL;
        }

        return finishBalance( ns, distribution, from, to );
    }

    MigrateInfo* BalancerPolicy::shed( const string& ns,
                                       const DistributionStatus& distribution,
                                       const string& from ) {
        // need to find a chunk on from that i can move somewhere good
        // but I HAVE to move chunk if at all possible 
        // so can overload someone, but can't violate a policy
        
        const vector<BSONObj>& chunks = distribution.getChunks( from );

        // since we have to move all chunks, lets just do in order
        for ( unsigned i=0; i<chunks.size(); i++ ) {
            BSONObj chunkToMove = distribution.getChunks( from )[i];
            string tag = distribution.getTagForChunk( chunkToMove );
            string to = distribution.getBestReceieverShard( tag );
            
            if ( to.size() == 0 ) {
                warning() << "want to move chunk: " << chunkToMove << "(" << tag << ") "
                          << "from " << from << " but can't find anywhere to put it" << endl;
                continue;
            }
            
            log() << "going to move " << chunkToMove << " from " << from << "(" << tag << ")" << " to " << to << endl;
            
            return new MigrateInfo( ns, to, from, chunkToMove.getOwned() );
        }
        warning() << "can't find any chunk to move from: " << from << endl;
        return NULL;
    }


    MigrateInfo* BalancerPolicy::finishBalance( const string& ns,
                                                const DistributionStatus& distribution, 
                                                const string& from,
                                                const string& to ) {

        const vector<BSONObj>& chunksFrom = distribution.getChunks( from );
        const vector<BSONObj>& chunksTo = distribution.getChunks( to );
        BSONObj chunkToMove = pickChunk( chunksFrom , chunksTo );
        log() << "chose [" << from << "] to [" << to << "] " << chunkToMove << endl;

        return new MigrateInfo( ns, to, from, chunkToMove.getOwned() );
    }


    BSONObj BalancerPolicy::pickChunk( const vector<BSONObj>& from, const vector<BSONObj>& to ) {
        // It is possible for a donor ('from') shard to have less chunks than a receiver one ('to')
        // if the donor is in draining mode.

        if ( to.size() == 0 )
            return from[0];

        if ( from[0]["min"].Obj().woCompare( to[to.size()-1]["max"].Obj() , BSONObj() , false ) == 0 )
            return from[0];

        if ( from[from.size()-1]["max"].Obj().woCompare( to[0]["min"].Obj() , BSONObj() , false ) == 0 )
            return from[from.size()-1];

        return from[0];
    }

    ShardInfo::ShardInfo( long long maxSize, long long currSize, bool draining, bool opsQueued )
        : _maxSize( maxSize ), 
          _currSize( currSize ),
          _draining( draining ),
          _hasOpsQueued( opsQueued ) {
    }

    ShardInfo::ShardInfo()
        : _maxSize( 0 ), 
          _currSize( 0 ),
          _draining( false ),
          _hasOpsQueued( false ) {
    }

    void ShardInfo::addTag( const string& tag ) {
        _tags.insert( tag );
    }


    bool ShardInfo::isSizeMaxed() const {
        if ( _maxSize == 0 || _currSize == 0 )
            return false;
        
        return _currSize >= _maxSize;
    }

    string ShardInfo::toString() const {
        StringBuilder ss;
        ss << " maxSize: " << _maxSize;
        ss << " currSize: " << _currSize;
        ss << " draining: " << _draining;
        ss << " hasOpsQueued: " << _hasOpsQueued;
        return ss.str();
    }

    string ChunkInfo::toString() const {
        StringBuilder buf;
        buf << " min: " << min;
        buf << " max: " << min;
        return buf.str();
    }

}  // namespace mongo
