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

#include "mongo/base/string_data.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

// MetadataManager maintains pointers to CollectionMetadata objects in a member list named
// _metadata.  Each CollectionMetadata contains an immutable _chunksMap of chunks assigned to this
// shard, along with details related to its own lifecycle in a member _tracker.
//
// The current chunk mapping, used by queries starting up, is at _metadata.back().  Each query,
// when it starts up, requests and holds a ScopedCollectionMetadata object, and destroys it on
// termination. Each ScopedCollectionMetadata keeps a shared_ptr to its CollectionMetadata chunk
// mapping, and to the MetadataManager itself.  CollectionMetadata mappings also keep a record of
// chunk ranges that may be deleted when it is determined that the range can no longer be in use.
//
// ScopedCollectionMetadata's destructor decrements the CollectionMetadata's usageCounter.
// Whenever a usageCounter drops to zero, we check whether any now-unused CollectionMetadata
// elements can be popped off the front of _metadata.  We need to keep the unused elements in the
// middle (as seen below) because they may schedule deletions of chunks depended on by older
// mappings.
//
// New chunk mappings are pushed onto the back of _metadata. Subsequently started queries use the
// new mapping while still-running queries continue using the older "snapshot" mappings.  We treat
// _metadata.back()'s usage count differently from the snapshots because it can't reliably be
// compared to zero; a new query may increment it at any time.
//
// (Note that the collection may be dropped or become unsharded, and even get made and sharded
// again, between construction and destruction of a ScopedCollectionMetadata).
//
// MetadataManager also contains a CollectionRangeDeleter _rangesToClean that queues orphan ranges
// being deleted in a background thread, and a mapping _receivingChunks of the ranges being migrated
// in, to avoid deleting them.  Each range deletion is paired with a notification object triggered
// when the deletion is completed or abandoned.
//
//                                        ____________________________
//  (s): std::shared_ptr<>       Clients:| ScopedCollectionMetadata   |
//   _________________________        +----(s) manager   metadata (s)------------------+
//  | CollectionShardingState |       |  |____________________________|  |             |
//  |  _metadataManager (s)   |       +-------(s) manager  metadata (s)--------------+ |
//  |____________________|____|       |     |____________________________|   |       | |
//   ____________________v________    +------------(s) manager  metadata (s)-----+   | |
//  | MetadataManager             |   |         |____________________________|   |   | |
//  |                             |<--+                                          |   | |
//  |                             |        ___________________________  (1 use)  |   | |
//  | getActiveMetadata():    /---------->| CollectionMetadata        |<---------+   | |
//  |     back(): [(s),------/    |       |  _________________________|_             | |
//  |              (s),-------------------->| CollectionMetadata        | (0 uses)   | |
//  |  _metadata:  (s)]------\    |       | |  _________________________|_           | |
//  |                         \-------------->| CollectionMetadata        |          | |
//  |  _receivingChunks           |       | | |                           | (2 uses) | |
//  |  _rangesToClean:            |       | | |  _tracker:                |<---------+ |
//  |  _________________________  |       | | |  _______________________  |<-----------+
//  | | CollectionRangeDeleter  | |       | | | | Tracker               | |
//  | |                         | |       | | | |                       | |
//  | |  _orphans [range,notif, | |       | | | | usageCounter          | |
//  | |            range,notif, | |       | | | | orphans [range,notif, | |
//  | |                 ...   ] | |       | | | |          range,notif, | |
//  | |                         | |       | | | |              ...    ] | |
//  | |_________________________| |       |_| | |_______________________| |
//  |_____________________________|         | |  _chunksMap               |
//                                          |_|  _chunkVersion            |
//                                            |  ...                      |
//                                            |___________________________|
//
//  Note that _metadata as shown here has its front() at the bottom, back() at the top. As usual,
//  new entries are pushed onto the back, popped off the front.

namespace mongo {
namespace {

using TaskExecutor = executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;

MONGO_FAIL_POINT_DEFINE(suspendRangeDeletion);

/**
 * Deletes ranges, in background, until done, normally using a task executor attached to the
 * ShardingState.
 *
 * Each time it completes cleaning up a range, it wakes up clients waiting on completion of that
 * range, which may then verify that their range has no more deletions scheduled, and proceed.
 */
void scheduleCleanup(executor::TaskExecutor* executor,
                     NamespaceString nss,
                     OID epoch,
                     Date_t when) {
    LOG(1) << "Scheduling cleanup on " << nss.ns() << " at " << when;
    auto swCallbackHandle = executor->scheduleWorkAt(
        when, [ executor, nss = std::move(nss), epoch = std::move(epoch) ](auto&) {
            Client::initThreadIfNotAlready("Collection Range Deleter");
            auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            const int maxToDelete = std::max(int(internalQueryExecYieldIterations.load()), 1);

            MONGO_FAIL_POINT_PAUSE_WHILE_SET(suspendRangeDeletion);

            auto next = CollectionRangeDeleter::cleanUpNextRange(opCtx, nss, epoch, maxToDelete);
            if (next) {
                scheduleCleanup(executor, std::move(nss), std::move(epoch), *next);
            }
        });

    if (!swCallbackHandle.isOK()) {
        log() << "Failed to schedule the orphan data cleanup task"
              << causedBy(redact(swCallbackHandle.getStatus()));
    }
}

}  // namespace

MetadataManager::MetadataManager(ServiceContext* serviceContext,
                                 NamespaceString nss,
                                 TaskExecutor* executor)
    : _serviceContext(serviceContext),
      _nss(std::move(nss)),
      _executor(executor),
      _receivingChunks(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()) {}

MetadataManager::~MetadataManager() {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    _clearAllCleanups(lg);
    _metadata.clear();
}

void MetadataManager::_clearAllCleanups(WithLock lock) {
    _clearAllCleanups(
        lock,
        {ErrorCodes::InterruptedDueToReplStateChange,
         str::stream() << "Range deletions in " << _nss.ns()
                       << " abandoned because collection was dropped or became unsharded"});
}

void MetadataManager::_clearAllCleanups(WithLock, Status status) {
    for (auto& tracker : _metadata) {
        std::ignore = _rangesToClean.add(std::move(tracker->orphans));
    }
    _rangesToClean.clear(status);
}

ScopedCollectionMetadata MetadataManager::getActiveMetadata(std::shared_ptr<MetadataManager> self) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    if (!_metadata.empty()) {
        return ScopedCollectionMetadata(lg, std::move(self), _metadata.back());
    }

    return ScopedCollectionMetadata();
}

ScopedCollectionMetadata MetadataManager::createMetadataAt(OperationContext* opCtx,
                                                           LogicalTime atClusterTime) {
    auto cache = Grid::get(opCtx)->catalogCache();
    if (!cache) {
        return ScopedCollectionMetadata();
    }

    auto routingTable = cache->getCollectionRoutingTableHistoryNoRefresh(_nss);
    if (!routingTable) {
        return ScopedCollectionMetadata();
    }
    auto cm = std::make_shared<ChunkManager>(routingTable, atClusterTime.asTimestamp());

    CollectionMetadata metadata(std::move(cm), ShardingState::get(opCtx)->getShardName());

    auto metadataTracker =
        std::make_shared<MetadataManager::CollectionMetadataTracker>(std::move(metadata));

    return ScopedCollectionMetadata(std::move(metadataTracker));
}

size_t MetadataManager::numberOfMetadataSnapshots() const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    if (_metadata.empty())
        return 0;

    return _metadata.size() - 1;
}

void MetadataManager::refreshActiveMetadata(std::unique_ptr<CollectionMetadata> remoteMetadata) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    // Collection was never sharded in the first place. This check is necessary in order to avoid
    // extraneous logging in the not-a-shard case, because all call sites always try to get the
    // collection sharding information regardless of whether the node is sharded or not.
    if (!remoteMetadata && _metadata.empty()) {
        invariant(_receivingChunks.empty());
        invariant(_rangesToClean.isEmpty());
        return;
    }

    // Collection is becoming unsharded
    if (!remoteMetadata) {
        log() << "Marking collection " << _nss.ns() << " with "
              << redact(_metadata.back()->metadata.toStringBasic()) << " as unsharded";

        _receivingChunks.clear();
        _clearAllCleanups(lg);
        _metadata.clear();
        return;
    }

    // Collection is becoming sharded
    if (_metadata.empty()) {
        log() << "Marking collection " << _nss.ns() << " as sharded with "
              << remoteMetadata->toStringBasic();

        invariant(_receivingChunks.empty());
        _setActiveMetadata(lg, std::move(*remoteMetadata));
        invariant(_rangesToClean.isEmpty());
        return;
    }

    auto* const activeMetadata = &_metadata.back()->metadata;

    // If the metadata being installed has a different epoch from ours, this means the collection
    // was dropped and recreated, so we must entirely reset the metadata state
    if (activeMetadata->getCollVersion().epoch() != remoteMetadata->getCollVersion().epoch()) {
        log() << "Overwriting metadata for collection " << _nss.ns() << " from "
              << activeMetadata->toStringBasic() << " to " << remoteMetadata->toStringBasic()
              << " due to epoch change";

        _receivingChunks.clear();
        _setActiveMetadata(lg, std::move(*remoteMetadata));
        _clearAllCleanups(lg);
        return;
    }

    // We already have newer version
    if (activeMetadata->getCollVersion() >= remoteMetadata->getCollVersion()) {
        LOG(1) << "Ignoring update of active metadata " << activeMetadata->toStringBasic()
               << " with an older " << remoteMetadata->toStringBasic();
        return;
    }

    log() << "Updating collection metadata for " << _nss.ns() << " from "
          << activeMetadata->toStringBasic() << " to " << remoteMetadata->toStringBasic();

    // Resolve any receiving chunks, which might have completed by now
    for (auto it = _receivingChunks.begin(); it != _receivingChunks.end();) {
        const ChunkRange receivingRange(it->first, it->second);

        if (!remoteMetadata->rangeOverlapsChunk(receivingRange)) {
            ++it;
            continue;
        }

        // The remote metadata contains a chunk we were earlier in the process of receiving, so we
        // deem it successfully received
        LOG(2) << "Verified chunk " << redact(receivingRange.toString()) << " for collection "
               << _nss.ns() << " has been migrated to this shard earlier";

        _receivingChunks.erase(it);
        it = _receivingChunks.begin();
    }

    _setActiveMetadata(lg, std::move(*remoteMetadata));
}

void MetadataManager::_setActiveMetadata(WithLock wl, CollectionMetadata newMetadata) {
    _metadata.emplace_back(std::make_shared<CollectionMetadataTracker>(std::move(newMetadata)));
    _retireExpiredMetadata(wl);
}

void MetadataManager::_retireExpiredMetadata(WithLock lock) {
    while (_metadata.size() > 1 && !_metadata.front()->usageCounter) {
        if (!_metadata.front()->orphans.empty()) {
            log() << "Queries possibly dependent on " << _nss.ns()
                  << " range(s) finished; scheduling ranges for deletion";
            // It is safe to push orphan ranges from _metadata.back(), even though new queries might
            // start any time, because any request to delete a range it maps is rejected.
            _pushListToClean(lock, std::move(_metadata.front()->orphans));
        }

        _metadata.pop_front();
    }
}

void MetadataManager::toBSONPending(BSONArrayBuilder& bb) const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    for (auto it = _receivingChunks.begin(); it != _receivingChunks.end(); ++it) {
        BSONArrayBuilder pendingBB(bb.subarrayStart());
        pendingBB.append(it->first);
        pendingBB.append(it->second);
        pendingBB.done();
    }
}

void MetadataManager::append(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);

    _rangesToClean.append(builder);

    BSONArrayBuilder pcArr(builder->subarrayStart("pendingChunks"));
    for (const auto& entry : _receivingChunks) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second);
        r.append(&obj);
        pcArr.append(obj.done());
    }
    pcArr.done();

    if (_metadata.empty()) {
        return;
    }

    BSONArrayBuilder amrArr(builder->subarrayStart("activeMetadataRanges"));
    for (const auto& entry : _metadata.back()->metadata.getChunks()) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second);
        r.append(&obj);
        amrArr.append(obj.done());
    }
    amrArr.done();
}

auto MetadataManager::_pushRangeToClean(WithLock lock, ChunkRange const& range, Date_t when)
    -> CleanupNotification {
    std::list<Deletion> ranges;
    ranges.emplace_back(ChunkRange(range.getMin().getOwned(), range.getMax().getOwned()), when);
    auto& notifn = ranges.back().notification;
    _pushListToClean(lock, std::move(ranges));
    return notifn;
}

void MetadataManager::_pushListToClean(WithLock, std::list<Deletion> ranges) {
    auto when = _rangesToClean.add(std::move(ranges));
    if (when) {
        scheduleCleanup(
            _executor, _nss, _metadata.back()->metadata.getCollVersion().epoch(), *when);
    }
    invariant(ranges.empty());
}

auto MetadataManager::beginReceive(ChunkRange const& range) -> CleanupNotification {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());

    if (_overlapsInUseChunk(lg, range)) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      "Documents in target range may still be in use on the destination shard."};
    }

    _receivingChunks.emplace(range.getMin().getOwned(), range.getMax().getOwned());

    log() << "Scheduling deletion of any documents in " << _nss.ns() << " range "
          << redact(range.toString()) << " before migrating in a chunk covering the range";

    return _pushRangeToClean(lg, range, Date_t{});
}

void MetadataManager::forgetReceive(ChunkRange const& range) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());

    // This is potentially a partially received chunk, which needs to be cleaned up. We know none
    // of these documents are in use, so they can go straight to the deletion queue.
    log() << "Abandoning in-migration of " << _nss.ns() << " range " << range
          << "; scheduling deletion of any documents already copied";

    invariant(!_overlapsInUseChunk(lg, range));

    auto it = _receivingChunks.find(range.getMin());
    invariant(it != _receivingChunks.end());
    _receivingChunks.erase(it);

    _pushRangeToClean(lg, range, Date_t{}).abandon();
}

auto MetadataManager::cleanUpRange(ChunkRange const& range, Date_t whenToDelete)
    -> CleanupNotification {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());

    auto* const activeMetadata = _metadata.back().get();
    auto* const overlapMetadata = _findNewestOverlappingMetadata(lg, range);

    if (overlapMetadata == activeMetadata) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      str::stream() << "Requested deletion range overlaps a live shard chunk"};
    }

    if (rangeMapOverlaps(_receivingChunks, range.getMin(), range.getMax())) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      str::stream() << "Requested deletion range overlaps a chunk being"
                                       " migrated in"};
    }

    if (!overlapMetadata) {
        // No running queries can depend on it, so queue it for deletion immediately.
        const auto whenStr = (whenToDelete == Date_t{}) ? "immediate"_sd : "deferred"_sd;
        log() << "Scheduling " << whenStr << " deletion of " << _nss.ns() << " range "
              << redact(range.toString());
        return _pushRangeToClean(lg, range, whenToDelete);
    }

    log() << "Deletion of " << _nss.ns() << " range " << redact(range.toString())
          << " will be scheduled after all possibly dependent queries finish";

    // Put it on the oldest metadata permissible; the current one might live a long time.
    auto& orphans = overlapMetadata->orphans;
    orphans.emplace_back(ChunkRange(range.getMin().getOwned(), range.getMax().getOwned()),
                         whenToDelete);
    return orphans.back().notification;
}

std::vector<ScopedCollectionMetadata> MetadataManager::overlappingMetadata(
    std::shared_ptr<MetadataManager> const& self, ChunkRange const& range) {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());

    std::vector<ScopedCollectionMetadata> result;
    result.reserve(_metadata.size());

    // Start with the active metadata
    auto it = _metadata.rbegin();
    if ((*it)->metadata.rangeOverlapsChunk(range)) {
        // We ignore the refcount of the active mapping; effectively, we assume it is in use.
        result.push_back(ScopedCollectionMetadata(lg, self, (*it)));
    }

    // Continue to snapshots
    ++it;
    for (; it != _metadata.rend(); ++it) {
        auto& tracker = *it;

        // We want all the overlapping snapshot mappings still possibly in use by a query.
        if (tracker->usageCounter > 0 && tracker->metadata.rangeOverlapsChunk(range)) {
            result.push_back(ScopedCollectionMetadata(lg, self, tracker));
        }
    }

    return result;
}

size_t MetadataManager::numberOfRangesToCleanStillInUse() const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    size_t count = 0;
    for (auto& tracker : _metadata) {
        count += tracker->orphans.size();
    }
    return count;
}

size_t MetadataManager::numberOfRangesToClean() const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    return _rangesToClean.size();
}

auto MetadataManager::trackOrphanedDataCleanup(ChunkRange const& range) const
    -> boost::optional<CleanupNotification> {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    auto overlaps = _overlapsInUseCleanups(lg, range);
    if (overlaps) {
        return overlaps;
    }

    return _rangesToClean.overlaps(range);
}

auto MetadataManager::_findNewestOverlappingMetadata(WithLock, ChunkRange const& range)
    -> CollectionMetadataTracker* {
    invariant(!_metadata.empty());

    auto it = _metadata.rbegin();
    if ((*it)->metadata.rangeOverlapsChunk(range)) {
        return (*it).get();
    }

    ++it;
    for (; it != _metadata.rend(); ++it) {
        auto& tracker = *it;
        if (tracker->usageCounter && tracker->metadata.rangeOverlapsChunk(range)) {
            return tracker.get();
        }
    }

    return nullptr;
}

bool MetadataManager::_overlapsInUseChunk(WithLock lk, ChunkRange const& range) {
    auto* cm = _findNewestOverlappingMetadata(lk, range);
    return (cm != nullptr);
}

auto MetadataManager::_overlapsInUseCleanups(WithLock, ChunkRange const& range) const
    -> boost::optional<CleanupNotification> {
    invariant(!_metadata.empty());

    for (auto it = _metadata.rbegin(); it != _metadata.rend(); ++it) {
        const auto& orphans = (*it)->orphans;
        for (auto itOrphans = orphans.rbegin(); itOrphans != orphans.rend(); ++itOrphans) {
            const auto& orphan = *itOrphans;
            if (orphan.range.overlapWith(range)) {
                return orphan.notification;
            }
        }
    }

    return boost::none;
}

boost::optional<ChunkRange> MetadataManager::getNextOrphanRange(BSONObj const& from) const {
    stdx::lock_guard<stdx::mutex> lg(_managerLock);
    invariant(!_metadata.empty());
    return _metadata.back()->metadata.getNextOrphanRange(_receivingChunks, from);
}

ScopedCollectionMetadata::ScopedCollectionMetadata() = default;

ScopedCollectionMetadata::ScopedCollectionMetadata(
    WithLock,
    std::shared_ptr<MetadataManager> metadataManager,
    std::shared_ptr<MetadataManager::CollectionMetadataTracker> metadataTracker)
    : _metadataManager(std::move(metadataManager)), _metadataTracker(std::move(metadataTracker)) {
    invariant(_metadataManager);
    invariant(_metadataTracker);
    ++_metadataTracker->usageCounter;
}

ScopedCollectionMetadata::ScopedCollectionMetadata(
    std::shared_ptr<MetadataManager::CollectionMetadataTracker> metadataTracker)
    : _metadataTracker(std::move(metadataTracker)) {
    invariant(_metadataTracker);
}

ScopedCollectionMetadata::ScopedCollectionMetadata(ScopedCollectionMetadata&& other) {
    *this = std::move(other);
}

ScopedCollectionMetadata& ScopedCollectionMetadata::operator=(ScopedCollectionMetadata&& other) {
    if (this != &other) {
        _clear();

        _metadataManager = std::move(other._metadataManager);
        _metadataTracker = std::move(other._metadataTracker);

        other._metadataManager = nullptr;
        other._metadataTracker = nullptr;
    }
    return *this;
}

CollectionMetadata* ScopedCollectionMetadata::getMetadata() const {
    return _metadataTracker ? &_metadataTracker->metadata : nullptr;
}

BSONObj ScopedCollectionMetadata::extractDocumentKey(BSONObj const& doc) const {
    BSONObj key;
    if (*this) {  // is sharded
        auto const& pattern = _metadataTracker->metadata.getChunkManager()->getShardKeyPattern();
        key = dotted_path_support::extractElementsBasedOnTemplate(doc, pattern.toBSON());
        if (pattern.hasId()) {
            return key;
        }
        // else, try to append an _id field from the document.
    }

    if (auto id = doc["_id"]) {
        return key.isEmpty() ? id.wrap() : BSONObjBuilder(std::move(key)).append(id).obj();
    }

    // For legacy documents that lack an _id, use the document itself as its key.
    return doc;
}

void ScopedCollectionMetadata::_clear() {
    if (!_metadataManager) {
        return;
    }

    stdx::lock_guard<stdx::mutex> managerLock(_metadataManager->_managerLock);
    invariant(_metadataTracker->usageCounter != 0);
    if (--_metadataTracker->usageCounter == 0) {
        // MetadataManager doesn't care which usageCounter went to zero. It just retires all that
        // are older than the oldest metadata still in use by queries (some start out at zero, some
        // go to zero but can't be expired yet).
        //
        // Note that new instances of ScopedCollectionMetadata may get attached to _metadata.back(),
        // so its usage count can increase from zero, unlike other reference counts.
        _metadataManager->_retireExpiredMetadata(managerLock);
    }

    _metadataManager.reset();
    _metadataTracker.reset();
}

}  // namespace mongo
