// @file d_state.cpp

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

    class ChunkMatcher {
    public:
        ChunkMatcher( ShardChunkVersion version , const BSONObj& key );
        ~ChunkMatcher() {}

        bool belongsToMe( const BSONObj& obj ) const;

        void addRange( const BSONObj& min , const BSONObj& max );
        void addChunk( const BSONObj& min , const BSONObj& max );

        //void splitChunk( const BSONObj& min , const BSONObj& max , const BSONObj& middle );
        //void removeChunk( const BSONObj& min , const BSONObj& max );
        
        // accessors

        ShardChunkVersion getVersion() const { return _version; } 

    private:
        // highest ShardChunkVersion for which this ChunkMatcher's information is accurate
        const ShardChunkVersion _version;

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
