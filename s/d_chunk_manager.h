// @file d_chunk_manager.h

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
    class ShardChunkManager {
    public:

        /**
         * Loads the ShardChunkManager with all boundaries for chunks of a given collection that live in an given
         * shard
         *
         * @param configServer name of the server where the configDB currently is. Can be empty to indicate
         *        that the configDB is running locally
         * @param ns namespace for the collections whose chunks we're interested
         * @param shardName name of the shard that this chunk matcher should track
         *
         * This constructor throws on connectivity errors
         */
        ShardChunkManager( const string& configServer , const string& ns , const string& shardName );

        /**
         * Same as the regular constructor but used in unittest (no access to configDB required)
         *
         * @param collectionDoc simulates config.collection's entry for one colleciton
         * @param chunksDocs simulates config.chunks' entries for one collection's shard
         */
        ShardChunkManager( const BSONObj& collectionDoc , const BSONArray& chunksDoc );
        
        ~ShardChunkManager() {}

        bool belongsToMe( const BSONObj& obj ) const;

        // accessors

        ShardChunkVersion getVersion() const { return _version; } 

    private:
        // highest ShardChunkVersion for which this ShardChunkManager's information is accurate
        ShardChunkVersion _version;

        // key pattern for chunks under this range
        BSONObj _key;

        // a map from a min key into the chunk's (or range's) max boundary
        typedef map< BSONObj, BSONObj , BSONObjCmp > RangeMap;
        RangeMap _chunksMap;

        // a map from a min key into a range or continguous chunks
        // redundant but we expect high chunk continguity, expecially in small installations
        RangeMap _rangesMap;

        /** constructors helpers */
        void _fillCollectionKey( const BSONObj& collectionDoc );
        void _fillChunkState( DBClientCursorInterface* cursor );
    };

    typedef shared_ptr<ShardChunkManager> ShardChunkManagerPtr;
    
}  // namespace mongo
