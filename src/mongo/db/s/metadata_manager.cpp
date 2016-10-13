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

#include "mongo/db/s/metadata_manager.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

MetadataManager::MetadataManager(ServiceContext* sc, NamespaceString nss)
    : _nss(std::move(nss)),
      _serviceContext(sc),
      _activeMetadataTracker(stdx::make_unique<CollectionMetadataTracker>(nullptr)),
      _receivingChunks(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()),
      _rangesToClean(
          SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<RangeToCleanDescriptor>()) {}

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
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    // Collection was never sharded in the first place. This check is necessary in order to avoid
    // extraneous logging in the not-a-shard case, because all call sites always try to get the
    // collection sharding information regardless of whether the node is sharded or not.
    if (!remoteMetadata && !_activeMetadataTracker->metadata) {
        invariant(_receivingChunks.empty());
        invariant(_rangesToClean.empty());
        return;
    }

    // Collection is becoming unsharded
    if (!remoteMetadata) {
        log() << "Marking collection " << _nss.ns() << " with "
              << _activeMetadataTracker->metadata->toStringBasic() << " as no longer sharded";

        _receivingChunks.clear();
        _rangesToClean.clear();

        _setActiveMetadata_inlock(nullptr);
        return;
    }

    // We should never be setting unsharded metadata
    invariant(!remoteMetadata->getCollVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));
    invariant(!remoteMetadata->getShardVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));

    // Collection is becoming sharded
    if (!_activeMetadataTracker->metadata) {
        log() << "Marking collection " << _nss.ns() << " as sharded with "
              << remoteMetadata->toStringBasic();

        invariant(_receivingChunks.empty());
        invariant(_rangesToClean.empty());

        _setActiveMetadata_inlock(std::move(remoteMetadata));
        return;
    }

    // If the metadata being installed has a different epoch from ours, this means the collection
    // was dropped and recreated, so we must entirely reset the metadata state
    if (_activeMetadataTracker->metadata->getCollVersion().epoch() !=
        remoteMetadata->getCollVersion().epoch()) {
        log() << "Overwriting metadata for collection " << _nss.ns() << " from "
              << _activeMetadataTracker->metadata->toStringBasic() << " to "
              << remoteMetadata->toStringBasic() << " due to epoch change";

        _receivingChunks.clear();
        _rangesToClean.clear();

        _setActiveMetadata_inlock(std::move(remoteMetadata));
        return;
    }

    // We already have newer version
    if (_activeMetadataTracker->metadata->getCollVersion() >= remoteMetadata->getCollVersion()) {
        LOG(1) << "Ignoring refresh of active metadata "
               << _activeMetadataTracker->metadata->toStringBasic() << " with an older "
               << remoteMetadata->toStringBasic();
        return;
    }

    log() << "Refreshing metadata for collection " << _nss.ns() << " from "
          << _activeMetadataTracker->metadata->toStringBasic() << " to "
          << remoteMetadata->toStringBasic();

    // Resolve any receiving chunks, which might have completed by now
    for (auto it = _receivingChunks.begin(); it != _receivingChunks.end();) {
        const BSONObj min = it->first;
        const BSONObj max = it->second;

        // Our pending range overlaps at least one chunk
        if (rangeMapContains(remoteMetadata->getChunks(), min, max)) {
            // The remote metadata contains a chunk we were earlier in the process of receiving, so
            // we deem it successfully received.
            LOG(2) << "Verified chunk " << redact(ChunkRange(min, max).toString())
                   << " for collection " << _nss.ns() << " has been migrated to this shard earlier";

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
    _removeRangeToClean_inlock(range, Status::OK());
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
        invariant(SimpleBSONObjComparator::kInstance.evaluate(it->second == range.getMax()));

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
    _decrementUsageCounter();
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
        // If "this" was previously initialized, make sure we perform the same logic as in the
        // destructor to decrement _tracker->usageCounter for the CollectionMetadata "this" had a
        // reference to before replacing _tracker with other._tracker.
        if (_tracker) {
            _decrementUsageCounter();
        }

        _manager = other._manager;
        _tracker = other._tracker;
        other._manager = nullptr;
        other._tracker = nullptr;
    }

    return *this;
}

void ScopedCollectionMetadata::_decrementUsageCounter() {
    invariant(_manager);
    invariant(_tracker);
    stdx::lock_guard<stdx::mutex> scopedLock(_manager->_managerLock);
    invariant(_tracker->usageCounter > 0);
    if (--_tracker->usageCounter == 0) {
        _manager->_removeMetadata_inlock(_tracker);
    }
}

ScopedCollectionMetadata::operator bool() const {
    return _tracker && _tracker->metadata.get();
}

RangeMap MetadataManager::getCopyOfRangesToClean() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _getCopyOfRangesToClean_inlock();
}

RangeMap MetadataManager::_getCopyOfRangesToClean_inlock() {
    RangeMap ranges = SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>();
    for (auto it = _rangesToClean.begin(); it != _rangesToClean.end(); ++it) {
        ranges.insert(std::make_pair(it->first, it->second.getMax()));
    }
    return ranges;
}

std::shared_ptr<Notification<Status>> MetadataManager::addRangeToClean(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _addRangeToClean_inlock(range);
}

std::shared_ptr<Notification<Status>> MetadataManager::_addRangeToClean_inlock(
    const ChunkRange& range) {
    // This first invariant currently makes an unnecessary copy, to reuse the
    // rangeMapOverlaps helper function.
    invariant(!rangeMapOverlaps(_getCopyOfRangesToClean_inlock(), range.getMin(), range.getMax()));
    invariant(!rangeMapOverlaps(_receivingChunks, range.getMin(), range.getMax()));

    RangeToCleanDescriptor descriptor(range.getMax().getOwned());
    _rangesToClean.insert(std::make_pair(range.getMin().getOwned(), descriptor));

    // If _rangesToClean was previously empty, we need to start the collection range deleter
    if (_rangesToClean.size() == 1UL) {
        ShardingState::get(_serviceContext)->scheduleCleanup(_nss);
    }

    return descriptor.getNotification();
}

void MetadataManager::removeRangeToClean(const ChunkRange& range, Status deletionStatus) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    _removeRangeToClean_inlock(range, deletionStatus);
}

void MetadataManager::_removeRangeToClean_inlock(const ChunkRange& range, Status deletionStatus) {
    auto it = _rangesToClean.upper_bound(range.getMin());
    // We want our iterator to point at the greatest value
    // that is still less than or equal to range.
    if (it != _rangesToClean.begin()) {
        --it;
    }

    for (; it != _rangesToClean.end() &&
         SimpleBSONObjComparator::kInstance.evaluate(it->first < range.getMax());) {
        if (SimpleBSONObjComparator::kInstance.evaluate(it->second.getMax() <= range.getMin())) {
            ++it;
            continue;
        }

        // There's overlap between *it and range so we remove *it
        // and then replace with new ranges.
        BSONObj oldMin = it->first;
        BSONObj oldMax = it->second.getMax();
        it->second.complete(deletionStatus);
        _rangesToClean.erase(it++);
        if (SimpleBSONObjComparator::kInstance.evaluate(oldMin < range.getMin())) {
            _addRangeToClean_inlock(ChunkRange(oldMin, range.getMin()));
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(oldMax > range.getMax())) {
            _addRangeToClean_inlock(ChunkRange(range.getMax(), oldMax));
        }
    }
}

void MetadataManager::append(BSONObjBuilder* builder) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    BSONArrayBuilder rtcArr(builder->subarrayStart("rangesToClean"));
    for (const auto& entry : _rangesToClean) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second.getMax());
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

bool MetadataManager::hasRangesToClean() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return !_rangesToClean.empty();
}

bool MetadataManager::isInRangesToClean(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    // For convenience, this line makes an unnecessary copy, to reuse the
    // rangeMapContains helper function.
    return rangeMapContains(_getCopyOfRangesToClean_inlock(), range.getMin(), range.getMax());
}

ChunkRange MetadataManager::getNextRangeToClean() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    invariant(!_rangesToClean.empty());
    auto it = _rangesToClean.begin();
    return ChunkRange(it->first, it->second.getMax());
}

}  // namespace mongo
