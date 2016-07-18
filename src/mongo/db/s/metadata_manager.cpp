/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

MetadataManager::MetadataManager()
    : _activeMetadataTracker(stdx::make_unique<CollectionMetadataTracker>(nullptr)) {}

MetadataManager::~MetadataManager() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    invariant(!_activeMetadataTracker || _activeMetadataTracker->usageCounter == 0);
}

ScopedCollectionMetadata MetadataManager::getActiveMetadata() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    if (!_activeMetadataTracker) {
        return ScopedCollectionMetadata();
    }

    return ScopedCollectionMetadata(this, _activeMetadataTracker.get());
}

void MetadataManager::refreshActiveMetadata(std::unique_ptr<CollectionMetadata> remoteMetadata) {
    LOG(1) << "Refreshing the active metadata from "
           << (_activeMetadataTracker->metadata ? _activeMetadataTracker->metadata->toStringBasic()
                                                : "(empty)")
           << ", to " << (remoteMetadata ? remoteMetadata->toStringBasic() : "(empty)");

    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    // Collection is not sharded anymore
    if (!remoteMetadata) {
        log() << "Marking collection as not sharded.";

        _receivingChunks.clear();
        _rangesToClean.clear();

        _setActiveMetadata_inlock(nullptr);
        return;
    }

    invariant(!remoteMetadata->getCollVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));
    invariant(!remoteMetadata->getShardVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));

    // Collection is not sharded currently
    if (!_activeMetadataTracker->metadata) {
        log() << "Marking collection as sharded with version " << remoteMetadata->toStringBasic();

        invariant(_receivingChunks.empty());
        invariant(_rangesToClean.empty());

        _setActiveMetadata_inlock(std::move(remoteMetadata));
        return;
    }

    // If the metadata being installed has a different epoch from ours, this means the collection
    // was dropped and recreated, so we must entirely reset the metadata state
    if (_activeMetadataTracker->metadata->getCollVersion().epoch() !=
        remoteMetadata->getCollVersion().epoch()) {
        log() << "Overwriting collection metadata due to epoch change.";

        _receivingChunks.clear();
        _rangesToClean.clear();

        _setActiveMetadata_inlock(std::move(remoteMetadata));
        return;
    }

    // We already have newer version
    if (_activeMetadataTracker->metadata->getCollVersion() >= remoteMetadata->getCollVersion()) {
        LOG(1) << "Attempted to refresh active metadata "
               << _activeMetadataTracker->metadata->toStringBasic() << " with an older version "
               << remoteMetadata->toStringBasic();

        return;
    }

    // Resolve any receiving chunks, which might have completed by now
    for (auto it = _receivingChunks.begin(); it != _receivingChunks.end();) {
        const BSONObj min = it->first;
        const BSONObj max = it->second;

        // Our pending range overlaps at least one chunk
        if (rangeMapContains(remoteMetadata->getChunks(), min, max)) {
            // The remote metadata contains a chunk we were earlier in the process of receiving, so
            // we deem it successfully received.
            LOG(2) << "Verified chunk " << ChunkRange(min, max).toString()
                   << " was migrated earlier to this shard";

            _receivingChunks.erase(it++);
            continue;
        } else if (!rangeMapOverlaps(remoteMetadata->getChunks(), min, max)) {
            ++it;
            continue;
        }

        // Partial overlap indicates that the earlier migration has failed, but the chunk being
        // migrated underwent some splits and other migrations and ended up here again. In this
        // case, we will request full reload of the metadata. Currently this cannot happen, because
        // all migrations are with the explicit knowledge of the recipient shard. However, we leave
        // the option open so that chunk splits can do empty chunk move without having to notify the
        // recipient.
        RangeVector overlappedChunks;
        getRangeMapOverlap(remoteMetadata->getChunks(), min, max, &overlappedChunks);

        for (const auto& overlapChunkMin : overlappedChunks) {
            auto itRecv = _receivingChunks.find(overlapChunkMin.first);
            invariant(itRecv != _receivingChunks.end());

            const ChunkRange receivingRange(itRecv->first, itRecv->second);

            _receivingChunks.erase(itRecv);

            // Make sure any potentially partially copied chunks are scheduled to be cleaned up
            _addRangeToClean_inlock(receivingRange);
        }

        // Need to reset the iterator
        it = _receivingChunks.begin();
    }

    // For compatibility with the current range deleter, which is driven entirely by the contents of
    // the CollectionMetadata update the pending chunks
    for (const auto& receivingChunk : _receivingChunks) {
        ChunkType chunk;
        chunk.setMin(receivingChunk.first);
        chunk.setMax(receivingChunk.second);
        remoteMetadata = remoteMetadata->clonePlusPending(chunk);
    }

    _setActiveMetadata_inlock(std::move(remoteMetadata));
}

void MetadataManager::beginReceive(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    // Collection is not known to be sharded if the active metadata tracker is null
    invariant(_activeMetadataTracker);

    // If range is contained within pending chunks, this means a previous migration must have failed
    // and we need to clean all overlaps
    RangeVector overlappedChunks;
    getRangeMapOverlap(_receivingChunks, range.getMin(), range.getMax(), &overlappedChunks);

    for (const auto& overlapChunkMin : overlappedChunks) {
        auto itRecv = _receivingChunks.find(overlapChunkMin.first);
        invariant(itRecv != _receivingChunks.end());

        const ChunkRange receivingRange(itRecv->first, itRecv->second);

        _receivingChunks.erase(itRecv);

        // Make sure any potentially partially copied chunks are scheduled to be cleaned up
        _addRangeToClean_inlock(receivingRange);
    }

    // Need to ensure that the background range deleter task won't delete the range we are about to
    // receive
    _removeRangeToClean_inlock(range);
    _receivingChunks.insert(std::make_pair(range.getMin().getOwned(), range.getMax().getOwned()));

    // For compatibility with the current range deleter, update the pending chunks on the collection
    // metadata to include the chunk being received
    ChunkType chunk;
    chunk.setMin(range.getMin());
    chunk.setMax(range.getMax());
    _setActiveMetadata_inlock(_activeMetadataTracker->metadata->clonePlusPending(chunk));
}

void MetadataManager::forgetReceive(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    {
        auto it = _receivingChunks.find(range.getMin());
        invariant(it != _receivingChunks.end());

        // Verify entire ChunkRange is identical, not just the min key.
        invariant(it->second == range.getMax());

        _receivingChunks.erase(it);
    }

    // This is potentially a partially received data, which needs to be cleaned up
    _addRangeToClean_inlock(range);

    // For compatibility with the current range deleter, update the pending chunks on the collection
    // metadata to exclude the chunk being received, which was added in beginReceive
    ChunkType chunk;
    chunk.setMin(range.getMin());
    chunk.setMax(range.getMax());
    _setActiveMetadata_inlock(_activeMetadataTracker->metadata->cloneMinusPending(chunk));
}

RangeMap MetadataManager::getCopyOfReceivingChunks() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _receivingChunks;
}

void MetadataManager::_setActiveMetadata_inlock(std::unique_ptr<CollectionMetadata> newMetadata) {
    invariant(!newMetadata || newMetadata->isValid());

    if (_activeMetadataTracker->usageCounter > 0) {
        _metadataInUse.push_front(std::move(_activeMetadataTracker));
    }

    _activeMetadataTracker = stdx::make_unique<CollectionMetadataTracker>(std::move(newMetadata));
}

void MetadataManager::_removeMetadata_inlock(CollectionMetadataTracker* metadataTracker) {
    invariant(metadataTracker->usageCounter == 0);

    auto i = _metadataInUse.begin();
    const auto e = _metadataInUse.end();
    while (i != e) {
        if (metadataTracker == i->get()) {
            _metadataInUse.erase(i);
            return;
        }

        ++i;
    }
}

MetadataManager::CollectionMetadataTracker::CollectionMetadataTracker(
    std::unique_ptr<CollectionMetadata> m)
    : metadata(std::move(m)) {}

ScopedCollectionMetadata::ScopedCollectionMetadata() = default;

// called in lock
ScopedCollectionMetadata::ScopedCollectionMetadata(
    MetadataManager* manager, MetadataManager::CollectionMetadataTracker* tracker)
    : _manager(manager), _tracker(tracker) {
    _tracker->usageCounter++;
}

ScopedCollectionMetadata::~ScopedCollectionMetadata() {
    if (!_tracker)
        return;

    stdx::lock_guard<stdx::mutex> scopedLock(_manager->_managerLock);
    invariant(_tracker->usageCounter > 0);
    if (--_tracker->usageCounter == 0) {
        _manager->_removeMetadata_inlock(_tracker);
    }
}

CollectionMetadata* ScopedCollectionMetadata::operator->() {
    return _tracker->metadata.get();
}

CollectionMetadata* ScopedCollectionMetadata::getMetadata() {
    return _tracker->metadata.get();
}

ScopedCollectionMetadata::ScopedCollectionMetadata(ScopedCollectionMetadata&& other) {
    *this = std::move(other);
}

ScopedCollectionMetadata& ScopedCollectionMetadata::operator=(ScopedCollectionMetadata&& other) {
    if (this != &other) {
        _manager = other._manager;
        _tracker = other._tracker;
        other._manager = nullptr;
        other._tracker = nullptr;
    }

    return *this;
}

ScopedCollectionMetadata::operator bool() const {
    return _tracker && _tracker->metadata.get();
}

RangeMap MetadataManager::getCopyOfRangesToClean() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _rangesToClean;
}

void MetadataManager::addRangeToClean(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    _addRangeToClean_inlock(range);
}

void MetadataManager::_addRangeToClean_inlock(const ChunkRange& range) {
    invariant(!rangeMapOverlaps(_rangesToClean, range.getMin(), range.getMax()));
    invariant(!rangeMapOverlaps(_receivingChunks, range.getMin(), range.getMax()));
    _rangesToClean.insert(std::make_pair(range.getMin().getOwned(), range.getMax().getOwned()));
}

void MetadataManager::removeRangeToClean(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    _removeRangeToClean_inlock(range);
}

void MetadataManager::_removeRangeToClean_inlock(const ChunkRange& range) {
    auto it = _rangesToClean.upper_bound(range.getMin());
    // We want our iterator to point at the greatest value
    // that is still less than or equal to range.
    if (it != _rangesToClean.begin()) {
        --it;
    }

    for (; it != _rangesToClean.end() && it->first < range.getMax();) {
        if (it->second <= range.getMin()) {
            ++it;
            continue;
        }

        // There's overlap between *it and range so we remove *it
        // and then replace with new ranges.
        BSONObj oldMin = it->first, oldMax = it->second;
        _rangesToClean.erase(it++);
        if (oldMin < range.getMin()) {
            _addRangeToClean_inlock(ChunkRange(oldMin, range.getMin()));
        }

        if (oldMax > range.getMax()) {
            _addRangeToClean_inlock(ChunkRange(range.getMax(), oldMax));
        }
    }
}

void MetadataManager::append(BSONObjBuilder* builder) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    BSONArrayBuilder rtcArr(builder->subarrayStart("rangesToClean"));
    for (const auto& entry : _rangesToClean) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second);
        r.append(&obj);
        rtcArr.append(obj.done());
    }
    rtcArr.done();

    BSONArrayBuilder pcArr(builder->subarrayStart("pendingChunks"));
    for (const auto& entry : _receivingChunks) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second);
        r.append(&obj);
        pcArr.append(obj.done());
    }
    pcArr.done();

    BSONArrayBuilder amrArr(builder->subarrayStart("activeMetadataRanges"));
    for (const auto& entry : _activeMetadataTracker->metadata->getChunks()) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second);
        r.append(&obj);
        amrArr.append(obj.done());
    }
    amrArr.done();
}

}  // namespace mongo
