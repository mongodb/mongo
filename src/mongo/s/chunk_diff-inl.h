// @file chunk_diff_impl.hpp

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

#include "mongo/logger/log_severity.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/util/concurrency/thread_name.h"

namespace mongo {

    template < class ValType, class ShardType >
    bool ConfigDiffTracker<ValType,ShardType>::
        isOverlapping( const BSONObj& min, const BSONObj& max )
    {
        RangeOverlap overlap = overlappingRange( min, max );

        return overlap.first != overlap.second;
    }

    template < class ValType, class ShardType >
    void ConfigDiffTracker<ValType,ShardType>::
        removeOverlapping( const BSONObj& min, const BSONObj& max )
    {
        verifyAttached();

        RangeOverlap overlap = overlappingRange( min, max );

        _currMap->erase( overlap.first, overlap.second );
    }

    template < class ValType, class ShardType >
    typename ConfigDiffTracker<ValType,ShardType>::RangeOverlap ConfigDiffTracker<ValType,ShardType>::
        overlappingRange( const BSONObj& min, const BSONObj& max )
    {
        verifyAttached();

        typename RangeMap::iterator low;
        typename RangeMap::iterator high;

        if( isMinKeyIndexed() ){
            // Returns the first chunk with a min key that is >= min - implies the
            // previous chunk cannot overlap min
            low = _currMap->lower_bound( min );
            // Returns the first chunk with a min key that is >= max - implies the
            // chunk does not overlap max
            high = _currMap->lower_bound( max );
        }
        else{
            // Returns the first chunk with a max key that is > min - implies that
            // the chunk overlaps min
            low = _currMap->upper_bound( min );
            // Returns the first chunk with a max key that is > max - implies that
            // the next chunk cannot not overlap max
            high = _currMap->upper_bound( max );
        }

        return RangeOverlap( low, high );
    }

    template<class ValType, class ShardType>
    int ConfigDiffTracker<ValType, ShardType>::calculateConfigDiff(const std::string& config) {
        verifyAttached();

        // Get the diff query required
        Query diffQuery = configDiffQuery();

        ScopedDbConnection conn(config, 30.0);

        try {

            // Open a cursor for the diff chunks
            std::auto_ptr<DBClientCursor> cursor = conn->query(
                    ChunkType::ConfigNS, diffQuery, 0, 0, 0, 0, ( kDebugBuild ? 2 : 1000000 ) );
            verify( cursor.get() );

            int diff = calculateConfigDiff( *cursor.get() );

            conn.done();

            return diff;
        }
        catch( DBException& e ){
            // Should only happen on connection errors
            e.addContext( str::stream() << "could not calculate config difference for ns " << _ns << " on " << config );
            throw;
        }
    }

    template < class ValType, class ShardType >
    int ConfigDiffTracker<ValType,ShardType>::
        calculateConfigDiff( DBClientCursorInterface& diffCursor )
    {
        verifyAttached();

        // Apply the chunk changes to the ranges and versions

        //
        // Overall idea here is to work in two steps :
        // 1. For all the new chunks we find, increment the maximum version per-shard and
        //    per-collection, and remove any conflicting chunks from the ranges
        // 2. For all the new chunks we're interested in (all of them for mongos, just chunks on the
        //    shard for mongod) add them to the ranges
        //

        std::vector<BSONObj> newTracked;
        // Store epoch now so it doesn't change when we change max
        OID currEpoch = _maxVersion->epoch();

        _validDiffs = 0;
        while( diffCursor.more() ){

            BSONObj diffChunkDoc = diffCursor.next();

            ChunkVersion chunkVersion = ChunkVersion::fromBSON(diffChunkDoc, ChunkType::DEPRECATED_lastmod());

            if( diffChunkDoc[ChunkType::min()].type() != Object ||
                diffChunkDoc[ChunkType::max()].type() != Object ||
                diffChunkDoc[ChunkType::shard()].type() != String )
            {
                using namespace logger;
                LogstreamBuilder(globalLogDomain(), getThreadName(), LogSeverity::Warning(),
                                 LogComponent::kSharding)
                          << "got invalid chunk document " << diffChunkDoc
                          << " when trying to load differing chunks" << std::endl;
                continue;
            }

            if( ! chunkVersion.isSet() || ! chunkVersion.hasEqualEpoch( currEpoch ) ){

                using namespace logger;
                LogstreamBuilder(globalLogDomain(), getThreadName(), LogSeverity::Warning(),
                                 LogComponent::kSharding)
                          << "got invalid chunk version " << chunkVersion << " in document " << diffChunkDoc
                          << " when trying to load differing chunks at version "
                          << ChunkVersion( _maxVersion->majorVersion(),
                                           _maxVersion->minorVersion(),
                                           currEpoch ) << std::endl;

                // Don't keep loading, since we know we'll be broken here
                return -1;
            }

            _validDiffs++;

            // Get max changed version and chunk version
            if( chunkVersion > *_maxVersion ) *_maxVersion = chunkVersion;

            // Chunk version changes
            ShardType shard = shardFor( diffChunkDoc[ChunkType::shard()].String() );
            typename std::map<ShardType, ChunkVersion>::iterator shardVersionIt = _maxShardVersions->find( shard );
            if( shardVersionIt == _maxShardVersions->end() || shardVersionIt->second < chunkVersion ){
                (*_maxShardVersions)[ shard ] = chunkVersion;
            }

            // See if we need to remove any chunks we are currently tracking b/c of this chunk's changes
            removeOverlapping(diffChunkDoc[ChunkType::min()].Obj(),
                              diffChunkDoc[ChunkType::max()].Obj());

            // Figure out which of the new chunks we need to track
            // Important - we need to actually own this doc, in case the cursor decides to getMore or unbuffer
            if( isTracked( diffChunkDoc ) ) newTracked.push_back( diffChunkDoc.getOwned() );
        }

        using namespace logger;
        if (globalLogDomain()->shouldLog(LogComponent::kSharding, LogSeverity::Debug(3))) {
            LogstreamBuilder(globalLogDomain(), getThreadName(), LogSeverity::Debug(3),
                             LogComponent::kSharding)
                << "found " << _validDiffs
                << " new chunks for collection " << _ns
                << " (tracking " << newTracked.size()
                << "), new version is " << *_maxVersion
                << std::endl;
        }

        for( std::vector<BSONObj>::iterator it = newTracked.begin(); it != newTracked.end(); it++ ){

            BSONObj chunkDoc = *it;

            // Important - we need to make sure we actually own the min and max here
            BSONObj min = chunkDoc[ChunkType::min()].Obj().getOwned();
            BSONObj max = chunkDoc[ChunkType::max()].Obj().getOwned();

            // Invariant enforced by sharding
            // It's possible to read inconsistent state b/c of getMore() and yielding, so we want
            // to detect as early as possible.
            // TODO: This checks for overlap, we also should check for holes here iff we're tracking
            // all chunks
            if( isOverlapping( min, max ) ) return -1;

            _currMap->insert( rangeFor( chunkDoc, min, max ) );
        }

        return _validDiffs;
    }

    template<class ValType, class ShardType>
    Query ConfigDiffTracker<ValType, ShardType>::configDiffQuery() const {

        verifyAttached();

        //
        // Basic idea behind the query is to find all the chunks $gte the current max version.
        // Currently, any splits and merges will increment the current max version.
        //

        BSONObjBuilder queryB;
        queryB.append(ChunkType::ns(), _ns);
        BSONObjBuilder tsBuilder(queryB.subobjStart(ChunkType::DEPRECATED_lastmod()));
        tsBuilder.appendTimestamp( "$gte", _maxVersion->toLong() );
        tsBuilder.done();
        BSONObj query = queryB.obj();

        //
        // NOTE: IT IS IMPORTANT FOR CONSISTENCY THAT WE SORT BY ASC VERSION, TO HANDLE
        // CURSOR YIELDING BETWEEN CHUNKS BEING MIGRATED.
        //
        // This ensures that changes to chunk version (which will always be higher) will always
        // come *after* our current position in the chunk cursor.
        //

        Query queryObj(query);
        queryObj.sort(BSON( "lastmod" << 1 ));

        using namespace logger;
        if (globalLogDomain()->shouldLog(LogComponent::kSharding, LogSeverity::Debug(2))) {
            LogstreamBuilder(globalLogDomain(), getThreadName(), LogSeverity::Debug(2),
                             LogComponent::kSharding)
                << "major version query from " << *_maxVersion << " and over "
                << _maxShardVersions->size() << " shards is " << queryObj << std::endl;
        }

        return queryObj;
    }

} // namespace mongo

