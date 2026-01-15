/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

#include <list>

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
class MONGO_MOD_PARENT_PRIVATE CriticalSectionStatistics {
public:
    void report(BSONObjBuilder& builder) const {
        Microseconds totalTimeActiveCatchup{0};
        Microseconds totalTimeActiveCommit{0};
        Microseconds totalTimeWaiting{0};
        int64_t activeCatchupCriticalSectionsCount;
        int64_t activeCommitCriticalSectionsCount;
        int64_t numWaiters;

        {
            stdx::lock_guard lk(_mutex);

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
        stdx::lock_guard lk(_mutex);
        _catchupModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.erase(keyNamespace);
        _catchupModeCriticalSections.emplace(keyNamespace, Timer{});
    }

    void registerCommitCriticalSection(const Key& keyNamespace) {
        stdx::lock_guard lk(_mutex);
        _catchupModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.emplace(keyNamespace, Timer{});
    }

    void releaseCriticalSection(const Key& keyNamespace) {
        stdx::lock_guard lk(_mutex);
        _catchupModeCriticalSections.erase(keyNamespace);
        _commitModeCriticalSections.erase(keyNamespace);
    }

    CriticalSectionWaiterToken startWaiter() {
        CriticalSectionWaiterToken token;
        stdx::lock_guard lk(_mutex);
        _waitersList.emplace_back();
        token.pos = std::prev(_waitersList.end());
        return token;
    }

    void finishWaiter(CriticalSectionWaiterToken token) {
        stdx::lock_guard lk(_mutex);
        _waitersList.erase(token.pos);
    }

private:
    mutable stdx::mutex _mutex;
    stdx::unordered_map<Key, Timer> _commitModeCriticalSections;
    stdx::unordered_map<Key, Timer> _catchupModeCriticalSections;
    std::list<Timer> _waitersList;

    AtomicWord<int64_t> _activeWaiters{0};
    AtomicWord<int64_t> _totalTimeWaiting{0};
};
}  // namespace mongo
