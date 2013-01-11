/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

// DeadlineMonitor unit tests

#include "mongo/pch.h"

#include "mongo/scripting/v8_deadline_monitor.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
    class TaskGroup {
    public:
        TaskGroup() : _m("TestGroup"), _c(), _killCount(0), _targetKillCount(0) { }
        void noteKill() {
            scoped_lock lk(_m);
            ++_killCount;
            if (_killCount >= _targetKillCount)
                _c.notify_one();
        }
        void waitForKillCount(uint64_t target) {
            scoped_lock lk(_m);
            _targetKillCount = target;
            while (_killCount < _targetKillCount)
                _c.wait(lk.boost());
        }
    private:
        mongo::mutex _m;
        boost::condition _c;
        uint64_t _killCount;
        uint64_t _targetKillCount;
    };

    class Task {
    public:
        Task() : _group(NULL), _killed(0) { }
        explicit Task(TaskGroup* group) : _group(group), _killed(0) { }
        void kill() {
            _killed = curTimeMillis64();
            if (_group)
                _group->noteKill();
        }
        TaskGroup* _group;
        uint64_t _killed;
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
        vector<shared_ptr<Task> > tasks;

        // start 100 tasks with varying deadlines (1-100 hours)
        for (int i=1; i<=100; i++) {
            shared_ptr<Task> task(new Task());
            dm.startDeadline(task.get(), i * 3600 * 1000);
            tasks.push_back(task);
        }

        // verify each deadline is stopped arrival
        for (vector<shared_ptr<Task> >::iterator i = tasks.begin();
             i != tasks.end();
             ++i) {
            ASSERT(dm.stopDeadline(i->get()));
            ASSERT(!(*i)->_killed);
        }
    }

    // multiple tasks expire before stopping the deadline
    TEST(DeadlineMonitor, MultipleTasksExpire) {
        DeadlineMonitor<Task> dm;
        TaskGroup group;
        vector<shared_ptr<Task> > tasks;

        // start 100 tasks with varying deadlines
        for (int i=1; i<=100; i++) {
            shared_ptr<Task> task(new Task(&group));
            dm.startDeadline(task.get(), i);
            tasks.push_back(task);
        }

        group.waitForKillCount(100);

        // verify each deadline has expired
        for (vector<shared_ptr<Task> >::iterator i = tasks.begin();
             i != tasks.end();
             ++i) {
            ASSERT(!dm.stopDeadline(i->get()));
            ASSERT((*i)->_killed);
        }
    }

    // mixed expiration and completion
    TEST(DeadlineMonitor, MultipleTasksExpireOrComplete) {
        DeadlineMonitor<Task> dm;
        TaskGroup group;
        vector<shared_ptr<Task> > expiredTasks; // tasks that should expire
        vector<shared_ptr<Task> > stoppedTasks; // tasks that should not expire

        // start 100 tasks with varying deadlines
        for (int i=1; i<=100; i++) {
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
        for (vector<shared_ptr<Task> >::iterator i = expiredTasks.begin();
             i != expiredTasks.end();
             ++i) {
            ASSERT(!dm.stopDeadline(i->get()));
            ASSERT((*i)->_killed);
        }

        // check tasks with a deadline that was stopped
        for (vector<shared_ptr<Task> >::iterator i = stoppedTasks.begin();
             i != stoppedTasks.end();
             ++i) {
            ASSERT(!(*i)->_killed);
        }
    }

} // namespace mongo
