// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// DeadlineMonitor unit tests

#include "mongo/scripting/deadline_monitor.h"

#include "mongo/platform/atomic.h"
#include "mongo/unittest/unittest.h"

#include <vector>

#include <absl/container/node_hash_map.h>

namespace mongo {

using std::shared_ptr;
using std::vector;

class TaskGroup {
public:
    TaskGroup() : _c(), _killCount(0), _targetKillCount(0) {}
    void noteKill() {
        std::lock_guard<std::mutex> lk(_m);
        ++_killCount;
        if (_killCount >= _targetKillCount)
            _c.notify_one();
    }
    void waitForKillCount(uint64_t target) {
        std::unique_lock<std::mutex> lk(_m);
        _targetKillCount = target;
        while (_killCount < _targetKillCount)
            _c.wait(lk);
    }

private:
    std::mutex _m;
    stdx::condition_variable _c;
    uint64_t _killCount;
    uint64_t _targetKillCount;
};

class Task {
public:
    Task() : _group(nullptr), _killed(0) {}
    explicit Task(TaskGroup* group) : _group(group), _killed(0) {}
    void kill() {
        _killed = curTimeMillis64();
        if (_group)
            _group->noteKill();
    }
    void interrupt() {}
    bool isKillPending() {
        return killPending.load();
    }
    TaskGroup* _group;
    uint64_t _killed;
    Atomic<bool> killPending{false};
};

// single task expires before stopping the deadline
TEST(DeadlineMonitor, ExpireThenRemove) {
    DeadlineMonitor<Task> dm;
    TaskGroup group;
    Task task(&group);
    dm.startDeadline(&task, 10);
    group.waitForKillCount(1);
    ASSERT(task._killed);
    ASSERT(!dm.stopDeadline(&task));
}

// single task deadline stopped before the task expires
TEST(DeadlineMonitor, RemoveBeforeExpire) {
    DeadlineMonitor<Task> dm;
    Task task;
    dm.startDeadline(&task, 3600 * 1000);
    ASSERT(dm.stopDeadline(&task));
    ASSERT(!task._killed);
}

// multiple tasks complete before deadline expires (with 10ms window)
TEST(DeadlineMonitor, MultipleTasksCompleteBeforeExpire) {
    DeadlineMonitor<Task> dm;
    vector<shared_ptr<Task>> tasks;

    // start 100 tasks with varying deadlines (1-100 hours)
    for (int i = 1; i <= 100; i++) {
        shared_ptr<Task> task(new Task());
        dm.startDeadline(task.get(), i * 3600 * 1000);
        tasks.push_back(task);
    }

    // verify each deadline is stopped arrival
    for (vector<shared_ptr<Task>>::iterator i = tasks.begin(); i != tasks.end(); ++i) {
        ASSERT(dm.stopDeadline(i->get()));
        ASSERT(!(*i)->_killed);
    }
}

// multiple tasks expire before stopping the deadline
TEST(DeadlineMonitor, MultipleTasksExpire) {
    DeadlineMonitor<Task> dm;
    TaskGroup group;
    vector<shared_ptr<Task>> tasks;

    // start 100 tasks with varying deadlines
    for (int i = 1; i <= 100; i++) {
        shared_ptr<Task> task(new Task(&group));
        dm.startDeadline(task.get(), i);
        tasks.push_back(task);
    }

    group.waitForKillCount(100);

    // verify each deadline has expired
    for (vector<shared_ptr<Task>>::iterator i = tasks.begin(); i != tasks.end(); ++i) {
        ASSERT(!dm.stopDeadline(i->get()));
        ASSERT((*i)->_killed);
    }
}

// mixed expiration and completion
TEST(DeadlineMonitor, MultipleTasksExpireOrComplete) {
    DeadlineMonitor<Task> dm;
    TaskGroup group;
    vector<shared_ptr<Task>> expiredTasks;  // tasks that should expire
    vector<shared_ptr<Task>> stoppedTasks;  // tasks that should not expire

    // start 100 tasks with varying deadlines
    for (int i = 1; i <= 100; i++) {
        shared_ptr<Task> task(new Task(&group));
        if (i % 2 == 0) {
            // stop every other task
            dm.startDeadline(task.get(), i * 3600 * 1000);
            dm.stopDeadline(task.get());
            stoppedTasks.push_back(task);
            continue;
        }
        dm.startDeadline(task.get(), i);
        expiredTasks.push_back(task);
    }

    group.waitForKillCount(50);

    // check tasks which exceed the deadline
    for (vector<shared_ptr<Task>>::iterator i = expiredTasks.begin(); i != expiredTasks.end();
         ++i) {
        ASSERT(!dm.stopDeadline(i->get()));
        ASSERT((*i)->_killed);
    }

    // check tasks with a deadline that was stopped
    for (vector<shared_ptr<Task>>::iterator i = stoppedTasks.begin(); i != stoppedTasks.end();
         ++i) {
        ASSERT(!(*i)->_killed);
    }
}

TEST(DeadlineMonitor, IsKillPendingKills) {
    DeadlineMonitor<Task> dm;
    TaskGroup group;
    Task task(&group);
    dm.startDeadline(&task, -1);
    task.killPending.store(true);
    group.waitForKillCount(1);
    ASSERT(task._killed);
}

// Regression test for BF-44678. A task with an infinite deadline (timeoutMs <= 0) is killed only
// via the periodic isKillPending() poll. Registering such a task must wake a monitor that is
// already parked in its indefinite wait because it previously had no tasks -- otherwise the poll
// never resumes and the task hangs forever (the notify used to be sent only when the new deadline
// beat _nearestDeadlineWallclock, which an infinite deadline never does).
TEST(DeadlineMonitor, InfiniteDeadlineWakesIdleMonitor) {
    DeadlineMonitor<Task> dm;
    TaskGroup group;

    // Prime the monitor: register a task that expires almost immediately and wait for its kill.
    // Once observed, the monitor has emptied its task list and is parking in the indefinite wait.
    // This reliably reproduces the "monitor idle, then task added" order that hung.
    Task primer(&group);
    dm.startDeadline(&primer, 1);
    group.waitForKillCount(1);

    // The mutex is released by the monitor only once it is parked, so this call is serialized after
    // the monitor is idle. Without the fix it sends no notify and the monitor sleeps forever.
    Task task(&group);
    dm.startDeadline(&task, -1);
    task.killPending.store(true);
    group.waitForKillCount(2);
    ASSERT(task._killed);
}
}  // namespace mongo
