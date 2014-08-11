/* threadpool.cpp
*/

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

#include "mongo/pch.h"

#include "mongo/util/concurrency/thread_pool.h"

#include <boost/thread/thread.hpp>

#include "mongo/util/concurrency/mvar.h"

namespace mongo {
    namespace threadpool {

        // Worker thread
        class Worker : boost::noncopyable {
        public:
            explicit Worker(ThreadPool& owner)
                : _owner(owner)
                , _is_done(true)
                , _thread(stdx::bind(&Worker::loop, this))
            {}

            // destructor will block until current operation is completed
            // Acts as a "join" on this thread
            ~Worker() {
                _task.put(Task());
                _thread.join();
            }

            void set_task(Task& func) {
                verify(func);
                verify(_is_done);
                _is_done = false;

                _task.put(func);
            }

        private:
            ThreadPool& _owner;
            MVar<Task> _task;
            bool _is_done; // only used for error detection
            boost::thread _thread;

            void loop() {
                while (true) {
                    Task task = _task.take();
                    if (!task)
                        break; // ends the thread

                    try {
                        task();
                    }
                    catch (DBException& e) {
                        log() << "Unhandled DBException: " << e.toString() << endl;
                    }
                    catch (std::exception& e) {
                        log() << "Unhandled std::exception in worker thread: " << e.what() << endl;;
                    }
                    catch (...) {
                        log() << "Unhandled non-exception in worker thread" << endl;
                    }
                    _is_done = true;
                    _owner.task_done(this);
                }
            }
        };

        ThreadPool::ThreadPool(int nThreads)
            : _mutex("ThreadPool"), _tasksRemaining(0)
            , _nThreads(nThreads) {
            startThreads();
        }

        ThreadPool::ThreadPool(const DoNotStartThreadsTag&, int nThreads)
            : _mutex("ThreadPool"), _tasksRemaining(0)
            , _nThreads(nThreads) {
        }

        void ThreadPool::startThreads() {
            scoped_lock lock(_mutex);
            for (int i = 0; i < _nThreads; ++i) {
                Worker* worker = new Worker(*this);
                if (_tasks.empty()) {
                    _freeWorkers.push_front(worker);
                }
                else {
                    worker->set_task(_tasks.front());
                    _tasks.pop_front();
                }
            }
        }

        ThreadPool::~ThreadPool() {
            join();

            verify(_tasksRemaining == 0);

            while(!_freeWorkers.empty()) {
                delete _freeWorkers.front();
                _freeWorkers.pop_front();
            }
        }

        void ThreadPool::join() {
            scoped_lock lock(_mutex);
            while(_tasksRemaining) {
                _condition.wait(lock.boost());
            }
        }

        void ThreadPool::schedule(Task task) {
            scoped_lock lock(_mutex);

            _tasksRemaining++;

            if (!_freeWorkers.empty()) {
                _freeWorkers.front()->set_task(task);
                _freeWorkers.pop_front();
            }
            else {
                _tasks.push_back(task);
            }
        }

        // should only be called by a worker from the worker thread
        void ThreadPool::task_done(Worker* worker) {
            scoped_lock lock(_mutex);

            if (!_tasks.empty()) {
                worker->set_task(_tasks.front());
                _tasks.pop_front();
            }
            else {
                _freeWorkers.push_front(worker);
            }

            _tasksRemaining--;

            if(_tasksRemaining == 0)
                _condition.notify_all();
        }

    } //namespace threadpool
} //namespace mongo
