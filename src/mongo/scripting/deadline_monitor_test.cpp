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

// DeadlineMonitor unit tests

#include "mongo/scripting/deadline_monitor.h"

#include "mongo/base/string_data.h"
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
        stdx::lock_guard<stdx::mutex> lk(_m);
        ++_killCount;
        if (_killCount >= _targetKillCount)
            _c.notify_one();
    }
    void waitForKillCount(uint64_t target) {
        stdx::unique_lock<stdx::mutex> lk(_m);
        _targetKillCount = target;
        while (_killCount < _targetKillCount)
            _c.wait(lk);
    }

private:
    stdx::mutex _m;
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
        return killPending;
    }
    TaskGroup* _group;
    uint64_t _killed;
    bool killPending = false;
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
    task.killPending = true;
    group.waitForKillCount(1);
    ASSERT(task._killed);
}
}  // namespace mongo
