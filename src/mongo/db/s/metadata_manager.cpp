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

using TaskExecutor = executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;

MetadataManager::MetadataManager(ServiceContext* sc, NamespaceString nss, TaskExecutor* executor)
    : _nss(std::move(nss)),
      _serviceContext(sc),
      _receivingChunks(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>()),
      _executor(executor),
      _rangesToClean() {}

MetadataManager::~MetadataManager() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    _clearAllCleanups();
    auto metadata = std::move(_metadata);
}

void MetadataManager::_clearAllCleanups() {
    _clearAllCleanups(
        {ErrorCodes::InterruptedDueToReplStateChange,
         str::stream() << "Range deletions in " << _nss.ns()
                       << " abandoned because collection was dropped or became unsharded"});
}

void MetadataManager::_clearAllCleanups(Status status) {
    for (auto& metadata : _metadata) {
        _pushListToClean(std::move(metadata->_tracker.orphans));
    }
    _rangesToClean.clear(status);
}

ScopedCollectionMetadata MetadataManager::getActiveMetadata(std::shared_ptr<MetadataManager> self) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    if (!_metadata.empty()) {
        return ScopedCollectionMetadata(std::move(self), _metadata.back());
    }
    return ScopedCollectionMetadata();
}

size_t MetadataManager::numberOfMetadataSnapshots() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    return _metadata.size() - 1;
}

void MetadataManager::refreshActiveMetadata(std::unique_ptr<CollectionMetadata> remoteMetadata) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);

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
        log() << "Marking collection " << _nss.ns() << " with " << _metadata.back()->toStringBasic()
              << " as no longer sharded";

        _receivingChunks.clear();
        _clearAllCleanups();
        _metadata.clear();
        return;
    }

    // We should never be setting unsharded metadata
    invariant(!remoteMetadata->getCollVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));
    invariant(!remoteMetadata->getShardVersion().isWriteCompatibleWith(ChunkVersion::UNSHARDED()));

    // Collection is becoming sharded
    if (_metadata.empty()) {
        log() << "Marking collection " << _nss.ns() << " as sharded with "
              << remoteMetadata->toStringBasic();

        invariant(_receivingChunks.empty());
        invariant(_rangesToClean.isEmpty());

        _setActiveMetadata_inlock(std::move(remoteMetadata));
        return;
    }

    auto* activeMetadata = _metadata.back().get();

    // If the metadata being installed has a different epoch from ours, this means the collection
    // was dropped and recreated, so we must entirely reset the metadata state
    if (activeMetadata->getCollVersion().epoch() != remoteMetadata->getCollVersion().epoch()) {
        log() << "Overwriting metadata for collection " << _nss.ns() << " from "
              << activeMetadata->toStringBasic() << " to " << remoteMetadata->toStringBasic()
              << " due to epoch change";

        _receivingChunks.clear();
        _setActiveMetadata_inlock(std::move(remoteMetadata));
        _clearAllCleanups();
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
    invariant(newMetadata);
    _metadata.push_back(std::move(newMetadata));
    _retireExpiredMetadata();
}

void MetadataManager::_retireExpiredMetadata() {
    if (_metadata.empty()) {
        return;  // The collection was dropped, or went unsharded, before the query was cleaned up.
    }
    for (; _metadata.front()->_tracker.usageCounter == 0; _metadata.pop_front()) {
        // No ScopedCollectionMetadata can see _metadata->front(), other than, maybe, the caller.
        if (!_metadata.front()->_tracker.orphans.empty()) {
            log() << "Queries possibly dependent on " << _nss.ns()
                  << " range(s) finished; scheduling for deletion";
            // It is safe to push orphan ranges from _metadata.back(), even though new queries might
            // start any time, because any request to delete a range it maps is rejected.
            _pushListToClean(std::move(_metadata.front()->_tracker.orphans));
        }
        if (&_metadata.front() == &_metadata.back())
            break;  // do not pop the active chunk mapping!
    }
}

// ScopedCollectionMetadata members

// call with MetadataManager locked
ScopedCollectionMetadata::ScopedCollectionMetadata(std::shared_ptr<MetadataManager> manager,
                                                   std::shared_ptr<CollectionMetadata> metadata)
    : _metadata(std::move(metadata)), _manager(std::move(manager)) {
    invariant(_metadata);
    invariant(_manager);
    ++_metadata->_tracker.usageCounter;
}

ScopedCollectionMetadata::~ScopedCollectionMetadata() {
    _clear();
}

CollectionMetadata* ScopedCollectionMetadata::operator->() const {
    return _metadata ? _metadata.get() : nullptr;
}

CollectionMetadata* ScopedCollectionMetadata::getMetadata() const {
    return _metadata ? _metadata.get() : nullptr;
}

void ScopedCollectionMetadata::_clear() {
    if (!_manager) {
        return;
    }
    stdx::lock_guard<stdx::mutex> managerLock(_manager->_managerLock);
    invariant(_metadata->_tracker.usageCounter != 0);
    if (--_metadata->_tracker.usageCounter == 0) {
        // MetadataManager doesn't care which usageCounter went to zero.  It justs retires all
        // that are older than the oldest metadata still in use by queries. (Some start out at
        // zero, some go to zero but can't be expired yet.)  Note that new instances of
        // ScopedCollectionMetadata may get attached to _metadata.back(), so its usage count can
        // increase from zero, unlike other reference counts.
        _manager->_retireExpiredMetadata();
    }
    _metadata.reset();
    _manager.reset();
}

// do not call with MetadataManager locked
ScopedCollectionMetadata::ScopedCollectionMetadata(ScopedCollectionMetadata&& other) {
    *this = std::move(other);  // Rely on being zero-initialized already.
}

// do not call with MetadataManager locked
ScopedCollectionMetadata& ScopedCollectionMetadata::operator=(ScopedCollectionMetadata&& other) {
    if (this != &other) {
        _clear();
        _metadata = std::move(other._metadata);
        _manager = std::move(other._manager);
    }
    return *this;
}

ScopedCollectionMetadata::operator bool() const {
    return _metadata.get();
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

    if (_metadata.empty()) {
        return;
    }
    BSONArrayBuilder amrArr(builder->subarrayStart("activeMetadataRanges"));
    for (const auto& entry : _metadata.back()->getChunks()) {
        BSONObjBuilder obj;
        ChunkRange r = ChunkRange(entry.first, entry.second.getMaxKey());
        r.append(&obj);
        amrArr.append(obj.done());
    }
    amrArr.done();
}

void MetadataManager::_scheduleCleanup(executor::TaskExecutor* executor,
                                       NamespaceString nss,
                                       CollectionRangeDeleter::Action action) {
    executor
        ->scheduleWork([executor, nss, action](auto&) {
            const int maxToDelete = std::max(int(internalQueryExecYieldIterations.load()), 1);
            Client::initThreadIfNotAlready("Collection Range Deleter");
            auto UniqueOpCtx = Client::getCurrent()->makeOperationContext();
            auto opCtx = UniqueOpCtx.get();
            auto next = CollectionRangeDeleter::cleanUpNextRange(opCtx, nss, action, maxToDelete);
            if (next != CollectionRangeDeleter::Action::kFinished) {
                _scheduleCleanup(executor, nss, next);
            }
        })
        .status_with_transitional_ignore();
}

auto MetadataManager::_pushRangeToClean(ChunkRange const& range) -> CleanupNotification {
    std::list<Deletion> ranges;
    ranges.emplace_back(Deletion{ChunkRange{range.getMin().getOwned(), range.getMax().getOwned()}});
    auto& notifn = ranges.back().notification;
    _pushListToClean(std::move(ranges));
    return notifn;
}

void MetadataManager::_pushListToClean(std::list<Deletion> ranges) {
    if (_rangesToClean.add(std::move(ranges))) {
        _scheduleCleanup(_executor, _nss, CollectionRangeDeleter::Action::kWriteOpLog);
    }
    invariant(ranges.empty());
}

void MetadataManager::_addToReceiving(ChunkRange const& range) {
    _receivingChunks.insert(
        std::make_pair(range.getMin().getOwned(),
                       CachedChunkInfo(range.getMax().getOwned(), ChunkVersion::IGNORED())));
}

auto MetadataManager::beginReceive(ChunkRange const& range) -> CleanupNotification {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    invariant(!_metadata.empty());

    if (_overlapsInUseChunk(range)) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      "Documents in target range may still be in use on the destination shard."};
    }
    _addToReceiving(range);
    log() << "Scheduling deletion of any documents in " << _nss.ns() << " range "
          << redact(range.toString()) << " before migrating in a chunk covering the range";
    return _pushRangeToClean(range);
}

void MetadataManager::_removeFromReceiving(ChunkRange const& range) {
    auto it = _receivingChunks.find(range.getMin());
    invariant(it != _receivingChunks.end());
    _receivingChunks.erase(it);
}

void MetadataManager::forgetReceive(ChunkRange const& range) {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    invariant(!_metadata.empty());

    // This is potentially a partially received chunk, which needs to be cleaned up. We know none
    // of these documents are in use, so they can go straight to the deletion queue.
    log() << "Abandoning in-migration of " << _nss.ns() << " range " << range
          << "; scheduling deletion of any documents already copied";

    invariant(!_overlapsInUseChunk(range));
    _removeFromReceiving(range);
    _pushRangeToClean(range).abandon();
}

auto MetadataManager::cleanUpRange(ChunkRange const& range) -> CleanupNotification {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    invariant(!_metadata.empty());

    auto* activeMetadata = _metadata.back().get();
    if (activeMetadata->rangeOverlapsChunk(range)) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      str::stream() << "Requested deletion range overlaps a live shard chunk"};
    }

    if (rangeMapOverlaps(_receivingChunks, range.getMin(), range.getMax())) {
        return Status{ErrorCodes::RangeOverlapConflict,
                      str::stream() << "Requested deletion range overlaps a chunk being"
                                       " migrated in"};
    }

    if (!_overlapsInUseChunk(range)) {
        // No running queries can depend on it, so queue it for deletion immediately.
        log() << "Scheduling " << _nss.ns() << " range " << redact(range.toString())
              << " for immediate deletion";
        return _pushRangeToClean(range);
    }

    activeMetadata->_tracker.orphans.emplace_back(
        Deletion{ChunkRange{range.getMin().getOwned(), range.getMax().getOwned()}});

    log() << "Scheduling " << _nss.ns() << " range " << redact(range.toString())
          << " for deletion after all possibly-dependent queries finish";

    return activeMetadata->_tracker.orphans.back().notification;
}

auto MetadataManager::overlappingMetadata(std::shared_ptr<MetadataManager> const& self,
                                          ChunkRange const& range)
    -> std::vector<ScopedCollectionMetadata> {
    invariant(!_metadata.empty());
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    std::vector<ScopedCollectionMetadata> result;
    result.reserve(_metadata.size());
    auto it = _metadata.crbegin();  // start with the current active chunk mapping
    if ((*it)->rangeOverlapsChunk(range)) {
        // We ignore the refcount of the active mapping; effectively, we assume it is in use.
        result.push_back(ScopedCollectionMetadata(self, *it));
    }
    ++it;  // step to snapshots
    for (auto end = _metadata.crend(); it != end; ++it) {
        // We want all the overlapping snapshot mappings still possibly in use by a query.
        if ((*it)->_tracker.usageCounter > 0 && (*it)->rangeOverlapsChunk(range)) {
            result.push_back(ScopedCollectionMetadata(self, *it));
        }
    }
    return result;
}

size_t MetadataManager::numberOfRangesToCleanStillInUse() {
    stdx::lock_guard<stdx::mutex> scopedLock(_managerLock);
    size_t count = 0;
    for (auto& metadata : _metadata) {
        count += metadata->_tracker.orphans.size();
    }
    return count;
}

size_t MetadataManager::numberOfRangesToClean() {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    return _rangesToClean.size();
}

auto MetadataManager::trackOrphanedDataCleanup(ChunkRange const& range)
    -> boost::optional<CleanupNotification> {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    auto overlaps = _overlapsInUseCleanups(range);
    if (overlaps) {
        return overlaps;
    }
    return _rangesToClean.overlaps(range);
}

bool MetadataManager::_overlapsInUseChunk(ChunkRange const& range) {
    invariant(!_metadata.empty());
    for (auto it = _metadata.begin(), end = --_metadata.end(); it != end; ++it) {
        if (((*it)->_tracker.usageCounter != 0) && (*it)->rangeOverlapsChunk(range)) {
            return true;
        }
    }
    if (_metadata.back()->rangeOverlapsChunk(range)) {  // for active metadata, ignore refcount.
        return true;
    }
    return false;
}

auto MetadataManager::_overlapsInUseCleanups(ChunkRange const& range)
    -> boost::optional<CleanupNotification> {
    invariant(!_metadata.empty());

    for (auto it = _metadata.crbegin(), et = _metadata.crend(); it != et; ++it) {
        auto cleanup = (*it)->_tracker.orphans.crbegin();
        auto ec = (*it)->_tracker.orphans.crend();
        for (; cleanup != ec; ++cleanup) {
            if (bool(cleanup->range.overlapWith(range))) {
                return cleanup->notification;
            }
        }
    }
    return boost::none;
}

boost::optional<KeyRange> MetadataManager::getNextOrphanRange(BSONObj const& from) {
    stdx::unique_lock<stdx::mutex> scopedLock(_managerLock);
    invariant(!_metadata.empty());
    return _metadata.back()->getNextOrphanRange(_receivingChunks, from);
}

}  // namespace mongo
