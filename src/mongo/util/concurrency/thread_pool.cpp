/* threadpool.cpp
*/

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
                , _thread(boost::bind(&Worker::loop, this))
            {}

            // destructor will block until current operation is completed
            // Acts as a "join" on this thread
            ~Worker() {
                _task.put(Task());
                _thread.join();
            }

            void set_task(Task& func) {
                verify(!func.empty());
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
                    if (task.empty())
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
            scoped_lock lock(_mutex);
            while (nThreads-- > 0) {
                Worker* worker = new Worker(*this);
                _freeWorkers.push_front(worker);
            }
        }

        ThreadPool::~ThreadPool() {
            join();

            verify(_tasks.empty());

            // O(n) but n should be small
            verify(_freeWorkers.size() == (unsigned)_nThreads);

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
