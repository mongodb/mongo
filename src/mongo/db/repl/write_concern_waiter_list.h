// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/db/repl/clang_checked/checked_mutex.h"
#include "mongo/db/repl/clang_checked/mutex.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/write_concern_idl.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/trusted_hasher.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/future.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/overloaded_visitor.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

// HashWriteConcernForReplication and EqualWriteConcernForReplication are used to make a hash
// map of write concerns.  They should include all the fields, and only the fields which are
// relevant to _doneWaitingForReplication -- syncMode, w, and checkCondition.
// They are declared here, rather than inside ReplCoordinatorImpl, because it is not possible
// to use the IsTrustedHasher template for an inner class.
class HashWriteConcernForReplication {
public:
    std::size_t operator()(const WriteConcernOptions& a) const {
        std::size_t seed = 0;
        boost::hash_combine(seed, stdx::to_underlying(a.syncMode));
        boost::hash_combine(seed, a.checkCondition);
        std::visit(OverloadedVisitor{[&](const std::string& s) { boost::hash_combine(seed, s); },
                                     [&](std::int64_t n) { boost::hash_combine(seed, n); },
                                     [&](const WTags& tags) {
                                         for (const auto& tag : tags) {
                                             boost::hash_combine(seed, tag.first);
                                             boost::hash_combine(seed, tag.second);
                                         }
                                     }},
                   a.w);
        return seed;
    }
};

class EqualWriteConcernForReplication {
public:
    bool operator()(const WriteConcernOptions& a, const WriteConcernOptions& b) const {
        return a.syncMode == b.syncMode && a.checkCondition == b.checkCondition && a.w == b.w;
    }
};

}  // namespace repl

template <>
struct IsTrustedHasher<repl::HashWriteConcernForReplication, WriteConcernOptions> : std::true_type {
};

namespace repl {

// A client waiting on an OpTime, optionally together with a WriteConcern. A waiter's promise is
// fulfilled at most once -- the waiter is removed from the list when fulfilled, and all mutation of
// the list is serialized by the list's own mutex -- so it needs no fulfillment
// synchronization of its own.
struct Waiter {
    Promise<void> promise;
    boost::optional<WriteConcernOptions> writeConcern;
    // Marks this waiter abandoned, which allows early clean-up for the waiter.
    Atomic<bool> givenUp{false};
    explicit Waiter(Promise<void> p, boost::optional<WriteConcernOptions> w = boost::none)
        : promise(std::move(p)), writeConcern(w) {}
};

using SharedWaiterHandle = std::shared_ptr<Waiter>;

template <typename T>
using WriteConcernMap = stdx::unordered_map<WriteConcernOptions,
                                            T,
                                            HashWriteConcernForReplication,
                                            EqualWriteConcernForReplication>;

// What to do with the waiters of a single WriteConcern:
//  - OpTime: wake every waiter whose opTime is <= this value, i.e. the highest OpTime that
//            currently satisfies the WriteConcern. A null OpTime wakes none of its waiters, which
//            is how an unsatisfied condition is reported;
//  - bool:   only ever true, and only for OpTime-independent (config-commitment) conditions: wake
//            all of that WriteConcern's waiters regardless of their opTime;
//  - Status: fail all of that WriteConcern's waiters with this error.
using WriteConcernFulfillment = std::variant<OpTime, bool, Status>;

// One fulfillment per WriteConcern being waited on. A WriteConcern with no entry has none of its
// waiters woken, so unsatisfied write concerns can equivalently be omitted.
using WriteConcernFulfillmentMap = WriteConcernMap<WriteConcernFulfillment>;

// Waiters grouped by WriteConcern and, within a group, ordered by OpTime so a group can be
// satisfied as a monotonic prefix.
using WaiterListMap = WriteConcernMap<std::multimap<OpTime, SharedWaiterHandle, std::less<>>>;

// Tracks clients waiting on an OpTime together with a WriteConcern.
//
// The list owns the mutex guarding its waiters, independent of the ReplicationCoordinator mutex, so
// its public methods are self-synchronizing: callers do not need to hold (or reason about) the
// ReplicationCoordinator mutex to add, remove or wake a waiter.
//
// Deciding which waiters may be woken is separated from waking them: the caller computes a
// WriteConcernFulfillmentMap -- one decision per waited-on WriteConcern, obtained from
// getWriteConcerns() -- and hands it to setValueIf(), which applies it without having to test each
// waiter in turn.
class WriteConcernWaiterList {
public:
    WriteConcernWaiterList() = delete;
    explicit WriteConcernWaiterList(Counter64& waiterCount);

    WriteConcernWaiterList(const WriteConcernWaiterList&) = delete;
    WriteConcernWaiterList& operator=(const WriteConcernWaiterList&) = delete;

    // Adds a new waiter for `opTime`/`wc` and returns its future and handle.
    std::pair<SharedSemiFuture<void>, SharedWaiterHandle> add(const OpTime& opTime,
                                                              WriteConcernOptions wc);
    // Adds an already-constructed waiter (whose future the caller already holds).
    void add(const OpTime& opTime, SharedWaiterHandle waiter);

    // Removes `waiter`; returns whether it was found and removed.
    bool remove(const OpTime& opTime, SharedWaiterHandle waiter);

    // Fails (and removes) every waiter with `status`.
    void setErrorAll(Status status);

    // Applies a precomputed per-WriteConcern fulfillment decision (see
    // _makeWriteConcernFulfillmentMap): collects the waiters to wake under `_mutex`, then fulfills
    // their promises off the lock so their continuations do not run under it.
    void setValueIf(const WriteConcernFulfillmentMap& fulfillmentMap);

    // Returns the distinct WriteConcerns being waited on, so the caller can precompute a
    // fulfillment decision per WriteConcern. The same vector is handed back on every call until a
    // WriteConcern the list has not seen before is added, so a caller normally pays only a
    // shared_ptr copy.
    std::shared_ptr<const std::vector<WriteConcernOptions>> getWriteConcerns() const;

    // Returns how many waiters `writeConcern` currently has. A WriteConcern keeps its bucket once
    // seen, even after the last of its waiters goes away, so getWriteConcerns() can name one that
    // nobody is waiting on; this lets a caller skip deciding a fulfillment that would wake nobody.
    size_t numWaiters(const WriteConcernOptions& writeConcern) const;

private:
    // Observable so its contention shows up in the lockContentionMetrics serverStatus section, and
    // checked so the members below have a capability to be guarded by. CheckedMutex has to be the
    // outer wrapper: MONGO_LOCKING_GUARDED_BY only analyses a type annotated as a capability.
    using Mutex = clang_checked::CheckedMutex<ObservableMutex<std::mutex>>;

    // Guards the members below. Independent of the ReplicationCoordinator mutex, and never held
    // while acquiring it.
    mutable Mutex _mutex;

    // Waiters grouped by WriteConcern and, within a group, ordered by OpTime.
    WaiterListMap _waiterList MONGO_LOCKING_GUARDED_BY(_mutex);

    // The keys of `_waiterList`, so getWriteConcerns() does not have to rebuild the vector on every
    // call. Replaced rather than mutated whenever a new WriteConcern appears, so a vector already
    // handed out stays valid and immutable for as long as its holder keeps it.
    std::shared_ptr<const std::vector<WriteConcernOptions>> _writeConcerns MONGO_LOCKING_GUARDED_BY(
        _mutex) = std::make_shared<const std::vector<WriteConcernOptions>>();

    // Number of waiters currently in `_waiterList`, kept in step with it. Owned by the
    // ReplicationCoordinator, which reads it as a serverStatus metric; the annotation constrains
    // only the accesses this class makes.
    Counter64& _waiterCount MONGO_LOCKING_GUARDED_BY(_mutex);
};

}  // namespace repl
}  // namespace mongo
