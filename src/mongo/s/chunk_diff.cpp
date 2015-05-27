/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_diff.h"

#include <boost/shared_ptr.hpp>

#include "mongo/client/dbclientinterface.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/util/log.h"

namespace mongo {

    template <class ValType, class ShardType>
    ConfigDiffTracker<ValType, ShardType>::ConfigDiffTracker() {
        _ns.clear();
        _currMap = NULL;
        _maxVersion = NULL;
        _maxShardVersions = NULL;
        _validDiffs = 0;
    }

    template <class ValType, class ShardType>
    ConfigDiffTracker<ValType, ShardType>::~ConfigDiffTracker() = default;

    template <class ValType, class ShardType>
    void ConfigDiffTracker<ValType, ShardType>::attach(const std::string& ns,
                                                       RangeMap& currMap,
                                                       ChunkVersion& maxVersion,
                                                       MaxChunkVersionMap& maxShardVersions) {
        _ns = ns;
        _currMap = &currMap;
        _maxVersion = &maxVersion;
        _maxShardVersions = &maxShardVersions;
        _validDiffs = 0;
    }

    template <class ValType, class ShardType>
    bool ConfigDiffTracker<ValType, ShardType>::isOverlapping(const BSONObj& min,
                                                              const BSONObj& max) {
        RangeOverlap overlap = overlappingRange(min, max);

        return overlap.first != overlap.second;
    }

    template <class ValType, class ShardType>
    void ConfigDiffTracker<ValType, ShardType>::removeOverlapping(const BSONObj& min,
                                                                  const BSONObj& max) {
        _assertAttached();

        RangeOverlap overlap = overlappingRange(min, max);

        _currMap->erase(overlap.first, overlap.second);
    }

    template <class ValType, class ShardType>
    typename ConfigDiffTracker<ValType, ShardType>::RangeOverlap
    ConfigDiffTracker<ValType, ShardType>::overlappingRange(const BSONObj& min,
                                                            const BSONObj& max) {
        _assertAttached();

        typename RangeMap::iterator low;
        typename RangeMap::iterator high;

        if (isMinKeyIndexed()) {
            // Returns the first chunk with a min key that is >= min - implies the
            // previous chunk cannot overlap min
            low = _currMap->lower_bound(min);

            // Returns the first chunk with a min key that is >= max - implies the
            // chunk does not overlap max
            high = _currMap->lower_bound(max);
        }
        else {
            // Returns the first chunk with a max key that is > min - implies that
            // the chunk overlaps min
            low = _currMap->upper_bound(min);

            // Returns the first chunk with a max key that is > max - implies that
            // the next chunk cannot not overlap max
            high = _currMap->upper_bound(max);
        }

        return RangeOverlap(low, high);
    }

    template<class ValType, class ShardType>
    int ConfigDiffTracker<ValType, ShardType>::calculateConfigDiff(
                                                    CatalogManager* catalogManager) {
        _assertAttached();

        // Get the diff query required
        Query diffQuery = configDiffQuery();

        try {
            std::vector<ChunkType> chunks;
            uassertStatusOK(catalogManager->getChunks(diffQuery, 0, &chunks));

            return calculateConfigDiff(chunks);
        }
        catch (DBException& e) {
            // Should only happen on connection errors
            e.addContext(str::stream() << "could not calculate config difference for ns " << _ns);
            throw;
        }
    }

    template<class ValType, class ShardType>
    int ConfigDiffTracker<ValType, ShardType>::calculateConfigDiff(
                                                    const std::vector<ChunkType>& chunks) {
        _assertAttached();

        // Apply the chunk changes to the ranges and versions
        //
        // Overall idea here is to work in two steps :
        // 1. For all the new chunks we find, increment the maximum version per-shard and
        //      per-collection, and remove any conflicting chunks from the ranges.
        // 2. For all the new chunks we're interested in (all of them for mongos, just chunks on
        //      the shard for mongod) add them to the ranges.

        std::vector<ChunkType> newTracked;

        // Store epoch now so it doesn't change when we change max
        OID currEpoch = _maxVersion->epoch();

        _validDiffs = 0;

        for (const ChunkType& chunk : chunks) {
            ChunkVersion chunkVersion = ChunkVersion::fromBSON(chunk.toBSON(),
                                                               ChunkType::DEPRECATED_lastmod());

            if (!chunkVersion.isSet() || !chunkVersion.hasEqualEpoch(currEpoch)) {
                warning() << "got invalid chunk version " << chunkVersion
                          << " in document " << chunk.toString()
                          << " when trying to load differing chunks at version "
                          << ChunkVersion(_maxVersion->majorVersion(),
                                          _maxVersion->minorVersion(),
                                          currEpoch);

                // Don't keep loading, since we know we'll be broken here
                return -1;
            }

            _validDiffs++;

            // Get max changed version and chunk version
            if (chunkVersion > *_maxVersion) {
                *_maxVersion = chunkVersion;
            }

            // Chunk version changes
            ShardType shard = shardFor(chunk.getShard());

            typename MaxChunkVersionMap::const_iterator shardVersionIt = _maxShardVersions->find(shard);
            if (shardVersionIt == _maxShardVersions->end() ||
                        shardVersionIt->second < chunkVersion) {
                (*_maxShardVersions)[shard] = chunkVersion;
            }

            // See if we need to remove any chunks we are currently tracking because of this
            // chunk's changes
            removeOverlapping(chunk.getMin(), chunk.getMax());

            // Figure out which of the new chunks we need to track
            // Important - we need to actually own this doc, in case the cursor decides to getMore
            // or unbuffer.
            if (isTracked(chunk)) {
                newTracked.push_back(chunk);
            }
        }

        LOG(3) << "found " << _validDiffs << " new chunks for collection " << _ns
               << " (tracking " << newTracked.size() << "), new version is " << *_maxVersion;

        for (const ChunkType& chunk : newTracked) {
            // Invariant enforced by sharding - it's possible to read inconsistent state due to
            // getMore and yielding, so we want to detect it as early as possible.
            //
            // TODO: This checks for overlap, we also should check for holes here iff we're
            // tracking all chunks.
            if (isOverlapping(chunk.getMin(), chunk.getMax())) {
                return -1;
            }

            _currMap->insert(rangeFor(chunk));
        }

        return _validDiffs;
    }

    template<class ValType, class ShardType>
    Query ConfigDiffTracker<ValType, ShardType>::configDiffQuery() const {
        _assertAttached();

        // Basic idea behind the query is to find all the chunks $gte the current max version.
        // Currently, any splits and merges will increment the current max version.
        BSONObjBuilder queryB;
        queryB.append(ChunkType::ns(), _ns);

        {
            BSONObjBuilder tsBuilder(queryB.subobjStart(ChunkType::DEPRECATED_lastmod()));
            tsBuilder.appendTimestamp("$gte", _maxVersion->toLong());
            tsBuilder.done();
        }

        // NOTE: IT IS IMPORTANT FOR CONSISTENCY THAT WE SORT BY ASC VERSION, IN ORDER TO HANDLE
        // CURSOR YIELDING BETWEEN CHUNKS BEING MIGRATED.
        //
        // This ensures that changes to chunk version (which will always be higher) will always
        // come *after* our current position in the chunk cursor.

        Query queryObj(queryB.obj());
        queryObj.sort(BSON("lastmod" << 1));

        LOG(2) << "major version query from " << *_maxVersion
               << " and over " << _maxShardVersions->size() << " shards is " << queryObj;

        return queryObj;
    }

    template <class ValType, class ShardType>
    void ConfigDiffTracker<ValType, ShardType>::_assertAttached() const {
        invariant(_currMap);
        invariant(_maxVersion);
        invariant(_maxShardVersions);
    }


    // Ensures that these instances of the template are compiled
    template class ConfigDiffTracker<BSONObj, std::string>;
    template class ConfigDiffTracker<boost::shared_ptr<Chunk>, std::string>;

} // namespace mongo
