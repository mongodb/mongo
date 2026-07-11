// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <list>
#include <mutex>

namespace mongo {
struct CriticalSectionWaiterToken {
private:
    std::list<Timer>::iterator pos;
    template <typename Key>
    friend class CriticalSectionStatistics;
};

/**
 * Critical Section statistics that get reported into FTDC.
 * The statistics defined here are as follows:
 * - activeWaitersCount: The number of waiters for the critical section being released.
 * - activeCatchupCount: The number of critical sections acquired under the "catchup" reason.
 * - activeCommitCount: The number of critical sections acquired under the "commit" reason.
 * - totalTimeWaiting: The amount of milliseconds spent waiting for the critical section being
 *   released.
 * - totalTimeActiveCatchupMillis: The amount of time spent with a critical section in the "catchup"
 *   phase.
 * - totalTimeActiveCommitMillis: The amount of time spent with a critical section in the "commit"
 *   phase.
 *
 * All fields of the "totalTime*" family are non-cumulative timings that will reflect the current
 * state of the system. This means that a stuck operation will show up in FTDC as an increasing
 * amount of time spent waiting/with the critical section.
 */
template <typename Key>
class [[MONGO_MOD_PARENT_PRIVATE]] CriticalSectionStatistics {
public:
    void report(BSONObjBuilder& builder) const {
        Microseconds totalTimeActiveCatchup{0};
        Microseconds totalTimeActiveCommit{0};
        Microseconds totalTimeWaiting{0};
        int64_t activeCatchupCriticalSectionsCount;
        int64_t activeCommitCriticalSectionsCount;
        int64_t numWaiters;

        {
            std::lock_guard lk(_mutex);

            for (const auto& [_, timer] : _catchupModeCriticalSections) {
                totalTimeActiveCatchup += timer.elapsed();
            }
            activeCatchupCriticalSectionsCount = _catchupModeCriticalSections.size();

            for (const auto& [_, timer] : _commitModeCriticalSections) {
                totalTimeActiveCommit += timer.elapsed();
            }
            activeCommitCriticalSectionsCount = _commitModeCriticalSections.size();

            for (const auto& timer : _waitersList) {
                totalTimeWaiting += timer.elapsed();
            }
            numWaiters = _waitersList.size();
        }

        builder.append("totalTimeWaiting", duration_cast<Milliseconds>(totalTimeWaiting).count());
        builder.append("activeWaitersCount", numWaiters);
        builder.append("activeCatchupCount", activeCatchupCriticalSectionsCount);
        builder.append("activeCommitCount", activeCommitCriticalSectionsCount);
        builder.append("totalTimeActiveCatchupMillis",
                       duration_cast<Milliseconds>(totalTimeActiveCatchup).count());
        builder.append("totalTimeActiveCommitMillis",
                       duration_cast<Milliseconds>(totalTimeActiveCommit).count());
    }

    void registerCatchupCriticalSection(const Key& keyNamespace) {
        std::lock_guard lk(_mutex);
        _catchupModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.erase(keyNamespace);
        _catchupModeCriticalSections.emplace(keyNamespace, Timer{});
    }

    void registerCommitCriticalSection(const Key& keyNamespace) {
        std::lock_guard lk(_mutex);
        _catchupModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.emplace(keyNamespace, Timer{});
    }

    void releaseCriticalSection(const Key& keyNamespace) {
        std::lock_guard lk(_mutex);
        _catchupModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.erase(keyNamespace);
    }

    CriticalSectionWaiterToken startWaiter() {
        CriticalSectionWaiterToken token;
        std::lock_guard lk(_mutex);
        _waitersList.emplace_back();
        token.pos = std::prev(_waitersList.end());
        return token;
    }

    void finishWaiter(CriticalSectionWaiterToken token) {
        std::lock_guard lk(_mutex);
        _waitersList.erase(token.pos);
    }

private:
    mutable std::mutex _mutex;
    stdx::unordered_map<Key, Timer> _commitModeCriticalSections;
    stdx::unordered_map<Key, Timer> _catchupModeCriticalSections;
    std::list<Timer> _waitersList;

    Atomic<int64_t> _activeWaiters{0};
    Atomic<int64_t> _totalTimeWaiting{0};
};
}  // namespace mongo
