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

    class ClientCursor;
    class DBClientCursorInterface;
    /**
     * Controls the boundaries of all the chunks for a given collection that live in this shard.
     *
     * ShardChunkManager instances never change after construction. There are methods provided that would generate a
     * new manager if new chunks are added, subtracted, or split.
     *
     * TODO
     *   The responsibility of maintaining the version for a shard is still shared between this class and its caller. The
     *   manager does check corner cases (e.g. cloning out the last chunk generates a manager with version 0) but ultimately
     *   still cannot be responsible to set all versions. Currently, they are a function of the global state as opposed to
     *   the per-shard one.
     */
    class ShardChunkManager : public boost::noncopyable {
    public:

        /**
         * Loads the ShardChunkManager with all boundaries for chunks of a given collection that live in an given
         * shard.
         *
         * @param configServer name of the server where the configDB currently is. Can be empty to indicate
         *        that the configDB is running locally
         * @param ns namespace for the collections whose chunks we're interested
         * @param shardName name of the shard that this chunk matcher should track
         *
         * This constructor throws if collection is dropped/malformed and on connectivity errors
         */
        ShardChunkManager( const string& configServer , const string& ns , const string& shardName );

        /**
         * Same as the regular constructor but used in unittest (no access to configDB required).
         *
         * @param collectionDoc simulates config.collection's entry for one colleciton
         * @param chunksDocs simulates config.chunks' entries for one collection's shard
         */
        ShardChunkManager( const BSONObj& collectionDoc , const BSONArray& chunksDoc );

        ~ShardChunkManager() {}

        /**
         * Generates a new manager based on 'this's state minus a given chunk.
         *
         * @param min max chunk boundaries for the chunk to subtract
         * @param version that the resulting manager should be at. The version has to be higher than the current one.
         *        When cloning away the last chunk, verstion must be 0.
         * @return a new ShardChunkManager, to be owned by the caller
         */
        ShardChunkManager* cloneMinus( const BSONObj& min , const BSONObj& max , const ShardChunkVersion& version );

        /**
         * Generates a new manager based on 'this's state plus a given chunk.
         *
         * @param min max chunk boundaries for the chunk to add
         * @param version that the resulting manager should be at. It can never be 0, though (see CloneMinus).
         * @return a new ShardChunkManager, to be owned by the caller
         */
        ShardChunkManager* clonePlus( const BSONObj& min , const BSONObj& max , const ShardChunkVersion& version );

        /**
         * Generates a new manager by splitting an existing chunk at one or more points.
         *
         * @param min max boundaries of chunk to be split
         * @param splitKeys points to split original chunk at
         * @param version to be used in first chunk. The subsequent chunks would increment the minor version.
         * @return a new ShardChunkManager with the chunk split, to be owned by the caller
         */
        ShardChunkManager* cloneSplit( const BSONObj& min , const BSONObj& max , const vector<BSONObj>& splitKeys ,
                                       const ShardChunkVersion& version );

        /**
         * Checks whether a document belongs to this shard.
         *
         * @param obj document containing sharding keys (and, optionally, other attributes)
         * @return true if shards hold the object
         */
        bool belongsToMe( const BSONObj& obj ) const;

        /**
         * Checks whether a document belongs to this shard.
         *
         * @param obj document containing sharding keys (and, optionally, other attributes)
         * @return true if shards hold the object
         */
        bool belongsToMe( ClientCursor* cc ) const;
        
        /**
         * Given a chunk's min key (or empty doc), gets the boundary of the chunk following that one (the first).
         *
         * @param lookupKey is the min key for a previously obtained chunk or the empty document
         * @param foundMin IN/OUT min for chunk following the one starting at lookupKey
         * @param foundMax IN/OUT max for the above chunk
         * @return true if the chunk returned is the last one
         */
        bool getNextChunk( const BSONObj& lookupKey, BSONObj* foundMin , BSONObj* foundMax ) const;

        // accessors

        ShardChunkVersion getVersion() const { return _version; }
        BSONObj getKey() const { return _key.getOwned(); }
        unsigned getNumChunks() const { return _chunksMap.size(); }

        string toString() const;
    private:

        /**
         * @same as belongsToMe to but key has to be the shard key
         */
        bool _belongsToMe( const BSONObj& key ) const;

        
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
        void _fillChunks( DBClientCursorInterface* cursor );
        void _fillRanges();

        /** throws if the exact chunk is not in the chunks' map */
        void _assertChunkExists( const BSONObj& min , const BSONObj& max ) const;

        /** can only be used in the cloning calls */
        ShardChunkManager() {}
    };

    typedef shared_ptr<ShardChunkManager> ShardChunkManagerPtr;

}  // namespace mongo
