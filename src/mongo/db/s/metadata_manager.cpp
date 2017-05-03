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
#include "mongo/bson/util/builder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/collection_range_deleter.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

// MetadataManager exists only as a data member of a CollectionShardingState object.
//
// It maintains a set of std::shared_ptr<MetadataManager::Tracker> pointers: one in
// _activeMetadataTracker, and more in a list _metadataInUse. It also contains a
// CollectionRangeDeleter that queues orphan ranges to delete in a background thread, and a record
// of the ranges being migrated in, to avoid deleting them.
//
// Free-floating MetadataManager::Tracker objects are maintained by these pointers, and also by
// clients in ScopedCollectionMetadata objects obtained via CollectionShardingState::getMetadata().
//
// A Tracker object keeps:
//   a std::unique_ptr<CollectionMetadata>, owning a map of the chunks owned by the shard,
//   a key range [min,max) of orphaned documents that may be deleted when the count goes to zero,
//   a count of the ScopedCollectionMetadata objects that have pointers to it,
//   a mutex lock, serializing access to:
//     a pointer back to the MetadataManager object that created it.
//
//                                          __________________________
//  (s): std::shared_ptr<>         Clients:| ScopedCollectionMetadata |
//  (u): std::unique_ptr<>                 |              tracker (s)-----------+
//   ________________________________      |__________________________| |       |
//  | CollectionShardingState        |       |             tracker (s)--------+ +
//  |                                |       |__________________________| |   | |
//  |  ____________________________  |          |            tracker (s)----+ | |
//  | | MetadataManager            | |          |_________________________| | | |
//  | |                            | |      ________________________        | | |
//  | | _activeMetadataTracker (s)-------->| Tracker                |<------+ | | (1 reference)
//  | |                            | |     |  ______________________|_        | |
//  | |                 [ (s),-------------->| Tracker                |       | | (0 references)
//  | |                   (s),---------\   | |  ______________________|_      | |
//  | | _metadataInUse   ...  ]    | |  \----->| Tracker                |<----+-+ (2 references)
//  | |  ________________________  | |     | | |                        |   ______________________
//  | | | CollectionRangeDeleter | | |     | | | metadata (u)------------->| CollectionMetadata   |
//  | | |                        | | |     | | | [ orphans [min,max) ]  |  |                      |
//  | | | _orphans [ [min,max),  | | |     | | | usageCounter           |  |  _chunksMap          |
//  | | |            [min,max),  | | |     | | | trackerLock:           |  |  _chunkVersion       |
//  | | |                  ... ] | |<--------------manager              |  |  ...                 |
//  | | |                        | | |     |_| |                        |  |______________________|
//  | | |________________________| | |       |_|                        |
//  | |                            | |         |________________________|
//
//  A ScopedCollectionMetadata object is created and held during a query, and destroyed when the
//  query no longer needs access to the collection. Its destructor decrements the Tracker's
//  usageCounter.
//
//  When a new chunk mapping replaces _activeMetadata, if any queries still depend on the current
//  mapping, it is pushed onto the back of _metadataInUse.
//
//  Trackers pointed to from _metadataInUse, and their associated CollectionMetadata, are maintained
//  at least as long as any query holds a ScopedCollectionMetadata object referring to them, or to
//  any older tracker. In the diagram above, the middle Tracker must be kept until the one below it
//  is disposed of.  (Note that _metadataInUse as shown here has its front() at the bottom, back()
//  at the top. As usual, new entries are pushed onto the back, popped off the front.)

namespace mongo {

using TaskExecutor = executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;

struct MetadataManager::Tracker {
    /**
     * Creates a new Tracker with the usageCounter initialized to zero.
     */
    Tracker(std::unique_ptr<CollectionMetadata>, MetadataManager*);

    std::unique_ptr<CollectionMetadata> metadata;
    uint32_t usageCounter{0};
    boost::optional<ChunkRange> orphans{boost::none};

    // lock guards access to manager, which is zeroed by the ~MetadataManager(), but used by
    // ScopedCollectionMetadata when usageCounter falls to zero.
    stdx::mutex trackerLock;
    MetadataManager* manager{nullptr};
};

MetadataManager::MetadataManager(ServiceContext* sc, NamespaceString nss, TaskExecutor* executor)
    : _nss(std::move(nss)),
      _serviceContext(sc),
      _activeMetadataTracker(std::make_shared<Tracker>(nullptr, this)),
      _receivingChunks(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()),
      _notification(std::make_shared<Notification<Status>>()),
      _executor(executor),
      _rangesToClean() {}

MetadataManager::~MetadataManager() {
    {
        stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
        _shuttingDown = true;
    }
    std::list<std::shared_ptr<Tracker>> inUse;
    {
        // drain any threads that might remove _metadataInUse entries, push to deleter
        stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
        inUse = std::move(_metadataInUse);
    }

    // Trackers can outlive MetadataManager, so we still need to lock each tracker...
    std::for_each(inUse.begin(), inUse.end(), [](auto& tp) {
        stdx::lock_guard<stdx::mutex> scopedLock(tp->trackerLock);
        tp->manager = nullptr;
    });
    {  // ... and the active one too
        stdx::lock_guard<stdx::mutex> scopedLock(_activeMetadataTracker->trackerLock);
        _activeMetadataTracker->manager = nullptr;
    }

    // still need to block the deleter thread:
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    Status status{ErrorCodes::InterruptedDueToReplStateChange,
                  "tracking orphaned range deletion abandoned because the"
                  " collection was dropped or became unsharded"};
    if (!*_notification) {  // check just because test driver triggers it
        _notification->set(status);
    }
    _rangesToClean.clear(status);
}

ScopedCollectionMetadata MetadataManager::getActiveMetadata() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    if (_activeMetadataTracker) {
        return ScopedCollectionMetadata(_activeMetadataTracker);
    }
    return ScopedCollectionMetadata();
}

size_t MetadataManager::numberOfMetadataSnapshots() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _metadataInUse.size();
}

void MetadataManager::refreshActiveMetadata(std::unique_ptr<CollectionMetadata> remoteMetadata) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    // Collection was never sharded in the first place. This check is necessary in order to avoid
    // extraneous logging in the not-a-shard case, because all call sites always try to get the
    // collection sharding information regardless of whether the node is sharded or not.
    if (!remoteMetadata && !_activeMetadataTracker->metadata) {
        invariant(_receivingChunks.empty());
        invariant(_rangesToClean.isEmpty());
        return;
    }

    // Collection is becoming unsharded
    if (!remoteMetadata) {
        log() << "Marking collection " << _nss.ns() << " with "
              << _activeMetadataTracker->metadata->toStringBasic() << " as no longer sharded";

        _receivingChunks.clear();
        _rangesToClean.clear(Status{ErrorCodes::InterruptedDueToReplStateChange,
                                    "Collection sharding metadata destroyed"});
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
        invariant(_rangesToClean.isEmpty());

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
        _rangesToClean.clear(Status::OK());
        _metadataInUse.clear();
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

    // Resolve any receiving chunks, which might have completed by now.
    // Should be no more than one.
    for (auto it = _receivingChunks.begin(); it != _receivingChunks.end();) {
        BSONObj const& min = it->first;
        BSONObj const& max = it->second.getMaxKey();

        if (!remoteMetadata->rangeOverlapsChunk(ChunkRange(min, max))) {
            ++it;
            continue;
        }
        // The remote metadata contains a chunk we were earlier in the process of receiving, so
        // we deem it successfully received.
        LOG(2) << "Verified chunk " << ChunkRange(min, max) << " for collection " << _nss.ns()
               << " has been migrated to this shard earlier";

        _receivingChunks.erase(it);
        it = _receivingChunks.begin();
    }

    _setActiveMetadata_inlock(std::move(remoteMetadata));
}

void MetadataManager::_setActiveMetadata_inlock(std::unique_ptr<CollectionMetadata> newMetadata) {
    if (_activeMetadataTracker->usageCounter != 0 || _activeMetadataTracker->orphans) {
        _metadataInUse.push_back(std::move(_activeMetadataTracker));
    }
    _activeMetadataTracker = std::make_shared<Tracker>(std::move(newMetadata), this);
}

// call locked
void MetadataManager::_retireExpiredMetadata() {
    bool notify = false;
    while (!_metadataInUse.empty() && _metadataInUse.front()->usageCounter == 0) {
        // No ScopedCollectionMetadata can see this Tracker, other than, maybe, the caller.
        auto& tracker = _metadataInUse.front();
        if (tracker->orphans) {
            notify = true;
            log() << "Queries possibly dependent on " << _nss.ns() << " range " << *tracker->orphans
                  << " finished; scheduling range for deletion";
            _pushRangeToClean(*tracker->orphans);
        }
        tracker->metadata.reset();   // Discard the CollectionMetadata.
        _metadataInUse.pop_front();  // Disconnect from the tracker (and maybe destroy it)
    }
    if (_metadataInUse.empty() && _activeMetadataTracker->orphans) {
        notify = true;
        log() << "Queries possibly dependent on " << _nss.ns() << " range "
              << *_activeMetadataTracker->orphans << " finished; scheduling range for deletion";
        _pushRangeToClean(*_activeMetadataTracker->orphans);
        _activeMetadataTracker->orphans = boost::none;
    }
    if (notify) {
        _notifyInUse();  // wake up waitForClean because we changed inUse
    }
}

MetadataManager::Tracker::Tracker(std::unique_ptr<CollectionMetadata> md, MetadataManager* mgr)
    : metadata(std::move(md)), manager(mgr) {}

// ScopedCollectionMetadata members

// call with MetadataManager locked
ScopedCollectionMetadata::ScopedCollectionMetadata(
    std::shared_ptr<MetadataManager::Tracker> tracker)
    : _tracker(std::move(tracker)) {
    ++_tracker->usageCounter;
}

ScopedCollectionMetadata::~ScopedCollectionMetadata() {
    _clear();
}

CollectionMetadata* ScopedCollectionMetadata::operator->() const {
    return _tracker ? _tracker->metadata.get() : nullptr;
}

CollectionMetadata* ScopedCollectionMetadata::getMetadata() const {
    return _tracker ? _tracker->metadata.get() : nullptr;
}

void ScopedCollectionMetadata::_clear() {
    if (!_tracker) {
        return;
    }
    // Note: There is no risk of deadlock here because the only other place in MetadataManager
    // that takes the trackerLock, ~MetadataManager(), does not hold _managerLock at the same time,
    // and ScopedCollectionMetadata takes _managerLock only here.
    stdx::unique_lock<stdx::mutex> trackerLock(_tracker->trackerLock);
    MetadataManager* manager = _tracker->manager;
    if (manager) {
        stdx::lock_guard<stdx::mutex> managerLock(_tracker->manager->_managerLock);
        trackerLock.unlock();
        invariant(_tracker->usageCounter != 0);
        if (--_tracker->usageCounter == 0 && !manager->_shuttingDown) {
            // MetadataManager doesn't care which usageCounter went to zero.  It justs retires all
            // that are older than the oldest tracker still in use by queries. (Some start out at
            // zero, some go to zero but can't be expired yet.)  Note that new instances of
            // ScopedCollectionMetadata may get attached to the active tracker, so its usage
            // count can increase from zero, unlike most reference counts.
            manager->_retireExpiredMetadata();
        }
    } else {
        trackerLock.unlock();
    }
    _tracker.reset();  // disconnect from the tracker.
}

// do not call with MetadataManager locked
ScopedCollectionMetadata::ScopedCollectionMetadata(ScopedCollectionMetadata&& other) {
    *this = std::move(other);  // Rely on this->_tracker being zero-initialized already.
}

// do not call with MetadataManager locked
ScopedCollectionMetadata& ScopedCollectionMetadata::operator=(ScopedCollectionMetadata&& other) {
    if (this != &other) {
        _clear();
        _tracker = std::move(other._tracker);
    }
    return *this;
}

ScopedCollectionMetadata::operator bool() const {
    return _tracker && _tracker->metadata;  // with a Collection lock the metadata member is stable
}

void MetadataManager::toBSONPending(BSONArrayBuilder& bb) const {
    for (auto it = _receivingChunks.begin(); it != _receivingChunks.end(); ++it) {
        BSONArrayBuilder pendingBB(bb.subarrayStart());
        pendingBB.append(it->first);
        pendingBB.append(it->second.getMaxKey());
        pendingBB.done();
    }
}

void MetadataManager::append(BSONObjBuilder* builder) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

    _rangesToClean.append(builder);

    BSONArrayBuilder pcArr(builder->subarrayStart("pendingChunks"));
    for (const auto& entry : _receivingChunks) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second.getMaxKey());
        r.append(&obj);
        pcArr.append(obj.done());
    }
    pcArr.done();

    BSONArrayBuilder amrArr(builder->subarrayStart("activeMetadataRanges"));
    for (const auto& entry : _activeMetadataTracker->metadata->getChunks()) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second.getMaxKey());
        r.append(&obj);
        amrArr.append(obj.done());
    }
    amrArr.done();
}

void MetadataManager::_scheduleCleanup(executor::TaskExecutor* executor, NamespaceString nss) {
    executor->scheduleWork([executor, nss](auto&) {
        const int maxToDelete = std::max(int(internalQueryExecYieldIterations.load()), 1);
        Client::initThreadIfNotAlready("Collection Range Deleter");
        auto UniqueOpCtx = Client::getCurrent()->makeOperationContext();
        auto opCtx = UniqueOpCtx.get();
        bool again = CollectionRangeDeleter::cleanUpNextRange(opCtx, nss, maxToDelete);
        if (again) {
            _scheduleCleanup(executor, nss);
        }
    });
}

// call locked
void MetadataManager::_pushRangeToClean(ChunkRange const& range) {
    _rangesToClean.add(range);
    if (_rangesToClean.size() == 1) {
        _scheduleCleanup(_executor, _nss);
    }
}

void MetadataManager::_addToReceiving(ChunkRange const& range) {
    _receivingChunks.insert(
        std::make_pair(range.getMin().getOwned(),
                       CachedChunkInfo(range.getMax().getOwned(), ChunkVersion::IGNORED())));
}

bool MetadataManager::beginReceive(ChunkRange const& range) {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);

    auto* metadata = _activeMetadataTracker->metadata.get();
    if (_overlapsInUseChunk(range) || metadata->rangeOverlapsChunk(range)) {
        log() << "Rejecting in-migration to " << _nss.ns() << " range " << range
              << " because a running query might depend on documents in the range";
        return false;
    }
    _addToReceiving(range);
    _pushRangeToClean(range);
    log() << "Scheduling deletion of any documents in " << _nss.ns() << " range " << range
          << " before migrating in a chunk covering the range";
    return true;
}

void MetadataManager::_removeFromReceiving(ChunkRange const& range) {
    auto it = _receivingChunks.find(range.getMin());
    invariant(it != _receivingChunks.end());
    _receivingChunks.erase(it);
}

void MetadataManager::forgetReceive(ChunkRange const& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    // This is potentially a partially received chunk, which needs to be cleaned up. We know none
    // of these documents are in use, so they can go straight to the deletion queue.
    log() << "Abandoning in-migration of " << _nss.ns() << " range " << range
          << "; scheduling deletion of any documents already copied";

    invariant(!_overlapsInUseChunk(range) &&
              !_activeMetadataTracker->metadata->rangeOverlapsChunk(range));

    _removeFromReceiving(range);
    _pushRangeToClean(range);
}

Status MetadataManager::cleanUpRange(ChunkRange const& range) {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    CollectionMetadata* metadata = _activeMetadataTracker->metadata.get();
    invariant(metadata != nullptr);

    if (metadata->rangeOverlapsChunk(range)) {
        return {ErrorCodes::RangeOverlapConflict,
                str::stream() << "Requested deletion range overlaps a live shard chunk"};
    }

    if (rangeMapOverlaps(_receivingChunks, range.getMin(), range.getMax())) {
        return {ErrorCodes::RangeOverlapConflict,
                str::stream() << "Requested deletion range overlaps a chunk being migrated in"};
    }

    if (!_overlapsInUseChunk(range)) {
        // No running queries can depend on it, so queue it for deletion immediately.
        log() << "Scheduling " << _nss.ns() << " range " << redact(range.toString())
              << " for deletion";

        _pushRangeToClean(range);
    } else {
        invariant(!_metadataInUse.empty());

        if (_activeMetadataTracker->orphans) {
            _setActiveMetadata_inlock(_activeMetadataTracker->metadata->clone());
        }

        _activeMetadataTracker->orphans.emplace(range.getMin().getOwned(),
                                                range.getMax().getOwned());

        log() << "Scheduling " << _nss.ns() << " range " << redact(range.toString())
              << " for deletion after all possibly-dependent queries finish";
    }

    return Status::OK();
}

size_t MetadataManager::numberOfRangesToCleanStillInUse() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    size_t count = _activeMetadataTracker->orphans ? 1 : 0;
    count += std::count_if(_metadataInUse.begin(), _metadataInUse.end(), [](auto& tracker) {
        return bool(tracker->orphans);
    });
    return count;
}

size_t MetadataManager::numberOfRangesToClean() {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    return _rangesToClean.size();
}

MetadataManager::CleanupNotification MetadataManager::trackOrphanedDataCleanup(
    ChunkRange const& range) {

    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    if (_overlapsInUseCleanups(range))
        return _notification;
    return _rangesToClean.overlaps(range);
}

// call locked
bool MetadataManager::_overlapsInUseChunk(ChunkRange const& range) {
    if (_activeMetadataTracker->metadata->rangeOverlapsChunk(range)) {
        return true;  // refcount doesn't matter for the active case
    }
    for (auto& tracker : _metadataInUse) {
        if (tracker->usageCounter != 0 && tracker->metadata->rangeOverlapsChunk(range)) {
            return true;
        }
    }
    return false;
}

// call locked
bool MetadataManager::_overlapsInUseCleanups(ChunkRange const& range) {
    if (_activeMetadataTracker->orphans && _activeMetadataTracker->orphans->overlapWith(range)) {
        return true;
    }
    for (auto& tracker : _metadataInUse) {
        if (tracker->orphans && bool(tracker->orphans->overlapWith(range))) {
            return true;
        }
    }
    return false;
}

// call locked
void MetadataManager::_notifyInUse() {
    _notification->set(Status::OK());  // wake up waitForClean
    _notification = std::make_shared<Notification<Status>>();
}

boost::optional<KeyRange> MetadataManager::getNextOrphanRange(BSONObj const& from) {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    invariant(_activeMetadataTracker->metadata);
    return _activeMetadataTracker->metadata->getNextOrphanRange(_receivingChunks, from);
}


}  // namespace mongo
