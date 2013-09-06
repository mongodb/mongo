/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/s/collection_metadata.h"

#include "mongo/bson/util/builder.h" // for StringBuilder
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    CollectionMetadata::CollectionMetadata() { }

    CollectionMetadata::~CollectionMetadata() { }

    CollectionMetadata* CollectionMetadata::cloneMigrate( const ChunkType& chunk,
                                                          const ChunkVersion& newShardVersion,
                                                          string* errMsg ) const {
        // The error message string is optional.
        string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // Check that we have the exact chunk that will be subtracted.
        if ( !rangeMapContains( _chunksMap, chunk.getMin(), chunk.getMax() ) ) {

            *errMsg = stream() << "cannot remove chunk "
                               << rangeToString( chunk.getMin(), chunk.getMax() )
                               << ", this shard does not contain the chunk";

            if ( rangeMapOverlaps( _chunksMap, chunk.getMin(), chunk.getMax() ) ) {

                RangeVector overlap;
                getRangeMapOverlap( _chunksMap, chunk.getMin(), chunk.getMax(), &overlap );

                *errMsg += stream() << " and it overlaps " << overlapToString( overlap );
            }

            warning() << *errMsg << endl;
            return NULL;
        }

        // If left with no chunks, check that the version is zero.
        if (_chunksMap.size() == 1) {
            if (newShardVersion.isSet()) {

                *errMsg = stream() << "cannot set shard version to non-zero value "
                                   << newShardVersion.toString() << " when removing last chunk "
                                   << rangeToString( chunk.getMin(), chunk.getMax() );

                warning() << *errMsg << endl;
                return NULL;
            }
        }
        // Can't move version backwards when subtracting chunks.  This is what guarantees that
        // no read or write would be taken once we subtract data from the current shard.
        else if (newShardVersion <= _shardVersion) {

            *errMsg = stream() << "cannot remove chunk "
                               << rangeToString( chunk.getMin(), chunk.getMax() )
                               << " because the new shard version " << newShardVersion.toString()
                               << " is not greater than the current shard version "
                               << _shardVersion.toString();

            warning() << *errMsg << endl;
            return NULL;
        }

        auto_ptr<CollectionMetadata> metadata( new CollectionMetadata );
        metadata->_keyPattern = this->_keyPattern;
        metadata->_keyPattern.getOwned();
        metadata->_pendingMap = this->_pendingMap;
        metadata->_chunksMap = this->_chunksMap;
        metadata->_chunksMap.erase( chunk.getMin() );
        metadata->_shardVersion = newShardVersion;
        metadata->_collVersion =
                newShardVersion > _collVersion ? newShardVersion : this->_collVersion;
        metadata->fillRanges();

        dassert(metadata->isValid());
        return metadata.release();
    }

    CollectionMetadata* CollectionMetadata::clonePlusChunk( const ChunkType& chunk,
                                                            const ChunkVersion& newShardVersion,
                                                            string* errMsg ) const {
        // The error message string is optional.
        string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // It is acceptable to move version backwards (e.g., undoing a migration that went bad
        // during commit) but only cloning away the last chunk may reset the version to 0.
        if (!newShardVersion.isSet()) {

            *errMsg = stream() << "cannot add chunk "
                               << rangeToString( chunk.getMin(), chunk.getMax() )
                               << " with zero shard version";

            warning() << *errMsg << endl;
            return NULL;
        }

        // Check that there isn't any chunk on the interval to be added.
        if ( rangeMapOverlaps( _chunksMap, chunk.getMin(), chunk.getMax() ) ) {

            RangeVector overlap;
            getRangeMapOverlap( _chunksMap, chunk.getMin(), chunk.getMax(), &overlap );

            *errMsg = stream() << "cannot add chunk "
                               << rangeToString( chunk.getMin(), chunk.getMax() )
                               << " because the chunk overlaps " << overlapToString( overlap );

            warning() << *errMsg << endl;
            return NULL;
        }

        auto_ptr<CollectionMetadata> metadata( new CollectionMetadata );
        metadata->_keyPattern = this->_keyPattern;
        metadata->_keyPattern.getOwned();
        metadata->_pendingMap = this->_pendingMap;
        metadata->_chunksMap = this->_chunksMap;
        metadata->_chunksMap.insert( make_pair( chunk.getMin().getOwned(),
                                                chunk.getMax().getOwned() ) );
        metadata->_shardVersion = newShardVersion;
        metadata->_collVersion =
                newShardVersion > _collVersion ? newShardVersion : this->_collVersion;
        metadata->fillRanges();

        dassert(metadata->isValid());
        return metadata.release();
    }

    CollectionMetadata* CollectionMetadata::cloneMinusPending( const ChunkType& pending,
                                                               string* errMsg ) const {
        // The error message string is optional.
        string dummy;
        if ( errMsg == NULL ) {
            errMsg = &dummy;
        }

        // Check that we have the exact chunk that will be subtracted.
        if ( !rangeMapContains( _pendingMap, pending.getMin(), pending.getMax() ) ) {

            *errMsg = stream() << "cannot remove pending chunk "
                               << rangeToString( pending.getMin(), pending.getMax() )
                               << ", this shard does not contain the chunk";

            if ( rangeMapOverlaps( _pendingMap, pending.getMin(), pending.getMax() ) ) {

                RangeVector overlap;
                getRangeMapOverlap( _pendingMap, pending.getMin(), pending.getMax(), &overlap );

                *errMsg += stream() << " and it overlaps " << overlapToString( overlap );
            }

            warning() << *errMsg << endl;
            return NULL;
        }

        auto_ptr<CollectionMetadata> metadata( new CollectionMetadata );
        metadata->_keyPattern = this->_keyPattern;
        metadata->_keyPattern.getOwned();
        metadata->_pendingMap = this->_pendingMap;
        metadata->_pendingMap.erase( pending.getMin() );
        metadata->_chunksMap = this->_chunksMap;
        metadata->_rangesMap = this->_rangesMap;
        metadata->_shardVersion = _shardVersion;
        metadata->_collVersion = _collVersion;

        dassert(metadata->isValid());
        return metadata.release();
    }

    CollectionMetadata* CollectionMetadata::clonePlusPending( const ChunkType& pending,
                                                              string* errMsg ) const {
        // The error message string is optional.
        string dummy;
        if ( errMsg == NULL ) {
            errMsg = &dummy;
        }

        if ( rangeMapOverlaps( _chunksMap, pending.getMin(), pending.getMax() ) ) {

            RangeVector overlap;
            getRangeMapOverlap( _chunksMap, pending.getMin(), pending.getMax(), &overlap );

            *errMsg = stream() << "cannot add pending chunk "
                               << rangeToString( pending.getMin(), pending.getMax() )
                               << " because the chunk overlaps " << overlapToString( overlap );

            warning() << *errMsg << endl;
            return NULL;
        }

        auto_ptr<CollectionMetadata> metadata( new CollectionMetadata );
        metadata->_keyPattern = this->_keyPattern;
        metadata->_keyPattern.getOwned();
        metadata->_pendingMap = this->_pendingMap;
        metadata->_chunksMap = this->_chunksMap;
        metadata->_rangesMap = this->_rangesMap;
        metadata->_shardVersion = _shardVersion;
        metadata->_collVersion = _collVersion;

        // If there are any pending chunks on the interval to be added this is ok, since pending
        // chunks aren't officially tracked yet and something may have changed on servers we do not
        // see yet.
        // We remove any chunks we overlap, the remote request starting a chunk migration must have
        // been authoritative.

        if ( rangeMapOverlaps( _pendingMap, pending.getMin(), pending.getMax() ) ) {

            RangeVector pendingOverlap;
            getRangeMapOverlap( _pendingMap, pending.getMin(), pending.getMax(), &pendingOverlap );

            warning() << "new pending chunk " << rangeToString( pending.getMin(), pending.getMax() )
                      << " overlaps existing pending chunks " << overlapToString( pendingOverlap )
                      << ", a migration may not have completed" << endl;

            for ( RangeVector::iterator it = pendingOverlap.begin(); it != pendingOverlap.end();
                    ++it ) {
                metadata->_pendingMap.erase( it->first );
            }
        }

        metadata->_pendingMap.insert( make_pair( pending.getMin(), pending.getMax() ) );

        dassert(metadata->isValid());
        return metadata.release();
    }

    CollectionMetadata* CollectionMetadata::cloneSplit( const ChunkType& chunk,
                                                        const vector<BSONObj>& splitKeys,
                                                        const ChunkVersion& newShardVersion,
                                                        string* errMsg ) const {
        // The error message string is optional.
        string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // The version required in both resulting chunks could be simply an increment in the
        // minor portion of the current version.  However, we are enforcing uniqueness over the
        // attributes <ns, version> of the configdb collection 'chunks'.  So in practice, a
        // migrate somewhere may force this split to pick up a version that has the major
        // portion higher than the one that this shard has been using.
        //
        // TODO drop the uniqueness constraint and tighten the check below so that only the
        // minor portion of version changes
        if (newShardVersion <= _shardVersion) {

            *errMsg = stream() << "cannot split chunk "
                               << rangeToString( chunk.getMin(), chunk.getMax() )
                               << ", new shard version "
                               << newShardVersion.toString()
                               << " is not greater than current version "
                               << _shardVersion.toString();

            warning() << *errMsg << endl;
            return NULL;
        }

        // Check that we have the exact chunk that will be subtracted.
        if ( !rangeMapContains( _chunksMap, chunk.getMin(), chunk.getMax() ) ) {

            *errMsg = stream() << "cannot split chunk "
                               << rangeToString( chunk.getMin(), chunk.getMax() )
                               << ", this shard does not contain the chunk";

            if ( rangeMapOverlaps( _chunksMap, chunk.getMin(), chunk.getMax() ) ) {

                RangeVector overlap;
                getRangeMapOverlap( _chunksMap, chunk.getMin(), chunk.getMax(), &overlap );

                *errMsg += stream() << " and it overlaps " << overlapToString( overlap );
            }

            warning() << *errMsg << endl;
            return NULL;
        }

        // Check that the split key is valid
        for ( vector<BSONObj>::const_iterator it = splitKeys.begin(); it != splitKeys.end(); ++it )
        {
            if (!rangeContains(chunk.getMin(), chunk.getMax(), *it)) {

                *errMsg = stream() << "cannot split chunk "
                                   << rangeToString( chunk.getMin(), chunk.getMax() ) << " at key "
                                   << *it;

                warning() << *errMsg << endl;
                return NULL;
            }
        }

        auto_ptr<CollectionMetadata> metadata(new CollectionMetadata);
        metadata->_keyPattern = this->_keyPattern;
        metadata->_keyPattern.getOwned();
        metadata->_pendingMap = this->_pendingMap;
        metadata->_chunksMap = this->_chunksMap;
        metadata->_shardVersion = newShardVersion; // will increment 2nd, 3rd,... chunks below

        BSONObj startKey = chunk.getMin();
        for ( vector<BSONObj>::const_iterator it = splitKeys.begin(); it != splitKeys.end();
                ++it ) {
            BSONObj split = *it;
            metadata->_chunksMap[chunk.getMin()] = split.getOwned();
            metadata->_chunksMap.insert( make_pair( split.getOwned(), chunk.getMax().getOwned() ) );
            metadata->_shardVersion.incMinor();
            startKey = split;
        }

        metadata->_collVersion =
                metadata->_shardVersion > _collVersion ? metadata->_shardVersion : _collVersion;
        metadata->fillRanges();

        dassert(metadata->isValid());
        return metadata.release();
    }

    CollectionMetadata* CollectionMetadata::cloneMerge( const BSONObj& minKey,
                                                        const BSONObj& maxKey,
                                                        const ChunkVersion& newShardVersion,
                                                        string* errMsg ) const {

        if (newShardVersion <= _shardVersion) {

            *errMsg = stream() << "cannot merge range " << rangeToString( minKey, maxKey )
                               << ", new shard version " << newShardVersion.toString()
                               << " is not greater than current version "
                               << _shardVersion.toString();

            warning() << *errMsg << endl;
            return NULL;
        }

        RangeVector overlap;
        getRangeMapOverlap( _chunksMap, minKey, maxKey, &overlap );

        if ( overlap.empty() || overlap.size() == 1 ) {

            *errMsg = stream() << "cannot merge range " << rangeToString( minKey, maxKey )
                               << ( overlap.empty() ? ", no chunks found in this range" :
                                                      ", only one chunk found in this range" );

            warning() << *errMsg << endl;
            return NULL;
        }

        bool validStartEnd = true;
        bool validNoHoles = true;
        if ( overlap.begin()->first.woCompare( minKey ) != 0 ) {
            // First chunk doesn't start with minKey
            validStartEnd = false;
        }
        else if ( overlap.rbegin()->second.woCompare( maxKey ) != 0 ) {
            // Last chunk doesn't end with maxKey
            validStartEnd = false;
        }
        else {
            // Check that there are no holes
            BSONObj prevMaxKey = minKey;
            for ( RangeVector::iterator it = overlap.begin(); it != overlap.end(); ++it ) {
                if ( it->first.woCompare( prevMaxKey ) != 0 ) {
                    validNoHoles = false;
                    break;
                }
                prevMaxKey = it->second;
            }
        }

        if ( !validStartEnd || !validNoHoles ) {

            *errMsg = stream() << "cannot merge range " << rangeToString( minKey, maxKey )
                               << ", overlapping chunks " << overlapToString( overlap )
                               << ( !validStartEnd ? " do not have the same min and max key" :
                                                     " are not all adjacent" );

            warning() << *errMsg << endl;
            return NULL;
        }

        auto_ptr<CollectionMetadata> metadata( new CollectionMetadata );
        metadata->_keyPattern = this->_keyPattern;
        metadata->_keyPattern.getOwned();
        metadata->_pendingMap = this->_pendingMap;
        metadata->_chunksMap = this->_chunksMap;
        metadata->_rangesMap = this->_rangesMap;
        metadata->_shardVersion = newShardVersion;
        metadata->_collVersion =
                newShardVersion > _collVersion ? newShardVersion : this->_collVersion;

        for ( RangeVector::iterator it = overlap.begin(); it != overlap.end(); ++it ) {
            metadata->_chunksMap.erase( it->first );
        }

        metadata->_chunksMap.insert( make_pair( minKey, maxKey ) );

        dassert(metadata->isValid());
        return metadata.release();
    }

    bool CollectionMetadata::keyBelongsToMe( const BSONObj& key ) const {
        // For now, collections don't move. So if the collection is not sharded, assume
        // the document with the given key can be accessed.
        if ( _keyPattern.isEmpty() ) {
            return true;
        }

        if ( _rangesMap.size() <= 0 ) {
            return false;
        }

        RangeMap::const_iterator it = _rangesMap.upper_bound( key );
        if ( it != _rangesMap.begin() ) it--;

        bool good = rangeContains( it->first, it->second, key );

#ifdef _DEBUG
        // Logs if in debugging mode and the point doesn't belong here.
        if ( !good ) {
            log() << "bad: " << key << " " << it->first << " " << key.woCompare( it->first ) << " "
                  << key.woCompare( it->second ) << endl;

            for ( RangeMap::const_iterator i = _rangesMap.begin(); i != _rangesMap.end(); ++i ) {
                log() << "\t" << i->first << "\t" << i->second << "\t" << endl;
            }
        }
#endif

        return good;
    }

    bool CollectionMetadata::keyIsPending( const BSONObj& key ) const {
        // If we aren't sharded, then the key is never pending (though it belongs-to-me)
        if ( _keyPattern.isEmpty() ) {
            return false;
        }

        if ( _pendingMap.size() <= 0 ) {
            return false;
        }

        RangeMap::const_iterator it = _pendingMap.upper_bound( key );
        if ( it != _pendingMap.begin() ) it--;

        bool isPending = rangeContains( it->first, it->second, key );
        return isPending;
    }

    bool CollectionMetadata::getNextChunk( const BSONObj& lookupKey, ChunkType* chunk ) const {

        RangeMap::const_iterator upperChunkIt = _chunksMap.upper_bound( lookupKey );
        RangeMap::const_iterator lowerChunkIt = upperChunkIt;
        if ( upperChunkIt != _chunksMap.begin() ) --lowerChunkIt;
        else lowerChunkIt = _chunksMap.end();

        if ( lowerChunkIt != _chunksMap.end() &&
             lowerChunkIt->second.woCompare( lookupKey ) > 0 ) {
            chunk->setMin( lowerChunkIt->first );
            chunk->setMax( lowerChunkIt->second );
            return true;
        }

        if ( upperChunkIt != _chunksMap.end() ) {
            chunk->setMin( upperChunkIt->first );
            chunk->setMax( upperChunkIt->second );
            return true;
        }

        return false;
    }

    BSONObj CollectionMetadata::toBSON() const {
        BSONObjBuilder bb;
        toBSON( bb );
        return bb.obj();
    }

    void CollectionMetadata::toBSONChunks( BSONArrayBuilder& bb ) const {

        if ( _chunksMap.empty() ) return;

        for (RangeMap::const_iterator it = _chunksMap.begin(); it != _chunksMap.end(); ++it ) {
            BSONArrayBuilder chunkBB( bb.subarrayStart() );
            chunkBB.append( it->first );
            chunkBB.append( it->second );
            chunkBB.done();
        }
    }

    void CollectionMetadata::toBSONPending( BSONArrayBuilder& bb ) const {

        if ( _pendingMap.empty() ) return;

        for (RangeMap::const_iterator it = _pendingMap.begin(); it != _pendingMap.end(); ++it ) {
            BSONArrayBuilder pendingBB( bb.subarrayStart() );
            pendingBB.append( it->first );
            pendingBB.append( it->second );
            pendingBB.done();
        }
    }

    void CollectionMetadata::toBSON( BSONObjBuilder& bb ) const {

        _collVersion.addToBSON( bb, "collVersion" );
        _shardVersion.addToBSON( bb, "shardVersion" );
        bb.append( "keyPattern", _keyPattern );

        BSONArrayBuilder chunksBB( bb.subarrayStart( "chunks" ) );
        toBSONChunks( chunksBB );
        chunksBB.done();

        BSONArrayBuilder pendingBB( bb.subarrayStart( "pending" ) );
        toBSONPending( pendingBB );
        pendingBB.done();
    }

    bool CollectionMetadata::getNextOrphanRange( const BSONObj& origLookupKey,
                                                 KeyRange* range ) const {

        if ( _keyPattern.isEmpty() ) return false;

        BSONObj lookupKey = origLookupKey;
        BSONObj maxKey = getMaxKey(); // so we don't keep rebuilding
        while ( lookupKey.woCompare( maxKey ) < 0 ) {

            RangeMap::const_iterator lowerChunkIt = _chunksMap.end();
            RangeMap::const_iterator upperChunkIt = _chunksMap.end();

            if ( !_chunksMap.empty() ) {
                upperChunkIt = _chunksMap.upper_bound( lookupKey );
                lowerChunkIt = upperChunkIt;
                if ( upperChunkIt != _chunksMap.begin() ) --lowerChunkIt;
                else lowerChunkIt = _chunksMap.end();
            }

            // If we overlap, continue after the overlap
            // TODO: Could optimize slightly by finding next non-contiguous chunk
            if ( lowerChunkIt != _chunksMap.end()
                && lowerChunkIt->second.woCompare( lookupKey ) > 0 ) {
                lookupKey = lowerChunkIt->second;
                continue;
            }

            RangeMap::const_iterator lowerPendingIt = _pendingMap.end();
            RangeMap::const_iterator upperPendingIt = _pendingMap.end();

            if ( !_pendingMap.empty() ) {

                upperPendingIt = _pendingMap.upper_bound( lookupKey );
                lowerPendingIt = upperPendingIt;
                if ( upperPendingIt != _pendingMap.begin() ) --lowerPendingIt;
                else lowerPendingIt = _pendingMap.end();
            }

            // If we overlap, continue after the overlap
            // TODO: Could optimize slightly by finding next non-contiguous chunk
            if ( lowerPendingIt != _pendingMap.end()
                && lowerPendingIt->second.woCompare( lookupKey ) > 0 ) {
                lookupKey = lowerPendingIt->second;
                continue;
            }

            //
            // We know that the lookup key is not covered by a chunk or pending range, and where the
            // previous chunk and pending chunks are.  Now we fill in the bounds as the closest
            // bounds of the surrounding ranges in both maps.
            //

            range->minKey = getMinKey();
            range->maxKey = maxKey;

            if ( lowerChunkIt != _chunksMap.end()
                && lowerChunkIt->second.woCompare( range->minKey ) > 0 ) {
                range->minKey = lowerChunkIt->second;
            }

            if ( upperChunkIt != _chunksMap.end()
                && upperChunkIt->first.woCompare( range->maxKey ) < 0 ) {
                range->maxKey = upperChunkIt->first;
            }

            if ( lowerPendingIt != _pendingMap.end()
                && lowerPendingIt->second.woCompare( range->minKey ) > 0 ) {
                range->minKey = lowerPendingIt->second;
            }

            if ( upperPendingIt != _pendingMap.end()
                && upperPendingIt->first.woCompare( range->maxKey ) < 0 ) {
                range->maxKey = upperPendingIt->first;
            }

            return true;
        }

        return false;
    }

    string CollectionMetadata::toString() const {
        StringBuilder ss;
        ss << " CollectionManager version: " << _shardVersion.toString() << " key: " << _keyPattern;
        if (_rangesMap.empty()) {
            return ss.str();
        }

        RangeMap::const_iterator it = _rangesMap.begin();
        ss << it->first << " -> " << it->second;
        while (it != _rangesMap.end()) {
            ss << ", "<< it->first << " -> " << it->second;
        }
        return ss.str();
    }

    BSONObj CollectionMetadata::getMinKey() const {
        BSONObjIterator it( _keyPattern );
        BSONObjBuilder minKeyB;
        while ( it.more() ) minKeyB << it.next().fieldName() << MINKEY;
        return minKeyB.obj();
    }

    BSONObj CollectionMetadata::getMaxKey() const {
        BSONObjIterator it( _keyPattern );
        BSONObjBuilder maxKeyB;
        while ( it.more() ) maxKeyB << it.next().fieldName() << MAXKEY;
        return maxKeyB.obj();
    }

    bool CollectionMetadata::isValid() const {
        if ( _shardVersion > _collVersion ) return false;
        if ( _collVersion.majorVersion() == 0 ) return false;
        if ( _collVersion.epoch() != _shardVersion.epoch() ) return false;
        return true;
    }

    bool CollectionMetadata::isValidKey( const BSONObj& key ) const {
        BSONObjIterator it( _keyPattern );
        BSONObjBuilder maxKeyB;
        while ( it.more() ) {
            BSONElement next = it.next();
            if ( !key.hasField( next.fieldName() ) ) return false;
        }
        return key.nFields() == _keyPattern.nFields();
    }

    void CollectionMetadata::fillRanges() {
        if (_chunksMap.empty())
            return;

        // Load the chunk information, coallesceing their ranges.  The version for this shard
        // would be the highest version for any of the chunks.
        RangeMap::const_iterator it = _chunksMap.begin();
        BSONObj min,max;
        while (it != _chunksMap.end()) {
            BSONObj currMin = it->first;
            BSONObj currMax = it->second;
            ++it;

            // coalesce the chunk's bounds in ranges if they are adjacent chunks
            if (min.isEmpty()) {
                min = currMin;
                max = currMax;
                continue;
            }
            if (max == currMin) {
                max = currMax;
                continue;
            }

            _rangesMap.insert(make_pair(min, max));

            min = currMin;
            max = currMax;
        }
        dassert(!min.isEmpty());

        _rangesMap.insert(make_pair(min, max));
    }

} // namespace mongo
