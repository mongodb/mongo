/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/locker.h"

#include "mongo/bson/json.h"
#include "mongo/db/admission/ticketing_system.h"
#include "mongo/db/local_catalog/lock_manager/dump_lock_manager.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/local_catalog/lock_manager/resource_catalog.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/background.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

// Ignore data races in certain functions when running with TSAN. For performance reasons,
// diagnostic commands are expected to race with concurrent lock acquisitions while gathering
// statistics.
#if __has_feature(thread_sanitizer)
#define MONGO_TSAN_IGNORE __attribute__((no_sanitize("thread")))
#else
#define MONGO_TSAN_IGNORE
#endif

MONGO_FAIL_POINT_DEFINE(enableTestOnlyFlagforRSTL);
MONGO_FAIL_POINT_DEFINE(failNonIntentLocksIfWaitNeeded);

/**
 * Tracks global (across all clients) lock acquisition statistics, partitioned into multiple
 * buckets to minimize concurrent access conflicts.
 *
 * Each client has a LockerId that monotonically increases across all client instances. The
 * LockerId % 8 is used to index into one of 8 LockStats instances. These LockStats objects must be
 * atomically accessed, so maintaining 8 that are indexed by LockerId reduces client conflicts and
 * improves concurrent write access. A reader, to collect global lock statics for reporting, will
 * sum the results of all 8 disjoint 'buckets' of stats.
 */
class PartitionedInstanceWideLockStats {
    PartitionedInstanceWideLockStats(const PartitionedInstanceWideLockStats&) = delete;
    PartitionedInstanceWideLockStats& operator=(const PartitionedInstanceWideLockStats&) = delete;

public:
    PartitionedInstanceWideLockStats() {}

    void recordAcquisition(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordAcquisition(resId, mode);
    }

    void recordWait(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordWait(resId, mode);
    }

    void recordWaitTime(LockerId id, ResourceId resId, LockMode mode, uint64_t waitMicros) {
        _get(id).recordWaitTime(resId, mode, waitMicros);
    }

    void report(SingleThreadedLockStats* outStats) const {
        for (int i = 0; i < NumPartitions; i++) {
            outStats->append(_partitions[i].stats);
        }
    }

    void reset() {
        for (int i = 0; i < NumPartitions; i++) {
            _partitions[i].stats.reset();
        }
    }

private:
    // This alignment is a best effort approach to ensure that each partition falls on a
    // separate page/cache line in order to avoid false sharing.
    struct alignas(stdx::hardware_destructive_interference_size) AlignedLockStats {
        AtomicLockStats stats;
    };

    enum { NumPartitions = 8 };

    AtomicLockStats& _get(LockerId id) {
        return _partitions[id % NumPartitions].stats;
    }


    AlignedLockStats _partitions[NumPartitions];
};

/**
 * Periodically purges unused lock buckets. The first time the lock is used again after cleanup it
 * needs to be allocated, and similarly, every first use by a client for an intent  mode may need to
 * create a partitioned lock head. Cleanup is done roughly once a minute.
 */
class UnusedLockCleaner : PeriodicTask {
public:
    std::string taskName() const override {
        return "UnusedLockCleaner";
    }

    void taskDoWork() override {
        LOGV2_DEBUG(20524, 2, "Cleaning up unused lock buckets of the global lock manager");
        LockManager::get(getGlobalServiceContext())->cleanupUnusedLocks();
    }
} unusedLockCleaner;

// Dispenses unique LockerId identifiers
AtomicWord<LockerId> idCounter(0);

// Tracks lock statistics across all Locker instances. Distributes stats across multiple buckets
// indexed by LockerId in order to minimize concurrent access conflicts.
PartitionedInstanceWideLockStats globalStats;

// How often (in millis) to check for deadlock if a lock has not been granted for some time
constexpr Milliseconds kMaxWaitTime = Milliseconds(500);

}  // namespace

#ifdef MONGO_CONFIG_DEBUG_BUILD
namespace {
const auto hasDisabledLockerRuntimeOrderingChecks = OperationContext::declareDecoration<bool>();
}

DisableLockerRuntimeOrderingChecks::DisableLockerRuntimeOrderingChecks(OperationContext* opCtx)
    : _opCtx(opCtx) {
    _oldValue = hasDisabledLockerRuntimeOrderingChecks(opCtx);
    hasDisabledLockerRuntimeOrderingChecks(opCtx) = true;
}

DisableLockerRuntimeOrderingChecks::~DisableLockerRuntimeOrderingChecks() {
    hasDisabledLockerRuntimeOrderingChecks(_opCtx) = _oldValue;
}

namespace locker_internals {
/**
 * A class used to identify potential deadlock cycles in the server.
 *
 * To solve this problem we can't naively assume that all operations lock resources in increasing
 * ResourceId order. This is because ResourceMutex have an arbitrary ResourceId that may depend on
 * runtime initialisation order and it potentially would invalidate lock orderings that are
 * completely fine. Instead we have to abstract the order into the actual order used. That is, we
 * want to ensure there are no lock cycles with the lock orderings we really use.
 *
 * The way this class works is by building a lock ordering graph of dependencies across all
 * operations. Consider the following graph of lock dependencies constructed from two operations
 * that have locked A -> B -> C and A -> B -> D:
 *
 * ┌───┐   ┌───┐   ┌───┐
 * │   │   │   │   │   │
 * │ A ┼───► B ┼───► C │
 * │   │   │   │   │   │
 * └───┘   └─┬─┘   └───┘
 *           │
 *           │     ┌───┐
 *           │     │   │
 *           └─────► D │
 *                 │   │
 *                 └───┘
 * Now, if we have an operation that has locked D and attempts to lock A this should be caught. If
 * we were to store the graph above as is this would require a graph path algorithm to detect the
 * dependency.
 *
 * Instead what we do is that for all nodes we record which are their ancestors. In the graph above
 * this would map to:
 * - A -> []
 * - B -> [A]
 * - C -> [A, B]
 * - D -> [A, B]
 *
 * So answering if there's a lock cycle by an operation locking a resource is just checking if any
 * of the locks we have has as an ancestor the one we're trying to lock. If not, then we record the
 * new dependency.
 */
class LockOrderingsSet {
public:
    void add(ResourceId from, ResourceId to) {
        stdx::lock_guard lk{_mutex};
        _precomputedPaths[to].emplace(from);
    }
    bool hasPath(ResourceId from, ResourceId to) {
        stdx::lock_guard lk{_mutex};
        return _precomputedPaths[to].contains(from);
    }

private:
    stdx::mutex _mutex;
    // As the only question we have to answer is whether there exists a path that goes from a source
    // node to a target node we just store all nodes that can lead to the target node.
    stdx::unordered_map<ResourceId, stdx::unordered_set<ResourceId>> _precomputedPaths;
};
}  // namespace locker_internals

namespace {
const auto globalLockOrderingsSet =
    ServiceContext::declareDecoration<locker_internals::LockOrderingsSet>();
}
#endif

Locker::Locker(ServiceContext* serviceContext)
    : _id(idCounter.addAndFetch(1)),
      _lockManager(LockManager::get(serviceContext)),
      _ticketingSystem(admission::TicketingSystem::get(serviceContext)) {
#ifdef MONGO_CONFIG_DEBUG_BUILD
    _lockOrderingsSet = &globalLockOrderingsSet(serviceContext);
#endif
    updateThreadIdToCurrentThread();
}

Locker::~Locker() {
    // Cannot delete the Locker while there are still outstanding requests, because the
    // LockManager may attempt to access deleted memory. Besides it is probably incorrect
    // to delete with unaccounted locks anyways.
    invariant(!inAWriteUnitOfWork());
    invariant(_numResourcesToUnlockAtEndUnitOfWork == 0);
    invariant(!_ticket || !_ticket->valid());

    if (!_requests.empty()) {
        _dumpLockerAndLockManagerRequests();
    }
    invariant(_requests.empty());

    invariant(_modeForTicket == MODE_NONE);
}

void Locker::updateThreadIdToCurrentThread() {
    _threadId = stdx::this_thread::get_id();
}

void Locker::unsetThreadId() {
    _threadId = stdx::thread::id();  // Reset to represent a non-executing thread.
}

std::string Locker::getDebugInfo() const {
    return _debugInfo;
}

void Locker::setDebugInfo(std::string info) {
    _debugInfo = std::move(info);
}

Locker::ClientState Locker::getClientState() const {
    auto state = _clientState.load();

    if (state == kActiveReader && hasLockPending())
        state = kQueuedReader;
    if (state == kActiveWriter && hasLockPending())
        state = kQueuedWriter;

    return state;
}

void Locker::getFlowControlTicket(OperationContext* opCtx, LockMode lockMode) {
    auto ticketholder = FlowControlTicketholder::get(opCtx);
    if (ticketholder && lockMode == LockMode::MODE_IX && _clientState.load() == kInactive &&
        ExecutionAdmissionContext::get(opCtx).getPriority() !=
            AdmissionContext::Priority::kExempt &&
        !opCtx->uninterruptibleLocksRequested_DO_NOT_USE())  // NOLINT
    {
        // FlowControl only acts when a MODE_IX global lock is being taken. The clientState is only
        // being modified here to change serverStatus' `globalLock.currentQueue` metrics. This
        // method must not exit with a side-effect on the clientState. That value is also used for
        // tracking whether other resources need to be released.
        _clientState.store(kQueuedWriter);
        ScopeGuard restoreState([&] { _clientState.store(kInactive); });

        if (MONGO_unlikely(_assertOnLockAttempt)) {
            LOGV2_FATAL(9360804,
                        "Operation attempted to acquire an execution ticket after indicating that "
                        "it should not");
        }
        ticketholder->getTicket(opCtx, &_flowControlStats);
    }
}

MONGO_TSAN_IGNORE
FlowControlTicketholder::CurOp Locker::getFlowControlStats() const {
    return _flowControlStats;
}

void Locker::reacquireTicket(OperationContext* opCtx) {
    invariant(_modeForTicket != MODE_NONE);
    auto clientState = _clientState.load();
    const bool reader = isSharedLockMode(_modeForTicket);

    // Ensure that either we don't have a ticket, or the current ticket mode matches the lock mode.
    invariant(clientState == kInactive || (clientState == kActiveReader && reader) ||
              (clientState == kActiveWriter && !reader));

    // If we already have a ticket, there's nothing to do.
    if (clientState != kInactive)
        return;

    if (_acquireTicket(opCtx, _modeForTicket, Date_t::now())) {
        return;
    }

    do {
        for (auto it = _requests.begin(); it; it.next()) {
            invariant(it->mode == LockMode::MODE_IS || it->mode == LockMode::MODE_IX);
            // TODO SERVER-80206: Remove opCtx->checkForInterrupt().
            if (!opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
                opCtx->checkForInterrupt();
            }

            // If we've reached this point then that means we tried to acquire a ticket but were
            // unsuccessful, implying that tickets are currently exhausted. Additionally, since
            // we're holding an IS or IX lock for this resource, any pending requests for the same
            // resource must be S or X and will not be able to be granted. Thus, since such a
            // pending lock request may also be holding a ticket, if there are any present we fail
            // this ticket reacquisition in order to avoid a deadlock.
            uassert(ErrorCodes::LockTimeout,
                    fmt::format("Unable to acquire ticket with mode '{}' due to detected lock "
                                "conflict for resource {}",
                                fmt::underlying(_modeForTicket),
                                it.key().toString()),
                    !_lockManager->hasConflictingRequests(it.key(), it.objAddr()));
        }
    } while (!_acquireTicket(opCtx, _modeForTicket, Date_t::now() + Milliseconds{100}));
}

void Locker::releaseTicket() {
    invariant(_modeForTicket != MODE_NONE);
    _releaseTicket();
}


void Locker::lockGlobal(OperationContext* opCtx, LockMode mode, Date_t deadline) {
    dassert(isLocked() == (_modeForTicket != MODE_NONE));
    if (_modeForTicket == MODE_NONE) {
        if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
            // Ignore deadline.
            invariant(_acquireTicket(opCtx, mode, Date_t::max()));
        } else {
            auto beforeAcquire = Date_t::now();
            uassert(ErrorCodes::LockTimeout,
                    str::stream() << "Unable to acquire ticket with mode '" << mode
                                  << "' within a max lock request timeout of '"
                                  << Date_t::now() - beforeAcquire << "' milliseconds.",
                    _acquireTicket(opCtx, mode, deadline));
        }
        _modeForTicket = mode;
    } else if (TestingProctor::instance().isEnabled() && !isModeCovered(mode, _modeForTicket)) {
        LOGV2_FATAL(
            6614500,
            "Ticket held does not cover requested mode for global lock. Global lock upgrades are "
            "not allowed",
            "held"_attr = modeName(_modeForTicket),
            "requested"_attr = modeName(mode));
    }

    const LockResult result = _lockBegin(opCtx, resourceIdGlobal, mode);
    // Fast, uncontended path
    if (result == LOCK_OK)
        return;

    invariant(result == LOCK_WAITING);
    _lockComplete(opCtx, resourceIdGlobal, mode, deadline, nullptr);
}

bool Locker::unlockGlobal() {
    if (!unlock(resourceIdGlobal)) {
        return false;
    }

    invariant(!inAWriteUnitOfWork());

    LockRequestsMap::Iterator it = _requests.begin();
    while (!it.finished()) {
        // If we're here we should only have one reference to any lock. It is a programming
        // error for any lock used with multi-granularity locking to have more references than
        // the global lock, because every scope starts by calling lockGlobal.
        const auto resType = it.key().getType();
        if (resType == RESOURCE_GLOBAL || resType == RESOURCE_MUTEX ||
            resType == RESOURCE_DDL_DATABASE || resType == RESOURCE_DDL_COLLECTION) {
            it.next();
        } else {
            invariant(_unlockImpl(&it));
        }
    }

    return true;
}

LockResult Locker::lockRSTLBegin(OperationContext* opCtx, LockMode mode) {
    bool testOnly = false;

    if (MONGO_unlikely(enableTestOnlyFlagforRSTL.shouldFail())) {
        testOnly = true;
    }

    invariant(testOnly || mode == MODE_IX || mode == MODE_X);
    return _lockBegin(opCtx, resourceIdReplicationStateTransitionLock, mode);
}

void Locker::lockRSTLComplete(OperationContext* opCtx,
                              LockMode mode,
                              Date_t deadline,
                              const LockTimeoutCallback& onTimeout) {
    _lockComplete(opCtx, resourceIdReplicationStateTransitionLock, mode, deadline, onTimeout);
}

bool Locker::unlockRSTLforPrepare() {
    auto rstlRequest = _requests.find(resourceIdReplicationStateTransitionLock);

    // Don't attempt to unlock twice. This can happen when an interrupted global lock is destructed.
    if (!rstlRequest)
        return false;

    // If the RSTL is 'unlockPending' and we are fully unlocking it, then we do not want to
    // attempt to unlock the RSTL when the WUOW ends, since it will already be unlocked.
    if (rstlRequest->unlockPending) {
        rstlRequest->unlockPending = 0;
        _numResourcesToUnlockAtEndUnitOfWork--;
    }

    // Reset the recursiveCount to 1 so that we fully unlock the RSTL. Since it will be fully
    // unlocked, any future unlocks will be noops anyways.
    rstlRequest->recursiveCount = 1;

    return _unlockImpl(&rstlRequest);
}

void Locker::lock(OperationContext* opCtx, ResourceId resId, LockMode mode, Date_t deadline) {
    // `lockGlobal` must be called to lock `resourceIdGlobal`.
    invariant(resId != resourceIdGlobal);

    const LockResult result = _lockBegin(opCtx, resId, mode);

    // Fast, uncontended path
    if (result == LOCK_OK)
        return;

    invariant(result == LOCK_WAITING);
    _lockComplete(opCtx, resId, mode, deadline, nullptr);
}

bool Locker::unlock(ResourceId resId) {
    LockRequestsMap::Iterator it = _requests.find(resId);

    // Don't attempt to unlock twice. This can happen when an interrupted global lock is destructed.
    if (it.finished())
        return false;

    if (inAWriteUnitOfWork() && _shouldDelayUnlock(it.key(), (it->mode))) {
        // Only delay unlocking if the lock is not acquired more than once. Otherwise, we can simply
        // call _unlockImpl to decrement recursiveCount instead of incrementing unlockPending. This
        // is safe because the lock is still being held in the strongest mode necessary.
        if (it->recursiveCount > 1) {
            // Invariant that the lock is still being held.
            invariant(!_unlockImpl(&it));
            return false;
        }
        if (!it->unlockPending) {
            _numResourcesToUnlockAtEndUnitOfWork++;
        }
        it->unlockPending++;
        // unlockPending will be incremented if a lock is acquired in the same mode recursively, and
        // unlock() is called multiple times on one ResourceId.
        invariant(it->unlockPending <= it->recursiveCount);
        return false;
    }

    return _unlockImpl(&it);
}

void Locker::beginWriteUnitOfWork() {
    _wuowNestingLevel++;
}

void Locker::endWriteUnitOfWork() {
    invariant(_wuowNestingLevel > 0);

    if (--_wuowNestingLevel > 0) {
        // Don't do anything unless leaving outermost WUOW.
        return;
    }

    LockRequestsMap::Iterator it = _requests.begin();
    while (_numResourcesToUnlockAtEndUnitOfWork > 0) {
        if (it->unlockPending) {
            invariant(!it.finished());
            _numResourcesToUnlockAtEndUnitOfWork--;
        }
        while (it->unlockPending > 0) {
            // If a lock is acquired recursively, unlock() may be called multiple times on a
            // resource within the same WriteUnitOfWork. All such unlock() requests must thus be
            // fulfilled here.
            it->unlockPending--;
            unlock(it.key());
        }
        it.next();
    }
}

ResourceId Locker::getWaitingResource() const {
    scoped_spinlock lg(_lock);
    return _waitingResource;
}

MONGO_TSAN_IGNORE
void Locker::getLockerInfo(
    LockerInfo* lockerInfo,
    const boost::optional<SingleThreadedLockStats>& alreadyCountedStats) const {
    invariant(lockerInfo);

    // Zero-out the contents
    lockerInfo->locks.clear();
    lockerInfo->waitingResource = ResourceId();

    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        OneLock info;
        info.resourceId = it.key();
        info.mode = it->mode;

        lockerInfo->locks.push_back(info);
        it.next();
    }
    _lock.unlock();

    std::sort(lockerInfo->locks.begin(), lockerInfo->locks.end());

    lockerInfo->waitingResource = getWaitingResource();
    lockerInfo->stats.set(_stats);

    // alreadyCountedStats is a snapshot of lock stats taken when the sub-operation starts. Only
    // sub-operations have alreadyCountedStats.
    if (alreadyCountedStats) {
        // Adjust the lock stats by subtracting the alreadyCountedStats. No mutex is needed because
        // alreadyCountedStats is immutable.
        lockerInfo->stats.subtract(*alreadyCountedStats);
    }
}

Locker::LockerInfo Locker::getLockerInfo(
    const boost::optional<SingleThreadedLockStats>& alreadyCountedStats) const {
    Locker::LockerInfo lockerInfo;
    getLockerInfo(&lockerInfo, alreadyCountedStats);
    return lockerInfo;
}

bool Locker::canSaveLockState() {
    // We cannot yield strong global locks.
    if (_modeForTicket == MODE_S || _modeForTicket == MODE_X) {
        return false;
    }

    // If we don't have a global lock, we do not yield.
    if (_modeForTicket == MODE_NONE) {
        auto globalRequest = _requests.find(resourceIdGlobal);
        invariant(!globalRequest);

        // If there's no global lock there isn't really anything to do. Check that.
        for (auto it = _requests.begin(); !it.finished(); it.next()) {
            const ResourceType resType = it.key().getType();
            invariant(resType == RESOURCE_MUTEX || resType == RESOURCE_DDL_DATABASE ||
                      resType == RESOURCE_DDL_COLLECTION);
        }
        return false;
    }

    for (auto it = _requests.begin(); !it.finished(); it.next()) {
        const ResourceId resId = it.key();
        const ResourceType resType = resId.getType();
        if (resType == RESOURCE_MUTEX || resType == RESOURCE_DDL_DATABASE ||
            resType == RESOURCE_DDL_COLLECTION)
            continue;

        // If any lock has been acquired more than once, we're probably somewhere in a
        // DBDirectClient call.  It's not safe to release and reacquire locks -- the context using
        // the DBDirectClient is probably not prepared for lock release. This logic applies to all
        // locks in the hierarchy.
        if (it->recursiveCount > 1) {
            return false;
        }

        // We cannot yield any other lock in a strong lock mode.
        if (it->mode == MODE_S || it->mode == MODE_X) {
            return false;
        }
    }

    return true;
}

void Locker::saveLockStateAndUnlock(Locker::LockSnapshot* stateOut) {
    // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());

    // Callers must guarantee that they can actually yield.
    if (MONGO_unlikely(!canSaveLockState())) {
        dump();
        LOGV2_FATAL(7033800,
                    "Attempted to yield locks but we are either not holding locks, holding a "
                    "strong MODE_S/MODE_X lock, or holding one recursively");
    }

    // Clear out whatever is in stateOut.
    stateOut->locks.clear();
    stateOut->globalMode = MODE_NONE;

    // First, we look at the global lock.  There is special handling for this so we store it
    // separately from the more pedestrian locks.
    auto globalRequest = _requests.find(resourceIdGlobal);
    invariant(globalRequest);

    stateOut->globalMode = globalRequest->mode;
    invariant(unlock(resourceIdGlobal));

    // Next, the non-global locks.
    for (LockRequestsMap::Iterator it = _requests.begin(); !it.finished(); it.next()) {
        const ResourceId resId = it.key();
        const ResourceType resType = resId.getType();
        if (resType == RESOURCE_MUTEX || resType == RESOURCE_DDL_DATABASE ||
            resType == RESOURCE_DDL_COLLECTION)
            continue;

        // We should never have to save and restore metadata locks.
        invariant(RESOURCE_DATABASE == resType || RESOURCE_COLLECTION == resType ||
                  RESOURCE_TENANT == resType ||
                  resId == resourceIdMultiDocumentTransactionsBarrier ||
                  resId == resourceIdReplicationStateTransitionLock);

        // And, stuff the info into the out parameter.
        OneLock info;
        info.resourceId = resId;
        info.mode = it->mode;
        stateOut->locks.push_back(info);
        invariant(unlock(resId));
    }
    invariant(!isLocked());

    // Sort locks by ResourceId. They'll later be acquired in this canonical locking order.
    std::sort(stateOut->locks.begin(), stateOut->locks.end());
}

void Locker::restoreLockState(OperationContext* opCtx, const Locker::LockSnapshot& state) {
    // We shouldn't be restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());
    invariant(_modeForTicket == MODE_NONE);
    invariant(_clientState.load() == kInactive);

    getFlowControlTicket(opCtx, state.globalMode);

    std::vector<OneLock>::const_iterator it = state.locks.begin();

    // If we locked the MultiDocumentTransactionsBarrier lock, it must be locked before the
    // resourceIdReplicationStateTransitionLock and resourceIdGlobal resources.
    if (it != state.locks.end() && it->resourceId == resourceIdMultiDocumentTransactionsBarrier) {
        lock(opCtx, it->resourceId, it->mode);
        it++;
    }

    // If we locked the RSTL, it must be locked before the resourceIdGlobal resource.
    if (it != state.locks.end() && it->resourceId == resourceIdReplicationStateTransitionLock) {
        lock(opCtx, it->resourceId, it->mode);
        it++;
    }

    lockGlobal(opCtx, state.globalMode);
    for (; it != state.locks.end(); it++) {
        // Ensures we don't acquire locks out of order which can lead to deadlock.
        invariant(it->resourceId.getType() != ResourceType::RESOURCE_GLOBAL);
        lock(opCtx, it->resourceId, it->mode);
    }
    invariant(_modeForTicket != MODE_NONE);
}

void Locker::releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut) {
    // Only the global WUOW can be released, since we never need to release and restore
    // nested WUOW's. Thus we don't have to remember the nesting level.
    invariant(_wuowNestingLevel == 1);
    --_wuowNestingLevel;
    invariant(!isGlobalLockedRecursively());

    // All locks should be pending to unlock.
    invariant(_requests.size() == _numResourcesToUnlockAtEndUnitOfWork);
    for (auto it = _requests.begin(); it; it.next()) {
        invariant(it->unlockPending == 1);
        it->unlockPending--;
    }
    _numResourcesToUnlockAtEndUnitOfWork = 0;

    saveLockStateAndUnlock(stateOut);
}

void Locker::restoreWriteUnitOfWorkAndLock(OperationContext* opCtx,
                                           const LockSnapshot& stateToRestore) {
    if (stateToRestore.globalMode != MODE_NONE) {
        restoreLockState(opCtx, stateToRestore);
    }

    invariant(_numResourcesToUnlockAtEndUnitOfWork == 0);
    for (auto it = _requests.begin(); it; it.next()) {
        invariant(_shouldDelayUnlock(it.key(), (it->mode)));
        invariant(it->unlockPending == 0);
        it->unlockPending++;
    }
    _numResourcesToUnlockAtEndUnitOfWork = static_cast<unsigned>(_requests.size());

    beginWriteUnitOfWork();
}

void Locker::releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut) {
    stateOut->wuowNestingLevel = _wuowNestingLevel;
    _wuowNestingLevel = 0;

    for (auto it = _requests.begin(); _numResourcesToUnlockAtEndUnitOfWork > 0; it.next()) {
        if (it->unlockPending) {
            while (it->unlockPending) {
                it->unlockPending--;
                stateOut->unlockPendingLocks.push_back({it.key(), it->mode});
            }
            _numResourcesToUnlockAtEndUnitOfWork--;
        }
    }
}

void Locker::restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore) {
    invariant(_numResourcesToUnlockAtEndUnitOfWork == 0);
    invariant(!inAWriteUnitOfWork());

    for (auto& lock : stateToRestore.unlockPendingLocks) {
        auto it = _requests.begin();
        while (it && !(it.key() == lock.resourceId && it->mode == lock.mode)) {
            it.next();
        }
        invariant(!it.finished());
        if (!it->unlockPending) {
            _numResourcesToUnlockAtEndUnitOfWork++;
        }
        it->unlockPending++;
    }
    // Equivalent to call beginWriteUnitOfWork() multiple times.
    _wuowNestingLevel = stateToRestore.wuowNestingLevel;
}

LockMode Locker::getLockMode(ResourceId resId) const {
    scoped_spinlock scopedLock(_lock);
    if (auto it = _requests.find(resId))
        return it->mode;

    return MODE_NONE;
}

bool Locker::isDbLockedForMode(const DatabaseName& dbName, LockMode mode) const {
    if (auto lockedForMode =
            _globalAndTenantLocksImplyDBOrCollectionLockedForMode(dbName.tenantId(), mode);
        lockedForMode) {
        return *lockedForMode;
    }

    return isLockHeldForMode(ResourceId(RESOURCE_DATABASE, dbName), mode);
}

bool Locker::isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const {
    invariant(nss.coll().size());

    if (auto lockedForMode =
            _globalAndTenantLocksImplyDBOrCollectionLockedForMode(nss.tenantId(), mode);
        lockedForMode) {
        return *lockedForMode;
    }

    LockMode dbMode = getLockMode(ResourceId(RESOURCE_DATABASE, nss.dbName()));
    switch (dbMode) {
        case MODE_NONE:
            return false;
        case MODE_X:
            return true;
        case MODE_S:
            return isSharedLockMode(mode);
        case MODE_IX:
        case MODE_IS:
            return isLockHeldForMode(ResourceId(RESOURCE_COLLECTION, nss), mode);
        case LockModesCount:
            break;
    }

    MONGO_UNREACHABLE_TASSERT(10083510);
}

bool Locker::isGlobalLockedRecursively() const {
    auto globalLockRequest = _requests.find(resourceIdGlobal);
    return !globalLockRequest.finished() && globalLockRequest->recursiveCount > 1;
}

void Locker::setGlobalLockTakenInMode(LockMode mode) {
    _globalLockMode |= (1 << mode);

    if (mode == MODE_IX || mode == MODE_X || mode == MODE_S) {
        _wasGlobalLockTakenInModeConflictingWithWrites.store(true);
    }
}

std::vector<LogDebugInfo> Locker::getLockInfoFromResourceHolders(ResourceId resId) const {
    return _lockManager->getLockInfoFromResourceHolders(resId);
}

void Locker::dump() const {
    struct Entry {
        ResourceId key;
        LockRequest::Status status;
        LockMode mode;
        unsigned int recursiveCount;
        unsigned int unlockPending;

        BSONObj toBSON() const {
            BSONObjBuilder b;
            b.append("key", key.toString());
            b.append("status", lockRequestStatusName(status));
            b.append("recursiveCount", static_cast<int>(recursiveCount));
            b.append("unlockPending", static_cast<int>(unlockPending));
            b.append("mode", modeName(mode));
            return b.obj();
        }
        std::string toString() const {
            return tojson(toBSON());
        }
    };

    std::vector<Entry> entries;

    {
        scoped_spinlock lg(_lock);
        for (auto it = _requests.begin(); !it.finished(); it.next())
            entries.push_back(
                {it.key(), it->status, it->mode, it->recursiveCount, it->unlockPending});
    }
    LOGV2(20523, "Locker status", "id"_attr = _id, "requests"_attr = entries);
}

MONGO_TSAN_IGNORE
LockResult Locker::_lockBegin(OperationContext* opCtx, ResourceId resId, LockMode mode) {
    dassert(!getWaitingResource().isValid());

    const ResourceType resType = resId.getType();
    if (MONGO_unlikely(_assertOnLockAttempt && resType != RESOURCE_METADATA &&
                       resType != RESOURCE_MUTEX && resType != RESOURCE_DDL_DATABASE &&
                       resType != RESOURCE_DDL_COLLECTION)) {
        LOGV2_FATAL(9360800,
                    "Operation attempted to acquire lock after indicating that it should not",
                    "resourceId"_attr = resId.toString(),
                    "mode"_attr = modeName(mode));
    }

    LockRequest* request;

    LockRequestsMap::Iterator it = _requests.find(resId);
    if (!it) {
        scoped_spinlock lg(_lock);
        LockRequestsMap::Iterator itNew = _requests.insert(resId);
        itNew->initNew(this, &_notify);

        request = itNew.objAddr();

#ifdef MONGO_CONFIG_DEBUG_BUILD
        // We only do these checks for operations that don't release all locks in case of
        // lock acquisition failure. This is a perfect match with multi-document transactions since
        // all lock acquisitions have a timeout and we release all locks in case of failure. Using
        // the fact the operation is in a multi-document transaction is a layering violation but we
        // acknowledge it here as an acceptable case since there's no other case of acting like
        // them.
        //
        // One alternative that may initially make sense but doesn't is to look at whether the
        // operation has a deadline for the acquisition or not. This is wrong because we could end
        // up with the following scenario:
        // * T1: lock A MODE_IX
        // * T2: lock B MODE_IX
        // * T1: lock B MODE_X (blocks on T2)
        // * T2: trylock A MODE_X ad infinitum (blocks on T1, deadlock)
        if (hasDisabledLockerRuntimeOrderingChecks(opCtx) || opCtx->inMultiDocumentTransaction()) {
            // Nothing to check here since we don't want to participate in lock ordering or the
            // operation cannot incur a deadlock as it has a timeout.
        } else {
            for (auto it = _requests.begin(); it; it.next()) {
                const auto& lockRequest = *it;
                if (lockRequest.mode == MODE_NONE ||
                    lockRequest.status != LockRequest::STATUS_GRANTED) {
                    continue;
                }
                const auto& from = it.key();
                // If there is a lock ordering path it means there's a potential deadlock in the
                // codebase. This is because if we have locked A before locking B, there should have
                // been no other operation that locked B before A.
                const auto hasLockCycle = _lockOrderingsSet->hasPath(resId, from);
                if (hasLockCycle) {
                    LOGV2_FATAL(9915000,
                                "Detected potential lock cycle for operation, the from resource "
                                "eventually locks the target resource. There has been at least one "
                                "other operation that has locked the target resource before "
                                "locking from, which is the inverse order of locking",
                                "from"_attr = from,
                                "target"_attr = resId);
                }
                _lockOrderingsSet->add(from, resId);
            }
        }
#endif
    } else {
        request = it.objAddr();
        invariant(isModeCovered(mode, request->mode), "Lock upgrade is disallowed");

        // If unlockPending is nonzero, that means a LockRequest already exists for this resource
        // but is planned to be released at the end of this WUOW due to two-phase locking. Rather
        // than unlocking the existing request, we can reuse it.
        if (request->unlockPending) {
            if (!opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
                // Lock acquisitions are not allowed to succeed when opCtx is marked as interrupted,
                // unless the caller requested an uninterruptible lock.
                opCtx->checkForInterrupt();
            }
            request->unlockPending--;
            if (!request->unlockPending) {
                _numResourcesToUnlockAtEndUnitOfWork--;
            }
            return LOCK_OK;
        } else {
            ++request->recursiveCount;
        }
    }

    // Making this call here will record lock re-acquisitions as well.
    globalStats.recordAcquisition(_id, resId, mode);
    _stats.recordAcquisition(resId, mode);

    // Give priority to the full modes for Global and RSTL resources so we don't stall global
    // operations such as shutdown or stepdown.
    if (resType == RESOURCE_GLOBAL) {
        if (mode == MODE_S || mode == MODE_X) {
            request->enqueueAtFront = true;
            request->compatibleFirst = true;
        }
    } else if (resType != RESOURCE_MUTEX && resType != RESOURCE_DDL_DATABASE &&
               resType != RESOURCE_DDL_COLLECTION) {
        // This is all sanity checks that the global locks are always be acquired
        // before any other lock has been acquired and they must be in sync with the nesting.
        if (kDebugBuild) {
            const LockRequestsMap::Iterator itGlobal = _requests.find(resourceIdGlobal);
            invariant(itGlobal->recursiveCount > 0);
            invariant(itGlobal->mode != MODE_NONE);
        };
    }

    // The notification object must be cleared before we invoke the lock manager, because
    // otherwise we might reset state if the lock becomes granted very fast.
    _notify.clear();

    auto result = request->recursiveCount == 1 ? _lockManager->lock(resId, request, mode) : LOCK_OK;

    if (result == LOCK_WAITING) {
        globalStats.recordWait(_id, resId, mode);
        _stats.recordWait(resId, mode);
        _setWaitingResource(resId);
    } else if (result == LOCK_OK && !opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        // Lock acquisitions are not allowed to succeed when opCtx is marked as interrupted, unless
        // the caller requested an uninterruptible lock.
        auto interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            auto unlockIt = _requests.find(resId);
            invariant(unlockIt);
            _unlockImpl(&unlockIt);
            uassertStatusOK(interruptStatus);
        }
    }

    return result;
}

void Locker::_lockComplete(OperationContext* opCtx,
                           ResourceId resId,
                           LockMode mode,
                           Date_t deadline,
                           const LockTimeoutCallback& onTimeout) {
    // Operations which are holding open an oplog hole cannot block when acquiring locks. Lock
    // requests entering this function have been queued up and will be granted the lock as soon as
    // the lock is released, which is a blocking operation.
    const ResourceType resType = resId.getType();
    if (MONGO_unlikely(_assertOnLockAttempt && resType != RESOURCE_METADATA &&
                       resType != RESOURCE_MUTEX && resType != RESOURCE_DDL_DATABASE &&
                       resType != RESOURCE_DDL_COLLECTION)) {
        LOGV2_FATAL(9360801,
                    "Operation attempted to acquire lock after indicating that it should not",
                    "resourceId"_attr = resId.toString(),
                    "mode"_attr = modeName(mode));
    }

    // Clean up the state on any failed lock attempts.
    ScopeGuard unlockOnErrorGuard([&] {
        LockRequestsMap::Iterator it = _requests.find(resId);
        invariant(it);
        _unlockImpl(&it);
        _setWaitingResource(ResourceId());
    });

    // This failpoint is used to time out non-intent locks if they cannot be granted immediately
    // for user operations. Testing-only.
    const bool isUserOperation = opCtx->getClient()->isFromUserConnection();
    if (!opCtx->uninterruptibleLocksRequested_DO_NOT_USE() && isUserOperation &&  // NOLINT
        MONGO_unlikely(failNonIntentLocksIfWaitNeeded.shouldFail())) {
        uassert(ErrorCodes::LockTimeout,
                str::stream() << "Cannot immediately acquire lock '" << resId.toString()
                              << "'. Timing out due to failpoint.",
                (mode == MODE_IS || mode == MODE_IX));
    }

    LockResult result;
    Milliseconds timeout;
    if (deadline == Date_t::max()) {
        timeout = Milliseconds::max();
    } else if (deadline <= Date_t()) {
        timeout = Milliseconds(0);
    } else {
        timeout = deadline - Date_t::now();
    }
    timeout = std::min(timeout, _maxLockTimeout ? *_maxLockTimeout : Milliseconds::max());
    if (opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
        timeout = Milliseconds::max();
    }

    // Don't go sleeping without bound in order to be able to report long waits.
    Milliseconds waitTime = std::min(timeout, kMaxWaitTime);
    const uint64_t startOfTotalWaitTime = curTimeMicros64();
    uint64_t startOfCurrentWaitTime = startOfTotalWaitTime;

    while (true) {
        // It is OK if this call wakes up spuriously, because we re-evaluate the remaining
        // wait time anyways.
        // Unless a caller has requested an uninterruptible lock, we want to use the opCtx's
        // interruptible wait so that pending lock acquisitions can be cancelled.
        if (!opCtx->uninterruptibleLocksRequested_DO_NOT_USE()) {  // NOLINT
            result = _notify.wait(opCtx, waitTime);
        } else {
            result = _notify.wait(waitTime);
        }

        // Account for the time spent waiting on the notification object
        const uint64_t curTimeMicros = curTimeMicros64();
        const uint64_t elapsedTimeMicros = curTimeMicros - startOfCurrentWaitTime;
        startOfCurrentWaitTime = curTimeMicros;

        globalStats.recordWaitTime(_id, resId, mode, elapsedTimeMicros);
        _stats.recordWaitTime(resId, mode, elapsedTimeMicros);

        if (result == LOCK_OK)
            break;

        // If infinite timeout was requested, just keep waiting
        if (timeout == Milliseconds::max()) {
            continue;
        }

        const auto totalBlockTime = duration_cast<Milliseconds>(
            Microseconds(int64_t(curTimeMicros - startOfTotalWaitTime)));
        waitTime = (totalBlockTime < timeout) ? std::min(timeout - totalBlockTime, kMaxWaitTime)
                                              : Milliseconds(0);

        // Check if the lock acquisition has timed out. If we have an operation context and client
        // we can provide additional diagnostics data.
        if (waitTime == Milliseconds(0)) {
            if (onTimeout) {
                onTimeout();
            }
            std::string timeoutMessage = str::stream()
                << "Unable to acquire " << modeName(mode) << " lock on '" << resId.toString()
                << "' within " << timeout << ".";
            if (opCtx->getClient()) {
                timeoutMessage = str::stream()
                    << timeoutMessage << " opId: " << opCtx->getOpID()
                    << ", op: " << opCtx->getClient()->desc()
                    << ", connId: " << opCtx->getClient()->getConnectionId() << ".";
            }
            uasserted(ErrorCodes::LockTimeout, timeoutMessage);
        }
    }

    invariant(result == LOCK_OK);
    unlockOnErrorGuard.dismiss();

    _setWaitingResource(ResourceId());
}

bool Locker::_acquireTicket(OperationContext* opCtx, LockMode mode, Date_t deadline) {
    // MODE_X is exclusive of all other locks, thus acquiring a ticket is unnecessary.
    if (mode == MODE_X || mode == MODE_NONE || !_ticketingSystem) {
        _clientState.store(isSharedLockMode(mode) ? kActiveReader : kActiveWriter);
        return true;
    }

    _clientState.store(isSharedLockMode(mode) ? kQueuedReader : kQueuedWriter);

    // If the ticket wait is interrupted, restore the state of the client.
    ScopeGuard restoreStateOnErrorGuard([&] { _clientState.store(kInactive); });

    if (MONGO_unlikely(_assertOnLockAttempt)) {
        LOGV2_FATAL(9360803,
                    "Operation attempted to acquire an execution ticket after indicating that "
                    "it should not",
                    "mode"_attr = modeName(mode));
    }

    _ticket = _ticketingSystem->waitForTicketUntil(
        opCtx,
        isSharedLockMode(mode) ? admission::TicketingSystem::Operation::kRead
                               : admission::TicketingSystem::Operation::kWrite,
        deadline);

    if (!_ticket) {
        return false;
    }

    restoreStateOnErrorGuard.dismiss();

    _clientState.store(isSharedLockMode(mode) ? kActiveReader : kActiveWriter);
    return true;
}

bool Locker::_unlockImpl(LockRequestsMap::Iterator* it) {
    if (_lockManager->unlock(it->objAddr())) {
        if (it->key() == resourceIdGlobal) {
            invariant(_modeForTicket != MODE_NONE);

            // We may have already released our ticket through a call to releaseTicket().
            if (_clientState.load() != kInactive) {
                _releaseTicket();
            }

            _modeForTicket = MODE_NONE;
        }

        scoped_spinlock lg(_lock);
        it->remove();

        return true;
    }

    return false;
}

void Locker::_releaseTicket() {
    _ticket.reset();
    _clientState.store(kInactive);
}

void Locker::_setWaitingResource(ResourceId resId) {
    scoped_spinlock lg(_lock);
    _waitingResource = resId;
}

bool Locker::_shouldDelayUnlock(ResourceId resId, LockMode mode) const {
    switch (resId.getType()) {
        case RESOURCE_MUTEX:
            return false;

        case RESOURCE_GLOBAL:
        case RESOURCE_TENANT:
        case RESOURCE_DATABASE:
        case RESOURCE_COLLECTION:
        case RESOURCE_METADATA:
        case RESOURCE_DDL_DATABASE:
        case RESOURCE_DDL_COLLECTION:
            break;

        default:
            MONGO_UNREACHABLE_TASSERT(10083508);
    }

    switch (mode) {
        case MODE_X:
        case MODE_IX:
            return true;

        case MODE_IS:
        case MODE_S:
            return _sharedLocksShouldTwoPhaseLock;

        default:
            MONGO_UNREACHABLE_TASSERT(10083509);
    }
}

boost::optional<bool> Locker::_globalAndTenantLocksImplyDBOrCollectionLockedForMode(
    const boost::optional<TenantId>& tenantId, LockMode lockMode) const {
    if (isW()) {
        return true;
    }
    if (isR() && isSharedLockMode(lockMode)) {
        return true;
    }
    if (tenantId) {
        const ResourceId tenantResourceId{ResourceType::RESOURCE_TENANT, *tenantId};
        switch (getLockMode(tenantResourceId)) {
            case MODE_NONE:
                return false;
            case MODE_X:
                return true;
            case MODE_S:
                return isSharedLockMode(lockMode);
            case MODE_IX:
            case MODE_IS:
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(6671502);
        }
    }
    return boost::none;
}

void Locker::_dumpLockerAndLockManagerRequests() {
    // Log the _requests that this locker holds. This will provide identifying information to cross
    // reference with the LockManager dump below for extra information.
    dump();

    LOGV2_ERROR(5736000, "Operation ending while holding locks.");

    // Log the LockManager's lock information. Given the locker 'dump()' above, we should be able to
    // easily cross reference to find the lock info matching this operation. The LockManager can
    // safely access (under internal locks) the LockRequest data that the locker cannot.
    dumpLockManager();
}

void reportGlobalLockingStats(SingleThreadedLockStats* outStats) {
    globalStats.report(outStats);
}

void resetGlobalLockStats() {
    globalStats.reset();
}

ResourceMutex::ResourceMutex(std::string resourceLabel)
    : _rid(ResourceCatalog::get().newResourceIdForMutex(std::move(resourceLabel))) {}

bool ResourceMutex::isExclusivelyLocked(Locker* locker) {
    return locker->isLockHeldForMode(_rid, MODE_X);
}

bool ResourceMutex::isAtLeastReadLocked(Locker* locker) {
    return locker->isLockHeldForMode(_rid, MODE_IS);
}

}  // namespace mongo
