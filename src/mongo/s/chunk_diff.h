// @file chunk_diff.h

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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/connpool.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

    /**
     * This class manages and applies diffs from partial config server data reloads.  Because the
     * config data can be large, we want to update it in small parts, not all-at-once.  Once a
     * ConfigDiffTracker is created, the current config data is *attached* to it, and it is then
     * able to modify the data.
     *
     * The current form is templated b/c the overall algorithm is identical between mongos and
     * mongod, but the actual chunk maps used differ in implementation.  We don't want to copy the
     * implementation, because the logic is identical, or the chunk data, because that would be
     * slow for big clusters, so this is the alternative for now.
     * TODO: Standardize between mongos and mongod and convert template parameters to types.
     */
    template < class ValType, class ShardType >
    class ConfigDiffTracker {
    public:

        //
        // Useful composite types
        //

        // RangeMap stores ranges indexed by max or  min key
        typedef typename std::map<BSONObj, ValType, BSONObjCmp> RangeMap;

        // RangeOverlap is a pair of iterators defining a subset of ranges
        typedef typename std::pair< typename RangeMap::iterator, typename RangeMap::iterator> RangeOverlap;

        ConfigDiffTracker() { detach(); }
        virtual ~ConfigDiffTracker() {}

        /**
         * The tracker attaches to a set of ranges with versions, and uses a config server
         * connection to update these. Because the set of ranges and versions may be large, they
         * aren't owned by the tracker, they're just passed in and updated.  Therefore they must all
         * stay in scope while the tracker is working.
         *
         * TODO: Make a standard VersionedRange to encapsulate this info in both mongod and mongos?
         */
        void attach( const string& ns,
                     RangeMap& currMap,
                     ChunkVersion& maxVersion,
                     map<ShardType, ChunkVersion>& maxShardVersions )
        {
            _ns = ns;
            _currMap = &currMap;
            _maxVersion = &maxVersion;
            _maxShardVersions = &maxShardVersions;
            _validDiffs = 0;
        }

        void detach(){
            _ns = "";
            _currMap = NULL;
            _maxVersion = NULL;
            _maxShardVersions = NULL;
            _validDiffs = 0;
        }

        void verifyAttached() const { verify( _currMap ); verify( _maxVersion ); verify( _maxShardVersions ); }

        // Call after load for more information
        int numValidDiffs() const { return _validDiffs; }

    protected:

        //
        // To be implemented by subclasses
        //

        // Determines which chunks are actually being remembered by our RangeMap
        virtual bool isTracked( const BSONObj& chunkDoc ) const = 0;

        // Whether or not our RangeMap uses min or max keys
        virtual bool isMinKeyIndexed() const { return true; }

        ///
        /// Start adapter functions
        /// TODO: Remove these when able
        ///

        // If we're indexing on the min of the chunk bound, implement maxFrom (default)
        virtual BSONObj maxFrom( const ValType& max ) const { verify( false ); return BSONObj(); }
        // If we're indexing on the max of the chunk bound, implement minFrom
        virtual BSONObj minFrom( const ValType& max ) const { verify( false ); return BSONObj(); }

        virtual std::pair<BSONObj,ValType> rangeFor( const BSONObj& chunkDoc, const BSONObj& min, const BSONObj& max ) const = 0;
        virtual ShardType shardFor( const string& name ) const = 0;
        virtual string nameFrom( const ShardType& shard ) const = 0;

        ///
        /// End adapter functions
        ///

    public:

        // Whether or not a range exists in the min/max region
        bool isOverlapping( const BSONObj& min, const BSONObj& max );

        // Removes all ranges in the region from min/max
        void removeOverlapping( const BSONObj& min, const BSONObj& max );

        // Returns a subset of ranges overlapping the region min/max
        RangeOverlap overlappingRange( const BSONObj& min, const BSONObj& max );

        // Finds and applies the changes to a collection from the config server specified
        // Also includes minor version changes for particular major-version chunks if explicitly
        // specified.
        // Returns the number of diffs processed, or -1 if the diffs were inconsistent
        // Throws a DBException on connection errors
        int calculateConfigDiff( string config,
                                 const set<ChunkVersion>& extraMinorVersions = set<ChunkVersion>() );

        // Applies changes to the config data from a cursor passed in
        // Returns the number of diffs processed, or -1 if the diffs were inconsistent
        // Throws a DBException on connection errors
        int calculateConfigDiff( DBClientCursorInterface& diffCursor );

        // Returns the query needed to find new changes to a collection from the config server
        // Needed only if a custom connection is required to the config server
        Query configDiffQuery( const set<ChunkVersion>& extraMinorVersions = set<ChunkVersion>() ) const;

    private:

        string _ns;
        RangeMap* _currMap;
        ChunkVersion* _maxVersion;
        map<ShardType, ChunkVersion>* _maxShardVersions;

        // Store for later use
        int _validDiffs;

    };

} // namespace mongo

// Include template definition
// TODO: Convert to normal .cpp file when normalized
#include "chunk_diff.hpp"

