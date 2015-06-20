/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/old_thread_pool.h"

#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mvar.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// Worker thread
class OldThreadPool::Worker {
public:
    explicit Worker(OldThreadPool& owner, const std::string& threadName)
        : _owner(owner), _is_done(true), _thread(stdx::bind(&Worker::loop, this, threadName)) {}

    // destructor will block until current operation is completed
    // Acts as a "join" on this thread
    ~Worker() {
        _task.put(Task());
        _thread.join();
    }

    void set_task(Task& func) {
        invariant(func);
        invariant(_is_done);
        _is_done = false;

        _task.put(func);
    }

private:
    OldThreadPool& _owner;
    MVar<Task> _task;
    bool _is_done;  // only used for error detection
    stdx::thread _thread;

    void loop(const std::string& threadName) {
        setThreadName(threadName);
        while (true) {
            Task task = _task.take();
            if (!task)
                break;  // ends the thread

            try {
                task();
            } catch (DBException& e) {
                log() << "Unhandled DBException: " << e.toString();
            } catch (std::exception& e) {
                log() << "Unhandled std::exception in worker thread: " << e.what();
            } catch (...) {
                log() << "Unhandled non-exception in worker thread";
            }
            _is_done = true;
            _owner.task_done(this);
        }
    }
};

OldThreadPool::OldThreadPool(int nThreads, const std::string& threadNamePrefix)
    : OldThreadPool(DoNotStartThreadsTag(), nThreads, threadNamePrefix) {
    startThreads();
}

OldThreadPool::OldThreadPool(const DoNotStartThreadsTag&,
                             int nThreads,
                             const std::string& threadNamePrefix)
    : _tasksRemaining(0), _nThreads(nThreads), _threadNamePrefix(threadNamePrefix) {}

void OldThreadPool::startThreads() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    for (int i = 0; i < _nThreads; ++i) {
        const std::string threadName(_threadNamePrefix.empty() ? _threadNamePrefix : str::stream()
                                             << _threadNamePrefix << i);
        Worker* worker = new Worker(*this, threadName);
        if (_tasks.empty()) {
            _freeWorkers.push_front(worker);
        } else {
            worker->set_task(_tasks.front());
            _tasks.pop_front();
        }
    }
}

OldThreadPool::~OldThreadPool() {
    join();

    invariant(_tasksRemaining == 0);

    while (!_freeWorkers.empty()) {
        delete _freeWorkers.front();
        _freeWorkers.pop_front();
    }
}

void OldThreadPool::join() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    while (_tasksRemaining) {
        _condition.wait(lock);
    }
}

void OldThreadPool::schedule(Task task) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _tasksRemaining++;

    if (!_freeWorkers.empty()) {
        _freeWorkers.front()->set_task(task);
        _freeWorkers.pop_front();
    } else {
        _tasks.push_back(task);
    }
}

// should only be called by a worker from the worker thread
void OldThreadPool::task_done(Worker* worker) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (!_tasks.empty()) {
        worker->set_task(_tasks.front());
        _tasks.pop_front();
    } else {
        _freeWorkers.push_front(worker);
    }

    _tasksRemaining--;

    if (_tasksRemaining == 0)
        _condition.notify_all();
}

}  // namespace mongo
