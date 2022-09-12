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


#include "mongo/platform/basic.h"

#include "mongo/db/s/metadata_manager.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using TaskExecutor = executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;

/**
 * Returns whether the given metadata object has a chunk owned by this shard that overlaps the
 * input range.
 */
bool metadataOverlapsRange(const CollectionMetadata& metadata, const ChunkRange& range) {
    auto chunkRangeToCompareToMetadata =
        migrationutil::extendOrTruncateBoundsForMetadata(metadata, range);
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

    ~RangePreserver() {
        stdx::lock_guard<Latch> managerLock(_metadataManager->_managerLock);

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
    const CollectionMetadata& get() {
        invariant(_metadataTracker->metadata);
        return _metadataTracker->metadata.value();
    }

private:
    std::shared_ptr<MetadataManager> _metadataManager;
    std::shared_ptr<MetadataManager::CollectionMetadataTracker> _metadataTracker;
};

MetadataManager::MetadataManager(ServiceContext* serviceContext,
                                 NamespaceString nss,
                                 std::shared_ptr<TaskExecutor> executor,
                                 CollectionMetadata initialMetadata)
    : _serviceContext(serviceContext),
      _nss(std::move(nss)),
      _collectionUuid(initialMetadata.getChunkManager()->getUUID()),
      _executor(std::move(executor)) {
    _metadata.emplace_back(std::make_shared<CollectionMetadataTracker>(std::move(initialMetadata)));
}

std::shared_ptr<ScopedCollectionDescription::Impl> MetadataManager::getActiveMetadata(
    const boost::optional<LogicalTime>& atClusterTime) {
    stdx::lock_guard<Latch> lg(_managerLock);

    auto activeMetadataTracker = _metadata.back();
    const auto& activeMetadata = activeMetadataTracker->metadata;

    // We don't keep routing history for unsharded collections, so if the collection is unsharded
    // just return the active metadata
    if (!atClusterTime || !activeMetadata->isSharded()) {
        return std::make_shared<RangePreserver>(
            lg, shared_from_this(), std::move(activeMetadataTracker));
    }

    class MetadataAtTimestamp : public ScopedCollectionDescription::Impl {
    public:
        MetadataAtTimestamp(CollectionMetadata metadata) : _metadata(std::move(metadata)) {}

        const CollectionMetadata& get() override {
            return _metadata;
        }

    private:
        CollectionMetadata _metadata;
    };

    return std::make_shared<MetadataAtTimestamp>(CollectionMetadata(
        ChunkManager::makeAtTime(*activeMetadata->getChunkManager(), atClusterTime->asTimestamp()),
        activeMetadata->shardId()));
}

size_t MetadataManager::numberOfMetadataSnapshots() const {
    stdx::lock_guard<Latch> lg(_managerLock);
    invariant(!_metadata.empty());
    return _metadata.size() - 1;
}

int MetadataManager::numberOfEmptyMetadataSnapshots() const {
    stdx::lock_guard<Latch> lg(_managerLock);

    int emptyMetadataSnapshots = 0;
    for (const auto& collMetadataTracker : _metadata) {
        if (!collMetadataTracker->metadata)
            emptyMetadataSnapshots++;
    }

    return emptyMetadataSnapshots;
}

void MetadataManager::setFilteringMetadata(CollectionMetadata remoteMetadata) {
    stdx::lock_guard<Latch> lg(_managerLock);
    invariant(!_metadata.empty());
    // The active metadata should always be available (not boost::none)
    invariant(_metadata.back()->metadata);
    const auto& activeMetadata = _metadata.back()->metadata.value();

    const auto remoteCollVersion = remoteMetadata.getCollVersion();
    const auto activeCollVersion = activeMetadata.getCollVersion();
    // Do nothing if the remote version is older than or equal to the current active one
    if (remoteCollVersion.isOlderOrEqualThan(activeCollVersion)) {
        LOGV2_DEBUG(21984,
                    1,
                    "Ignoring incoming metadata update {activeMetadata} for {namespace} because "
                    "the active (current) metadata {remoteMetadata} has the same or a newer "
                    "collection version",
                    "Ignoring incoming metadata update for this namespace because the active "
                    "(current) metadata has the same or a newer collection version",
                    "namespace"_attr = _nss.ns(),
                    "activeMetadata"_attr = activeMetadata.toStringBasic(),
                    "remoteMetadata"_attr = remoteMetadata.toStringBasic());
        return;
    }

    LOGV2(21985,
          "Updating metadata {activeMetadata} for {namespace} because the remote metadata "
          "{remoteMetadata} has a newer collection version",
          "Updating metadata for this namespace because the remote metadata has a newer "
          "collection version",
          "namespace"_attr = _nss.ns(),
          "activeMetadata"_attr = activeMetadata.toStringBasic(),
          "remoteMetadata"_attr = remoteMetadata.toStringBasic());

    _setActiveMetadata(lg, std::move(remoteMetadata));
}

void MetadataManager::_setActiveMetadata(WithLock wl, CollectionMetadata newMetadata) {
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

void MetadataManager::append(BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_managerLock);

    BSONArrayBuilder arr(builder->subarrayStart("rangesToClean"));
    for (auto const& [range, _] : _rangesScheduledForDeletion) {
        BSONObjBuilder obj;
        range.append(&obj);
        arr.append(obj.done());
    }

    invariant(!_metadata.empty());

    BSONArrayBuilder amrArr(builder->subarrayStart("activeMetadataRanges"));
    for (const auto& entry : _metadata.back()->metadata->getChunks()) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second);
        r.append(&obj);
        amrArr.append(obj.done());
    }
    amrArr.done();
}

SharedSemiFuture<void> MetadataManager::cleanUpRange(ChunkRange const& range,
                                                     bool shouldDelayBeforeDeletion) {
    stdx::lock_guard<Latch> lg(_managerLock);
    invariant(!_metadata.empty());

    auto* const activeMetadata = _metadata.back().get();
    auto* const overlapMetadata = _findNewestOverlappingMetadata(lg, range);

    if (overlapMetadata == activeMetadata) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      str::stream() << "Requested deletion range overlaps a live shard chunk"};
    }

    auto delayForActiveQueriesOnSecondariesToComplete =
        shouldDelayBeforeDeletion ? Seconds(orphanCleanupDelaySecs.load()) : Seconds(0);

    if (overlapMetadata) {
        LOGV2_OPTIONS(21989,
                      {logv2::LogComponent::kShardingMigration},
                      "Deletion of {namespace} range {range} will be scheduled after all possibly "
                      "dependent queries finish",
                      "Deletion of the collection's specified range will be scheduled after all "
                      "possibly dependent queries finish",
                      "namespace"_attr = _nss.ns(),
                      "range"_attr = redact(range.toString()));
        ++overlapMetadata->numContingentRangeDeletionTasks;
        // Schedule the range for deletion once the overlapping metadata object is destroyed
        // (meaning no more queries can be using the range) and obtain a future which will be
        // signaled when deletion is complete.
        return _submitRangeForDeletion(lg,
                                       overlapMetadata->onDestructionPromise.getFuture().semi(),
                                       range,
                                       delayForActiveQueriesOnSecondariesToComplete);
    } else {
        // No running queries can depend on this range, so queue it for deletion immediately.
        LOGV2_OPTIONS(21990,
                      {logv2::LogComponent::kShardingMigration},
                      "Scheduling deletion of {namespace} range {range}",
                      "Scheduling deletion of the collection's specified range",
                      "namespace"_attr = _nss.ns(),
                      "range"_attr = redact(range.toString()));

        return _submitRangeForDeletion(
            lg, SemiFuture<void>::makeReady(), range, delayForActiveQueriesOnSecondariesToComplete);
    }
}

size_t MetadataManager::numberOfRangesToCleanStillInUse() const {
    stdx::lock_guard<Latch> lg(_managerLock);
    size_t count = 0;
    for (auto& tracker : _metadata) {
        count += tracker->numContingentRangeDeletionTasks;
    }
    return count;
}

size_t MetadataManager::numberOfRangesToClean() const {
    auto rangesToCleanInUse = numberOfRangesToCleanStillInUse();
    stdx::lock_guard<Latch> lg(_managerLock);
    return _rangesScheduledForDeletion.size() - rangesToCleanInUse;
}

size_t MetadataManager::numberOfRangesScheduledForDeletion() const {
    stdx::lock_guard<Latch> lg(_managerLock);
    return _rangesScheduledForDeletion.size();
}

boost::optional<SharedSemiFuture<void>> MetadataManager::trackOrphanedDataCleanup(
    ChunkRange const& range) const {
    stdx::lock_guard<Latch> lg(_managerLock);
    for (const auto& [orphanRange, deletionComplete] : _rangesScheduledForDeletion) {
        if (orphanRange.overlapWith(range)) {
            return deletionComplete;
        }
    }

    return boost::none;
}

auto MetadataManager::_findNewestOverlappingMetadata(WithLock, ChunkRange const& range)
    -> CollectionMetadataTracker* {
    invariant(!_metadata.empty());

    auto it = _metadata.rbegin();
    if (metadataOverlapsRange((*it)->metadata, range)) {
        return (*it).get();
    }

    ++it;
    for (; it != _metadata.rend(); ++it) {
        auto& tracker = *it;
        if (tracker->usageCounter && metadataOverlapsRange(tracker->metadata, range)) {
            return tracker.get();
        }
    }

    return nullptr;
}

bool MetadataManager::_overlapsInUseChunk(WithLock lk, ChunkRange const& range) {
    auto* cm = _findNewestOverlappingMetadata(lk, range);
    return (cm != nullptr);
}

SharedSemiFuture<void> MetadataManager::_submitRangeForDeletion(
    const WithLock&,
    SemiFuture<void> waitForActiveQueriesToComplete,
    const ChunkRange& range,
    Seconds delayForActiveQueriesOnSecondariesToComplete) {
    auto cleanupComplete = [&]() {
        const auto collUUID = _metadata.back()->metadata->getChunkManager()->getUUID();

        if (feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
            return RangeDeleterService::get(_serviceContext)
                ->getOverlappingRangeDeletionsFuture(collUUID, range);
        }

        return removeDocumentsInRange(_executor,
                                      std::move(waitForActiveQueriesToComplete),
                                      _nss,
                                      collUUID,
                                      _metadata.back()->metadata->getKeyPattern().getOwned(),
                                      range,
                                      delayForActiveQueriesOnSecondariesToComplete);
    }();

    _rangesScheduledForDeletion.emplace_front(range, cleanupComplete);
    // Attach a continuation so that once the range has been deleted, we will remove the deletion
    // from the _rangesScheduledForDeletion.  std::list iterators are never invalidated, which
    // allows us to save the iterator pointing to the newly added element for use later when
    // deleting it.
    cleanupComplete.thenRunOn(_executor).getAsync(
        [self = shared_from_this(), it = _rangesScheduledForDeletion.begin()](Status s) {
            stdx::lock_guard<Latch> lg(self->_managerLock);
            self->_rangesScheduledForDeletion.erase(it);
        });

    return cleanupComplete;
}

}  // namespace mongo
