/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/platform/atomic_word.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/tick_source.h"

namespace mongo {
namespace transport {
/*
 * This is the base class of ServiceExecutors.
 *
 * Service executors should derive from this class and implement scheduleImpl(). They may
 * get timing/counter statistics by calling getStats().
 */
class ServiceExecutorBase : public ServiceExecutor {
public:
    Status schedule(Task task) final;

    struct Stats {
        TickSource::Tick ticksRunning;  // Total number of ticks spent running tasks
        TickSource::Tick ticksQueued;   // Total number of ticks tasks have spent waiting to run
        int64_t tasksExecuted;          // Total number of tasks executed
        int64_t tasksScheduled;         // Total number of tasks scheduled
        int64_t outstandingTasks;       // Current number of tasks waiting to be run
    };

    Stats getStats() const;
    void appendStats(BSONObjBuilder* bob) const final;

protected:
    explicit ServiceExecutorBase(ServiceContext* ctx);

    TickSource* tickSource() const;

private:
    // Sub-classes should implement this function to actually schedule the task. It will be called
    // by schedule() with a wrapped task that does all the necessary stats/timing tracking.
    virtual Status _schedule(Task task) = 0;

    TickSource* _tickSource;
    AtomicWord<TickSource::Tick> _ticksRunning{0};
    AtomicWord<TickSource::Tick> _ticksQueued{0};
    AtomicWord<int64_t> _tasksExecuted{0};
    AtomicWord<int64_t> _tasksScheduled{0};
    AtomicWord<int64_t> _outstandingTasks{0};
};

}  // namespace transport
}  // namespace mongo
