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

#include "mongo/platform/basic.h"

#include "mongo/db/s/metadata_manager.h"

#include "mongo/stdx/memory.h"

namespace mongo {

MetadataManager::MetadataManager() = default;

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

void MetadataManager::setActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    if (_activeMetadataTracker && _activeMetadataTracker->usageCounter > 0) {
        _metadataInUse.push_front(std::move(_activeMetadataTracker));
    }
    _activeMetadataTracker = stdx::make_unique<CollectionMetadataTracker>(std::move(newMetadata));
}

void MetadataManager::_removeMetadata_inlock(CollectionMetadataTracker* metadataTracker) {
    invariant(metadataTracker->usageCounter == 0);
    auto i = _metadataInUse.begin();
    auto e = _metadataInUse.end();
    while (i != e) {
        if (metadataTracker == i->get()) {
            _metadataInUse.erase(i);
            return;
        }
        i++;
    }
}

MetadataManager::CollectionMetadataTracker::CollectionMetadataTracker(
    std::unique_ptr<CollectionMetadata> m)
    : metadata(std::move(m)), usageCounter(0){};

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

std::map<BSONObj, ChunkRange> MetadataManager::getCopyOfRanges() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _rangesToClean;
}

void MetadataManager::addRangeToClean(const ChunkRange& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    _addRangeToClean_inlock(range);
}

void MetadataManager::_addRangeToClean_inlock(const ChunkRange& range) {
    auto itLow = _rangesToClean.upper_bound(range.getMin());
    if (itLow != _rangesToClean.begin()) {
        --itLow;
    }

    if (itLow != _rangesToClean.end()) {
        const ChunkRange& cr = itLow->second;
        if (cr.getMin() < range.getMin()) {
            // Checks that there is no overlap between range and any other ChunkRange
            // Specifically, checks that the greatest chunk less than or equal to range, if such a
            // chunk exists, does not overlap with the min of range.
            invariant(cr.getMax() <= range.getMin());
        }
    }

    auto itHigh = _rangesToClean.lower_bound(range.getMin());
    if (itHigh != _rangesToClean.end()) {
        const ChunkRange& cr = itHigh->second;
        // Checks that there is no overlap between range and any other ChunkRange
        // Specifically, checks that the least chunk greater than or equal to range
        // does not overlap with the max of range.
        invariant(cr.getMin() >= range.getMax());
    }

    _rangesToClean.insert(std::make_pair(range.getMin(), range));
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

    for (; it != _rangesToClean.end() && it->second.getMin() < range.getMax();) {
        if (it->second.getMax() <= range.getMin()) {
            ++it;
            continue;
        }
        // There's overlap between *it and range so we remove *it
        // and then replace with new ranges.
        ChunkRange oldChunk = it->second;
        _rangesToClean.erase(it++);
        if (oldChunk.getMin() < range.getMin()) {
            ChunkRange newChunk = ChunkRange(oldChunk.getMin(), range.getMin());
            _addRangeToClean_inlock(newChunk);
        }
        if (oldChunk.getMax() > range.getMax()) {
            ChunkRange newChunk = ChunkRange(range.getMax(), oldChunk.getMax());
            _addRangeToClean_inlock(newChunk);
        }
    }
}

void MetadataManager::append(BSONObjBuilder* builder) {
    BSONArrayBuilder arr(builder->subarrayStart("rangesToClean"));
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    for (const auto& entry : _rangesToClean) {
        BSONObjBuilder obj;
        entry.second.append(&obj);
        arr.append(obj.done());
    }
}

}  // namespace mongo
