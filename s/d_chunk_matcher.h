// @file d_chunk_matcher.h

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

#pragma once

#include "../pch.h"
#include "../db/jsobj.h"
#include "util.h"

namespace mongo {

    /**
     * Controls the boundaries of all the chunks for a given collection that live in this shard.
     */
    class ChunkMatcher {
    public:

        /**
         * Loads the ChunkMatcher with all boundaries for chunks of a given collection that live in an given
         * shard
         *
         * @param configServer name of the server where the configDB currently is. Can be empty to indicate
         *        that the configDB is running locally
         * @param ns namespace for the collections whose chunks we're interested
         * @param shardName name of the shard that this chunk matcher should track
         *
         * This constructor throws on connectivity errors
         */
        ChunkMatcher( const string& configServer , const string& ns , const string& shardName );

        ~ChunkMatcher() {}

        bool belongsToMe( const BSONObj& obj ) const;

        //void splitChunk( const BSONObj& min , const BSONObj& max , const BSONObj& middle );
        //void removeChunk( const BSONObj& min , const BSONObj& max );
        
        // accessors

        ShardChunkVersion getVersion() const { return _version; } 

    private:
        // highest ShardChunkVersion for which this ChunkMatcher's information is accurate
        ShardChunkVersion _version;

        // key pattern for chunks under this range
        BSONObj _key;

        // a map from a min key into the chunk boundaries
        typedef map<BSONObj,pair<BSONObj,BSONObj>,BSONObjCmp> RangeMap;
        RangeMap _chunksMap;

        // a map from a min key into a range or continguous chunks
        // redundant but we expect high chunk continguity, expecially in small installations
        RangeMap _rangesMap;
    };

    typedef shared_ptr<ChunkMatcher> ChunkMatcherPtr;
    
}  // namespace mongo
