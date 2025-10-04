/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/local_catalog/shard_role_catalog/metadata_manager.h"

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/logv2/log.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Returns whether the given metadata object has a chunk owned by this shard that overlaps the
 * input range.
 */
bool metadataOverlapsRange(const CollectionMetadata& metadata, const ChunkRange& range) {
    if (!metadata.hasRoutingTable()) {
        return true;
    }
    auto chunkRangeToCompareToMetadata =
        shardkeyutil::extendOrTruncateBoundsForMetadata(metadata, range);
    return metadata.rangeOverlapsChunk(chunkRangeToCompareToMetadata);
}

bool metadataOverlapsRange(const boost::optional<CollectionMetadata>& metadata,
                           const ChunkRange& range) {
    if (!metadata) {
        return false;
    }
    return metadataOverlapsRange(metadata.value(), range);
}

}  // namespace

class RangePreserver : public ScopedCollectionDescription::Impl {
public:
    // Must be called locked with the MetadataManager's _managerLock
    RangePreserver(WithLock,
                   std::shared_ptr<MetadataManager> metadataManager,
                   std::shared_ptr<MetadataManager::CollectionMetadataTracker> metadataTracker)
        : _metadataManager(std::move(metadataManager)),
          _metadataTracker(std::move(metadataTracker)) {
        ++_metadataTracker->usageCounter;
    }

    ~RangePreserver() override {
        stdx::lock_guard<stdx::mutex> managerLock(_metadataManager->_managerLock);

        invariant(_metadataTracker->usageCounter != 0);
        if (--_metadataTracker->usageCounter == 0) {
            // MetadataManager doesn't care which usageCounter went to zero. It just retires all
            // that are older than the oldest metadata still in use by queries (some start out at
            // zero, some go to zero but can't be expired yet).
            //
            // Note that new instances of ScopedCollectionDescription may get attached to
            // _metadata.back(), so its usage count can increase from zero, unlike other reference
            // counts.
            _metadataManager->_retireExpiredMetadata(managerLock);
        }
    }

    // This will only ever refer to the active metadata, so CollectionMetadata should never be
    // boost::none
    const CollectionMetadata& get() override {
        invariant(_metadataTracker->metadata);
        return _metadataTracker->metadata.value();
    }

    // This determines whether the metadata currently held by the _metadataTracker is still
    // considered valid.
    bool isMetadataStillValid() const override {
        stdx::lock_guard<stdx::mutex> managerLock(_metadataManager->_managerLock);
        return _metadataTracker->valid;
    }

private:
    std::shared_ptr<MetadataManager> _metadataManager;
    std::shared_ptr<MetadataManager::CollectionMetadataTracker> _metadataTracker;
};

MetadataManager::MetadataManager(ServiceContext* serviceContext,
                                 NamespaceString nss,
                                 CollectionMetadata initialMetadata)
    : _serviceContext(serviceContext), _nss(std::move(nss)) {
    _metadata.emplace_back(std::make_shared<CollectionMetadataTracker>(std::move(initialMetadata)));
}

std::shared_ptr<ScopedCollectionDescription::Impl> MetadataManager::getActiveMetadata(
    const boost::optional<LogicalTime>& atClusterTime, bool preserveRange) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    auto activeMetadataTracker = _metadata.back();
    const auto& activeMetadata = activeMetadataTracker->metadata;

    class MetadataAtTimestamp : public ScopedCollectionDescription::Impl {
    public:
        MetadataAtTimestamp(CollectionMetadata metadata) : _metadata(std::move(metadata)) {}

        const CollectionMetadata& get() override {
            return _metadata;
        }

    private:
        CollectionMetadata _metadata;
    };

    // We don't keep routing history for unsharded collections, so if the collection is unsharded
    // just return the active metadata
    if (!atClusterTime || !activeMetadata->isSharded()) {
        if (preserveRange) {
            return std::make_shared<RangePreserver>(
                lg, shared_from_this(), std::move(activeMetadataTracker));
        } else {
            return std::make_shared<MetadataAtTimestamp>(*activeMetadata);
        }
    }

    return std::make_shared<MetadataAtTimestamp>(CollectionMetadata(
        ChunkManager::makeAtTime(*activeMetadata->getChunkManager(), atClusterTime->asTimestamp()),
        activeMetadata->shardId()));
}

boost::optional<UUID> MetadataManager::getCollectionUuid() const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    return _getCollectionUuidWithLock(lg);
}

boost::optional<UUID> MetadataManager::_getCollectionUuidWithLock(WithLock wl) const {
    invariant(!_metadata.empty());
    invariant(_metadata.back()->metadata.has_value());

    const auto& activeMetadata = _metadata.back()->metadata.value();

    // The UUID is only present on a CollectionMetadataTracker if the collection is tracked.
    if (activeMetadata.hasRoutingTable()) {
        return activeMetadata.getUUID();
    }
    return boost::none;
}

size_t MetadataManager::numberOfMetadataSnapshots() const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());
    return _metadata.size() - 1;
}

int MetadataManager::numberOfEmptyMetadataSnapshots() const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    int emptyMetadataSnapshots = 0;
    for (const auto& collMetadataTracker : _metadata) {
        if (!collMetadataTracker->metadata)
            emptyMetadataSnapshots++;
    }

    return emptyMetadataSnapshots;
}

void MetadataManager::setFilteringMetadata(CollectionMetadata remoteMetadata) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());
    // The active metadata should always be available (not boost::none)
    invariant(_metadata.back()->metadata);
    const auto& activeMetadata = _metadata.back()->metadata.value();

    const auto remoteCollPlacementVersion = remoteMetadata.getCollPlacementVersion();
    const auto activeCollPlacementVersion = activeMetadata.getCollPlacementVersion();
    // Do nothing if the remote version is older than or equal to the current active one
    auto compareResult = remoteCollPlacementVersion <=> activeCollPlacementVersion;
    if (compareResult == std::partial_ordering::less ||
        compareResult == std::partial_ordering::equivalent) {
        LOGV2_DEBUG(21984,
                    1,
                    "Ignoring incoming metadata update for this namespace because the active "
                    "(current) metadata has the same or a newer collection placement version",
                    logAttrs(_nss),
                    "activeMetadata"_attr = activeMetadata.toStringBasic(),
                    "remoteMetadata"_attr = remoteMetadata.toStringBasic());
        return;
    }

    LOGV2(21985,
          "Updating metadata for this namespace because the remote metadata has a newer "
          "collection placement version",
          logAttrs(_nss),
          "activeMetadata"_attr = activeMetadata.toStringBasic(),
          "remoteMetadata"_attr = remoteMetadata.toStringBasic());

    _setActiveMetadata(lg, std::move(remoteMetadata));
}

void MetadataManager::_setActiveMetadata(WithLock wl, CollectionMetadata newMetadata) {
    tassert(10016218,
            "Attempted to update MetadataManager with incompatible new metadata",
            !this->hasRoutingTable() ||
                (newMetadata.hasRoutingTable() &&
                 this->_getCollectionUuidWithLock(wl) == newMetadata.getUUID()));
    _metadata.emplace_back(std::make_shared<CollectionMetadataTracker>(std::move(newMetadata)));
    _retireExpiredMetadata(wl);
}

void MetadataManager::_retireExpiredMetadata(WithLock) {
    // Remove entries with a usage count of 0 from the front of _metadata, which may schedule
    // orphans for deletion. We cannot remove an entry from the middle of _metadata because a
    // previous entry (whose usageCount is not 0) could have a query that is actually still
    // accessing those documents.
    while (_metadata.size() > 1 && !_metadata.front()->usageCounter) {
        _metadata.pop_front();
    }

    // To avoid memory build up of ChunkManager objects, we can clear the CollectionMetadata object
    // in an entry when its usageCount is 0 as long as it is not the last item in _metadata (which
    // is the active metadata). If _metadata is empty, decrementing iter will be out of bounds, so
    // we must check that the size is > 1 as well.
    if (_metadata.size() > 1) {
        auto iter = _metadata.begin();
        while (iter != (--_metadata.end())) {
            if ((*iter)->usageCounter == 0) {
                (*iter)->metadata = boost::none;
            }
            ++iter;
        }
    }
}

SharedSemiFuture<void> MetadataManager::getOngoingQueriesCompletionFuture(ChunkRange const& range) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    auto* const overlapMetadata = _findNewestOverlappingMetadata(lg, range);
    if (!overlapMetadata) {
        return SemiFuture<void>::makeReady().share();
    }
    return overlapMetadata->onDestructionPromise.getFuture();
}

void MetadataManager::invalidateRangePreserversOlderThanShardVersion(
    OperationContext* opCtx, const ChunkVersion& shardVersion) {
    if (shardVersion == ChunkVersion::IGNORED()) {
        return;
    }
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    // Invalidate all metadata trackers when shardPlacementVersion is lower than or equal
    // to the given version.
    // The _metadata is sorted from the oldest to the newest versions. The 'for' loop can be
    // exited if the current metadataTracker is newer, as it means there's no need to check the
    // rest of the items.
    for (const auto& metadataTracker : _metadata) {
        if (metadataTracker->metadata) {
            auto placementVersion = metadataTracker->metadata->getShardPlacementVersion();
            if ((placementVersion <=> shardVersion) != std::partial_ordering::greater) {
                metadataTracker->valid = false;
            } else {
                break;
            }
        }
    }
}

auto MetadataManager::_findNewestOverlappingMetadata(WithLock, ChunkRange const& range)
    -> CollectionMetadataTracker* {
    invariant(!_metadata.empty());

    for (auto it = _metadata.rbegin(); it != _metadata.rend(); ++it) {
        auto& tracker = *it;
        if (tracker->usageCounter && metadataOverlapsRange(tracker->metadata, range)) {
            return tracker.get();
        }
    }

    return nullptr;
}

bool MetadataManager::hasRoutingTable() {
    invariant(!_metadata.empty());
    return _metadata.back()->metadata->hasRoutingTable();
}

}  // namespace mongo
