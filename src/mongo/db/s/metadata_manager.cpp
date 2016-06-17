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

#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/stdx/memory.h"

namespace mongo {

MetadataManager::MetadataManager() = default;

ScopedCollectionMetadata MetadataManager::getActiveMetadata() {
    invariant(_activeMetadataTracker);
    return ScopedCollectionMetadata(this, _activeMetadataTracker.get());
}

void MetadataManager::setActiveMetadata(std::unique_ptr<CollectionMetadata> newMetadata) {
    if (_activeMetadataTracker && _activeMetadataTracker->usageCounter > 0) {
        _metadataInUse.push_front(std::move(_activeMetadataTracker));
    }
    _activeMetadataTracker = stdx::make_unique<CollectionMetadataTracker>(std::move(newMetadata));
}

void MetadataManager::_removeMetadata(CollectionMetadataTracker* metadataTracker) {
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

ScopedCollectionMetadata::ScopedCollectionMetadata(
    MetadataManager* manager, MetadataManager::CollectionMetadataTracker* tracker)
    : _manager(manager), _tracker(tracker) {
    _tracker->usageCounter++;
}

ScopedCollectionMetadata::~ScopedCollectionMetadata() {
    invariant(_tracker->usageCounter > 0);
    if (--_tracker->usageCounter == 0) {
        _manager->_removeMetadata(_tracker);
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

std::map<BSONObj, ChunkRange> MetadataManager::getCopyOfRanges() {
    return _rangesToClean;
}

void MetadataManager::addRangeToClean(const ChunkRange& range) {
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
            addRangeToClean(newChunk);
        }
        if (oldChunk.getMax() > range.getMax()) {
            ChunkRange newChunk = ChunkRange(range.getMax(), oldChunk.getMax());
            addRangeToClean(newChunk);
        }
    }
}
}  // namespace mongo
